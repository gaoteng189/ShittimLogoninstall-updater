// ---------------------------------------------------------------------------
//  CRC-32（IEEE 802.3，反射，多项式 0xEDB88320）—— 与 ZIP 及 C++ 端实现一致。
//
//  必须与 src/inflate.cpp 里的 Crc32 完全一致，否则 SLU/1 传输的尾部校验会失败：
//      C++:  crc = ~seed; ... ; return ~crc;
//  这里保持同样的「取反起、取反止」语义，因此可以按块累积计算。
// ---------------------------------------------------------------------------
using System;

namespace ShittimLogonUpdater
{
    internal static class Crc32
    {
        private static readonly uint[] Table = BuildTable();

        private static uint[] BuildTable()
        {
            var table = new uint[256];
            for (uint i = 0; i < 256; i++)
            {
                uint value = i;
                for (int bit = 0; bit < 8; bit++)
                {
                    value = (value & 1u) != 0 ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
                }
                table[i] = value;
            }
            return table;
        }

        /// <summary>按块累积计算。首次调用传 seed = 0。</summary>
        public static uint Update(uint seed, byte[] buffer, int offset, int count)
        {
            uint crc = ~seed;
            for (int i = 0; i < count; i++)
            {
                crc = Table[(crc ^ buffer[offset + i]) & 0xFFu] ^ (crc >> 8);
            }
            return ~crc;
        }
    }
}
