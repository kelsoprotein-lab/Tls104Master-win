cmake_minimum_required(VERSION 3.14)
project(Tls104MasterWin VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Windows-specific settings
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
    add_definitions(-DNOMINMAX)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
endif()

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# Find packages
find_package(Threads REQUIRED)

# WebView2 (Windows only)
if(WIN32)
    # Try to find WebView2 package
    find_package(WebView2 QUIET)
    if(NOT WebView2_FOUND)
        message(STATUS "WebView2 not found, will download on first build")
    endif()
endif()

# lib60870 configuration
set(BUILD_HAL ON CACHE BOOL "Build HAL" FORCE)
set(BUILD_COMMON ON CACHE BOOL "Build common code" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(WITH_MBEDTLS ON CACHE BOOL "Enable mbedTLS support" FORCE)

# Download lib60870
include(FetchContent)
FetchContent_Declare(
    lib60870
    GIT_REPOSITORY https://github.com/mz-automation/lib60870.git
    GIT_TAG master
)
set(FETCHCONTENT_UPDATES_DISCONNECTED TRUE)
FetchContent_MakeAvailable(lib60870)

add_subdirectory(${lib60870_SOURCE_DIR}/lib60870-C ${CMAKE_BINARY_DIR}/lib60870-C-build)

# Source files
set(SOURCES
    src/main.cpp
    src/platform/socket.cpp
    src/ipc/bridge.cpp
    src/http/server.cpp
    src/iec104/connection.cpp
)

# Headers
set(HEADERS
    src/platform/socket.h
    src/ipc/bridge.h
    src/http/server.h
    src/iec104/connection.h
)

# Create executable
add_executable(tls104_master_win ${SOURCES} ${HEADERS})

target_include_directories(tls104_master_win PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${lib60870_SOURCE_DIR}/lib60870-C/config
    ${lib60870_SOURCE_DIR}/lib60870-C/src/inc/api
    ${lib60870_SOURCE_DIR}/lib60870-C/src/inc/internal
    ${lib60870_SOURCE_DIR}/lib60870-C/src/common/inc
    ${lib60870_SOURCE_DIR}/lib60870-C/src/hal/inc
)

# Link libraries
target_link_libraries(tls104_master_win PRIVATE
    lib60870
    Threads::Threads
)

# Windows-specific libraries
if(WIN32)
    target_link_libraries(tls104_master_win PRIVATE ws2_32)
    target_compile_definitions(tls104_master_win PRIVATE _WIN32=1)

    # WebView2
    if(WebView2_FOUND)
        target_link_libraries(tls104_master_win PRIVATE ${WEBVIEW2_LIB})
        target_include_directories(tls104_master_win PRIVATE ${WEBVIEW2_INCLUDE_DIR})
    endif()
else()
    target_link_libraries(tls104_master_win PRIVATE dl)
endif()

# Force TLS support
target_compile_definitions(tls104_master_win PRIVATE LIB60870_HAS_TLS_SUPPORT=1)

# Copy web files to build directory
if(WIN32)
    add_custom_command(TARGET tls104_master_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/web
        $<TARGET_FILE_DIR:tls104_master_win>/web
    )
endif()

# Install
install(TARGETS tls104_master_win DESTINATION bin)
