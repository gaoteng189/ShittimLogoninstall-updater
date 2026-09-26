// ---------------------------------------------------------------------------
//  SLU/1 协议服务端
//
//  对应 C++ 版 src/file_server.cpp：监听 TCP 端口，把准备好的安装包分发给客户端。
//  协议定义见 include/updater/tcp_protocol.h，客户端实现见 net10\client\Updater.cs。
//
//  一次交互：
//    客户端 -> 服务端   Request(24B) + 文件名字节
//    服务端 -> 客户端   Header(24B)
//                       + nameLength 字节（确认的文件名）
//                       + payloadSize 字节数据
//                       + 4 字节 CRC32
//    出错时：           Header(24B) + messageLength 字节错误信息
//
//  所有多字节字段均为小端；结构都是 1 字节对齐的 24 字节。
// ---------------------------------------------------------------------------
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace ShittimLogonSender
{
    /// <summary>与 include/updater/tcp_protocol.h 的 Status 一致。</summary>
    internal enum SluStatus : byte
    {
        Ok = 0,
        FileNotFound = 1,
        AccessDenied = 2,
        BadRequest = 3,
        ServerError = 4,
    }

    internal enum ServerEventKind
    {
        Info,
        Warn,
        Error,
        StatsChanged,
        TransferStarted,
        TransferProgress,
        TransferCompleted,
    }

    /// <summary>服务端向界面汇报的事件。界面只认这个，不碰协议细节。</summary>
    internal sealed class ServerEvent
    {
        public ServerEventKind Kind;
        public string Message = string.Empty;
        public string Peer = string.Empty;
        public string FileName = string.Empty;
        public long Sent;
        public long Total;
        public double BytesPerSecond;
        public int ActiveClients;
        public int CompletedTransfers;
        public long TotalBytesSent;
    }

    internal sealed class FileServer : IDisposable
    {
        // ---- 与 include/updater/tcp_protocol.h 保持一致 ----
        private const ushort ProtocolVersion = 1;
        private const int MaxNameLength = 4096;
        private const int MaxMessageLength = 8192;
        private const int ChunkSize = 256 * 1024;
        private const int Backlog = 16;
        private const byte CommandGetFile = 1;

        private static readonly byte[] Magic = { (byte)'S', (byte)'L', (byte)'U', (byte)'1' };

        /// <summary>规范化后的绝对共享目录。客户端请求的名字是相对它的路径。</summary>
        private readonly string _rootDirectory;

        /// <summary>
        /// 客户端不指定文件名（地址只写到 host:port）时提供的默认文件，
        /// 对应 C++ 版的 FileServerOptions::defaultFileName。
        /// </summary>
        private readonly string _defaultFileName;

        private readonly int _port;
        private readonly Action<ServerEvent> _report;

        private TcpListener? _listener;
        private CancellationTokenSource? _cancellation;
        private Task? _acceptLoop;

        private int _activeClients;
        private int _completedTransfers;
        private long _totalBytesSent;

        public FileServer(string sharedDirectory, string defaultFileName, int port,
                          Action<ServerEvent> report)
        {
            _rootDirectory = Path.GetFullPath(sharedDirectory);
            _defaultFileName = defaultFileName ?? string.Empty;
            _port = port;
            _report = report;
        }

        public int Port => _port;
        public string RootDirectory => _rootDirectory;

        // -------------------------------------------------------------------
        //  启动 / 停止
        // -------------------------------------------------------------------
        public bool Start(out string error)
        {
            error = string.Empty;
            try
            {
                _listener = new TcpListener(IPAddress.Any, _port);
                _listener.Start(Backlog);
            }
            catch (Exception ex)
            {
                error = ex.Message;
                _listener = null;
                return false;
            }

            _cancellation = new CancellationTokenSource();
            _acceptLoop = Task.Run(() => AcceptLoopAsync(_cancellation.Token));
            return true;
        }

        public void Stop()
        {
            var cancellation = _cancellation;
            _cancellation = null;
            if (cancellation != null)
            {
                try { cancellation.Cancel(); } catch { /* 已释放 */ }
            }

            try { _listener?.Stop(); } catch { /* 已释放 */ }
            _listener = null;

            try { _acceptLoop?.Wait(TimeSpan.FromSeconds(3)); } catch { /* 超时或已结束 */ }
            _acceptLoop = null;
        }

        public void Dispose() => Stop();

        // -------------------------------------------------------------------
        //  接受连接
        // -------------------------------------------------------------------
        private async Task AcceptLoopAsync(CancellationToken token)
        {
            var listener = _listener;
            if (listener == null)
            {
                return;
            }

            while (!token.IsCancellationRequested)
            {
                TcpClient client;
                try
                {
                    client = await listener.AcceptTcpClientAsync(token);
                }
                catch (OperationCanceledException) { break; }
                catch (ObjectDisposedException) { break; }
                catch (SocketException)
                {
                    if (token.IsCancellationRequested) { break; }
                    continue;
                }

                // 每个连接独立处理，客户端之间互不阻塞
                _ = Task.Run(() => HandleClientAsync(client, token), CancellationToken.None);
            }
        }

        // -------------------------------------------------------------------
        //  单个客户端的完整会话
        // -------------------------------------------------------------------
        private async Task HandleClientAsync(TcpClient client, CancellationToken token)
        {
            Interlocked.Increment(ref _activeClients);
            string peer = DescribePeer(client);
            Report(ServerEventKind.Info, peer, string.Format("{0} 已连接", peer));

            try
            {
                using (client)
                using (NetworkStream stream = client.GetStream())
                {
                    client.NoDelay = true;

                    // ---- 1. 请求头（固定 24 字节）----
                    var request = new byte[24];
                    if (!await ReadExactlyAsync(stream, request, 0, request.Length, token))
                    {
                        Report(ServerEventKind.Warn, peer,
                            string.Format("{0} 读取请求失败，连接关闭", peer));
                        return;
                    }

                    if (!HasMagic(request))
                    {
                        Report(ServerEventKind.Warn, peer,
                            string.Format("{0} 协议标识不匹配，拒绝该连接", peer));
                        await SendErrorAsync(stream, SluStatus.BadRequest, "协议标识不匹配", token);
                        return;
                    }

                    ushort version = ReadUInt16(request, 4);
                    if (version != ProtocolVersion)
                    {
                        Report(ServerEventKind.Warn, peer,
                            string.Format("{0} 协议版本不支持：{1}", peer, version));
                        await SendErrorAsync(stream, SluStatus.BadRequest, "协议版本不支持", token);
                        return;
                    }

                    byte command = request[6];
                    if (command != CommandGetFile)
                    {
                        Report(ServerEventKind.Warn, peer,
                            string.Format("{0} 不支持的命令：{1}", peer, command));
                        await SendErrorAsync(stream, SluStatus.BadRequest, "不支持的命令", token);
                        return;
                    }

                    // 注意只判上界：nameLength 为 0 是合法的，
                    // 它表示“要服务端准备好的默认文件”（客户端地址只写到 host:port 时）。
                    int nameLength = ReadUInt16(request, 16);
                    if (nameLength > MaxNameLength)
                    {
                        Report(ServerEventKind.Warn, peer,
                            string.Format("{0} 文件名长度异常：{1}", peer, nameLength));
                        await SendErrorAsync(stream, SluStatus.BadRequest, "文件名长度异常", token);
                        return;
                    }

                    // ---- 2. 文件名（可能为空）----
                    var nameBytes = new byte[nameLength];
                    if (nameLength > 0 &&
                        !await ReadExactlyAsync(stream, nameBytes, 0, nameLength, token))
                    {
                        Report(ServerEventKind.Warn, peer,
                            string.Format("{0} 读取文件名失败，连接关闭", peer));
                        return;
                    }

                    string clientName = nameLength > 0
                        ? Encoding.UTF8.GetString(nameBytes)
                        : string.Empty;

                    // 未指定文件名时回退到默认文件
                    string requested = clientName.Length > 0 ? clientName : _defaultFileName;
                    if (requested.Length == 0)
                    {
                        await SendErrorAsync(stream, SluStatus.BadRequest,
                            "未指定文件名，且服务端没有配置默认文件", token);
                        return;
                    }

                    await ServeFileAsync(stream, requested, peer, token);
                }
            }
            catch (OperationCanceledException)
            {
                // 服务端被停止，属正常路径
            }
            catch (IOException ex)
            {
                Report(ServerEventKind.Warn, peer,
                    string.Format("{0} 连接中断：{1}", peer, ex.Message));
            }
            catch (Exception ex)
            {
                Report(ServerEventKind.Error, peer,
                    string.Format("{0} 处理连接时出错：{1}", peer, ex.Message));
            }
            finally
            {
                Interlocked.Decrement(ref _activeClients);
                PublishStats();
            }
        }

        // -------------------------------------------------------------------
        //  解析请求路径并发送文件
        // -------------------------------------------------------------------
        private async Task ServeFileAsync(NetworkStream stream, string requested,
                                          string peer, CancellationToken token)
        {
            string resolved;
            string resolveError;
            if (!TryResolvePath(requested, out resolved, out resolveError))
            {
                // 路径穿越一类的请求在这里被挡掉，绝不会碰到 _rootDirectory 之外的文件
                Report(ServerEventKind.Warn, peer,
                    string.Format("{0} 拒绝请求「{1}」：{2}", peer, requested, resolveError));
                await SendErrorAsync(stream, SluStatus.AccessDenied, resolveError, token);
                return;
            }

            FileStream file;
            try
            {
                file = new FileStream(resolved, FileMode.Open, FileAccess.Read, FileShare.Read,
                                      ChunkSize, FileOptions.SequentialScan | FileOptions.Asynchronous);
            }
            catch (FileNotFoundException)
            {
                await SendMissingAsync(stream, peer, resolved, token);
                return;
            }
            catch (DirectoryNotFoundException)
            {
                await SendMissingAsync(stream, peer, resolved, token);
                return;
            }
            catch (UnauthorizedAccessException)
            {
                Report(ServerEventKind.Warn, peer,
                    string.Format("{0} 无权读取 {1}", peer, resolved));
                await SendErrorAsync(stream, SluStatus.AccessDenied, "服务端无权读取该文件", token);
                return;
            }
            catch (IOException ex)
            {
                Report(ServerEventKind.Error, peer,
                    string.Format("{0} 打开文件失败：{1}", peer, ex.Message));
                await SendErrorAsync(stream, SluStatus.ServerError, "无法打开文件", token);
                return;
            }

            using (file)
            {
                long total = file.Length;

                // ---- 成功响应头 + 文件名 ----
                // 回给客户端的是「实际提供的文件名」，客户端据此落盘。
                // 请求里没带名字时，这里回的就是默认文件名。
                byte[] nameBytes = Encoding.UTF8.GetBytes(requested);
                int nameLength = Math.Min(nameBytes.Length, MaxNameLength);

                var header = new byte[24];
                Magic.CopyTo(header, 0);
                WriteUInt16(header, 4, ProtocolVersion);
                header[6] = (byte)SluStatus.Ok;
                WriteUInt64(header, 8, (ulong)total);
                WriteUInt16(header, 16, (ushort)nameLength);
                WriteUInt16(header, 18, 0);

                await stream.WriteAsync(header, 0, header.Length, token);
                await stream.WriteAsync(nameBytes, 0, nameLength, token);

                Report(ServerEventKind.TransferStarted, peer,
                    string.Format("{0} 开始发送 {1}（{2}）", peer, requested, FormatBytes(total)),
                    requested, 0, total, 0);

                // ---- 分块发送，同时累积 CRC32 ----
                var buffer = new byte[ChunkSize];
                long sent = 0;
                uint crc = 0;
                var startedAt = DateTime.UtcNow;
                var lastReportAt = startedAt;
                long lastReportSent = 0;

                while (sent < total)
                {
                    int want = (int)Math.Min(ChunkSize, total - sent);
                    int read = await file.ReadAsync(buffer.AsMemory(0, want), token);
                    if (read <= 0)
                    {
                        Report(ServerEventKind.Error, peer,
                            string.Format("{0} 读取文件失败（已发送 {1}）", peer, FormatBytes(sent)));
                        return;
                    }

                    crc = Crc32.Update(crc, buffer, 0, read);
                    await stream.WriteAsync(buffer, 0, read, token);
                    sent += read;

                    // 进度回调限流，避免刷爆界面
                    var now = DateTime.UtcNow;
                    double elapsed = (now - lastReportAt).TotalSeconds;
                    if (elapsed >= 0.2)
                    {
                        Report(ServerEventKind.TransferProgress, peer,
                            string.Format("{0} {1}　{2} / {3}　{4}", peer, requested,
                                FormatBytes(sent), FormatBytes(total),
                                FormatSpeed((sent - lastReportSent) / elapsed)),
                            requested, sent, total, (sent - lastReportSent) / elapsed);
                        lastReportAt = now;
                        lastReportSent = sent;
                    }
                }

                // ---- 尾部 CRC32，供客户端校验完整性 ----
                var crcBytes = new byte[4];
                WriteUInt32(crcBytes, 0, crc);
                await stream.WriteAsync(crcBytes, 0, crcBytes.Length, token);

                double seconds = (DateTime.UtcNow - startedAt).TotalSeconds;
                double average = seconds > 0.001 ? sent / seconds : 0;

                Interlocked.Increment(ref _completedTransfers);
                Interlocked.Add(ref _totalBytesSent, sent);

                Report(ServerEventKind.TransferCompleted, peer,
                    string.Format("{0} 发送完成：{1}，耗时 {2:0.0} 秒（{3}，CRC32 {4:X8}）",
                        peer, FormatBytes(sent), seconds, FormatSpeed(average), crc),
                    requested, sent, total, average);
                PublishStats();
            }
        }

        private async Task SendMissingAsync(NetworkStream stream, string peer, string resolved,
                                            CancellationToken token)
        {
            Report(ServerEventKind.Warn, peer,
                string.Format("{0} 文件不存在：{1}", peer, resolved));
            await SendErrorAsync(stream, SluStatus.FileNotFound, "请求的文件不存在", token);
        }

        // -------------------------------------------------------------------
        //  路径解析：双重防护
        //
        //  先按路径组件逐个净化（拒绝 ..、盘符、含冒号的 ADS 形式），
        //  再把结果规范化成绝对路径，确认它确实落在共享目录之内。
        //  这一段与 src/file_server.cpp 的 ResolveRequestPath 逐条对应。
        // -------------------------------------------------------------------
        private bool TryResolvePath(string requested, out string resolved, out string error)
        {
            resolved = string.Empty;
            error = string.Empty;

            if (string.IsNullOrEmpty(requested))
            {
                error = "请求的文件名为空";
                return false;
            }

            string text = requested.Replace('/', '\\');

            // 绝对路径与 UNC
            if (text[0] == '\\')
            {
                error = "不允许使用绝对路径";
                return false;
            }
            // 盘符形式 C:...
            if (text.Length >= 2 && text[1] == ':')
            {
                error = "不允许使用盘符路径";
                return false;
            }

            var parts = new List<string>();
            foreach (string component in text.Split('\\'))
            {
                if (component == "..")
                {
                    error = "不允许使用 .. 进行目录穿越";
                    return false;
                }

                if (component.Length > 0 && component != ".")
                {
                    // 顺带挡住 NTFS 备用数据流（file.txt:stream）
                    if (component.IndexOf(':') >= 0)
                    {
                        error = "文件名中包含非法字符";
                        return false;
                    }
                    parts.Add(component);
                }
            }

            if (parts.Count == 0)
            {
                error = "请求的文件名为空";
                return false;
            }

            string candidate = _rootDirectory;
            foreach (string part in parts)
            {
                candidate = Path.Combine(candidate, part);
            }

            string normalized = Path.GetFullPath(candidate);
            string prefix = _rootDirectory.EndsWith("\\", StringComparison.Ordinal)
                ? _rootDirectory
                : _rootDirectory + "\\";

            if (!normalized.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            {
                error = "请求的路径越出了服务根目录";
                return false;
            }

            resolved = normalized;
            return true;
        }

        // -------------------------------------------------------------------
        //  协议辅助
        // -------------------------------------------------------------------
        private static bool HasMagic(byte[] buffer)
        {
            return buffer[0] == Magic[0] && buffer[1] == Magic[1] &&
                   buffer[2] == Magic[2] && buffer[3] == Magic[3];
        }

        private async Task SendErrorAsync(NetworkStream stream, SluStatus status, string message,
                                          CancellationToken token)
        {
            var messageBytes = Encoding.UTF8.GetBytes(message);
            int length = Math.Min(messageBytes.Length, MaxMessageLength);

            var header = new byte[24];
            Magic.CopyTo(header, 0);
            WriteUInt16(header, 4, ProtocolVersion);
            header[6] = (byte)status;
            // payloadSize 与 nameLength 保持 0
            WriteUInt16(header, 18, (ushort)length);

            await stream.WriteAsync(header, 0, header.Length, token);
            if (length > 0)
            {
                await stream.WriteAsync(messageBytes, 0, length, token);
            }
        }

        private static async Task<bool> ReadExactlyAsync(NetworkStream stream, byte[] buffer,
                                                         int offset, int count,
                                                         CancellationToken token)
        {
            int done = 0;
            while (done < count)
            {
                int read = await stream.ReadAsync(buffer.AsMemory(offset + done, count - done), token);
                if (read <= 0)
                {
                    return false;
                }
                done += read;
            }
            return true;
        }

        private static string DescribePeer(TcpClient client)
        {
            try
            {
                var endpoint = client.Client.RemoteEndPoint as IPEndPoint;
                if (endpoint != null)
                {
                    return endpoint.Address.ToString() + ":" + endpoint.Port;
                }
            }
            catch
            {
                // 连接可能已经断了
            }
            return "未知地址";
        }

        // ---- 小端读写（BitConverter 虽然在小端机器上等价，这里显式写更稳）----
        private static ushort ReadUInt16(byte[] buffer, int offset)
        {
            return (ushort)(buffer[offset] | (buffer[offset + 1] << 8));
        }

        private static void WriteUInt16(byte[] buffer, int offset, ushort value)
        {
            buffer[offset] = (byte)(value & 0xFF);
            buffer[offset + 1] = (byte)((value >> 8) & 0xFF);
        }

        private static void WriteUInt32(byte[] buffer, int offset, uint value)
        {
            buffer[offset] = (byte)(value & 0xFF);
            buffer[offset + 1] = (byte)((value >> 8) & 0xFF);
            buffer[offset + 2] = (byte)((value >> 16) & 0xFF);
            buffer[offset + 3] = (byte)((value >> 24) & 0xFF);
        }

        private static void WriteUInt64(byte[] buffer, int offset, ulong value)
        {
            for (int i = 0; i < 8; i++)
            {
                buffer[offset + i] = (byte)((value >> (8 * i)) & 0xFF);
            }
        }

        // ---- 格式化 ----
        internal static string FormatBytes(long bytes)
        {
            string[] units = { "B", "KB", "MB", "GB", "TB" };
            double value = bytes;
            int unit = 0;
            while (value >= 1024 && unit < units.Length - 1)
            {
                value /= 1024;
                unit++;
            }
            return string.Format("{0:0.#} {1}", value, units[unit]);
        }

        internal static string FormatSpeed(double bytesPerSecond)
        {
            if (bytesPerSecond <= 0)
            {
                return "-";
            }
            return FormatBytes((long)bytesPerSecond) + "/s";
        }

        // -------------------------------------------------------------------
        //  事件上报
        // -------------------------------------------------------------------
        private void Report(ServerEventKind kind, string peer, string message,
                            string fileName = "", long sent = 0, long total = 0,
                            double speed = 0)
        {
            _report(new ServerEvent
            {
                Kind = kind,
                Peer = peer,
                Message = message,
                FileName = fileName,
                Sent = sent,
                Total = total,
                BytesPerSecond = speed,
                ActiveClients = Volatile.Read(ref _activeClients),
                CompletedTransfers = Volatile.Read(ref _completedTransfers),
                TotalBytesSent = Interlocked.Read(ref _totalBytesSent),
            });
        }

        private void PublishStats()
        {
            Report(ServerEventKind.StatsChanged, string.Empty, string.Empty);
        }
    }
}
