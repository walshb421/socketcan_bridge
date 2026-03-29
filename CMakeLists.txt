cmake_minimum_required(VERSION 3.16)
project(ash LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options($<$<CONFIG:Debug>:-Wall>
                    $<$<CONFIG:Debug>:-Wextra>
                    $<$<CONFIG:Debug>:-Werror>)

include_directories(include)

# Server
add_executable(ash-server server/main.c server/server.c server/proto.c server/session.c server/iface.c server/def.c server/own.c server/cfg.c server/app.c)
target_compile_definitions(ash-server PRIVATE _GNU_SOURCE)
target_link_libraries(ash-server m)

# Client library
add_library(libash_shared SHARED lib/ash.c)
add_library(libash_static STATIC lib/ash.c)

set_target_properties(libash_shared PROPERTIES OUTPUT_NAME ash)
set_target_properties(libash_static PROPERTIES OUTPUT_NAME ash)

# Example clients — C
add_executable(ash-example         examples/basic_client.c)
add_executable(ash-example-monitor examples/signal_monitor.c)
add_executable(ash-example-drive   examples/drive_sequence.c)
add_executable(ash-example-loopback examples/vcan_loopback.c)

target_link_libraries(ash-example          libash_static)
target_link_libraries(ash-example-monitor  libash_static)
target_link_libraries(ash-example-drive    libash_static)
target_link_libraries(ash-example-loopback libash_static m)

# Python client library (ctypes wrapper — no C compilation required)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(ash-python
        COMMAND ${Python3_EXECUTABLE} -m py_compile ${CMAKE_SOURCE_DIR}/lib/ash.py
        COMMENT "Syntax-check Python client library (lib/ash.py)"
        VERBATIM
    )
else()
    message(WARNING "Python3 not found — ash-python target unavailable")
endif()
