// ---------------------------------------------------------------------------
//  轻量日志模块：控制台（UTF-16 直写）+ 可选日志文件 + 行内进度条。
// ---------------------------------------------------------------------------
#pragma once

#include "updater/common.h"

namespace updater {

enum class LogLevel {
    Debug = 0,
    Info,
    Warn,
    Error,
    Off,
};

void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();

// 追加写入的日志文件；传入空串表示关闭文件日志。
// 文件日志不受控制台日志级别影响（始终保留信息级及以上），
// 因此 --quiet --log-file 组合仍能得到完整记录。
bool SetLogFilePath(const std::wstring& path);
void SetColorEnabled(bool enabled);

void LogMessage(LogLevel level, const std::wstring& message);
void LogDebug(const std::wstring& message);
void LogInfo(const std::wstring& message);
void LogWarn(const std::wstring& message);
void LogError(const std::wstring& message);

// 直接写标准输出，不经过日志级别过滤（用于用法说明、横幅等）。
void WriteRawText(const std::wstring& text);

// 行内刷新的进度显示（仅当标准输出是控制台时生效）。
void ShowProgress(const std::wstring& text);
void FinishProgress();

}  // namespace updater
