cmake_minimum_required(VERSION 3.14)
project(Tls104MasterWin VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_WEBVIEW_GUI "Build with native webview window support" OFF)

# Windows-specific settings
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
    add_definitions(-DNOMINMAX)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
    # Use static CRT library for standalone executable
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
endif()

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")

# Find packages
find_package(Threads REQUIRED)

# lib60870 configuration
set(BUILD_HAL ON CACHE BOOL "Build HAL" FORCE)
set(BUILD_COMMON ON CACHE BOOL "Build common code" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)

# Download lib60870 (populate only, build after mbedtls setup)
include(FetchContent)
FetchContent_Declare(
    lib60870
    GIT_REPOSITORY https://github.com/mz-automation/lib60870.git
    GIT_TAG master
)
set(FETCHCONTENT_UPDATES_DISCONNECTED TRUE)
FetchContent_GetProperties(lib60870)
if(NOT lib60870_POPULATED)
    FetchContent_Populate(lib60870)
endif()

# Upgrade to mbedtls 3.6 for TLS 1.2/1.3 support
set(LIB60870_DEPS "${lib60870_SOURCE_DIR}/lib60870-C/dependencies")
if(NOT EXISTS "${LIB60870_DEPS}/mbedtls-3.6.2")
    message(STATUS "Downloading mbedtls 3.6.2 for TLS 1.3 support...")
    FetchContent_Declare(
        mbedtls3
        URL https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v3.6.2.tar.gz
    )
    FetchContent_GetProperties(mbedtls3)
    if(NOT mbedtls3_POPULATED)
        FetchContent_Populate(mbedtls3)
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${mbedtls3_SOURCE_DIR}" "${LIB60870_DEPS}/mbedtls-3.6.2")
    # Remove old mbedtls-2.28 so lib60870 auto-detects 3.6
    if(EXISTS "${LIB60870_DEPS}/mbedtls-2.28")
        execute_process(COMMAND ${CMAKE_COMMAND} -E rename
            "${LIB60870_DEPS}/mbedtls-2.28" "${LIB60870_DEPS}/_mbedtls-2.28.old")
    endif()
endif()

# macOS fix: lib60870's CMakeLists.txt unconditionally adds -lrt for UNIX,
# but macOS doesn't have the 'rt' library. Remove -lrt for APPLE.
if(APPLE)
    # Patch lib60870/src/CMakeLists.txt
    set(LIB60870_SRC_CMAKE "${lib60870_SOURCE_DIR}/lib60870-C/src/CMakeLists.txt")
    if(EXISTS "${LIB60870_SRC_CMAKE}")
        file(READ "${LIB60870_SRC_CMAKE}" _content)
        # Remove "    -lrt" line from the IF(UNIX) block in lib60870
        string(REPLACE "\n        -lrt" "\n" _content "${_content}")
        file(WRITE "${LIB60870_SRC_CMAKE}" "${_content}")
        message(STATUS "Patched: removed -lrt from ${LIB60870_SRC_CMAKE}")
    endif()

    # Patch lib60870/src/hal/CMakeLists.txt
    set(LIB60870_HAL_CMAKE "${lib60870_SOURCE_DIR}/lib60870-C/src/hal/CMakeLists.txt")
    if(EXISTS "${LIB60870_HAL_CMAKE}")
        file(READ "${LIB60870_HAL_CMAKE}" _content)
        string(REPLACE "\n        -lrt" "\n" _content "${_content}")
        file(WRITE "${LIB60870_HAL_CMAKE}" "${_content}")
        message(STATUS "Patched: removed -lrt from ${LIB60870_HAL_CMAKE}")
    endif()
endif()

add_subdirectory(${lib60870_SOURCE_DIR}/lib60870-C ${CMAKE_BINARY_DIR}/lib60870-C-build)

# Optional: webview for native GUI window
if(ENABLE_WEBVIEW_GUI)
    FetchContent_Declare(
        webview
        GIT_REPOSITORY https://github.com/webview/webview.git
        GIT_TAG 0.12.0
    )
    FetchContent_MakeAvailable(webview)
endif()

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

if(ENABLE_WEBVIEW_GUI)
    list(APPEND SOURCES src/gui/webview_window.cpp)
    list(APPEND HEADERS src/gui/webview_window.h)
endif()

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
    target_link_options(tls104_master_win PRIVATE -static)
    target_link_libraries(tls104_master_win PRIVATE ws2_32)
    target_compile_definitions(tls104_master_win PRIVATE _WIN32=1)
else()
    target_link_libraries(tls104_master_win PRIVATE dl)
endif()

# Optional native GUI via webview
if(ENABLE_WEBVIEW_GUI)
    target_link_libraries(tls104_master_win PRIVATE webview::core)
    target_compile_definitions(tls104_master_win PRIVATE ENABLE_WEBVIEW_GUI=1)

    # Linux: need GTK3 and WebKit2GTK
    if(UNIX AND NOT APPLE)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
        pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.0)
        target_include_directories(tls104_master_win PRIVATE ${GTK3_INCLUDE_DIRS} ${WEBKIT_INCLUDE_DIRS})
        target_link_libraries(tls104_master_win PRIVATE ${GTK3_LIBRARIES} ${WEBKIT_LIBRARIES})
    endif()
endif()

# Force TLS support
target_compile_definitions(tls104_master_win PRIVATE LIB60870_HAS_TLS_SUPPORT=1)

# Embed web/index.html into the executable as a C byte array
set(EMBEDDED_WEB_DIR ${CMAKE_BINARY_DIR}/generated)
set(EMBEDDED_WEB_HEADER ${EMBEDDED_WEB_DIR}/embedded_web.h)
file(MAKE_DIRECTORY ${EMBEDDED_WEB_DIR})
add_custom_command(
    OUTPUT ${EMBEDDED_WEB_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${CMAKE_SOURCE_DIR}/web/index.html
        -DOUTPUT_FILE=${EMBEDDED_WEB_HEADER}
        -DVAR_NAME=embedded_index_html
        -P ${CMAKE_SOURCE_DIR}/cmake/embed_resource.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/web/index.html
    COMMENT "Embedding web/index.html into executable..."
)
add_custom_target(embedded_web DEPENDS ${EMBEDDED_WEB_HEADER})
add_dependencies(tls104_master_win embedded_web)
target_include_directories(tls104_master_win PRIVATE ${EMBEDDED_WEB_DIR})

# Install
install(TARGETS tls104_master_win DESTINATION bin)
