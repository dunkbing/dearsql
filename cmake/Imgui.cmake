set(IMGUI_SOURCES
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
)

# imshell compiles the platform and renderer backends (glfw, metal, dx11, opengl, uikit)

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
  if(NOT IOS)
    find_library(APPKIT_LIBRARY AppKit REQUIRED)
  endif()
  find_library(SECURITY_LIBRARY Security REQUIRED)

elseif(LINUX OR WIN32 OR ANDROID)
  # imshell links gtk4 + epoxy or d3d11
else()
  find_package(OpenGL REQUIRED)
  list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_opengl3.cpp)
endif()
