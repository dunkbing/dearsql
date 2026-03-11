# ODPI-C (Oracle Database Programming Interface for C)
#
# Builds ODPI-C from the vendored submodule (external/odpi).
# ODPI-C dynamically loads Oracle Instant Client at runtime via dlopen,
# so NO Oracle headers or libraries are needed at build time.
# The app compiles everywhere; Oracle connections work only when
# Instant Client is installed on the target machine.

add_library(odpi STATIC external/odpi/embed/dpi.c)
target_include_directories(odpi PUBLIC external/odpi/include)
set_target_properties(odpi PROPERTIES C_STANDARD 11)

if(UNIX AND NOT APPLE)
    target_link_libraries(odpi PUBLIC dl)
elseif(APPLE)
    # dlopen is in libSystem on macOS, no extra link needed
endif()
