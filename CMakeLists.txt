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

# Stage 2: Multi-Client Blocking Server & Full-Duplex Client
add_executable(stage2_server
    src/stage2/server.cpp
)
target_link_libraries(stage2_server PRIVATE net_common)

add_executable(stage2_client
    src/stage2/client.cpp
)
target_link_libraries(stage2_client PRIVATE net_common)

# Stage 3: Thread Pool and Work Queues
add_executable(stage3_server
    src/stage3/server.cpp
)
target_link_libraries(stage3_server PRIVATE net_common)

# Stage 4: Non-Blocking Sockets and select()
add_executable(stage4_server
    src/stage4/server.cpp
)
target_link_libraries(stage4_server PRIVATE net_common)

