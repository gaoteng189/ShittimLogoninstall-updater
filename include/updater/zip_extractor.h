// ---------------------------------------------------------------------------
//  ZIP 解析与解包（基于内置 DEFLATE 解压器，不依赖第三方库）。
//  支持：ZIP64 扩展字段、stored/deflate 两种压缩方式、UTF-8 与本地代码页文件名。
//  安全：防止 Zip-Slip（路径穿越）、绝对路径、盘符与 NTFS 数据流。
// ---------------------------------------------------------------------------
#pragma once

#include "updater/common.h"

#include <functional>

namespace updater {

struct ZipEntry {
    std::wstring relativePath;  // 已净化的相对路径，使用 '\\' 分隔
    std::wstring rawName;       // 压缩包中的原始名称
    bool directory = false;
    std::uint16_t method = 0;
    std::uint32_t crc32 = 0;
    std::uint64_t compressedSize = 0;
    std::uint64_t uncompressedSize = 0;
    std::uint64_t localHeaderOffset = 0;
};

struct ZipExtractOptions {
    std::wstring zipPath;
    std::wstring destinationRoot;
    bool overwrite = true;
    // 每个条目解压前调用；返回 false 可中止。
    std::function<bool(const ZipEntry& entry, std::size_t index, std::size_t total)> onEntry;
};

struct ZipExtractStats {
    std::size_t files = 0;
    std::size_t directories = 0;
    std::size_t skipped = 0;
    std::uint64_t bytesWritten = 0;
};

bool ListZipEntries(const std::wstring& zipPath, std::vector<ZipEntry>& entries, std::string& error);
bool ExtractZip(const ZipExtractOptions& options, ZipExtractStats& stats, std::string& error);

}  // namespace updater
