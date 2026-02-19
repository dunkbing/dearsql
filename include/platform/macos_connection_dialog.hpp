#pragma once

#ifdef __APPLE__

#include <memory>

class Application;
class DatabaseInterface;

void showMacOSConnectionDialog(Application* app);
void showMacOSEditConnectionDialog(Application* app, std::shared_ptr<DatabaseInterface> db,
                                   int connectionId);
void showMacOSCreateDatabaseDialog(Application* app, std::shared_ptr<DatabaseInterface> db);

#endif
