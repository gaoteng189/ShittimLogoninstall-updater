// ---------------------------------------------------------------------------
//  文件哈希（SHA-256），基于 Windows CNG，用于校验下载包的完整性。
// ---------------------------------------------------------------------------
#pragma once

#include "updater/common.h"

namespace updater {

// 计算文件的 SHA-256，输出 64 位小写十六进制字符串。
bool ComputeFileSha256(const std::wstring& path, std::string& hexDigest, std::string& error);

// 比较两个十六进制摘要（忽略大小写与首尾空白）。
bool Sha256Equals(const std::string& lhs, const std::string& rhs);

}  // namespace updater
