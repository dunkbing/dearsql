#pragma once

#include <memory>

class Application;
class DatabaseInterface;

void showConnectionDialog(Application* app);
void showEditConnectionDialog(Application* app, std::shared_ptr<DatabaseInterface> db);
void showCreateDatabaseDialog(Application* app, std::shared_ptr<DatabaseInterface> db);
