#include "ui/tab/tab.hpp"
#include <utility>

Tab::Tab(std::string name, const TabType type) : name(std::move(name)), type(type) {}
