#import <CoreData/CoreData.h>
#import <Foundation/Foundation.h>

#include "utils/synced_store.hpp"
#include "mydear/frames.hpp"

#include <spdlog/spdlog.h>

#include <atomic>

namespace {
    NSString* ns(const std::string& s) {
        return [NSString stringWithUTF8String:s.c_str()];
    }

    std::string str(NSString* s) {
        return s ? std::string(s.UTF8String) : std::string();
    }

    // built in code: no .xcdatamodeld to compile. every attribute optional, no unique
    // constraints, as cloudkit requires
    NSManagedObjectModel* model() {
        static NSManagedObjectModel* m = [] {
            auto attribute = [](NSString* name, NSAttributeType type) {
                NSAttributeDescription* a = [NSAttributeDescription new];
                a.name = name;
                a.attributeType = type;
                a.optional = YES;
                return a;
            };
            NSEntityDescription* record = [NSEntityDescription new];
            record.name = @"Record";
            record.managedObjectClassName = NSStringFromClass(NSManagedObject.class);
            record.properties = @[
                attribute(@"kind", NSStringAttributeType),
                attribute(@"uuid", NSStringAttributeType),
                attribute(@"json", NSStringAttributeType),
                attribute(@"modified", NSDateAttributeType),
            ];
            NSManagedObjectModel* model = [NSManagedObjectModel new];
            model.entities = @[ record ];
            return model;
        }();
        return m;
    }
} // namespace

struct SyncedStore::Impl {
    NSPersistentContainer* container = nil;
    NSManagedObjectContext* context = nil;
    id observer = nil;
    std::atomic<bool> remoteChange{false};
    bool ok = false;

    ~Impl() {
        if (observer)
            [NSNotificationCenter.defaultCenter removeObserver:observer];
    }

    NSArray<NSManagedObject*>* fetch(NSString* kind, NSString* uuid) const {
        NSFetchRequest* request = [NSFetchRequest fetchRequestWithEntityName:@"Record"];
        request.predicate =
            uuid ? [NSPredicate predicateWithFormat:@"kind == %@ AND uuid == %@", kind, uuid]
                 : [NSPredicate predicateWithFormat:@"kind == %@", kind];
        request.sortDescriptors = @[ [NSSortDescriptor sortDescriptorWithKey:@"modified"
                                                                   ascending:NO] ];
        NSError* error = nil;
        NSArray* rows = [context executeFetchRequest:request error:&error];
        if (error)
            spdlog::error("synced store: fetch failed: {}", str(error.localizedDescription));
        return rows ?: @[];
    }

    void save() const {
        if (!context.hasChanges)
            return;
        NSError* error = nil;
        if (![context save:&error])
            spdlog::error("synced store: save failed: {}", str(error.localizedDescription));
    }
};

SyncedStore::SyncedStore(const std::string& file, const bool mirror)
    : impl_(std::make_unique<Impl>()) {
    @autoreleasepool {
        NSPersistentStoreDescription* description = [NSPersistentStoreDescription
            persistentStoreDescriptionWithURL:[NSURL fileURLWithPath:ns(file)]];
        // every process opens it the same way: a store once opened with history
        // tracking turns read-only when opened without it
        [description setOption:@YES forKey:NSPersistentHistoryTrackingKey];
        [description setOption:@YES forKey:NSPersistentStoreRemoteChangeNotificationPostOptionKey];
        description.shouldAddStoreAsynchronously = NO;

        NSPersistentContainer* container = nil;
#ifdef DEARSQL_ICLOUD_CONTAINER
        NSPersistentCloudKitContainer* cloud =
            [NSPersistentCloudKitContainer persistentContainerWithName:@"DearSQL"
                                                    managedObjectModel:model()];
        if (mirror)
            description.cloudKitContainerOptions = [[NSPersistentCloudKitContainerOptions alloc]
                initWithContainerIdentifier:@DEARSQL_ICLOUD_CONTAINER];
        container = cloud;
#else
        (void)mirror;
        container = [NSPersistentContainer persistentContainerWithName:@"DearSQL"
                                                    managedObjectModel:model()];
#endif
        container.persistentStoreDescriptions = @[ description ];
        __block NSError* loadError = nil;
        [container loadPersistentStoresWithCompletionHandler:^(NSPersistentStoreDescription*,
                                                               NSError* error) {
          loadError = error;
        }];
        if (loadError) {
            spdlog::error("synced store: could not open {}: {}", file,
                          str(loadError.localizedDescription));
            return;
        }
        impl_->container = container;
        impl_->context = [container newBackgroundContext];
        impl_->context.automaticallyMergesChangesFromParent = YES;
        impl_->context.mergePolicy = NSMergeByPropertyObjectTrumpMergePolicy;

        Impl* impl = impl_.get();
        impl_->observer = [NSNotificationCenter.defaultCenter
            addObserverForName:NSPersistentStoreRemoteChangeNotification
                        object:container.persistentStoreCoordinator
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      impl->remoteChange = true;
                      mydear::requestFrames();
                    }];
        impl_->ok = true;
    }
}

SyncedStore::~SyncedStore() = default;

bool SyncedStore::ok() const {
    return impl_->ok;
}

std::vector<SyncedStore::Record> SyncedStore::all(const std::string& kind) const {
    std::vector<Record> records;
    if (!impl_->ok)
        return records;
    Impl* impl = impl_.get();
    std::vector<Record>* out = &records; // blocks capture c++ locals as const copies
    [impl->context performBlockAndWait:^{
      @autoreleasepool {
          NSMutableSet<NSString*>* seen = [NSMutableSet set];
          for (NSManagedObject* row in impl->fetch(ns(kind), nil)) {
              NSString* uuid = [row valueForKey:@"uuid"];
              if (!uuid || [seen containsObject:uuid]) {
                  // an older copy of a record edited on two devices at once
                  [impl->context deleteObject:row];
                  continue;
              }
              [seen addObject:uuid];
              out->push_back({str(uuid), str([row valueForKey:@"json"])});
          }
          impl->save();
      }
    }];
    return records;
}

void SyncedStore::put(const std::string& kind, const std::string& uuid,
                      const std::string& json) const {
    if (!impl_->ok)
        return;
    Impl* impl = impl_.get();
    [impl->context performBlockAndWait:^{
      @autoreleasepool {
          NSArray<NSManagedObject*>* rows = impl->fetch(ns(kind), ns(uuid));
          NSManagedObject* row = rows.firstObject;
          for (NSUInteger i = 1; i < rows.count; ++i)
              [impl->context deleteObject:rows[i]];
          if (!row) {
              row = [NSEntityDescription insertNewObjectForEntityForName:@"Record"
                                                  inManagedObjectContext:impl->context];
              [row setValue:ns(kind) forKey:@"kind"];
              [row setValue:ns(uuid) forKey:@"uuid"];
          }
          [row setValue:ns(json) forKey:@"json"];
          [row setValue:[NSDate date] forKey:@"modified"];
          impl->save();
      }
    }];
}

void SyncedStore::remove(const std::string& kind, const std::string& uuid) const {
    if (!impl_->ok)
        return;
    Impl* impl = impl_.get();
    [impl->context performBlockAndWait:^{
      @autoreleasepool {
          for (NSManagedObject* row in impl->fetch(ns(kind), ns(uuid)))
              [impl->context deleteObject:row];
          impl->save();
      }
    }];
}

bool SyncedStore::takeRemoteChange() const {
    return impl_->remoteChange.exchange(false);
}

std::string SyncedStore::newUuid() {
    return str(NSUUID.UUID.UUIDString);
}

bool SyncedStore::syncConfigured() {
#ifdef DEARSQL_ICLOUD_CONTAINER
    return true;
#else
    return false;
#endif
}
