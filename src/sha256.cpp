#include "updater/sha256.h"

#include "updater/util.h"

#include <bcrypt.h>

#include <memory>

#pragma comment(lib, "bcrypt.lib")

namespace updater {
namespace {

struct AlgorithmCloser {
    void operator()(void* handle) const {
        if (handle != nullptr) {
            BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(handle), 0);
        }
    }
};

struct HashCloser {
    void operator()(void* handle) const {
        if (handle != nullptr) {
            BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(handle));
        }
    }
};

std::string ToHex(const std::vector<UCHAR>& bytes) {
    static const char* kDigits = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const UCHAR byte : bytes) {
        text.push_back(kDigits[(byte >> 4) & 0x0F]);
        text.push_back(kDigits[byte & 0x0F]);
    }
    return text;
}

}  // namespace

bool ComputeFileSha256(const std::wstring& path, std::string& hexDigest, std::string& error) {
    hexDigest.clear();

    BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        error = "无法初始化 SHA-256 算法提供程序";
        return false;
    }
    std::unique_ptr<void, AlgorithmCloser> algorithm(rawAlgorithm);

    DWORD hashSize = 0;
    DWORD propertySize = 0;
    if (BCryptGetProperty(algorithm.get(), BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &propertySize,
                          0) < 0) {
        error = "无法读取 SHA-256 摘要长度";
        return false;
    }

    DWORD objectSize = 0;
    if (BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &propertySize,
                          0) < 0) {
        error = "无法读取 SHA-256 状态缓冲区大小";
        return false;
    }

    std::vector<UCHAR> object(objectSize);
    BCRYPT_HASH_HANDLE rawHash = nullptr;
    if (BCryptCreateHash(algorithm.get(), &rawHash, object.data(), objectSize, nullptr, 0, 0) < 0) {
        error = "无法创建 SHA-256 哈希对象";
        return false;
    }
    std::unique_ptr<void, HashCloser> hash(rawHash);

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = WideToUtf8(Format(L"无法打开文件 %s：%s", path.c_str(),
                                  FormatSystemError(GetLastError()).c_str()));
        return false;
    }

    std::vector<UCHAR> buffer(1 << 20);
    bool ok = true;
    std::string failure;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            failure = WideToUtf8(
                Format(L"读取文件失败：%s", FormatSystemError(GetLastError()).c_str()));
            break;
        }
        if (read == 0) {
            break;
        }
        if (BCryptHashData(hash.get(), buffer.data(), read, 0) < 0) {
            ok = false;
            failure = "计算 SHA-256 失败";
            break;
        }
    }
    CloseHandle(file);

    if (!ok) {
        error = failure;
        return false;
    }

    std::vector<UCHAR> digest(hashSize);
    if (BCryptFinishHash(hash.get(), digest.data(), hashSize, 0) < 0) {
        error = "无法获取 SHA-256 摘要";
        return false;
    }

    hexDigest = ToHex(digest);
    return true;
}

bool Sha256Equals(const std::string& lhs, const std::string& rhs) {
    auto normalize = [](const std::string& text) {
        std::string result;
        for (const char ch : text) {
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                continue;
            }
            result.push_back(
                static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
        }
        return result;
    };

    const std::string left = normalize(lhs);
    const std::string right = normalize(rhs);
    if (left.empty() || right.empty() || left.size() != right.size()) {
        return false;
    }
    return left == right;
}

}  // namespace updater
