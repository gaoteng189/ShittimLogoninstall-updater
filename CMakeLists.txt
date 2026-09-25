cmake_minimum_required(VERSION 3.20)

project(ShittimLogonUpdater
    VERSION 1.0.0
    DESCRIPTION "下载压缩包、自动解压并运行其中安装程序的更新器"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ShittimLogon Updater 仅支持 Windows 平台。")
endif()

# 版本资源（res/version.rc）需要 RC 语言支持，放在平台检查之后启用，
# 这样非 Windows 平台得到的是上面那条更友好的错误信息。
enable_language(RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
#  公共构建配置
# ---------------------------------------------------------------------------
function(configure_target target output_name)
    target_include_directories(${target} PRIVATE include)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /utf-8 /permissive- /MP)
        # 静态链接 CRT，生成的 exe 可独立分发。
        set_target_properties(${target} PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_link_options(${target} PRIVATE /SUBSYSTEM:CONSOLE)
    endif()
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${output_name}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endfunction()

# ---- 客户端（接收端）：下载/接收压缩包、解压并运行安装程序 ---------------
add_executable(ShittimLogonUpdater
    src/main.cpp
    src/http_client.cpp
    src/tcp_client.cpp
    src/inflate.cpp
    src/zip_extractor.cpp
    src/process_launcher.cpp
    src/sha256.cpp
    src/logger.cpp
    src/util.cpp
    res/version.rc
)

target_link_libraries(ShittimLogonUpdater PRIVATE
    winhttp
    bcrypt
    shell32
    ole32
    advapi32
    ws2_32
)

configure_target(ShittimLogonUpdater "ShittimLogonUpdater")

# ---- 发送端：通过原始 TCP 提供文件下载服务 ------------------------------
add_executable(ShittimLogonSender
    src/sender_main.cpp
    src/file_server.cpp
    src/inflate.cpp
    src/logger.cpp
    src/util.cpp
    res/version-sender.rc
)

target_link_libraries(ShittimLogonSender PRIVATE ws2_32)

configure_target(ShittimLogonSender "ShittimLogonSender")
