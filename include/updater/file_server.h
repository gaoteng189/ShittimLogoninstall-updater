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
    std::wstring bindAddress = L"0.0.0.0";
    std::uint16_t port = tcp::kDefaultPort;
    bool singleShot = false;      // 处理完一个连接就退出（便于测试/一次性分发）
    bool allowListing = false;    // 是否允许客户端查询文件列表
    int ioTimeoutMs = 120000;     // 单连接的读写超时
};

// 阻塞运行直到 stopRequested 置位。返回 false 表示启动失败。
bool RunFileServer(const FileServerOptions& options, const std::atomic<bool>& stopRequested,
                   std::string& error);

}  // namespace updater
