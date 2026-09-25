// ---------------------------------------------------------------------------
//  基于 WinHTTP 的下载模块：支持 HTTPS、手动重定向跟随、失败重试、
//  下载进度回调与取消。
// ---------------------------------------------------------------------------
#pragma once

#include "updater/common.h"

#include <functional>

namespace updater {

struct HttpDownloadOptions {
    std::wstring url;
    std::wstring destinationPath;
    std::wstring userAgent = L"ShittimLogonUpdater/1.0";
    std::wstring proxy;        // 形如 "host:port" 或 "http://host:port"，空则用系统默认代理
    std::wstring proxyBypass;  // 形如 "<local>"，空则用系统默认
    int timeoutMs = 30000;     // 单次网络操作超时
    int maxRedirects = 10;
    int maxRetries = 3;  // 失败后的额外重试次数
    bool allowInsecureTls = false;
    bool cancelRequested = false;
    // 返回 false 表示请求取消下载。
    std::function<bool(std::uint64_t received, std::uint64_t total, bool totalKnown)> onProgress;
};

struct HttpDownloadResult {
    bool ok = false;
    bool cancelled = false;
    int statusCode = 0;
    std::uint64_t bytesWritten = 0;
    std::wstring finalUrl;
    std::string error;
};

HttpDownloadResult HttpDownload(const HttpDownloadOptions& options);

}  // namespace updater
