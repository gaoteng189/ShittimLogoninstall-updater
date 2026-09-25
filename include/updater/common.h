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
constexpr const wchar_t* kDefaultUrl = L"http://tlwyuoybr.hd-bkt.clouddn.com/ShittimLogon.zip";
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
