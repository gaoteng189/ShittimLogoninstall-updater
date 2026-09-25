// ---------------------------------------------------------------------------
//  RFC 1951 DEFLATE 解压器（纯 C++17 实现，不依赖 zlib）。
//  支持存储块 / 固定霍夫曼 / 动态霍夫曼三种块类型。
// ---------------------------------------------------------------------------
#pragma once

#include "updater/common.h"

#include <functional>

namespace updater {

// 顺序输出解压结果的回调；返回 false 表示调用方主动中止。
using InflateSink = std::function<bool(const std::uint8_t* data, std::size_t size)>;

// 解压一段原始 DEFLATE 数据。
//   expectedSize  期望的输出大小（用于预分配，可传 0）
//   outputSize    实际输出字节数
bool Inflate(const std::uint8_t* input, std::size_t inputSize, std::size_t expectedSize,
             const InflateSink& sink, std::size_t& outputSize, std::string& error);

// CRC-32（IEEE 802.3，反射，多項式 0xEDB88320），与 ZIP 使用的一致。
std::uint32_t Crc32(std::uint32_t seed, const std::uint8_t* data, std::size_t size);

}  // namespace updater
