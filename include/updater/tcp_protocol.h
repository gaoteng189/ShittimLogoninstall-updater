// ---------------------------------------------------------------------------
//  ShittimLogon 原始 TCP 传输协议（SLU/1）
//
//  设计目标：不依赖任何 HTTP 服务，两台机器之间直接完成文件传输，
//  同时保留完整性校验、进度反馈与基本的访问安全。
//
//  字节序：全部为小端（Windows 仅运行于小端架构）。
//
//  一次交互：
//
//    客户端 ──▶ 服务端   Request(24B) + 文件名字节
//    服务端 ──▶ 客户端   Header(24B)  + [错误信息字节]
//                        + payloadSize 字节文件数据
//                        + 4 字节 CRC32（整段文件内容）
//
//  服务端在文件末尾追加 CRC32，因此无需预先扫描整个文件即可边读边算，
//  大文件也不会带来额外的一次完整读取。
// ---------------------------------------------------------------------------
#pragma once

#include "updater/common.h"

// WIN32_LEAN_AND_MEAN 已在 common.h 中定义，windows.h 不会引入旧的 winsock.h，
// 因此可以在这里安全地包含 winsock2.h。
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>

namespace updater {
namespace tcp {

constexpr char kMagic[4] = {'S', 'L', 'U', '1'};
constexpr std::uint16_t kProtocolVersion = 1;
// 发送端默认监听端口。客户端地址里通常写映射后的公网端口，
// 只有地址中省略端口时才会用到这个值。
constexpr std::uint16_t kDefaultPort = 50304;

constexpr std::size_t kMaxNameLength = 4096;
constexpr std::size_t kMaxMessageLength = 8192;
constexpr std::size_t kTrailerSize = sizeof(std::uint32_t);  // 尾部 CRC32

// 服务端响应状态
enum class Status : std::uint8_t {
    Ok = 0,
    FileNotFound = 1,
    AccessDenied = 2,
    BadRequest = 3,
    ServerError = 4,
};

// 客户端请求类型
enum class Command : std::uint8_t {
    GetFile = 1,
    ListFiles = 2,
};

#pragma pack(push, 1)

// 客户端 → 服务端
struct Request {
    char magic[4];               // "SLU1"
    std::uint16_t version;       // kProtocolVersion
    std::uint8_t command;        // Command
    std::uint8_t reserved;
    std::uint64_t offset;        // 预留：断点续传的起始偏移，当前恒为 0
    std::uint16_t nameLength;    // 文件名的 UTF-8 字节数
    std::uint16_t reserved2;
    std::uint32_t reserved3;
};

// 服务端 → 客户端
struct Header {
    char magic[4];               // "SLU1"
    std::uint16_t version;       // kProtocolVersion
    std::uint8_t status;         // Status
    std::uint8_t reserved;
    std::uint64_t payloadSize;   // 文件字节数
    std::uint16_t nameLength;    // 文件名的 UTF-8 字节数
    std::uint16_t messageLength; // 错误描述的 UTF-8 字节数
    std::uint32_t reserved2;
};

#pragma pack(pop)

static_assert(sizeof(Request) == 24, "请求结构必须正好 24 字节");
static_assert(sizeof(Header) == 24, "响应头必须正好 24 字节");

// ---------------------------------------------------------------------------
//  Winsock 生命周期管理
// ---------------------------------------------------------------------------
class WinsockScope {
public:
    WinsockScope() {
        WSADATA data{};
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~WinsockScope() {
        if (ready_) {
            WSACleanup();
        }
    }

    WinsockScope(const WinsockScope&) = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;

    bool ready() const { return ready_; }

private:
    bool ready_ = false;
};

// ---------------------------------------------------------------------------
//  套接字 RAII 包装
// ---------------------------------------------------------------------------
class Socket {
public:
    Socket() = default;
    explicit Socket(SOCKET handle) : handle_(handle) {}
    ~Socket() { Close(); }

    Socket(Socket&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_SOCKET;
    }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            Close();
            handle_ = other.handle_;
            other.handle_ = INVALID_SOCKET;
        }
        return *this;
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    SOCKET get() const { return handle_; }
    bool valid() const { return handle_ != INVALID_SOCKET; }

    void Reset(SOCKET handle = INVALID_SOCKET) {
        Close();
        handle_ = handle;
    }

    void Close() {
        if (handle_ != INVALID_SOCKET) {
            closesocket(handle_);
            handle_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET handle_ = INVALID_SOCKET;
};

// ---------------------------------------------------------------------------
//  收发辅助：TCP 是字节流，一次 recv/send 未必完成全部，必须循环处理。
// ---------------------------------------------------------------------------
constexpr std::size_t kChunkSize = 256 * 1024;

inline bool SendAll(SOCKET socket, const void* data, std::size_t size) {
    const char* cursor = static_cast<const char*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const int chunk = static_cast<int>(std::min<std::size_t>(remaining, kChunkSize));
        const int sent = send(socket, cursor, chunk, 0);
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

inline bool ReceiveAll(SOCKET socket, void* data, std::size_t size) {
    char* cursor = static_cast<char*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const int chunk = static_cast<int>(std::min<std::size_t>(remaining, kChunkSize));
        const int got = recv(socket, cursor, chunk, 0);
        if (got <= 0) {
            return false;
        }
        cursor += got;
        remaining -= static_cast<std::size_t>(got);
    }
    return true;
}

inline bool HasMagic(const char magic[4]) {
    return std::equal(kMagic, kMagic + 4, magic);
}

inline const wchar_t* StatusText(Status status) {
    switch (status) {
        case Status::Ok:
            return L"成功";
        case Status::FileNotFound:
            return L"文件不存在";
        case Status::AccessDenied:
            return L"访问被拒绝";
        case Status::BadRequest:
            return L"请求非法";
        case Status::ServerError:
            return L"服务端内部错误";
        default:
            return L"未知状态";
    }
}

}  // namespace tcp
}  // namespace updater
