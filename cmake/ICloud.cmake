# icloud sync of connections, workspaces and settings (apple). off until a team is
# set up in the developer portal: the icloud container, cloudkit and keychain sharing
# capabilities on the app id, and a provisioning profile embedded when signing.
#   -DDEARSQL_TEAM_ID=ABCDE12345 [-DDEARSQL_ICLOUD_CONTAINER=iCloud.io.gitlab.dunkbing.dearsql]
# writes ${CMAKE_BINARY_DIR}/DearSQL.entitlements for codesign --entitlements
set(DEARSQL_TEAM_ID "" CACHE STRING "apple team id; enables icloud sync")
set(DEARSQL_ICLOUD_CONTAINER "iCloud.io.gitlab.dunkbing.dearsql" CACHE STRING "cloudkit container")

function(dearsql_icloud target)
    if(NOT APPLE OR DEARSQL_TEAM_ID STREQUAL "")
        return()
    endif()
    target_compile_definitions(${target} PRIVATE DEARSQL_ICLOUD_CONTAINER="${DEARSQL_ICLOUD_CONTAINER}")
    set(APP_ID "${DEARSQL_TEAM_ID}.io.gitlab.dunkbing.dearsql")
    if(IOS)
        set(APP_ID_KEY "application-identifier")
        set(APS_KEY "aps-environment")
    else()
        set(APP_ID_KEY "com.apple.application-identifier")
        set(APS_KEY "com.apple.developer.aps-environment")
    endif()
    file(WRITE ${CMAKE_BINARY_DIR}/DearSQL.entitlements "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
    <key>${APP_ID_KEY}</key><string>${APP_ID}</string>
    <key>com.apple.developer.team-identifier</key><string>${DEARSQL_TEAM_ID}</string>
    <key>com.apple.developer.icloud-container-identifiers</key><array><string>${DEARSQL_ICLOUD_CONTAINER}</string></array>
    <key>com.apple.developer.icloud-services</key><array><string>CloudKit</string></array>
    <key>${APS_KEY}</key><string>production</string>
    <key>keychain-access-groups</key><array><string>${APP_ID}</string></array>
</dict>
</plist>
")
endfunction()
