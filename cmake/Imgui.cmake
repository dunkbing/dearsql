set(IMGUI_SOURCES
    ${IMSHELL_IMGUI_DIR}/imgui.cpp
    ${IMSHELL_IMGUI_DIR}/imgui_draw.cpp
    ${IMSHELL_IMGUI_DIR}/imgui_tables.cpp
    ${IMSHELL_IMGUI_DIR}/imgui_widgets.cpp
)

# imshell compiles the platform and renderer backends (glfw, metal, dx11, opengl, uikit)

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
  list(APPEND IMGUI_SOURCES ${IMSHELL_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
endif()
