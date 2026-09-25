#include "updater/inflate.h"

#include <mutex>

namespace updater {
namespace {

constexpr int kMaxBits = 15;
constexpr int kMaxLiteralCodes = 288;
constexpr int kMaxDistanceCodes = 32;
constexpr int kCodeLengthCodes = 19;
constexpr int kEndOfBlock = 256;

// 动态块中码长的存储顺序（RFC 1951 3.2.7）。
const int kCodeLengthOrder[kCodeLengthCodes] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                11, 4,  12, 3, 13, 2, 14, 1, 15};

const std::uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,
                                       23, 27, 31, 35, 43, 51, 59, 67, 83,  99,  115, 131, 163,
                                       195, 227, 258};
const std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

const std::uint16_t kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,
                                         17,   25,   33,   49,   65,   97,    129,   193,
                                         257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                         4097, 6145, 8193, 12289, 16385, 24577};
const std::uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5, 6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// DEFLATE 位流读取器：位按 LSB-first 打包，霍夫曼码字按 MSB-first 出现。
class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool ReadBits(int count, std::uint32_t& value) {
        if (count < 0 || count > 24) {
            return false;
        }
        while (bits_ < count) {
            if (position_ >= size_) {
                return false;
            }
            buffer_ |= static_cast<std::uint32_t>(data_[position_++]) << bits_;
            bits_ += 8;
        }
        value = buffer_ & ((1u << count) - 1u);
        buffer_ >>= count;
        bits_ -= count;
        return true;
    }

    bool ReadBit(int& bit) {
        std::uint32_t value = 0;
        if (!ReadBits(1, value)) {
            return false;
        }
        bit = static_cast<int>(value);
        return true;
    }

    // 跳过当前字节内剩余的位，使后续读取以字节为单位（存储块使用）。
    void AlignToByte() {
        const int drop = bits_ & 7;
        buffer_ >>= drop;
        bits_ -= drop;
    }

    bool ReadAlignedByte(std::uint8_t& value) {
        if (bits_ >= 8) {
            value = static_cast<std::uint8_t>(buffer_ & 0xFFu);
            buffer_ >>= 8;
            bits_ -= 8;
            return true;
        }
        if (position_ >= size_) {
            return false;
        }
        value = data_[position_++];
        return true;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_ = 0;
    std::uint32_t buffer_ = 0;
    int bits_ = 0;
};

// 规范（canonical）霍夫曼树：用二叉树表达，解码逻辑直观且不易出错。
struct HuffmanTree {
    struct Node {
        int child[2] = {-1, -1};
        int symbol = -1;
    };

    std::vector<Node> nodes;

    void Reset() {
        nodes.clear();
        nodes.push_back(Node{});
    }

    // lengths[i] 为符号 i 的码长；全 0 表示空树（合法，例如没有距离码）。
    bool Build(const std::uint8_t* lengths, int count) {
        Reset();

        int lengthCount[kMaxBits + 1] = {0};
        for (int i = 0; i < count; ++i) {
            if (lengths[i] > kMaxBits) {
                return false;
            }
            ++lengthCount[lengths[i]];
        }
        if (lengthCount[0] == count) {
            return true;  // 空树
        }

        // 检测超订（over-subscribed）码表。
        int remaining = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            remaining <<= 1;
            remaining -= lengthCount[len];
            if (remaining < 0) {
                return false;
            }
        }

        int nextCode[kMaxBits + 2] = {0};
        int code = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code = (code + lengthCount[len - 1]) << 1;
            nextCode[len] = code;
        }

        for (int symbol = 0; symbol < count; ++symbol) {
            const int len = lengths[symbol];
            if (len == 0) {
                continue;
            }
            const int codeValue = nextCode[len]++;
            int node = 0;
            for (int bit = len - 1; bit >= 0; --bit) {
                const int branch = (codeValue >> bit) & 1;
                if (bit == 0) {
                    if (nodes[node].child[branch] != -1) {
                        return false;
                    }
                    const int leaf = static_cast<int>(nodes.size());
                    nodes.push_back(Node{});
                    nodes[leaf].symbol = symbol;
                    nodes[node].child[branch] = leaf;
                } else {
                    int next = nodes[node].child[branch];
                    if (next == -1) {
                        next = static_cast<int>(nodes.size());
                        nodes.push_back(Node{});
                        nodes[node].child[branch] = next;
                    } else if (nodes[next].symbol >= 0) {
                        return false;  // 码字互为前缀
                    }
                    node = next;
                }
            }
        }
        return true;
    }

    bool Decode(BitReader& reader, int& symbol) const {
        int node = 0;
        for (int depth = 0; depth < kMaxBits; ++depth) {
            int bit = 0;
            if (!reader.ReadBit(bit)) {
                return false;
            }
            const int next = nodes[node].child[bit];
            if (next < 0) {
                return false;
            }
            node = next;
            if (nodes[node].symbol >= 0) {
                symbol = nodes[node].symbol;
                return true;
            }
        }
        return false;
    }
};

const std::uint32_t* CrcTable() {
    static std::uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, []() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            table[i] = value;
        }
    });
    return table;
}

}  // namespace

std::uint32_t Crc32(std::uint32_t seed, const std::uint8_t* data, std::size_t size) {
    const std::uint32_t* table = CrcTable();
    std::uint32_t crc = ~seed;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

bool Inflate(const std::uint8_t* input, std::size_t inputSize, std::size_t expectedSize,
             const InflateSink& sink, std::size_t& outputSize, std::string& error) {
    outputSize = 0;
    error.clear();

    if (input == nullptr && inputSize != 0) {
        error = "输入数据为空";
        return false;
    }

    BitReader reader(input, inputSize);

    // 保留完整输出用于回溯复制（滑动窗口语义），并按块增量推送给调用方。
    std::vector<std::uint8_t> output;
    if (expectedSize > 0 && expectedSize <= (1ull << 31)) {
        output.reserve(expectedSize);
    }
    std::size_t flushed = 0;

    auto flush = [&]() -> bool {
        if (output.size() == flushed) {
            return true;
        }
        if (sink && !sink(output.data() + flushed, output.size() - flushed)) {
            error = "解压输出被调用方中止";
            return false;
        }
        flushed = output.size();
        return true;
    };

    HuffmanTree literalTree;
    HuffmanTree distanceTree;

    bool finalBlock = false;
    while (!finalBlock) {
        std::uint32_t finalFlag = 0;
        std::uint32_t blockType = 0;
        if (!reader.ReadBits(1, finalFlag) || !reader.ReadBits(2, blockType)) {
            error = "DEFLATE 数据在块头处意外结束";
            return false;
        }
        finalBlock = finalFlag != 0;

        if (blockType == 0) {
            // ---- 存储块（未压缩）----
            reader.AlignToByte();
            std::uint8_t lenLow = 0;
            std::uint8_t lenHigh = 0;
            std::uint8_t inverseLow = 0;
            std::uint8_t inverseHigh = 0;
            if (!reader.ReadAlignedByte(lenLow) || !reader.ReadAlignedByte(lenHigh) ||
                !reader.ReadAlignedByte(inverseLow) || !reader.ReadAlignedByte(inverseHigh)) {
                error = "存储块的长度字段不完整";
                return false;
            }
            const std::uint16_t length =
                static_cast<std::uint16_t>(lenLow | (static_cast<std::uint16_t>(lenHigh) << 8));
            const std::uint16_t inverse = static_cast<std::uint16_t>(
                inverseLow | (static_cast<std::uint16_t>(inverseHigh) << 8));
            if (length != static_cast<std::uint16_t>(~inverse)) {
                error = "存储块长度校验失败";
                return false;
            }
            for (std::uint32_t i = 0; i < length; ++i) {
                std::uint8_t byte = 0;
                if (!reader.ReadAlignedByte(byte)) {
                    error = "存储块数据不完整";
                    return false;
                }
                output.push_back(byte);
            }
            if (!flush()) {
                return false;
            }
            continue;
        }

        if (blockType == 3) {
            error = "遇到非法的块类型（3）";
            return false;
        }

        if (blockType == 1) {
            // ---- 固定霍夫曼表 ----
            std::uint8_t literalLengths[kMaxLiteralCodes] = {0};
            for (int i = 0; i < 144; ++i) {
                literalLengths[i] = 8;
            }
            for (int i = 144; i < 256; ++i) {
                literalLengths[i] = 9;
            }
            for (int i = 256; i < 280; ++i) {
                literalLengths[i] = 7;
            }
            for (int i = 280; i < 288; ++i) {
                literalLengths[i] = 8;
            }
            if (!literalTree.Build(literalLengths, kMaxLiteralCodes)) {
                error = "构建固定字面量码表失败";
                return false;
            }

            std::uint8_t distanceLengths[kMaxDistanceCodes] = {0};
            for (int i = 0; i < 30; ++i) {
                distanceLengths[i] = 5;
            }
            if (!distanceTree.Build(distanceLengths, kMaxDistanceCodes)) {
                error = "构建固定距离码表失败";
                return false;
            }
        } else {
            // ---- 动态霍夫曼表 ----
            std::uint32_t literalCountRaw = 0;
            std::uint32_t distanceCountRaw = 0;
            std::uint32_t codeLengthCountRaw = 0;
            if (!reader.ReadBits(5, literalCountRaw) || !reader.ReadBits(5, distanceCountRaw) ||
                !reader.ReadBits(4, codeLengthCountRaw)) {
                error = "动态块头部不完整";
                return false;
            }
            const int literalCount = static_cast<int>(literalCountRaw) + 257;
            const int distanceCount = static_cast<int>(distanceCountRaw) + 1;
            const int codeLengthCount = static_cast<int>(codeLengthCountRaw) + 4;
            if (literalCount > kMaxLiteralCodes || distanceCount > kMaxDistanceCodes) {
                error = "动态块声明的码表数量非法";
                return false;
            }

            std::uint8_t codeLengthLengths[kCodeLengthCodes] = {0};
            for (int i = 0; i < codeLengthCount; ++i) {
                std::uint32_t value = 0;
                if (!reader.ReadBits(3, value)) {
                    error = "码长码表不完整";
                    return false;
                }
                codeLengthLengths[kCodeLengthOrder[i]] = static_cast<std::uint8_t>(value);
            }

            HuffmanTree codeLengthTree;
            if (!codeLengthTree.Build(codeLengthLengths, kCodeLengthCodes)) {
                error = "码长码表非法";
                return false;
            }

            std::uint8_t lengths[kMaxLiteralCodes + kMaxDistanceCodes] = {0};
            const int total = literalCount + distanceCount;
            int index = 0;
            while (index < total) {
                int symbol = 0;
                if (!codeLengthTree.Decode(reader, symbol)) {
                    error = "解码码长失败";
                    return false;
                }
                if (symbol < 16) {
                    lengths[index++] = static_cast<std::uint8_t>(symbol);
                    continue;
                }

                int repeat = 0;
                std::uint8_t value = 0;
                std::uint32_t extra = 0;
                if (symbol == 16) {
                    if (index == 0) {
                        error = "码长重复（16）引用了无效的前一个值";
                        return false;
                    }
                    if (!reader.ReadBits(2, extra)) {
                        error = "码长重复数据不完整";
                        return false;
                    }
                    repeat = 3 + static_cast<int>(extra);
                    value = lengths[index - 1];
                } else if (symbol == 17) {
                    if (!reader.ReadBits(3, extra)) {
                        error = "码长重复数据不完整";
                        return false;
                    }
                    repeat = 3 + static_cast<int>(extra);
                } else {
                    if (!reader.ReadBits(7, extra)) {
                        error = "码长重复数据不完整";
                        return false;
                    }
                    repeat = 11 + static_cast<int>(extra);
                }
                if (index + repeat > total) {
                    error = "码长重复次数超出码表范围";
                    return false;
                }
                for (int i = 0; i < repeat; ++i) {
                    lengths[index++] = value;
                }
            }

            if (lengths[kEndOfBlock] == 0) {
                error = "字面量码表中缺少块结束符";
                return false;
            }
            if (!literalTree.Build(lengths, literalCount)) {
                error = "字面量/长度码表非法";
                return false;
            }
            if (!distanceTree.Build(lengths + literalCount, distanceCount)) {
                error = "距离码表非法";
                return false;
            }
        }

        // ---- 解码压缩数据 ----
        for (;;) {
            int symbol = 0;
            if (!literalTree.Decode(reader, symbol)) {
                error = "解码字面量失败";
                return false;
            }
            if (symbol < kEndOfBlock) {
                output.push_back(static_cast<std::uint8_t>(symbol));
                continue;
            }
            if (symbol == kEndOfBlock) {
                break;
            }
            if (symbol > 285) {
                error = "遇到非法的长度符号";
                return false;
            }

            const int lengthIndex = symbol - 257;
            std::uint32_t extra = 0;
            if (!reader.ReadBits(kLengthExtra[lengthIndex], extra)) {
                error = "长度附加位不完整";
                return false;
            }
            const std::size_t copyLength = kLengthBase[lengthIndex] + extra;

            int distanceSymbol = 0;
            if (!distanceTree.Decode(reader, distanceSymbol)) {
                error = "解码距离失败";
                return false;
            }
            if (distanceSymbol < 0 || distanceSymbol > 29) {
                error = "遇到非法的距离符号";
                return false;
            }
            if (!reader.ReadBits(kDistanceExtra[distanceSymbol], extra)) {
                error = "距离附加位不完整";
                return false;
            }
            const std::size_t distance = kDistanceBase[distanceSymbol] + extra;
            if (distance == 0 || distance > output.size()) {
                error = "距离超出已解压数据的范围";
                return false;
            }

            // 逐字节复制，天然支持 distance < copyLength 的重叠情况。
            const std::size_t source = output.size() - distance;
            for (std::size_t i = 0; i < copyLength; ++i) {
                output.push_back(output[source + i]);
            }
        }

        if (!flush()) {
            return false;
        }
    }

    outputSize = output.size();
    return true;
}

}  // namespace updater
