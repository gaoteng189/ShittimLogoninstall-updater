// ---------------------------------------------------------------------------
//  进程启动：在解压目录中定位目标程序，并以正确的工作目录启动它。
// ---------------------------------------------------------------------------
#pragma once

#include "updater/common.h"

namespace updater {

struct LaunchOptions {
    std::wstring executable;
    std::wstring workingDirectory;  // 留空则使用可执行文件所在目录
    std::wstring arguments;
    bool wait = true;
    bool elevate = false;
    bool hidden = false;
    int timeoutMs = 0;  // 仅当 wait 为 true 时有效，0 表示无限等待
};

struct LaunchResult {
    bool ok = false;
    bool timedOut = false;
    DWORD exitCode = 0;
    std::string error;
};

// 在 root 下递归查找文件名匹配的文件（不区分大小写），层级越浅越优先。
bool FindFileInDirectory(const std::wstring& root, const std::wstring& fileName,
                         std::wstring& foundPath);

LaunchResult LaunchProcess(const LaunchOptions& options);

}  // namespace updater
