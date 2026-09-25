#include "updater/file_server.h"

#include "updater/inflate.h"
#include "updater/logger.h"
#include "updater/util.h"

#include <algorithm>
#include <memory>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace updater {
namespace {

constexpr int kBacklog = 16;
constexpr DWORD kGracefulShutdownMs = 3000;

std::atomic<int> g_activeClients{0};

struct FileCloser {
    void operator()(void* handle) const {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(handle));
        }
    }
};
using FileHandle = std::unique_ptr<void, FileCloser>;

std::wstring DescribePeer(const sockaddr_storage& address) {
    wchar_t host[NI_MAXHOST] = {0};
    wchar_t service[NI_MAXSERV] = {0};
    // Windows 上 getnameinfo 只有 ANSI 版本（不像 getaddrinfo 有 UNICODE 宏映射），
    // 宽字符版本必须显式调用 GetNameInfoW。
    if (GetNameInfoW(reinterpret_cast<const sockaddr*>(&address), sizeof(address), host,
                     NI_MAXHOST, service, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        return Format(L"%s:%s", host, service);
    }
    return L"未知地址";
}

std::wstring NormalizePath(const std::wstring& path) {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()),
                                              buffer.data(), nullptr);
        if (length == 0) {
            return path;
        }
        if (length < buffer.size()) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(static_cast<std::size_t>(length) + 1);
    }
}

bool StartsWithNoCase(const std::wstring& text, const std::wstring& prefix) {
    if (prefix.size() > text.size()) {
        return false;
    }
    return EqualsNoCase(text.substr(0, prefix.size()), prefix);
}

// 把客户端请求的名称解析为根目录下的真实路径。
// 双重防护：先做组件级净化，再用规范化后的绝对路径确认没有越出根目录。
bool ResolveRequestPath(const std::wstring& normalizedRoot, const std::wstring& requested,
                        std::wstring& resolved, std::string& error) {
    if (requested.empty()) {
        error = "请求的文件名为空";
        return false;
    }

    std::wstring text = requested;
    for (wchar_t& ch : text) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }

    // 拒绝绝对路径、UNC 与盘符形式。
    if (text.front() == L'\\') {
        error = "不允许使用绝对路径";
        return false;
    }
    if (text.size() >= 2 && text[1] == L':') {
        error = "不允许使用盘符路径";
        return false;
    }

    std::vector<std::wstring> parts;
    std::size_t index = 0;
    while (index <= text.size()) {
        const std::size_t next = text.find(L'\\', index);
        const std::size_t end = next == std::wstring::npos ? text.size() : next;
        const std::wstring component = text.substr(index, end - index);

        if (component == L"..") {
            error = "不允许使用 .. 进行目录穿越";
            return false;
        }
        if (!component.empty() && component != L".") {
            if (component.find(L':') != std::wstring::npos) {
                error = "文件名中包含非法字符";
                return false;
            }
            parts.push_back(component);
        }

        if (next == std::wstring::npos) {
            break;
        }
        index = next + 1;
    }

    if (parts.empty()) {
        error = "请求的文件名为空";
        return false;
    }

    std::wstring candidate = normalizedRoot;
    for (const std::wstring& part : parts) {
        candidate = JoinPath(candidate, part);
    }

    const std::wstring normalizedCandidate = NormalizePath(candidate);
    const std::wstring prefix = normalizedRoot.back() == L'\\' ? normalizedRoot
                                                               : normalizedRoot + L"\\";
    if (!StartsWithNoCase(normalizedCandidate, prefix)) {
        error = "请求的路径越出了服务根目录";
        return false;
    }

    resolved = normalizedCandidate;
    return true;
}

bool SendErrorResponse(SOCKET socket, tcp::Status status, const std::string& message) {
    tcp::Header header{};
    std::copy(tcp::kMagic, tcp::kMagic + 4, header.magic);
    header.version = tcp::kProtocolVersion;
    header.status = static_cast<std::uint8_t>(status);
    header.payloadSize = 0;
    header.nameLength = 0;
    header.messageLength = static_cast<std::uint16_t>(
        std::min<std::size_t>(message.size(), tcp::kMaxMessageLength));

    if (!tcp::SendAll(socket, &header, sizeof(header))) {
        return false;
    }
    if (header.messageLength > 0 &&
        !tcp::SendAll(socket, message.data(), header.messageLength)) {
        return false;
    }
    return true;
}

void ServeFile(SOCKET socket, const std::wstring& normalizedRoot, const std::wstring& requested,
               const std::wstring& peer) {
    std::wstring path;
    std::string resolveError;
    if (!ResolveRequestPath(normalizedRoot, requested, path, resolveError)) {
        LogWarn(Format(L"[%s] 拒绝请求「%s」：%s", peer.c_str(), requested.c_str(),
                       Utf8ToWide(resolveError).c_str()));
        SendErrorResponse(socket, tcp::Status::AccessDenied, resolveError);
        return;
    }

    FileHandle file(NormalizeFileHandle(
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)));
    if (!file) {
        const DWORD code = GetLastError();
        const bool missing = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
        LogWarn(Format(L"[%s] 无法打开 %s：%s", peer.c_str(), path.c_str(),
                       FormatSystemError(code).c_str()));
        SendErrorResponse(socket, missing ? tcp::Status::FileNotFound : tcp::Status::AccessDenied,
                          missing ? "请求的文件不存在" : "服务端无权读取该文件");
        return;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
        LogError(Format(L"[%s] 无法获取文件大小：%s", peer.c_str(),
                        FormatSystemError(GetLastError()).c_str()));
        SendErrorResponse(socket, tcp::Status::ServerError, "无法获取文件大小");
        return;
    }

    const std::string nameUtf8 = WideToUtf8(requested);
    tcp::Header header{};
    std::copy(tcp::kMagic, tcp::kMagic + 4, header.magic);
    header.version = tcp::kProtocolVersion;
    header.status = static_cast<std::uint8_t>(tcp::Status::Ok);
    header.payloadSize = static_cast<std::uint64_t>(size.QuadPart);
    header.nameLength = static_cast<std::uint16_t>(
        std::min<std::size_t>(nameUtf8.size(), tcp::kMaxNameLength));
    header.messageLength = 0;

    if (!tcp::SendAll(socket, &header, sizeof(header)) ||
        (header.nameLength > 0 &&
         !tcp::SendAll(socket, nameUtf8.data(), header.nameLength))) {
        LogWarn(Format(L"[%s] 发送响应头失败，客户端可能已断开", peer.c_str()));
        return;
    }

    LogInfo(Format(L"[%s] 开始发送 %s（%s）", peer.c_str(), requested.c_str(),
                   FormatBytes(header.payloadSize).c_str()));

    const std::uint64_t startedAt = GetTickCount64Ms();
    std::vector<char> buffer(tcp::kChunkSize);
    std::uint64_t sent = 0;
    std::uint32_t crc = 0;
    bool failed = false;

    while (sent < header.payloadSize) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(tcp::kChunkSize, header.payloadSize - sent));
        DWORD read = 0;
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(want), &read, nullptr) ||
            read == 0) {
            LogError(Format(L"[%s] 读取文件失败（已发送 %s）", peer.c_str(),
                            FormatBytes(sent).c_str()));
            failed = true;
            break;
        }

        crc = Crc32(crc, reinterpret_cast<const std::uint8_t*>(buffer.data()),
                    static_cast<std::size_t>(read));

        if (!tcp::SendAll(socket, buffer.data(), read)) {
            LogWarn(Format(L"[%s] 发送中断（已发送 %s / %s）", peer.c_str(),
                           FormatBytes(sent).c_str(), FormatBytes(header.payloadSize).c_str()));
            failed = true;
            break;
        }
        sent += read;
    }

    if (failed) {
        return;
    }

    // 尾部追加整段内容的 CRC32，供客户端校验传输完整性。
    if (!tcp::SendAll(socket, &crc, sizeof(crc))) {
        LogWarn(Format(L"[%s] 发送校验值失败", peer.c_str()));
        return;
    }

    const double seconds = static_cast<double>(GetTickCount64Ms() - startedAt) / 1000.0;
    const double speed = seconds > 0.001 ? static_cast<double>(sent) / seconds : 0.0;
    LogInfo(Format(L"[%s] 发送完成：%s，耗时 %.1f 秒（%s/s，CRC32 %08X）", peer.c_str(),
                   FormatBytes(sent).c_str(), seconds,
                   FormatBytes(static_cast<std::uint64_t>(speed)).c_str(),
                   static_cast<unsigned>(crc)));
}

void HandleClient(SOCKET rawSocket, const FileServerOptions& options,
                  const std::wstring& normalizedRoot, const sockaddr_storage& peerAddress) {
    g_activeClients.fetch_add(1);
    const std::wstring peer = DescribePeer(peerAddress);
    tcp::Socket socket(rawSocket);

    const DWORD ioTimeout = static_cast<DWORD>(std::max(1000, options.ioTimeoutMs));
    setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ioTimeout),
               sizeof(ioTimeout));
    setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ioTimeout),
               sizeof(ioTimeout));

    LogInfo(Format(L"[%s] 客户端已连接", peer.c_str()));

    tcp::Request request{};
    if (!tcp::ReceiveAll(socket.get(), &request, sizeof(request))) {
        LogWarn(Format(L"[%s] 读取请求失败，连接关闭", peer.c_str()));
        g_activeClients.fetch_sub(1);
        return;
    }

    if (!tcp::HasMagic(request.magic)) {
        LogWarn(Format(L"[%s] 协议标识不匹配，拒绝该连接", peer.c_str()));
        SendErrorResponse(socket.get(), tcp::Status::BadRequest, "协议标识不匹配");
        g_activeClients.fetch_sub(1);
        return;
    }
    if (request.version != tcp::kProtocolVersion) {
        LogWarn(Format(L"[%s] 协议版本不支持：%u", peer.c_str(),
                       static_cast<unsigned>(request.version)));
        SendErrorResponse(socket.get(), tcp::Status::BadRequest, "协议版本不支持");
        g_activeClients.fetch_sub(1);
        return;
    }
    if (request.nameLength > tcp::kMaxNameLength) {
        LogWarn(Format(L"[%s] 文件名长度异常：%u", peer.c_str(),
                       static_cast<unsigned>(request.nameLength)));
        SendErrorResponse(socket.get(), tcp::Status::BadRequest, "文件名长度异常");
        g_activeClients.fetch_sub(1);
        return;
    }

    std::string name(request.nameLength, '\0');
    if (request.nameLength > 0 &&
        !tcp::ReceiveAll(socket.get(), name.data(), name.size())) {
        LogWarn(Format(L"[%s] 读取文件名失败", peer.c_str()));
        g_activeClients.fetch_sub(1);
        return;
    }
    const std::wstring requested = Utf8ToWide(name);

    switch (static_cast<tcp::Command>(request.command)) {
        case tcp::Command::GetFile:
            if (requested.empty()) {
                SendErrorResponse(socket.get(), tcp::Status::BadRequest, "未指定文件名");
                break;
            }
            ServeFile(socket.get(), normalizedRoot, requested, peer);
            break;

        case tcp::Command::ListFiles:
            if (!options.allowListing) {
                SendErrorResponse(socket.get(), tcp::Status::AccessDenied,
                                  "服务端未开启文件列表功能");
                break;
            }
            SendErrorResponse(socket.get(), tcp::Status::BadRequest, "暂未实现列表功能");
            break;

        default:
            LogWarn(Format(L"[%s] 未知命令：%u", peer.c_str(), static_cast<unsigned>(request.command)));
            SendErrorResponse(socket.get(), tcp::Status::BadRequest, "不支持的命令");
            break;
    }

    g_activeClients.fetch_sub(1);
}

}  // namespace

bool RunFileServer(const FileServerOptions& options, const std::atomic<bool>& stopRequested,
                   std::string& error) {
    tcp::WinsockScope winsock;
    if (!winsock.ready()) {
        error = "Winsock 初始化失败";
        return false;
    }

    const std::wstring normalizedRoot = NormalizePath(options.rootDirectory);
    if (!DirectoryExists(normalizedRoot)) {
        error = WideToUtf8(Format(L"根目录不存在：%s", options.rootDirectory.c_str()));
        return false;
    }

    ADDRINFOW hints{};
    // 绑定地址含冒号则按 IPv6 处理（例如 "::" 或 "::1"）。
    hints.ai_family = options.bindAddress.find(L':') != std::wstring::npos ? AF_INET6 : AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    const std::wstring portText = std::to_wstring(options.port);

    ADDRINFOW* raw = nullptr;
    const int resolved = GetAddrInfoW(options.bindAddress.empty() ? nullptr
                                                                : options.bindAddress.c_str(),
                                      portText.c_str(), &hints, &raw);
    if (resolved != 0) {
        error = WideToUtf8(Format(L"无法解析绑定地址 %s（错误 %d）", options.bindAddress.c_str(),
                                  resolved));
        return false;
    }
    std::unique_ptr<ADDRINFOW, decltype(&FreeAddrInfoW)> addresses(raw, FreeAddrInfoW);

    tcp::Socket listener;
    for (ADDRINFOW* candidate = addresses.get(); candidate != nullptr;
         candidate = candidate->ai_next) {
        tcp::Socket socket(socket(candidate->ai_family, candidate->ai_socktype,
                                  candidate->ai_protocol));
        if (!socket.valid()) {
            continue;
        }

        const int reuse = 1;
        setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                   sizeof(reuse));

        if (bind(socket.get(), candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) != 0) {
            continue;
        }
        if (listen(socket.get(), kBacklog) != 0) {
            continue;
        }
        listener = std::move(socket);
        break;
    }

    if (!listener.valid()) {
        error = WideToUtf8(Format(L"无法在 %s:%u 上监听：%s", options.bindAddress.c_str(),
                                  static_cast<unsigned>(options.port),
                                  FormatSystemError(WSAGetLastError()).c_str()));
        return false;
    }

    LogInfo(Format(L"服务已启动：%s:%u，根目录 %s", options.bindAddress.c_str(),
                   static_cast<unsigned>(options.port), normalizedRoot.c_str()));
    LogInfo(L"等待客户端连接...（Ctrl+C 停止）");

    while (!stopRequested.load()) {
        // 用带超时的 select 等待连接，这样可以周期性检查停止标志。
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listener.get(), &readSet);
        timeval timeout{1, 0};

        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
        if (ready == 0) {
            continue;
        }
        if (ready == SOCKET_ERROR) {
            error = WideToUtf8(
                Format(L"select 失败：%s", FormatSystemError(WSAGetLastError()).c_str()));
            return false;
        }

        sockaddr_storage peerAddress{};
        int peerLength = sizeof(peerAddress);
        SOCKET accepted = accept(listener.get(), reinterpret_cast<sockaddr*>(&peerAddress),
                                 &peerLength);
        if (accepted == INVALID_SOCKET) {
            const int code = WSAGetLastError();
            if (code == WSAEINTR || code == WSAEWOULDBLOCK) {
                continue;
            }
            LogWarn(Format(L"接受连接失败：%s", FormatSystemError(code).c_str()));
            continue;
        }

        if (options.singleShot) {
            HandleClient(accepted, options, normalizedRoot, peerAddress);
            LogInfo(L"单次模式：已完成一次传输，服务退出");
            break;
        }

        // 参数按值传入：工作线程是 detach 的，不能引用本函数的局部变量。
        std::thread worker(HandleClient, accepted, options, normalizedRoot, peerAddress);
        worker.detach();
    }

    listener.Close();

    // 给正在传输的连接一点收尾时间，避免中途截断。
    const std::uint64_t deadline = GetTickCount64Ms() + kGracefulShutdownMs;
    while (g_activeClients.load() > 0 && GetTickCount64Ms() < deadline) {
        Sleep(50);
    }

    if (g_activeClients.load() > 0) {
        LogWarn(Format(L"仍有 %d 个连接在传输，进程将直接退出", g_activeClients.load()));
    } else {
        LogInfo(L"服务已停止");
    }
    return true;
}

}  // namespace updater
