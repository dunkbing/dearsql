#include "platform/updater.hpp"

#ifdef __APPLE__

#import <Sparkle/Sparkle.h>

static SPUStandardUpdaterController* sUpdaterController = nil;

void initializeUpdater() {
    if (sUpdaterController) {
        return;
    }

    // Skip if no EdDSA public key is configured (prevents "updater failed to start" error)
    NSString* edKey = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"SUPublicEDKey"];
    if (!edKey || edKey.length == 0) {
        NSLog(@"Sparkle: SUPublicEDKey not set, skipping updater initialization");
        return;
    }

    sUpdaterController = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                                       updaterDelegate:nil
                                                                    userDriverDelegate:nil];
}

void checkForUpdates() {
    if (!sUpdaterController) {
        return;
    }
    [sUpdaterController checkForUpdates:nil];
}

void pollUpdater() {}
void cleanupUpdater() {}
bool isUpdateAvailable() {
    return false;
}
std::string getLatestVersion() {
    return "";
}

#endif
