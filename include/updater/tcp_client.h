// ---------------------------------------------------------------------------
//  TCP 传输客户端：从发送端服务拉取文件。
// ---------------------------------------------------------------------------
#pragma once

#include "updater/common.h"
#include "updater/tcp_protocol.h"

#include <functional>

namespace updater {

struct TcpDownloadOptions {
    std::wstring host;
    std::uint16_t port = tcp::kDefaultPort;
    std::wstring remoteName;  // 相对服务端根目录的文件名（可含子目录）
    std::wstring destinationPath;
    int connectTimeoutMs = 10000;
    int ioTimeoutMs = 60000;
    int maxRetries = 3;
    // 返回 false 表示取消传输。
    std::function<bool(std::uint64_t received, std::uint64_t total)> onProgress;
};

struct TcpDownloadResult {
    bool ok = false;
    bool cancelled = false;
    bool retryable = true;
    std::uint64_t bytesWritten = 0;
    std::uint32_t crc32 = 0;
    std::string error;
};

// 解析 tcp://host[:port]/path 形式的地址。
bool ParseTcpUrl(const std::wstring& url, TcpDownloadOptions& options, std::string& error);

// 执行传输（内部含失败重试）。
bool TcpDownload(const TcpDownloadOptions& options, TcpDownloadResult& result);

}  // namespace updater
