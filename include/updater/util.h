// ---------------------------------------------------------------------------
//  通用工具：编码转换、路径处理、临时目录、文件读写、格式化。
// ---------------------------------------------------------------------------
#pragma once

#include "updater/common.h"

namespace updater {

// ---- 编码转换 ------------------------------------------------------------
std::string WideToUtf8(const std::wstring& text);
std::wstring Utf8ToWide(const std::string& text);
std::wstring MultiByteToWide(const std::string& text, UINT codePage);
std::string WideToMultiByte(const std::wstring& text, UINT codePage);

// ---- 路径 ----------------------------------------------------------------
std::wstring JoinPath(const std::wstring& base, const std::wstring& leaf);
std::wstring GetTempDirectory();
std::wstring GetCurrentDirectoryPath();
std::wstring GetExecutablePath();
std::wstring GetExecutableDirectory();
std::wstring GetDirectoryName(const std::wstring& path);
std::wstring GetFileName(const std::wstring& path);
std::wstring ToLower(const std::wstring& text);
bool EqualsNoCase(const std::wstring& lhs, const std::wstring& rhs);
bool EndsWithNoCase(const std::wstring& text, const std::wstring& suffix);
std::wstring Trim(const std::wstring& text);

// ---- 文件系统 ------------------------------------------------------------
// CreateFileW 失败时返回 INVALID_HANDLE_VALUE（不是 nullptr），
// 而 std::unique_ptr 之类的布尔判断只认 nullptr，因此必须先归一化再包装。
inline void* NormalizeFileHandle(HANDLE handle) {
    return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
}

bool PathExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
bool FileExists(const std::wstring& path);
bool EnsureDirectoryExists(const std::wstring& path);
bool CreateUniqueDirectory(const std::wstring& parent, std::wstring& createdPath);
bool RemoveDirectoryRecursive(const std::wstring& path);
bool ReadFileToMemory(const std::wstring& path, std::vector<std::uint8_t>& out, std::string& error);

// ---- 格式化 --------------------------------------------------------------
std::wstring Format(const wchar_t* format, ...);
std::wstring FormatBytes(std::uint64_t bytes);
std::wstring FormatDuration(double seconds);
std::wstring FormatSystemError(DWORD code);
std::uint64_t GetTickCount64Ms();

}  // namespace updater
