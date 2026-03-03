#pragma once

#if defined(__linux__)

#include <memory>

class Application;
class DatabaseInterface;

void showLinuxConnectionDialog(Application* app);
void showLinuxEditConnectionDialog(Application* app, std::shared_ptr<DatabaseInterface> db);
void showLinuxCreateDatabaseDialog(Application* app, std::shared_ptr<DatabaseInterface> db);

#endif
