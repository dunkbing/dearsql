# Dear SQL

A simple, cross-platform database client built with Dear ImGui.

## Features

- Support SQLite, PostgreSQL, MySQL, and Redis connections
- Cross-platform: macOS (Metal), Windows/Linux (OpenGL)
- Database browser: Sidebar for exploring schemas, tables, and structure
- Native file dialogs for sqlite files
- Saves and restores previous database connections
- Run SQL queries with formatted results display

## 🚀 Quick Start

1. **Clone with submodules**:

   ```bash
   git clone --recursive https://github.com/dunkbing/dear-sql.git
   ```

2. **Build**:

   ```bash
   mkdir build && cd build
   cmake ..
   make
   ```

3. **Run**:

   ```bash
   ./dear-sql
   ```

## Built With

- [Dear ImGui](https://github.com/ocornut/imgui) - Immediate mode GUI
- [Native File Dialog](https://github.com/btzy/nativefiledialog-extended) - File pickers
- [soci](https://github.com/SOCI/soci) - DB client
- [hiredis](https://github.com/redis/hiredis) - DB client
- [IconFontCppHeaders](https://github.com/juliettef/IconFontCppHeaders) - icon fonts
