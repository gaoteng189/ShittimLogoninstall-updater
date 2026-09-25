#include "updater/zip_extractor.h"

#include "updater/inflate.h"
#include "updater/logger.h"
#include "updater/util.h"

#include <algorithm>
#include <memory>

namespace updater {
namespace {

constexpr std::uint32_t kLocalHeaderSignature = 0x04034B50;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014B50;
constexpr std::uint32_t kEndOfCentralDirectorySignature = 0x06054B50;
constexpr std::uint32_t kZip64EndOfCentralDirectory = 0x06064B50;
constexpr std::uint32_t kZip64Locator = 0x07064B50;

constexpr std::uint16_t kMethodStored = 0;
constexpr std::uint16_t kMethodDeflate = 8;

// 通用标志位
constexpr std::uint16_t kFlagEncrypted = 0x0001;
constexpr std::uint16_t kFlagUtf8 = 0x0800;

constexpr std::uint64_t kMaxArchiveSize = 2ull * 1024 * 1024 * 1024;  // 2 GB 内存上限保护

struct FileCloser {
    void operator()(void* handle) const {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(handle));
        }
    }
};
using FileHandle = std::unique_ptr<void, FileCloser>;

std::uint16_t ReadU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0] | (static_cast<std::uint16_t>(data[1]) << 8));
}

std::uint32_t ReadU32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t ReadU64(const std::uint8_t* data) {
    return static_cast<std::uint64_t>(ReadU32(data)) |
           (static_cast<std::uint64_t>(ReadU32(data + 4)) << 32);
}

// 严格校验 UTF-8，用于在未设置标志位时判断文件名编码。
bool IsValidUtf8(const std::string& text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        int extra = 0;
        if (lead < 0x80) {
            ++index;
            continue;
        } else if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            if (lead < 0xC2) {
                return false;  // 过长编码
            }
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
            if (lead > 0xF4) {
                return false;
            }
        } else {
            return false;
        }
        if (index + static_cast<std::size_t>(extra) >= text.size()) {
            return false;
        }
        for (int i = 1; i <= extra; ++i) {
            if ((static_cast<unsigned char>(text[index + i]) & 0xC0) != 0x80) {
                return false;
            }
        }
        index += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

std::wstring DecodeEntryName(const std::uint8_t* data, std::size_t length, bool utf8Flag) {
    if (length == 0) {
        return std::wstring();
    }
    const std::string raw(reinterpret_cast<const char*>(data), length);
    if (utf8Flag) {
        std::wstring wide = Utf8ToWide(raw);
        if (!wide.empty() || raw.empty()) {
            return wide;
        }
        return MultiByteToWide(raw, CP_ACP);
    }
    // 未声明编码：优先按本地代码页（中文 Windows 即 GBK）解码，
    // 这与中国大陆常见压缩工具的行为一致；若字节序列本身就是合法 UTF-8
    // （部分现代工具不写标志位），则按 UTF-8 处理。
    if (IsValidUtf8(raw)) {
        std::wstring wide = Utf8ToWide(raw);
        if (!wide.empty()) {
            return wide;
        }
    }
    std::wstring wide = MultiByteToWide(raw, CP_ACP);
    if (!wide.empty() || raw.empty()) {
        return wide;
    }
    return Utf8ToWide(raw);
}

// 把压缩包内名称净化为安全的相对路径，失败表示该条目应被拒绝。
bool SanitizeEntryName(const std::wstring& rawName, std::wstring& normalized) {
    normalized.clear();

    std::wstring text = rawName;
    for (wchar_t& ch : text) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }

    // 跳过前导分隔符，避免被解释为绝对路径。
    std::size_t position = 0;
    while (position < text.size() && text[position] == L'\\') {
        ++position;
    }

    std::vector<std::wstring> parts;
    std::size_t index = position;
    for (;;) {
        const std::size_t next = text.find(L'\\', index);
        const std::size_t end = next == std::wstring::npos ? text.size() : next;
        std::wstring component = text.substr(index, end - index);

        if (component == L"..") {
            return false;  // 路径穿越
        }
        if (!component.empty() && component != L".") {
            if (component.find(L':') != std::wstring::npos) {
                return false;  // 盘符或 NTFS 备用数据流
            }
            // Windows 会静默丢弃结尾的空格与点，这里显式处理以保持前后一致。
            while (!component.empty() &&
                   (component.back() == L' ' || component.back() == L'.')) {
                component.pop_back();
            }
            if (!component.empty()) {
                parts.push_back(component);
            }
        }

        if (next == std::wstring::npos) {
            break;
        }
        index = next + 1;
    }

    if (parts.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            normalized.push_back(L'\\');
        }
        normalized.append(parts[i]);
    }
    return true;
}

std::size_t FindEndOfCentralDirectory(const std::vector<std::uint8_t>& data) {
    constexpr std::size_t kEocdSize = 22;
    constexpr std::size_t kMaxComment = 0xFFFF;

    if (data.size() < kEocdSize) {
        return std::string::npos;
    }

    const std::size_t lower = data.size() > kEocdSize + kMaxComment
                                  ? data.size() - kEocdSize - kMaxComment
                                  : 0;

    // 第一轮：要求注释长度与文件末尾严格对齐，避免误命中压缩数据中的伪签名。
    for (std::size_t position = data.size() - kEocdSize;; --position) {
        if (ReadU32(&data[position]) == kEndOfCentralDirectorySignature &&
            position + kEocdSize + ReadU16(&data[position + 20]) == data.size()) {
            return position;
        }
        if (position == lower) {
            break;
        }
    }

    // 第二轮：放松要求，取最后一次出现的签名（部分工具会写出不一致的注释长度）。
    for (std::size_t position = data.size() - kEocdSize;; --position) {
        if (ReadU32(&data[position]) == kEndOfCentralDirectorySignature) {
            return position;
        }
        if (position == lower) {
            break;
        }
    }

    return std::string::npos;
}

bool ParseCentralDirectory(const std::vector<std::uint8_t>& data, std::vector<ZipEntry>& entries,
                           std::string& error) {
    entries.clear();

    const std::size_t eocd = FindEndOfCentralDirectory(data);
    if (eocd == std::string::npos) {
        error = "未找到 ZIP 中央目录结构（文件可能不是合法 ZIP，或已损坏）";
        return false;
    }

    std::uint64_t entryCount = ReadU16(&data[eocd + 10]);
    std::uint64_t centralSize = ReadU32(&data[eocd + 12]);
    std::uint64_t centralOffset = ReadU32(&data[eocd + 16]);

    // ZIP64：当字段被置为哨兵值时，从 ZIP64 EOCD 记录读取真实值。
    if (centralOffset == 0xFFFFFFFFull || centralSize == 0xFFFFFFFFull ||
        entryCount == 0xFFFFull) {
        if (eocd >= 20) {
            const std::size_t locator = eocd - 20;
            if (ReadU32(&data[locator]) == kZip64Locator) {
                const std::uint64_t zip64Offset = ReadU64(&data[locator + 8]);
                if (zip64Offset + 56 <= data.size() &&
                    ReadU32(&data[zip64Offset]) == kZip64EndOfCentralDirectory) {
                    entryCount = ReadU64(&data[zip64Offset + 32]);
                    centralSize = ReadU64(&data[zip64Offset + 40]);
                    centralOffset = ReadU64(&data[zip64Offset + 48]);
                }
            }
        }
    }

    if (centralOffset > data.size() || centralOffset + centralSize > data.size()) {
        error = "ZIP 中央目录超出文件范围";
        return false;
    }

    std::size_t position = static_cast<std::size_t>(centralOffset);
    const std::size_t limit = static_cast<std::size_t>(centralOffset + centralSize);
    std::uint64_t parsed = 0;

    while (position + 46 <= limit) {
        if (ReadU32(&data[position]) != kCentralHeaderSignature) {
            break;
        }

        const std::uint16_t flags = ReadU16(&data[position + 8]);
        const std::uint16_t method = ReadU16(&data[position + 10]);
        const std::uint32_t crc = ReadU32(&data[position + 16]);
        std::uint64_t compressedSize = ReadU32(&data[position + 20]);
        std::uint64_t uncompressedSize = ReadU32(&data[position + 24]);
        const std::uint16_t nameLength = ReadU16(&data[position + 28]);
        const std::uint16_t extraLength = ReadU16(&data[position + 30]);
        const std::uint16_t commentLength = ReadU16(&data[position + 32]);
        const std::uint32_t externalAttributes = ReadU32(&data[position + 38]);
        std::uint64_t localOffset = ReadU32(&data[position + 42]);

        const std::size_t nameOffset = position + 46;
        const std::size_t extraOffset = nameOffset + nameLength;
        const std::size_t nextPosition =
            extraOffset + extraLength + static_cast<std::size_t>(commentLength);
        if (nextPosition > data.size() || extraOffset > data.size()) {
            error = "ZIP 中央目录条目越界";
            return false;
        }

        // 解析 ZIP64 扩展字段（id = 0x0001），按需覆盖哨兵值。
        {
            std::size_t cursor = extraOffset;
            const std::size_t extraEnd = extraOffset + extraLength;
            while (cursor + 4 <= extraEnd) {
                const std::uint16_t fieldId = ReadU16(&data[cursor]);
                const std::uint16_t fieldSize = ReadU16(&data[cursor + 2]);
                const std::size_t fieldData = cursor + 4;
                if (fieldData + fieldSize > extraEnd) {
                    break;
                }
                if (fieldId == 0x0001) {
                    std::size_t fieldCursor = fieldData;
                    if (uncompressedSize == 0xFFFFFFFFull && fieldCursor + 8 <= fieldData + fieldSize) {
                        uncompressedSize = ReadU64(&data[fieldCursor]);
                        fieldCursor += 8;
                    }
                    if (compressedSize == 0xFFFFFFFFull && fieldCursor + 8 <= fieldData + fieldSize) {
                        compressedSize = ReadU64(&data[fieldCursor]);
                        fieldCursor += 8;
                    }
                    if (localOffset == 0xFFFFFFFFull && fieldCursor + 8 <= fieldData + fieldSize) {
                        localOffset = ReadU64(&data[fieldCursor]);
                        fieldCursor += 8;
                    }
                }
                cursor = fieldData + fieldSize;
            }
        }

        ZipEntry entry;
        entry.rawName = DecodeEntryName(&data[nameOffset], nameLength, (flags & kFlagUtf8) != 0);
        entry.method = method;
        entry.crc32 = crc;
        entry.compressedSize = compressedSize;
        entry.uncompressedSize = uncompressedSize;
        entry.localHeaderOffset = localOffset;
        entry.directory = (!entry.rawName.empty() &&
                           (entry.rawName.back() == L'/' || entry.rawName.back() == L'\\')) ||
                          (externalAttributes & 0x10) != 0;

        // 净化后的路径；不可信的条目直接跳过。
        std::wstring sanitized;
        if (!SanitizeEntryName(entry.rawName, sanitized)) {
            LogWarn(Format(L"跳过不安全的压缩包条目：%s", entry.rawName.c_str()));
            position = nextPosition;
            continue;
        }
        entry.relativePath = sanitized;

        if ((flags & kFlagEncrypted) != 0) {
            error = WideToUtf8(Format(L"压缩包条目已加密，无法解压：%s", entry.rawName.c_str()));
            return false;
        }
        if (method != kMethodStored && method != kMethodDeflate) {
            error = WideToUtf8(
                Format(L"压缩方式不受支持（方法 %u）：%s", static_cast<unsigned>(method),
                       entry.rawName.c_str()));
            return false;
        }

        entries.push_back(entry);
        ++parsed;
        if (entryCount != 0 && parsed >= entryCount) {
            break;
        }
        position = nextPosition;
    }

    if (entries.empty()) {
        error = "压缩包内没有任何可解压的条目";
        return false;
    }
    return true;
}

bool LoadArchive(const std::wstring& zipPath, std::vector<std::uint8_t>& data, std::string& error) {
    if (!FileExists(zipPath)) {
        error = WideToUtf8(Format(L"压缩包不存在：%s", zipPath.c_str()));
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(zipPath.c_str(), GetFileExInfoStandard, &attributes)) {
        const std::uint64_t size =
            (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
        if (size > kMaxArchiveSize) {
            error = WideToUtf8(Format(L"压缩包过大（%s），超出内存处理上限",
                                      FormatBytes(size).c_str()));
            return false;
        }
    }

    return ReadFileToMemory(zipPath, data, error);
}

}  // namespace

bool ListZipEntries(const std::wstring& zipPath, std::vector<ZipEntry>& entries, std::string& error) {
    std::vector<std::uint8_t> data;
    if (!LoadArchive(zipPath, data, error)) {
        return false;
    }
    return ParseCentralDirectory(data, entries, error);
}

bool ExtractZip(const ZipExtractOptions& options, ZipExtractStats& stats, std::string& error) {
    stats = ZipExtractStats{};
    error.clear();

    if (options.destinationRoot.empty()) {
        error = "未指定解压目标目录";
        return false;
    }

    std::vector<std::uint8_t> data;
    if (!LoadArchive(options.zipPath, data, error)) {
        return false;
    }

    std::vector<ZipEntry> entries;
    if (!ParseCentralDirectory(data, entries, error)) {
        return false;
    }

    if (!EnsureDirectoryExists(options.destinationRoot)) {
        error = WideToUtf8(Format(L"无法创建解压目录：%s", options.destinationRoot.c_str()));
        return false;
    }

    const std::size_t total = entries.size();
    for (std::size_t index = 0; index < total; ++index) {
        const ZipEntry& entry = entries[index];

        if (options.onEntry && !options.onEntry(entry, index, total)) {
            error = "解压过程被调用方中止";
            return false;
        }

        const std::wstring targetPath = JoinPath(options.destinationRoot, entry.relativePath);

        if (entry.directory) {
            if (!EnsureDirectoryExists(targetPath)) {
                error = WideToUtf8(Format(L"无法创建目录：%s", targetPath.c_str()));
                return false;
            }
            ++stats.directories;
            continue;
        }

        const std::wstring parent = GetDirectoryName(targetPath);
        if (!parent.empty() && !EnsureDirectoryExists(parent)) {
            error = WideToUtf8(Format(L"无法创建目录：%s", parent.c_str()));
            return false;
        }

        // 定位本地文件头之后的数据区（本地头与中央目录的 extra 长度可能不同）。
        const std::size_t localOffset = static_cast<std::size_t>(entry.localHeaderOffset);
        if (localOffset + 30 > data.size() ||
            ReadU32(&data[localOffset]) != kLocalHeaderSignature) {
            error = WideToUtf8(Format(L"条目本地头无效：%s", entry.rawName.c_str()));
            return false;
        }
        const std::uint16_t localNameLength = ReadU16(&data[localOffset + 26]);
        const std::uint16_t localExtraLength = ReadU16(&data[localOffset + 28]);
        const std::uint64_t dataOffset =
            static_cast<std::uint64_t>(localOffset) + 30 + localNameLength + localExtraLength;
        if (dataOffset + entry.compressedSize > data.size()) {
            error = WideToUtf8(Format(L"条目数据超出文件范围：%s", entry.rawName.c_str()));
            return false;
        }

        FileHandle file(NormalizeFileHandle(
            CreateFileW(targetPath.c_str(), GENERIC_WRITE, 0, nullptr,
                        options.overwrite ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                        nullptr)));
        if (!file) {
            const DWORD code = GetLastError();
            if (!options.overwrite && code == ERROR_FILE_EXISTS) {
                ++stats.skipped;
                continue;
            }
            error = WideToUtf8(Format(L"无法创建文件 %s：%s", targetPath.c_str(),
                                      FormatSystemError(code).c_str()));
            return false;
        }

        const std::uint8_t* payload = data.data() + dataOffset;
        std::uint32_t crc = 0;
        std::uint64_t written = 0;
        std::string writeError;

        auto sink = [&](const std::uint8_t* chunk, std::size_t chunkSize) -> bool {
            DWORD done = 0;
            if (!WriteFile(file.get(), chunk, static_cast<DWORD>(chunkSize), &done, nullptr) ||
                done != chunkSize) {
                writeError = WideToUtf8(
                    Format(L"写入文件失败 %s：%s", targetPath.c_str(),
                           FormatSystemError(GetLastError()).c_str()));
                return false;
            }
            crc = Crc32(crc, chunk, chunkSize);
            written += chunkSize;
            return true;
        };

        if (entry.method == kMethodStored) {
            if (!sink(payload, static_cast<std::size_t>(entry.compressedSize))) {
                error = writeError;
                return false;
            }
        } else {
            std::size_t produced = 0;
            std::string inflateError;
            if (!Inflate(payload, static_cast<std::size_t>(entry.compressedSize),
                         static_cast<std::size_t>(entry.uncompressedSize), sink, produced,
                         inflateError)) {
                error = WideToUtf8(Format(L"解压失败 %s：%s", entry.rawName.c_str(),
                                          Utf8ToWide(inflateError).c_str()));
                return false;
            }
            if (produced != entry.uncompressedSize) {
                error = WideToUtf8(Format(L"解压后大小不符（%s）：期望 %llu，实际 %llu",
                                          entry.rawName.c_str(),
                                          static_cast<unsigned long long>(entry.uncompressedSize),
                                          static_cast<unsigned long long>(produced)));
                return false;
            }
        }

        if (written != entry.uncompressedSize) {
            error = WideToUtf8(Format(L"写入字节数不符（%s）：%llu / %llu", entry.rawName.c_str(),
                                      static_cast<unsigned long long>(written),
                                      static_cast<unsigned long long>(entry.uncompressedSize)));
            return false;
        }
        if (crc != entry.crc32) {
            error = WideToUtf8(Format(L"CRC32 校验失败（%s）：期望 %08X，实际 %08X",
                                      entry.rawName.c_str(),
                                      static_cast<unsigned>(entry.crc32),
                                      static_cast<unsigned>(crc)));
            return false;
        }

        stats.bytesWritten += written;
        ++stats.files;
    }

    return true;
}

}  // namespace updater
