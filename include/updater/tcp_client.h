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
    // 要请求的文件名；留空表示请求发送端准备好的默认文件。
    std::wstring remoteName;
    // 本地保存目录。实际文件名优先采用服务端返回的名字，
    // 因此像 tcp://host:port 这样不带文件名的地址也能正确落盘。
    std::wstring destinationDirectory;
    std::wstring preferredFileName;  // 服务端未返回文件名时的回退值
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
    std::wstring servedName;  // 服务端返回的文件名
    std::wstring savedPath;   // 实际落盘的完整路径
    std::string error;
};

// 解析 tcp://host[:port]/path 形式的地址。
bool ParseTcpUrl(const std::wstring& url, TcpDownloadOptions& options, std::string& error);

// 执行传输（内部含失败重试）。
bool TcpDownload(const TcpDownloadOptions& options, TcpDownloadResult& result);

}  // namespace updater
