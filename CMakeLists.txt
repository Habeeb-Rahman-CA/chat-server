cmake_minimum_required(VERSION 3.20)
project(tcp_chat_server VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict warnings for educational systems programming
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -g
    )
endif()

# Common networking library
add_library(net_common STATIC
    src/common/socket.cpp
)

target_include_directories(net_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Stage 1: Basic TCP Server & Client
add_executable(stage1_server
    src/stage1/server.cpp
)
target_link_libraries(stage1_server PRIVATE net_common)

add_executable(stage1_client
    src/stage1/client.cpp
)
target_link_libraries(stage1_client PRIVATE net_common)
