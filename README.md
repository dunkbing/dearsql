# Dear SQL

A simple, cross-platform database client built with Dear ImGui.

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

4. **Open Database**: Use `File > Open Database` to connect to an SQLite database

## 🛠️ Built With

- [Dear ImGui](https://github.com/ocornut/imgui) - Immediate mode GUI
- [Native File Dialog](https://github.com/btzy/nativefiledialog-extended) - File pickers
- [pqxx](https://github.com/jtv/libpqxx) - PostgreSQL client
- [SQLite](https://sqlite.org/) - SQLite client
