set(IMGUI_SOURCES
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
)

if(LINUX)
  list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_opengl3.cpp)
else()
  list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_glfw.cpp)
endif()

set(IMGUI_NODE_EDITOR_SOURCES
    external/imgui-node-editor/imgui_node_editor.cpp
    external/imgui-node-editor/imgui_node_editor_api.cpp
    external/imgui-node-editor/crude_json.cpp
    external/imgui-node-editor/imgui_canvas.cpp
)

if(APPLE)
  find_library(METAL_LIBRARY Metal REQUIRED)
  find_library(QUARTZCORE_LIBRARY QuartzCore REQUIRED)
  find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
  find_library(APPKIT_LIBRARY AppKit REQUIRED)
  find_library(SECURITY_LIBRARY Security REQUIRED)

  list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_metal.mm)
elseif(LINUX)
  # no extra imgui sources needed for GTK4+OpenGL
elseif(WIN32)
  list(
        APPEND IMGUI_SOURCES
        external/imgui/backends/imgui_impl_dx11.cpp
        external/imgui/backends/imgui_impl_win32.cpp
    )
else()
  find_package(OpenGL REQUIRED)
  list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_opengl3.cpp)
endif()
