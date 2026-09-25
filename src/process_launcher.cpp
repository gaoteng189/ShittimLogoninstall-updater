#include "updater/process_launcher.h"

#include "updater/logger.h"
#include "updater/util.h"

#include <shellapi.h>

#include <memory>

namespace updater {
namespace {

struct HandleCloser {
    void operator()(void* handle) const {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(handle));
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

std::vector<std::wstring> ListSubDirectories(const std::wstring& path) {    std::vector<std::wstring> result;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = JoinPath(path, L"*");
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return result;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        // 跳过符号链接/联接点，避免遍历逃出解压目录。
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        result.push_back(JoinPath(path, name));
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return result;
}

// 通过 ShellExecuteEx("runas") 启动，触发 UAC 提权。
bool StartWithElevation(const LaunchOptions& options, const std::wstring& workingDirectory,
                        int showCommand, UniqueHandle& process, std::string& error) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = options.executable.c_str();
    info.lpParameters = options.arguments.empty() ? nullptr : options.arguments.c_str();
    info.lpDirectory = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
    info.nShow = showCommand;

    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        error = code == ERROR_CANCELLED
                    ? std::string("用户取消了管理员权限（UAC）请求")
                    : WideToUtf8(
                          Format(L"以管理员身份启动失败：%s", FormatSystemError(code).c_str()));
        return false;
    }
    process.reset(info.hProcess);
    return true;
}

}  // namespace

bool FindFileInDirectory(const std::wstring& root, const std::wstring& fileName,
                         std::wstring& foundPath) {
    foundPath.clear();
    if (!DirectoryExists(root) || fileName.empty()) {
        return false;
    }

    // 广度优先：先浅后深，保证优先命中压缩包根目录下的同名文件。
    std::vector<std::wstring> pending;
    pending.push_back(root);
    std::size_t visited = 0;

    while (visited < pending.size()) {
        const std::wstring current = pending[visited++];

        const std::wstring candidate = JoinPath(current, fileName);
        if (FileExists(candidate)) {
            foundPath = candidate;
            return true;
        }

        for (const std::wstring& child : ListSubDirectories(current)) {
            pending.push_back(child);
        }
        // 安全上限，避免异常的超深目录结构拖垮程序。
        if (pending.size() > 8192) {
            break;
        }
    }
    return false;
}

LaunchResult LaunchProcess(const LaunchOptions& options) {
    LaunchResult result;

    if (options.executable.empty() || !FileExists(options.executable)) {
        result.error = WideToUtf8(Format(L"可执行文件不存在：%s", options.executable.c_str()));
        return result;
    }

    std::wstring workingDirectory = options.workingDirectory;
    if (workingDirectory.empty()) {
        workingDirectory = GetDirectoryName(options.executable);
    }

    const int showCommand = options.hidden ? SW_HIDE : SW_SHOWNORMAL;
    UniqueHandle process;

    if (options.elevate) {
        if (!StartWithElevation(options, workingDirectory, showCommand, process, result.error)) {
            return result;
        }
    } else {
        std::wstring commandLine = L"\"" + options.executable + L"\"";
        if (!options.arguments.empty()) {
            commandLine += L" " + options.arguments;
        }
        std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
        commandBuffer.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = static_cast<WORD>(showCommand);

        PROCESS_INFORMATION info{};
        if (!CreateProcessW(options.executable.c_str(), commandBuffer.data(), nullptr, nullptr,
                            FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr,
                            workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup,
                            &info)) {
            const DWORD code = GetLastError();
            if (code == ERROR_ELEVATION_REQUIRED) {
                // 目标程序清单声明了 requireAdministrator：自动改走 UAC 提权路径。
                LogWarn(L"目标程序要求管理员权限，正在通过 UAC 重新启动……");
                if (!StartWithElevation(options, workingDirectory, showCommand, process,
                                        result.error)) {
                    return result;
                }
            } else {
                result.error = WideToUtf8(
                    Format(L"启动进程失败：%s", FormatSystemError(code).c_str()));
                return result;
            }
        } else {
            if (info.hThread != nullptr) {
                CloseHandle(info.hThread);
            }
            process.reset(info.hProcess);
        }
    }

    if (!process) {
        result.error = "未能获取目标进程句柄";
        return result;
    }

    if (!options.wait) {
        result.ok = true;
        return result;
    }

    const DWORD timeout = options.timeoutMs > 0 ? static_cast<DWORD>(options.timeoutMs) : INFINITE;
    const DWORD waitResult = WaitForSingleObject(process.get(), timeout);
    if (waitResult == WAIT_TIMEOUT) {
        result.ok = true;
        result.timedOut = true;
        return result;
    }
    if (waitResult != WAIT_OBJECT_0) {
        result.error = WideToUtf8(
            Format(L"等待进程结束失败：%s", FormatSystemError(GetLastError()).c_str()));
        return result;
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(process.get(), &exitCode)) {
        result.exitCode = exitCode;
    }
    result.ok = true;
    return result;
}

}  // namespace updater
