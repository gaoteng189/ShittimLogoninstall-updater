// ---------------------------------------------------------------------------
//  TCP 文件发送服务：把本地目录下的文件提供给客户端拉取。
// ---------------------------------------------------------------------------
#pragma once

#include "updater/common.h"
#include "updater/tcp_protocol.h"

#include <atomic>

namespace updater {

struct FileServerOptions {
    std::wstring rootDirectory;
    // 客户端未指定文件名时提供的文件（相对于 rootDirectory）。
    // 启动时会校验它确实存在，缺失则直接报错退出。
    std::wstring defaultFileName;
    std::wstring bindAddress = L"0.0.0.0";
    std::uint16_t port = tcp::kDefaultPort;
    bool singleShot = false;
    bool allowListing = false;
    int ioTimeoutMs = 120000;
};

// 阻塞运行直到 stopRequested 置位。返回 false 表示启动失败。
bool RunFileServer(const FileServerOptions& options, const std::atomic<bool>& stopRequested,
                   std::string& error);

}  // namespace updater
