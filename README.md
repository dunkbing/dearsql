<p align="center">
  <img src="assets/appicon.png" width="128" alt="DearSQL">
</p>

<h1 align="center">DearSQL</h1>

<p align="center">A simple, cross-platform database client built with Dear ImGui.</p>

![Screenshot](assets/sc.webp)

## Features

- Support SQLite, PostgreSQL, MySQL, MariaDB, MongoDB, and Redis connections
- Cross-platform: macOS (Metal), Linux (GTK4 + OpenGL)
- Database browser: Sidebar for exploring schemas, tables, and structure
- Native file dialogs for SQLite files
- Native alert/confirm dialogs per platform
- Saves and restores previous database connections
- Run SQL queries with formatted results display
- Query history

## Built With

- [Dear ImGui](https://github.com/ocornut/imgui) - Immediate mode GUI
- [Native File Dialog](https://github.com/btzy/nativefiledialog-extended) - File pickers
- [hiredis](https://github.com/redis/hiredis) - Redis client
- [mongocxx](https://github.com/mongodb/mongo-cxx-driver) - MongoDB C++ driver
- [IconFontCppHeaders](https://github.com/juliettef/IconFontCppHeaders) - Icon fonts
