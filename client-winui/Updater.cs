// ---------------------------------------------------------------------------
//  SLU/1 传输协议客户端 + 下载 / 解压 / 运行流程
//
//  协议定义见 include/updater/tcp_protocol.h，服务端实现见 src/file_server.cpp。
//  字节序全部为小端；结构均为 1 字节对齐（24 字节）。
//
//  一次交互：
//    客户端 -> 服务端   Request(24B) + 文件名字节
//    服务端 -> 客户端   Header(24B)
//                       + nameLength 字节（服务端确认的文件名）
//                       + payloadSize 字节文件数据
//                       + 4 字节 CRC32
//    出错时：           Header(24B) + messageLength 字节错误信息
// ---------------------------------------------------------------------------
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Sockets;
using System.Text;
using System.Threading;

// 本文件与 .NET Framework 版（client\Updater.cs）是同一份代码，刻意保持一致，
// 方便两个版本对照修改。那份代码按 C# 7 风格写（返回 null 表示"没有"、
// out 参数先赋 null 再填），在启用可空引用类型后会刷出一批 CS86xx。
// 这里关掉该文件的空值分析，而不是把两个版本改成不一样。
#nullable disable

namespace ShittimLogonUpdater
{
    internal static class Defaults
    {
        // 与 include/updater/common.h 保持一致
        public const string Url = "tcp://1344a5becd3e.ofalias.com:41792";
        public const string PayloadArchive = "ShittimLogon.zip";
        public const string PayloadExe = "install.exe";

        // 与 include/updater/tcp_protocol.h 的 kDefaultPort 一致
        public const int ProtocolDefaultPort = 50304;
    }

    internal enum SluStatus : byte
    {
        Ok = 0,
        FileNotFound = 1,
        AccessDenied = 2,
        BadRequest = 3,
        ServerError = 4,
    }

    internal sealed class Updater
    {
        private static readonly byte[] Magic = { (byte)'S', (byte)'L', (byte)'U', (byte)'1' };
        private const ushort ProtocolVersion = 1;
        private const byte CommandGetFile = 1;
        private const int ChunkSize = 256 * 1024;

        // -------------------------------------------------------------------
        //  地址解析：tcp://host:port[/文件名]
        //  省略文件名时请求发送端准备好的默认压缩包。
        // -------------------------------------------------------------------
        public static void ParseUrl(string url, out string host, out int port, out string fileName)
        {
            host = null;
            port = Defaults.ProtocolDefaultPort;
            fileName = null;

            const string scheme = "tcp://";
            if (url == null || !url.StartsWith(scheme, StringComparison.OrdinalIgnoreCase))
            {
                throw new ArgumentException("地址必须以 tcp:// 开头：" + url);
            }

            string rest = url.Substring(scheme.Length);
            int slash = rest.IndexOf('/');
            string authority = slash < 0 ? rest : rest.Substring(0, slash);

            if (slash >= 0)
            {
                string path = rest.Substring(slash + 1);
                if (path.Length > 0)
                {
                    fileName = path;
                }
            }

            int colon = authority.LastIndexOf(':');
            if (colon > 0 && colon < authority.Length - 1)
            {
                host = authority.Substring(0, colon);
                int parsed;
                if (int.TryParse(authority.Substring(colon + 1), out parsed))
                {
                    port = parsed;
                }
            }
            else
            {
                host = authority;
            }

            if (string.IsNullOrEmpty(host))
            {
                throw new ArgumentException("地址缺少主机名：" + url);
            }
            if (string.IsNullOrEmpty(fileName))
            {
                fileName = Defaults.PayloadArchive;
            }
        }

        // -------------------------------------------------------------------
        //  下载。onProgress(已接收, 总大小)；首次回调总大小为 0（未知）。
        // -------------------------------------------------------------------
        public static void Download(string host, int port, string fileName, string targetPath,
                                    Action<long, long> onProgress, CancellationToken token)
        {
            byte[] nameBytes = Encoding.UTF8.GetBytes(fileName);

            using (var client = new TcpClient())
            {
                client.Connect(host, port);
                using (NetworkStream stream = client.GetStream())
                {
                    // 1) 请求
                    var request = new byte[24];
                    Buffer.BlockCopy(Magic, 0, request, 0, 4);
                    WriteUInt16(request, 4, ProtocolVersion);
                    request[6] = CommandGetFile;
                    // request[7] = reserved
                    // request[8..15] = offset = 0（预留：断点续传）
                    WriteUInt16(request, 16, (ushort)nameBytes.Length);
                    stream.Write(request, 0, request.Length);
                    if (nameBytes.Length > 0)
                    {
                        stream.Write(nameBytes, 0, nameBytes.Length);
                    }

                    // 2) 响应头
                    byte[] header = ReadExactly(stream, 24, token);
                    if (header[0] != Magic[0] || header[1] != Magic[1] ||
                        header[2] != Magic[2] || header[3] != Magic[3])
                    {
                        throw new InvalidDataException("协议标识不匹配，可能连到了非 SLU/1 服务");
                    }

                    ushort version = ReadUInt16(header, 4);
                    if (version != ProtocolVersion)
                    {
                        throw new InvalidDataException("协议版本不支持：" + version);
                    }

                    var status = (SluStatus)header[6];
                    ulong payloadSize = ReadUInt64(header, 8);
                    ushort nameLength = ReadUInt16(header, 16);
                    ushort messageLength = ReadUInt16(header, 18);

                    if (status != SluStatus.Ok)
                    {
                        string message = messageLength > 0
                            ? Encoding.UTF8.GetString(ReadExactly(stream, messageLength, token))
                            : "服务端未提供描述";
                        throw new IOException(string.Format("服务端拒绝（{0}）：{1}", status, message));
                    }

                    // 3) 服务端确认的文件名
                    if (nameLength > 0)
                    {
                        ReadExactly(stream, nameLength, token);
                    }

                    // 4) 文件数据，同时累积 CRC32
                    uint crc = 0;
                    long received = 0;
                    var buffer = new byte[ChunkSize];
                    using (var file = new FileStream(targetPath, FileMode.Create, FileAccess.Write,
                                                     FileShare.None))
                    {
                        while (received < (long)payloadSize)
                        {
                            token.ThrowIfCancellationRequested();

                            int want = (int)Math.Min((long)buffer.Length, (long)payloadSize - received);
                            int got = stream.Read(buffer, 0, want);
                            if (got <= 0)
                            {
                                throw new IOException(string.Format(
                                    "传输中断：已接收 {0} / {1} 字节", received, (long)payloadSize));
                            }

                            file.Write(buffer, 0, got);
                            crc = Crc32.Update(crc, buffer, 0, got);
                            received += got;

                            if (onProgress != null)
                            {
                                onProgress(received, (long)payloadSize);
                            }
                        }
                    }

                    // 5) 尾部 CRC32
                    uint expected = ReadUInt32(ReadExactly(stream, 4, token), 0);
                    if (expected != crc)
                    {
                        throw new InvalidDataException(string.Format(
                            "CRC32 校验失败：期望 {0:X8}，实际 {1:X8}", expected, crc));
                    }
                }
            }
        }

        // -------------------------------------------------------------------
        //  解压
        // -------------------------------------------------------------------
        public static void Extract(string zipPath, string destination)
        {
            if (Directory.Exists(destination))
            {
                Directory.Delete(destination, true);
            }
            Directory.CreateDirectory(destination);
            ZipFile.ExtractToDirectory(zipPath, destination);
        }

        // -------------------------------------------------------------------
        //  在解压结果里定位安装程序（先找根目录，再递归）
        // -------------------------------------------------------------------
        public static string FindExecutable(string directory, string exeName)
        {
            string direct = Path.Combine(directory, exeName);
            if (File.Exists(direct))
            {
                return direct;
            }

            string[] found = Directory.GetFiles(directory, exeName, SearchOption.AllDirectories);
            return found.Length > 0 ? found[0] : null;
        }

        // -------------------------------------------------------------------
        //  启动安装程序。
        //
        //  用 UseShellExecute 让 shell 按 install.exe 自带的清单决定是否需要 UAC。
        //  注意：目标要求提权时，ShellExecute 会一直阻塞到用户对 UAC 对话框做出响应，
        //  因此这里放到独立线程去启动 —— 否则调用方（工作线程）会卡住，
        //  界面会一直停在「下载中 100%」而不是「完成」。
        // -------------------------------------------------------------------
        public static void Run(string exePath, string workingDirectory)
        {
            var startInfo = new ProcessStartInfo
            {
                FileName = exePath,
                WorkingDirectory = workingDirectory,
                UseShellExecute = true,
            };

            var starter = new Thread(() =>
            {
                try
                {
                    Process.Start(startInfo);
                }
                catch (Exception error)
                {
                    LogHook(string.Format("启动安装程序失败：{0}", error.Message));
                }
            })
            {
                IsBackground = true,
                Name = "LaunchPayload",
            };
            starter.Start();
        }

        /// <summary>启动阶段的失败信息出口（由 RunAll 注入）。</summary>
        internal static Action<string> LogHook = _ => { };

        // -------------------------------------------------------------------
        //  完整流程：连接 -> 下载 -> 解压 -> 运行
        // -------------------------------------------------------------------
        public static void RunAll(string url, string workDirectory,
                                  Action<string> onLog, Action<long, long> onProgress,
                                  CancellationToken token)
        {
            string host;
            string fileName;
            int port;
            ParseUrl(url, out host, out port, out fileName);

            onLog(string.Format("连接 {0}:{1}，请求文件 {2}", host, port, fileName));
            if (onProgress != null)
            {
                onProgress(0, 0);
            }

            Directory.CreateDirectory(workDirectory);
            string zipPath = Path.Combine(workDirectory, fileName);

            Download(host, port, fileName, zipPath, onProgress, token);
            onLog("下载完成：" + zipPath);

            string extractDir = Path.Combine(workDirectory, "payload");
            onLog("解压到 " + extractDir);
            Extract(zipPath, extractDir);

            string exePath = FindExecutable(extractDir, Defaults.PayloadExe);
            if (exePath == null)
            {
                throw new FileNotFoundException(
                    "压缩包内找不到 " + Defaults.PayloadExe, Defaults.PayloadExe);
            }

            LogHook = onLog;
            onLog("启动 " + exePath);
            Run(exePath, Path.GetDirectoryName(exePath));
            onLog("已启动安装程序（若系统弹出 UAC 提示，请确认）");
        }

        // -------------------------------------------------------------------
        //  小工具
        // -------------------------------------------------------------------
        private static byte[] ReadExactly(NetworkStream stream, int count, CancellationToken token)
        {
            var buffer = new byte[count];
            int offset = 0;
            while (offset < count)
            {
                token.ThrowIfCancellationRequested();
                int got = stream.Read(buffer, offset, count - offset);
                if (got <= 0)
                {
                    throw new IOException("连接被对端关闭（期望再读 " + (count - offset) + " 字节）");
                }
                offset += got;
            }
            return buffer;
        }

        private static void WriteUInt16(byte[] buffer, int offset, ushort value)
        {
            buffer[offset] = (byte)(value & 0xFF);
            buffer[offset + 1] = (byte)((value >> 8) & 0xFF);
        }

        private static ushort ReadUInt16(byte[] buffer, int offset)
        {
            return (ushort)(buffer[offset] | (buffer[offset + 1] << 8));
        }

        private static uint ReadUInt32(byte[] buffer, int offset)
        {
            return (uint)(buffer[offset]
                          | (buffer[offset + 1] << 8)
                          | (buffer[offset + 2] << 16)
                          | (buffer[offset + 3] << 24));
        }

        private static ulong ReadUInt64(byte[] buffer, int offset)
        {
            ulong low = ReadUInt32(buffer, offset);
            ulong high = ReadUInt32(buffer, offset + 4);
            return low | (high << 32);
        }
    }
}
