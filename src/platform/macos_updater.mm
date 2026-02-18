#include "platform/macos_updater.hpp"

#ifdef __APPLE__

#import <Sparkle/Sparkle.h>

static SPUStandardUpdaterController* sUpdaterController = nil;

void initializeSparkleUpdater() {
    if (sUpdaterController) {
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

#endif
