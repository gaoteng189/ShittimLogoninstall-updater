#include "updater/tcp_client.h"

#include "updater/inflate.h"
#include "updater/logger.h"
#include "updater/util.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <memory>

#pragma comment(lib, "ws2_32.lib")

namespace updater {
namespace {

struct FileCloser {
    void operator()(void* handle) const {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(handle));
        }
    }
};
using FileHandle = std::unique_ptr<void, FileCloser>;

int WsaErrorCode() {
    return WSAGetLastError();
}

std::wstring DescribeWsaError(int code) {
    return Format(L"socket 错误 %d", code);
}

// 把 URL 中的 %XX 转义还原为 UTF-8 字节，再转成宽字符。
std::wstring PercentDecode(const std::wstring& text) {
    const auto hexValue = [](wchar_t ch) -> int {
        if (ch >= L'0' && ch <= L'9') {
            return ch - L'0';
        }
        if (ch >= L'a' && ch <= L'f') {
            return ch - L'a' + 10;
        }
        if (ch >= L'A' && ch <= L'F') {
            return ch - L'A' + 10;
        }
        return -1;
    };

    std::string bytes;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'%' && index + 2 < text.size()) {
            const int high = hexValue(text[index + 1]);
            const int low = hexValue(text[index + 2]);
            if (high >= 0 && low >= 0) {
                bytes.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        bytes += WideToUtf8(std::wstring(1, text[index]));
    }
    return Utf8ToWide(bytes);
}

// 非阻塞 connect + select，实现可控的连接超时。
tcp::Socket ConnectWithTimeout(const ADDRINFOW* address, int timeoutMs, std::string& error) {
    // 不能写成 tcp::Socket socket(socket(...))：变量名 socket 会遮蔽同名 WinSock 函数，
    // 新声明的名字在它自己的初始化器里就已可见，Clang 会把内层 socket(...) 当成调用该变量。
    const SOCKET rawSocket =
        ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    tcp::Socket socket(rawSocket);
    if (!socket.valid()) {
        error = "创建套接字失败：" + WideToUtf8(DescribeWsaError(WsaErrorCode()));
        return tcp::Socket();
    }

    u_long nonBlocking = 1;
    if (ioctlsocket(socket.get(), FIONBIO, &nonBlocking) != 0) {
        error = "设置套接字模式失败：" + WideToUtf8(DescribeWsaError(WsaErrorCode()));
        return tcp::Socket();
    }

    if (connect(socket.get(), address->ai_addr, static_cast<int>(address->ai_addrlen)) ==
        SOCKET_ERROR) {
        const int code = WsaErrorCode();
        if (code != WSAEWOULDBLOCK && code != WSAEINPROGRESS && code != WSAEINVAL) {
            error = "连接失败：" + WideToUtf8(DescribeWsaError(code));
            return tcp::Socket();
        }

        fd_set writeSet;
        fd_set errorSet;
        FD_ZERO(&writeSet);
        FD_ZERO(&errorSet);
        FD_SET(socket.get(), &writeSet);
        FD_SET(socket.get(), &errorSet);

        const int bounded = std::max(1, timeoutMs);
        timeval timeout{bounded / 1000, (bounded % 1000) * 1000};
        const int selected = select(0, nullptr, &writeSet, &errorSet, &timeout);

        if (selected == 0) {
            error = WideToUtf8(Format(L"连接超时（%d 毫秒）", bounded));
            return tcp::Socket();
        }
        if (selected == SOCKET_ERROR) {
            error = "select 失败：" + WideToUtf8(DescribeWsaError(WsaErrorCode()));
            return tcp::Socket();
        }

        int socketError = 0;
        int length = sizeof(socketError);
        if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError),
                       &length) != 0) {
            socketError = WsaErrorCode();
        }
        if (socketError != 0) {
            error = "连接失败：" + WideToUtf8(DescribeWsaError(socketError));
            return tcp::Socket();
        }
    }

    nonBlocking = 0;
    if (ioctlsocket(socket.get(), FIONBIO, &nonBlocking) != 0) {
        error = "恢复套接字模式失败：" + WideToUtf8(DescribeWsaError(WsaErrorCode()));
        return tcp::Socket();
    }
    return socket;
}

tcp::Socket ConnectToHost(const TcpDownloadOptions& options, std::string& error,
                          bool& retryable) {
    retryable = true;

    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::wstring port = std::to_wstring(options.port);

    ADDRINFOW* raw = nullptr;
    const int resolved = GetAddrInfoW(options.host.c_str(), port.c_str(), &hints, &raw);
    if (resolved != 0) {
        error = WideToUtf8(Format(L"无法解析主机名 %s（错误 %d）", options.host.c_str(), resolved));
        // 解析失败不会因为重试而改变，直接放弃。
        retryable = false;
        return tcp::Socket();
    }
    std::unique_ptr<ADDRINFOW, decltype(&FreeAddrInfoW)> addresses(raw, FreeAddrInfoW);

    std::string lastError;
    for (ADDRINFOW* candidate = addresses.get(); candidate != nullptr;
         candidate = candidate->ai_next) {
        std::string attemptError;
        tcp::Socket socket =
            ConnectWithTimeout(candidate, options.connectTimeoutMs, attemptError);
        if (!socket.valid()) {
            lastError = attemptError;
            continue;
        }

        const DWORD ioTimeout = static_cast<DWORD>(std::max(1000, options.ioTimeoutMs));
        setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&ioTimeout), sizeof(ioTimeout));
        setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&ioTimeout), sizeof(ioTimeout));

        const int noDelay = 1;
        setsockopt(socket.get(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
                   sizeof(noDelay));

        return socket;
    }

    error = lastError.empty() ? "无法连接到服务端" : lastError;
    return tcp::Socket();
}

// 服务端返回的文件名可能带路径成分，落盘时只取最后一段并过滤非法字符，
// 避免服务端借文件名把内容写到目标目录之外。
std::wstring SanitizeLocalFileName(const std::wstring& name, const std::wstring& fallback) {
    const std::size_t separator = name.find_last_of(L"\\/");
    std::wstring leaf = separator == std::wstring::npos ? name : name.substr(separator + 1);

    for (wchar_t& ch : leaf) {
        if (ch < 0x20 || wcschr(L"<>:\"|?*", ch) != nullptr) {
            ch = L'_';
        }
    }
    while (!leaf.empty() && (leaf.back() == L' ' || leaf.back() == L'.')) {
        leaf.pop_back();
    }

    if (leaf.empty() || leaf == L"." || leaf == L"..") {
        return fallback.empty() ? L"package.zip" : SanitizeLocalFileName(fallback, L"package.zip");
    }
    return leaf;
}

bool PerformTcpDownload(const TcpDownloadOptions& options, TcpDownloadResult& result) {
    const std::string nameUtf8 = WideToUtf8(options.remoteName);
    if (nameUtf8.size() > tcp::kMaxNameLength) {
        result.error = "请求的文件名过长";
        result.retryable = false;
        return false;
    }

    tcp::Request request{};
    std::copy(tcp::kMagic, tcp::kMagic + 4, request.magic);
    request.version = tcp::kProtocolVersion;
    request.command = static_cast<std::uint8_t>(tcp::Command::GetFile);
    request.nameLength = static_cast<std::uint16_t>(nameUtf8.size());

    std::string connectError;
    bool connectRetryable = true;
    tcp::Socket socket = ConnectToHost(options, connectError, connectRetryable);
    if (!socket.valid()) {
        result.error = connectError;
        result.retryable = connectRetryable;
        return false;
    }

    if (!tcp::SendAll(socket.get(), &request, sizeof(request)) ||
        (!nameUtf8.empty() &&
         !tcp::SendAll(socket.get(), nameUtf8.data(), nameUtf8.size()))) {
        result.error = "发送请求失败（连接可能已被服务端关闭）";
        result.retryable = true;
        return false;
    }

    tcp::Header header{};
    if (!tcp::ReceiveAll(socket.get(), &header, sizeof(header))) {
        result.error = "读取响应头失败（连接中断）";
        result.retryable = true;
        return false;
    }
    if (!tcp::HasMagic(header.magic)) {
        result.error = "响应格式不正确：该端口上运行的可能不是 ShittimLogon 发送端服务";
        result.retryable = false;
        return false;
    }
    if (header.version != tcp::kProtocolVersion) {
        result.error = WideToUtf8(Format(L"协议版本不匹配：服务端为 %u，本程序为 %u",
                                          static_cast<unsigned>(header.version),
                                          static_cast<unsigned>(tcp::kProtocolVersion)));
        result.retryable = false;
        return false;
    }

    // 响应头之后依次是文件名、错误信息、文件数据 —— 顺序必须与服务端严格一致，
    // 漏读任何一段都会导致后续数据整体错位。
    std::wstring servedName;
    if (header.nameLength > 0) {
        if (header.nameLength > tcp::kMaxNameLength) {
            result.error = "服务端返回的文件名长度异常";
            result.retryable = false;
            return false;
        }
        std::string name(header.nameLength, '\0');
        if (!tcp::ReceiveAll(socket.get(), name.data(), name.size())) {
            result.error = "读取文件名失败（连接中断）";
            result.retryable = true;
            return false;
        }
        servedName = Utf8ToWide(name);
    }

    std::string message;
    if (header.messageLength > 0) {
        if (header.messageLength > tcp::kMaxMessageLength) {
            result.error = "服务端返回的消息长度异常";
            result.retryable = false;
            return false;
        }
        message.resize(header.messageLength);
        if (!tcp::ReceiveAll(socket.get(), message.data(), message.size())) {
            result.error = "读取服务端消息失败";
            result.retryable = true;
            return false;
        }
    }

    const auto status = static_cast<tcp::Status>(header.status);
    if (status != tcp::Status::Ok) {
        const std::wstring detail = message.empty() ? std::wstring() : Utf8ToWide(message);
        result.error = WideToUtf8(Format(L"服务端拒绝了请求：%s%s%s", tcp::StatusText(status),
                                         detail.empty() ? L"" : L" — ",
                                         detail.c_str()));
        // 文件不存在或请求非法属于确定性失败，重试没有意义。
        result.retryable = status == tcp::Status::ServerError;
        return false;
    }

    const std::wstring localName = SanitizeLocalFileName(servedName, options.preferredFileName);
    if (options.destinationDirectory.empty()) {
        result.error = "未指定本地保存目录";
        result.retryable = false;
        return false;
    }
    if (!EnsureDirectoryExists(options.destinationDirectory)) {
        result.error =
            WideToUtf8(Format(L"无法创建保存目录：%s", options.destinationDirectory.c_str()));
        result.retryable = false;
        return false;
    }

    const std::wstring targetPath = JoinPath(options.destinationDirectory, localName);
    FileHandle file(NormalizeFileHandle(
        CreateFileW(targetPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr)));
    if (!file) {
        result.error = WideToUtf8(Format(L"无法创建目标文件 %s：%s", targetPath.c_str(),
                                          FormatSystemError(GetLastError()).c_str()));
        result.retryable = false;
        return false;
    }

    const std::uint64_t total = header.payloadSize;
    std::vector<char> buffer(tcp::kChunkSize);
    std::uint64_t received = 0;
    std::uint32_t crc = 0;

    while (received < total) {
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(tcp::kChunkSize, total - received));
        const int got = recv(socket.get(), buffer.data(), static_cast<int>(want), 0);
        if (got <= 0) {
            result.error = WideToUtf8(
                Format(L"传输中断：已接收 %s / %s", FormatBytes(received).c_str(),
                       FormatBytes(total).c_str()));
            result.retryable = true;
            return false;
        }

        DWORD written = 0;
        if (!WriteFile(file.get(), buffer.data(), static_cast<DWORD>(got), &written, nullptr) ||
            written != static_cast<DWORD>(got)) {
            result.error = WideToUtf8(
                Format(L"写入文件失败：%s", FormatSystemError(GetLastError()).c_str()));
            result.retryable = false;
            return false;
        }

        crc = Crc32(crc, reinterpret_cast<const std::uint8_t*>(buffer.data()),
                    static_cast<std::size_t>(got));
        received += static_cast<std::uint64_t>(got);
        result.bytesWritten = received;

        if (options.onProgress && !options.onProgress(received, total)) {
            result.cancelled = true;
            result.error = "传输已取消";
            return false;
        }
    }

    std::uint32_t expectedCrc = 0;
    if (!tcp::ReceiveAll(socket.get(), &expectedCrc, sizeof(expectedCrc))) {
        result.error = "读取校验值失败（传输未完整结束）";
        result.retryable = true;
        return false;
    }
    if (expectedCrc != crc) {
        result.error = WideToUtf8(Format(L"CRC32 校验失败：期望 %08X，实际 %08X",
                                          static_cast<unsigned>(expectedCrc),
                                          static_cast<unsigned>(crc)));
        result.retryable = true;
        return false;
    }

    if (!servedName.empty()) {
        LogDebug(Format(L"服务端确认的文件名：%s", servedName.c_str()));
    }

    result.crc32 = crc;
    result.servedName = localName;
    result.savedPath = targetPath;
    result.ok = true;
    return true;
}

}  // namespace

bool ParseTcpUrl(const std::wstring& url, TcpDownloadOptions& options, std::string& error) {
    const std::wstring prefix = L"tcp://";
    if (url.size() <= prefix.size() || ToLower(url.substr(0, prefix.size())) != prefix) {
        error = "不是合法的 tcp:// 地址";
        return false;
    }

    std::wstring rest = url.substr(prefix.size());
    const std::size_t stop = rest.find_first_of(L"?#");
    if (stop != std::wstring::npos) {
        rest = rest.substr(0, stop);
    }

    std::wstring authority;
    std::wstring path;
    const std::size_t slash = rest.find(L'/');
    if (slash == std::wstring::npos) {
        authority = rest;
    } else {
        authority = rest.substr(0, slash);
        path = rest.substr(slash + 1);
    }

    if (authority.empty()) {
        error = "地址中缺少主机名";
        return false;
    }

    std::wstring portText;
    if (authority.front() == L'[') {
        // IPv6 字面量：[::1]:9000
        const std::size_t close = authority.find(L']');
        if (close == std::wstring::npos) {
            error = "IPv6 地址缺少右方括号";
            return false;
        }
        options.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == L':') {
            portText = authority.substr(close + 2);
        }
    } else {
        const std::size_t colon = authority.rfind(L':');
        if (colon == std::wstring::npos) {
            options.host = authority;
        } else {
            options.host = authority.substr(0, colon);
            portText = authority.substr(colon + 1);
        }
    }

    if (options.host.empty()) {
        error = "地址中缺少主机名";
        return false;
    }

    if (!portText.empty()) {
        wchar_t* end = nullptr;
        const long port = wcstol(portText.c_str(), &end, 10);
        if (end == portText.c_str() || (end != nullptr && *end != L'\0') || port < 1 ||
            port > 65535) {
            error = "端口无效（应在 1..65535 之间）";
            return false;
        }
        options.port = static_cast<std::uint16_t>(port);
    }

    if (!path.empty()) {
        options.remoteName = PercentDecode(path);
    }
    return true;
}

bool TcpDownload(const TcpDownloadOptions& options, TcpDownloadResult& result) {
    result = TcpDownloadResult{};

    if (options.host.empty()) {
        result.error = "未指定服务端地址";
        result.retryable = false;
        return false;
    }

    // 整个传输（含重试）共用一次 Winsock 初始化。
    tcp::WinsockScope winsock;
    if (!winsock.ready()) {
        result.error = "Winsock 初始化失败";
        result.retryable = false;
        return false;
    }

    const int attempts = std::max(1, options.maxRetries + 1);
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (attempt > 1) {
            const int delaySeconds = std::min(2 * (attempt - 1), 10);
            LogWarn(Format(L"TCP 传输失败：%s", Utf8ToWide(result.error).c_str()));
            LogWarn(Format(L"%d 秒后进行第 %d 次重试（共 %d 次机会）...", delaySeconds, attempt,
                           attempts));
            Sleep(static_cast<DWORD>(delaySeconds) * 1000);
        }

        LogInfo(Format(L"开始通过 TCP 传输：%s:%u/%s", options.host.c_str(),
                       static_cast<unsigned>(options.port),
                       options.remoteName.empty() ? L"(服务端默认文件)"
                                                  : options.remoteName.c_str()));

        TcpDownloadResult single;
        if (PerformTcpDownload(options, single)) {
            result = single;
            LogInfo(Format(L"传输完成：%s，共 %s", GetFileName(single.savedPath).c_str(),
                           FormatBytes(single.bytesWritten).c_str()));
            return true;
        }

        result = single;
        if (single.cancelled || !single.retryable) {
            return false;
        }
    }

    LogError(Format(L"TCP 传输失败：%s", Utf8ToWide(result.error).c_str()));
    return false;
}

}  // namespace updater
