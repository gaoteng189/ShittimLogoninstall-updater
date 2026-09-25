cmake_minimum_required(VERSION 3.20)

project(ShittimLogonUpdater
    VERSION 1.0.0
    DESCRIPTION "下载压缩包、自动解压并运行其中安装程序的更新器"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ShittimLogon Updater 仅支持 Windows 平台。")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ShittimLogonUpdater
    src/main.cpp
    src/http_client.cpp
    src/inflate.cpp
    src/zip_extractor.cpp
    src/process_launcher.cpp
    src/sha256.cpp
    src/logger.cpp
    src/util.cpp
)

target_include_directories(ShittimLogonUpdater PRIVATE include)

target_compile_definitions(ShittimLogonUpdater PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(ShittimLogonUpdater PRIVATE
    winhttp
    bcrypt
    shell32
    ole32
    advapi32
)

if(MSVC)
    target_compile_options(ShittimLogonUpdater PRIVATE /W3 /utf-8 /permissive- /MP)
    # 静态链接 CRT，生成的 exe 可独立分发。
    set_target_properties(ShittimLogonUpdater PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(ShittimLogonUpdater PRIVATE /SUBSYSTEM:CONSOLE)
endif()

set_target_properties(ShittimLogonUpdater PROPERTIES
    OUTPUT_NAME "ShittimLogonUpdater"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
