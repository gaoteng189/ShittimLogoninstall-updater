#include "updater/http_client.h"

#include "updater/logger.h"
#include "updater/util.h"

#include <winhttp.h>

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace updater {
namespace {

struct WinHttpCloser {
    void operator()(void* handle) const {
        if (handle != nullptr) {
            WinHttpCloseHandle(static_cast<HINTERNET>(handle));
        }
    }
};
using WinHttpHandle = std::unique_ptr<void, WinHttpCloser>;

struct FileCloser {
    void operator()(void* handle) const {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(handle));
        }
    }
};
using FileHandle = std::unique_ptr<void, FileCloser>;

struct UrlParts {
    std::wstring host;
    std::wstring path;  // 含查询串
    INTERNET_PORT port = 80;
    bool secure = false;
};

struct RequestOutcome {
    bool ok = false;
    bool retryable = false;
    bool cancelled = false;
    int statusCode = 0;
    std::uint64_t bytesWritten = 0;
    std::wstring finalUrl;
    std::string error;
};

bool CrackUrl(const std::wstring& url, UrlParts& parts, std::string& error) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (url.empty() || !WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
        error = "URL 格式无效：" + WideToUtf8(url);
        return false;
    }
    if (components.dwHostNameLength == 0) {
        error = "URL 中缺少主机名：" + WideToUtf8(url);
        return false;
    }

    parts.host.assign(components.lpszHostName, components.dwHostNameLength);
    parts.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    parts.port = components.nPort;

    parts.path.assign(components.lpszUrlPath ? components.lpszUrlPath : L"", components.dwUrlPathLength);
    if (parts.path.empty()) {
        parts.path = L"/";
    }
    if (components.dwExtraInfoLength > 0 && components.lpszExtraInfo != nullptr) {
        parts.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    return true;
}

std::wstring BuildBaseUrl(const UrlParts& parts) {
    std::wstring base = parts.secure ? L"https://" : L"http://";
    base.append(parts.host);
    const bool defaultPort = (parts.secure && parts.port == 443) || (!parts.secure && parts.port == 80);
    if (!defaultPort) {
        base.push_back(L':');
        base.append(std::to_wstring(parts.port));
    }
    return base;
}

// 把 Location 头（可能是绝对或相对地址）解析为绝对 URL。
std::wstring ResolveLocation(const UrlParts& current, const std::wstring& location) {
    const std::wstring base = BuildBaseUrl(current);
    if (location.find(L"://") != std::wstring::npos) {
        return location;
    }
    if (location.empty()) {
        return base + current.path;
    }
    if (location[0] == L'/') {
        return base + location;
    }
    if (location[0] == L'?') {
        const std::size_t query = current.path.find(L'?');
        const std::wstring stem =
            query == std::wstring::npos ? current.path : current.path.substr(0, query);
        return base + stem + location;
    }

    // 相对路径：以当前路径所在目录为基准。
    std::wstring directory = current.path;
    const std::size_t slash = directory.find_last_of(L'/');
    directory = slash == std::wstring::npos ? L"/" : directory.substr(0, slash + 1);
    return base + directory + location;
}

std::wstring QueryHeaderString(HINTERNET request, DWORD query) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (size == 0) {
        return std::wstring();
    }
    std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
    DWORD actual = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &actual,
                             WINHTTP_NO_HEADER_INDEX)) {
        return std::wstring();
    }
    buffer.resize(actual / sizeof(wchar_t));
    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return buffer;
}

std::uint64_t QueryContentLength(HINTERNET request, bool& known) {
    known = false;
    const std::wstring text = Trim(QueryHeaderString(request, WINHTTP_QUERY_CONTENT_LENGTH));
    if (text.empty()) {
        return 0;
    }
    wchar_t* end = nullptr;
    const unsigned long long value = wcstoull(text.c_str(), &end, 10);
    if (end == text.c_str() || value == 0) {
        return 0;
    }
    known = true;
    return static_cast<std::uint64_t>(value);
}

std::wstring NormalizeProxy(const std::wstring& proxy) {
    std::wstring value = Trim(proxy);
    const std::size_t scheme = value.find(L"://");
    if (scheme != std::wstring::npos) {
        value = value.substr(scheme + 3);
    }
    const std::size_t slash = value.find(L'/');
    if (slash != std::wstring::npos) {
        value = value.substr(0, slash);
    }
    return value;
}

// 单次请求（内部自动跟随重定向）。retryable 表示该失败值得重试。
RequestOutcome PerformRequest(const std::wstring& url, const HttpDownloadOptions& options) {
    RequestOutcome outcome;
    std::wstring currentUrl = url;

    for (int redirect = 0; redirect <= options.maxRedirects; ++redirect) {
        UrlParts parts;
        if (!CrackUrl(currentUrl, parts, outcome.error)) {
            outcome.retryable = false;
            return outcome;
        }

        const std::wstring normalizedProxy = NormalizeProxy(options.proxy);
        const bool useProxy = !normalizedProxy.empty();
        WinHttpHandle session(WinHttpOpen(
            options.userAgent.c_str(),
            useProxy ? WINHTTP_ACCESS_TYPE_NAMED_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            useProxy ? normalizedProxy.c_str() : WINHTTP_NO_PROXY_NAME,
            useProxy && !options.proxyBypass.empty() ? options.proxyBypass.c_str()
                                                     : WINHTTP_NO_PROXY_BYPASS,
            0));
        if (!session) {
            outcome.error = "WinHttpOpen 失败：" +
                            WideToUtf8(FormatSystemError(GetLastError()));
            outcome.retryable = true;
            return outcome;
        }

        const int timeout = std::max(1000, options.timeoutMs);
        WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);

        WinHttpHandle connection(
            WinHttpConnect(session.get(), parts.host.c_str(), parts.port, 0));
        if (!connection) {
            outcome.error = "无法连接到 " + WideToUtf8(parts.host) + "：" +
                            WideToUtf8(FormatSystemError(GetLastError()));
            outcome.retryable = true;
            return outcome;
        }

        WinHttpHandle request(WinHttpOpenRequest(
            connection.get(), L"GET", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0));
        if (!request) {
            outcome.error = "创建 HTTP 请求失败：" +
                            WideToUtf8(FormatSystemError(GetLastError()));
            outcome.retryable = true;
            return outcome;
        }

        // 自行跟随重定向，以便记录最终 URL 并复用同一套日志；设置失败不影响正确性。
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                         sizeof(redirectPolicy));

        if (options.allowInsecureTls) {
            DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                                  SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                                  SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                                  SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(request.get(), WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags,
                             sizeof(securityFlags));
        }

        WinHttpAddRequestHeaders(request.get(),
                                 L"Accept: */*\r\nCache-Control: no-cache\r\n",
                                 static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

        if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            outcome.error = "发送请求失败：" + WideToUtf8(FormatSystemError(GetLastError()));
            outcome.retryable = true;
            return outcome;
        }
        if (!WinHttpReceiveResponse(request.get(), nullptr)) {
            outcome.error = "接收响应失败：" + WideToUtf8(FormatSystemError(GetLastError()));
            outcome.retryable = true;
            return outcome;
        }

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(request.get(),
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX)) {
            outcome.error = "读取 HTTP 状态码失败：" +
                            WideToUtf8(FormatSystemError(GetLastError()));
            outcome.retryable = true;
            return outcome;
        }
        outcome.statusCode = static_cast<int>(statusCode);

        if (statusCode >= 300 && statusCode < 400) {
            const std::wstring location =
                Trim(QueryHeaderString(request.get(), WINHTTP_QUERY_LOCATION));
            if (location.empty()) {
                outcome.error = WideToUtf8(
                    Format(L"收到 HTTP %u 重定向响应，但缺少 Location 头",
                           static_cast<unsigned>(statusCode)));
                outcome.retryable = false;
                return outcome;
            }
            currentUrl = ResolveLocation(parts, location);
            LogDebug(Format(L"重定向（%u）→ %s", static_cast<unsigned>(statusCode),
                            currentUrl.c_str()));
            continue;
        }

        if (statusCode != 200) {
            outcome.error = WideToUtf8(
                Format(L"服务器返回 HTTP %u", static_cast<unsigned>(statusCode)));
            // 5xx 视为可重试，4xx 是请求本身的问题。
            outcome.retryable = statusCode >= 500;
            return outcome;
        }

        bool totalKnown = false;
        const std::uint64_t total = QueryContentLength(request.get(), totalKnown);

        FileHandle file(NormalizeFileHandle(
            CreateFileW(options.destinationPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr)));
        if (!file) {
            outcome.error = "无法创建目标文件 " + WideToUtf8(options.destinationPath) + "：" +
                            WideToUtf8(FormatSystemError(GetLastError()));
            outcome.retryable = false;
            return outcome;
        }

        std::vector<std::uint8_t> buffer(64 * 1024);
        for (;;) {
            if (options.cancelRequested) {
                outcome.cancelled = true;
                outcome.error = "下载已取消";
                return outcome;
            }

            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &available)) {
                outcome.error = "读取数据失败：" +
                                WideToUtf8(FormatSystemError(GetLastError()));
                outcome.retryable = true;
                return outcome;
            }
            if (available == 0) {
                break;  // 响应体结束
            }

            if (available > buffer.size()) {
                buffer.resize(available);
            }
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), available, &read)) {
                outcome.error = "读取数据失败：" +
                                WideToUtf8(FormatSystemError(GetLastError()));
                outcome.retryable = true;
                return outcome;
            }
            if (read == 0) {
                break;
            }

            DWORD written = 0;
            if (!WriteFile(file.get(), buffer.data(), read, &written, nullptr) || written != read) {
                outcome.error = "写入文件失败：" +
                                WideToUtf8(FormatSystemError(GetLastError()));
                outcome.retryable = false;
                return outcome;
            }

            outcome.bytesWritten += read;
            if (options.onProgress &&
                !options.onProgress(outcome.bytesWritten, total, totalKnown)) {
                outcome.cancelled = true;
                outcome.error = "下载已取消";
                return outcome;
            }
        }

        if (totalKnown && outcome.bytesWritten != total) {
            outcome.error = WideToUtf8(Format(L"下载不完整：期望 %llu 字节，实际 %llu 字节",
                                              static_cast<unsigned long long>(total),
                                              static_cast<unsigned long long>(
                                                  outcome.bytesWritten)));
            outcome.retryable = true;
            return outcome;
        }

        outcome.ok = true;
        outcome.finalUrl = currentUrl;
        return outcome;
    }

    outcome.error = WideToUtf8(Format(L"重定向次数超过上限（%d）", options.maxRedirects));
    outcome.retryable = false;
    return outcome;
}

}  // namespace

HttpDownloadResult HttpDownload(const HttpDownloadOptions& options) {
    HttpDownloadResult result;

    if (options.url.empty()) {
        result.error = "URL 为空";
        return result;
    }

    const int attempts = std::max(1, options.maxRetries + 1);
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (attempt > 1) {
            const int delaySeconds = std::min(2 * (attempt - 1), 10);
            LogWarn(Format(L"下载失败：%s", Utf8ToWide(result.error).c_str()));
            LogWarn(Format(L"%d 秒后进行第 %d 次重试（共 %d 次机会）...", delaySeconds, attempt,
                           attempts));
            Sleep(static_cast<DWORD>(delaySeconds) * 1000);
        }

        LogInfo(Format(L"开始下载：%s", options.url.c_str()));
        RequestOutcome outcome = PerformRequest(options.url, options);
        result.statusCode = outcome.statusCode;
        result.bytesWritten = outcome.bytesWritten;
        result.finalUrl = outcome.finalUrl;
        result.cancelled = outcome.cancelled;
        result.error = outcome.error;

        if (outcome.ok) {
            result.ok = true;
            result.error.clear();
            if (!outcome.finalUrl.empty() && outcome.finalUrl != options.url) {
                LogDebug(Format(L"最终地址：%s", outcome.finalUrl.c_str()));
            }
            LogInfo(Format(L"下载完成：%s，共 %s",
                           GetFileName(options.destinationPath).c_str(),
                           FormatBytes(outcome.bytesWritten).c_str()));
            return result;
        }

        if (outcome.cancelled || !outcome.retryable) {
            return result;
        }
    }

    LogError(Format(L"下载失败：%s", Utf8ToWide(result.error).c_str()));
    return result;
}

}  // namespace updater
