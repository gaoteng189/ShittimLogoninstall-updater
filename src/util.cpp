#include "updater/util.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

namespace updater {

// ---------------------------------------------------------------------------
//  编码转换
// ---------------------------------------------------------------------------
std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& text) {
    return MultiByteToWide(text, CP_UTF8);
}

std::wstring MultiByteToWide(const std::string& text, UINT codePage) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string WideToMultiByte(const std::wstring& text, UINT codePage) {
    if (text.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(codePage, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(codePage, 0, text.c_str(), static_cast<int>(text.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

// ---------------------------------------------------------------------------
//  路径
// ---------------------------------------------------------------------------
std::wstring JoinPath(const std::wstring& base, const std::wstring& leaf) {
    if (base.empty()) {
        return leaf;
    }
    if (leaf.empty()) {
        return base;
    }
    std::wstring result = base;
    const wchar_t last = result.back();
    if (last != L'\\' && last != L'/') {
        result.push_back(L'\\');
    }
    // 避免出现 "C:\a\\b" 这类重复分隔符。
    std::size_t index = 0;
    while (index < leaf.size() && (leaf[index] == L'\\' || leaf[index] == L'/')) {
        ++index;
    }
    result.append(leaf, index, std::wstring::npos);
    return result;
}

std::wstring GetTempDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0) {
            return L".";
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(length + 1);
    }
    while (!buffer.empty() && (buffer.back() == L'\\' || buffer.back() == L'/')) {
        buffer.pop_back();
    }
    return buffer.empty() ? L"." : buffer;
}

std::wstring GetExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::wstring();
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring GetExecutableDirectory() {
    return GetDirectoryName(GetExecutablePath());
}

std::wstring GetDirectoryName(const std::wstring& path) {
    const std::size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return std::wstring();
    }
    if (pos == 0) {
        return path.substr(0, 1);
    }
    // 形如 "C:\" 时保留盘符根。
    if (pos == 2 && path[1] == L':') {
        return path.substr(0, 3);
    }
    return path.substr(0, pos);
}

std::wstring GetFileName(const std::wstring& path) {
    const std::size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring ToLower(const std::wstring& text) {
    std::wstring result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return result;
}

bool EqualsNoCase(const std::wstring& lhs, const std::wstring& rhs) {
    return lhs.size() == rhs.size() && ToLower(lhs) == ToLower(rhs);
}

bool EndsWithNoCase(const std::wstring& text, const std::wstring& suffix) {
    if (suffix.size() > text.size()) {
        return false;
    }
    return EqualsNoCase(text.substr(text.size() - suffix.size()), suffix);
}

std::wstring Trim(const std::wstring& text) {
    const wchar_t* spaces = L" \t\r\n";
    const std::size_t begin = text.find_first_not_of(spaces);
    if (begin == std::wstring::npos) {
        return std::wstring();
    }
    const std::size_t end = text.find_last_not_of(spaces);
    return text.substr(begin, end - begin + 1);
}

// ---------------------------------------------------------------------------
//  文件系统
// ---------------------------------------------------------------------------
bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool EnsureDirectoryExists(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    if (DirectoryExists(path)) {
        return true;
    }

    // 逐级向上创建，兼容 UNC 与盘符根路径。
    std::wstring current;
    std::size_t index = 0;
    if (path.size() >= 2 && path[1] == L':') {
        current = path.substr(0, 2);
        index = 2;
    } else if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        // UNC 前缀 \\server\share
        std::size_t serverEnd = path.find(L'\\', 2);
        if (serverEnd == std::wstring::npos) {
            return false;
        }
        const std::size_t shareEnd = path.find(L'\\', serverEnd + 1);
        if (shareEnd == std::wstring::npos) {
            return DirectoryExists(path);
        }
        current = path.substr(0, shareEnd);
        index = shareEnd;
    }

    while (index < path.size()) {
        while (index < path.size() && (path[index] == L'\\' || path[index] == L'/')) {
            ++index;
        }
        const std::size_t next = path.find_first_of(L"\\/", index);
        const std::size_t end = next == std::wstring::npos ? path.size() : next;
        if (end > index) {
            if (!current.empty()) {
                current.push_back(L'\\');
            }
            current.append(path, index, end - index);
            if (!CreateDirectoryW(current.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS && !DirectoryExists(current)) {
                return false;
            }
        }
        index = end;
    }
    return DirectoryExists(path);
}

bool CreateUniqueDirectory(const std::wstring& parent, std::wstring& createdPath) {
    if (!EnsureDirectoryExists(parent)) {
        return false;
    }

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const std::uint64_t seed = static_cast<std::uint64_t>(counter.QuadPart) ^
                               (static_cast<std::uint64_t>(GetTickCount64Ms()) << 17) ^
                               (static_cast<std::uint64_t>(GetCurrentProcessId()) << 41);

    for (int attempt = 0; attempt < 64; ++attempt) {
        const std::uint64_t mixed = seed + static_cast<std::uint64_t>(attempt) * 0x9E3779B97F4A7C15ull;
        const std::wstring name =
            Format(L"run-%08x%08x", static_cast<unsigned>((mixed >> 32) & 0xFFFFFFFFu),
                   static_cast<unsigned>(mixed & 0xFFFFFFFFu));
        const std::wstring candidate = JoinPath(parent, name);
        if (CreateDirectoryW(candidate.c_str(), nullptr)) {
            createdPath = candidate;
            return true;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            continue;
        }
        break;
    }

    // 退化为随机后缀，避免整体失败。
    for (int attempt = 0; attempt < 64; ++attempt) {
        const unsigned suffix = static_cast<unsigned>(rand()) ^ (static_cast<unsigned>(attempt) << 16);
        const std::wstring candidate = JoinPath(parent, Format(L"run-%08x", suffix));
        if (CreateDirectoryW(candidate.c_str(), nullptr)) {
            createdPath = candidate;
            return true;
        }
    }
    return false;
}

bool RemoveDirectoryRecursive(const std::wstring& path) {
    if (!DirectoryExists(path)) {
        return true;
    }

    const std::wstring pattern = JoinPath(path, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring child = JoinPath(path, name);
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                RemoveDirectoryRecursive(child);
            } else {
                // 只读文件必须先清属性才能删除。
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
                    SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                }
                if (!DeleteFileW(child.c_str())) {
                    SetFileAttributesW(child.c_str(),
                                       data.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                    DeleteFileW(child.c_str());
                }
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    return RemoveDirectoryW(path.c_str()) != FALSE || !DirectoryExists(path);
}

bool ReadFileToMemory(const std::wstring& path, std::vector<std::uint8_t>& out, std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = WideToUtf8(Format(L"无法打开文件 %s：%s", path.c_str(),
                                  FormatSystemError(GetLastError()).c_str()));
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        error = WideToUtf8(Format(L"无法获取文件大小 %s：%s", path.c_str(),
                                  FormatSystemError(GetLastError()).c_str()));
        CloseHandle(file);
        return false;
    }

    out.clear();
    out.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < out.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(out.size() - offset, 8u * 1024u * 1024u));
        DWORD read = 0;
        if (!ReadFile(file, out.data() + offset, chunk, &read, nullptr)) {
            error = WideToUtf8(Format(L"读取文件失败 %s：%s", path.c_str(),
                                      FormatSystemError(GetLastError()).c_str()));
            CloseHandle(file);
            return false;
        }
        if (read == 0) {
            break;
        }
        offset += read;
    }
    out.resize(offset);
    CloseHandle(file);
    return true;
}

// ---------------------------------------------------------------------------
//  格式化
// ---------------------------------------------------------------------------
std::wstring Format(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    va_list probe;
    va_copy(probe, args);
    const int needed = _vscwprintf(format, probe);
    va_end(probe);

    if (needed < 0) {
        va_end(args);
        return std::wstring();
    }

    std::wstring buffer(static_cast<std::size_t>(needed) + 1, L'\0');
    _vsnwprintf_s(buffer.data(), buffer.size(), _TRUNCATE, format, args);
    va_end(args);
    buffer.resize(static_cast<std::size_t>(needed));
    return buffer;
}

std::wstring FormatBytes(std::uint64_t bytes) {
    static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return Format(L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return Format(L"%.1f %s", value, units[unit]);
}

std::wstring FormatDuration(double seconds) {
    if (seconds < 0.0 || seconds > 359999.0) {
        return L"--:--";
    }
    const int total = static_cast<int>(seconds + 0.5);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    if (hours > 0) {
        return Format(L"%d:%02d:%02d", hours, minutes, secs);
    }
    return Format(L"%02d:%02d", minutes, secs);
}

std::wstring FormatSystemError(DWORD code) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer),
        0, nullptr);

    std::wstring message;
    if (length != 0 && buffer != nullptr) {
        message.assign(buffer, length);
        LocalFree(buffer);
    }
    if (message.empty()) {
        message = Format(L"未知错误");
    }
    return Format(L"%s(0x%08X)", Trim(message).c_str(), static_cast<unsigned>(code));
}

std::uint64_t GetTickCount64Ms() {
    return static_cast<std::uint64_t>(GetTickCount64());
}

}  // namespace updater
