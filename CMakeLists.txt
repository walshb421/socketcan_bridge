cmake_minimum_required(VERSION 3.16)
project(ash LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options($<$<CONFIG:Debug>:-Wall>
                    $<$<CONFIG:Debug>:-Wextra>
                    $<$<CONFIG:Debug>:-Werror>)

include_directories(include)

# Server
add_executable(ash-server server/main.c server/server.c server/proto.c server/session.c server/iface.c server/def.c server/own.c)
target_compile_definitions(ash-server PRIVATE _GNU_SOURCE)

# Client library
add_library(libash_shared SHARED lib/ash.c)
add_library(libash_static STATIC lib/ash.c)

set_target_properties(libash_shared PROPERTIES OUTPUT_NAME ash)
set_target_properties(libash_static PROPERTIES OUTPUT_NAME ash)
