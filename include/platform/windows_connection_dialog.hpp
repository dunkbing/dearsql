#pragma once

#if defined(_WIN32)

#include <memory>

class Application;
class DatabaseInterface;

void showWindowsConnectionDialog(Application* app);
void showWindowsEditConnectionDialog(Application* app, std::shared_ptr<DatabaseInterface> db);
void showWindowsCreateDatabaseDialog(Application* app, std::shared_ptr<DatabaseInterface> db);

#endif
