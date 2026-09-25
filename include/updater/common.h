// ---------------------------------------------------------------------------
//  ShittimLogon Updater - 公共基础头文件
//
//  统一收敛 Windows 头文件的包含顺序与宏定义，避免 min/max 宏污染
//  以及 winsock 与 windows.h 的冲突。
// ---------------------------------------------------------------------------
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

// 目标是 Windows 10 及以上：既用到 WinHTTP 的重定向策略选项，
// 也需要 WinHttpQueryDataAvailable 的现代行为。
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace updater {

// 程序版本，同时用于 User-Agent。
constexpr const wchar_t* kAppName = L"ShittimLogon Updater";
constexpr const wchar_t* kAppVersion = L"1.0.0";

// 客户端默认拉取地址：公网映射入口。发送端实际监听在 tcp::kDefaultPort（50304），
// 由端口映射 / 内网穿透将其暴露为 41792。
// 地址不带文件名，表示请求发送端准备好的默认文件（kDefaultPayloadArchive）。
constexpr const wchar_t* kDefaultUrl = L"tcp://1344a5becd3e.ofalias.com:41792";

// 发送端默认提供的压缩包名，以及解压后要运行的程序名。
constexpr const wchar_t* kDefaultPayloadArchive = L"ShittimLogon.zip";
constexpr const wchar_t* kDefaultPayloadExe = L"install.exe";

// 统一的进程退出码，便于脚本判断失败原因。
enum ExitCode : int {
    kExitSuccess = 0,
    kExitBadArguments = 1,
    kExitDownloadFailed = 2,
    kExitExtractFailed = 3,
    kExitPayloadNotFound = 4,
    kExitLaunchFailed = 5,
    kExitPayloadFailed = 6,
    kExitHashMismatch = 7,
    kExitInternalError = 8,
};

}  // namespace updater
