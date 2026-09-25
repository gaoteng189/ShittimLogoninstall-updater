#include "updater/logger.h"

#include "updater/util.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace updater {
namespace {

std::mutex g_mutex;
LogLevel g_level = LogLevel::Info;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
bool g_colorEnabled = true;
bool g_progressVisible = false;

bool StreamIsConsole(HANDLE handle) {
    DWORD mode = 0;
    return handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) != FALSE;
}

HANDLE StandardHandle(DWORD stdHandle) {
    return GetStdHandle(stdHandle);
}

// 控制台句柄走 WriteConsoleW（原生 UTF-16，中文不会乱码）；
// 被重定向到文件/管道时按控制台输出代码页编码，与 Windows 控制台程序的惯例保持一致
// （中文系统即 GBK），这样 PowerShell 捕获输出、记事本打开日志都不会乱码。
void WriteWide(HANDLE handle, const std::wstring& text) {
    if (handle == INVALID_HANDLE_VALUE || text.empty()) {
        return;
    }
    if (StreamIsConsole(handle)) {
        DWORD written = 0;
        WriteConsoleW(handle, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else {
        UINT codePage = GetConsoleOutputCP();
        if (codePage == 0) {
            codePage = CP_ACP;
        }
        const std::string encoded = WideToMultiByte(text, codePage);
        DWORD written = 0;
        WriteFile(handle, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr);
    }
}

const wchar_t* LevelLabel(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return L"调试";
        case LogLevel::Info:
            return L"信息";
        case LogLevel::Warn:
            return L"警告";
        case LogLevel::Error:
            return L"错误";
        default:
            return L"";
    }
}

WORD LevelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return FOREGROUND_INTENSITY;
        case LogLevel::Warn:
            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case LogLevel::Error:
            return FOREGROUND_RED | FOREGROUND_INTENSITY;
        default:
            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }
}

void WriteToLogFile(const std::wstring& line) {
    if (g_logFile == INVALID_HANDLE_VALUE) {
        return;
    }
    const std::string utf8 = WideToUtf8(line) + "\r\n";
    DWORD written = 0;
    WriteFile(g_logFile, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    FlushFileBuffers(g_logFile);
}

// 清空光标所在的整行并把光标移回行首。
// 用 FillConsoleOutputCharacter 而不是输出空格，是因为中文为双宽字符，
// 按字符数计算的空格数无法覆盖其在屏幕上占用的列数。
void ClearCurrentLineLocked(HANDLE out) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(out, &info)) {
        WriteWide(out, L"\r");
        return;
    }
    const COORD lineStart = {0, info.dwCursorPosition.Y};
    DWORD written = 0;
    FillConsoleOutputCharacterW(out, L' ', static_cast<DWORD>(info.dwSize.X), lineStart, &written);
    SetConsoleCursorPosition(out, lineStart);
}

void ClearProgressLocked() {
    if (!g_progressVisible) {
        return;
    }
    HANDLE out = StandardHandle(STD_OUTPUT_HANDLE);
    if (StreamIsConsole(out)) {
        ClearCurrentLineLocked(out);
    }
    g_progressVisible = false;
}

// 生成 "[HH:MM:SS] 级别 消息" 形式的行。
std::wstring ComposeLine(LogLevel level, const std::wstring& message) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return Format(L"[%02u:%02u:%02u] %s %s", st.wHour, st.wMinute, st.wSecond, LevelLabel(level),
                  message.c_str());
}

}  // namespace

void SetLogLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

LogLevel GetLogLevel() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_level;
}

bool SetLogFilePath(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    if (path.empty()) {
        return true;
    }
    g_logFile = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    // 写入 UTF-8 BOM，便于记事本等工具正确识别中文。
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    WriteFile(g_logFile, bom, sizeof(bom), &written, nullptr);
    return true;
}

void SetColorEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_colorEnabled = enabled;
}

void LogMessage(LogLevel level, const std::wstring& message) {
    std::lock_guard<std::mutex> lock(g_mutex);

    // 文件日志始终保留信息级以上的记录，便于 --quiet 场景下事后排查。
    const LogLevel fileLevel = g_level == LogLevel::Debug ? LogLevel::Debug : LogLevel::Info;
    const bool toConsole = g_level != LogLevel::Off && level >= g_level;
    const bool toFile = g_logFile != INVALID_HANDLE_VALUE && level >= fileLevel;
    if (!toConsole && !toFile) {
        return;
    }

    const std::wstring line = ComposeLine(level, message);

    if (toConsole) {
        ClearProgressLocked();
        HANDLE out = StandardHandle(level >= LogLevel::Warn ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
        if (StreamIsConsole(out) && g_colorEnabled) {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (GetConsoleScreenBufferInfo(out, &info)) {
                SetConsoleTextAttribute(out, LevelColor(level));
                WriteWide(out, line + L"\n");
                SetConsoleTextAttribute(out, info.wAttributes);
            } else {
                WriteWide(out, line + L"\n");
            }
        } else {
            WriteWide(out, line + L"\n");
        }
    }

    if (toFile) {
        WriteToLogFile(line);
    }
}

void LogDebug(const std::wstring& message) {
    LogMessage(LogLevel::Debug, message);
}

void LogInfo(const std::wstring& message) {
    LogMessage(LogLevel::Info, message);
}

void LogWarn(const std::wstring& message) {
    LogMessage(LogLevel::Warn, message);
}

void LogError(const std::wstring& message) {
    LogMessage(LogLevel::Error, message);
}

void WriteRawText(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ClearProgressLocked();
    WriteWide(StandardHandle(STD_OUTPUT_HANDLE), text);
}

void ShowProgress(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_level == LogLevel::Off) {
        return;
    }
    HANDLE out = StandardHandle(STD_OUTPUT_HANDLE);
    if (!StreamIsConsole(out)) {
        return;
    }
    ClearCurrentLineLocked(out);
    WriteWide(out, text);
    g_progressVisible = true;
}

void FinishProgress() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ClearProgressLocked();
}

}  // namespace updater
