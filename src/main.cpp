// ---------------------------------------------------------------------------
//  ShittimLogon Updater
//
//  流程：下载压缩包 → （可选）校验 SHA-256 → 解压到临时目录
//        → 定位 install.exe → 以解压目录为工作目录启动它 → 清理临时文件。
// ---------------------------------------------------------------------------
#include "updater/http_client.h"
#include "updater/logger.h"
#include "updater/process_launcher.h"
#include "updater/sha256.h"
#include "updater/tcp_client.h"
#include "updater/util.h"
#include "updater/zip_extractor.h"

#include <objbase.h>

#include <atomic>
#include <cstdio>

namespace {

using namespace updater;

std::atomic<bool> g_cancelled{false};

BOOL WINAPI ConsoleControlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_cancelled.store(true);
            return TRUE;
        default:
            return FALSE;
    }
}

// ---------------------------------------------------------------------------
//  命令行选项
// ---------------------------------------------------------------------------
struct Options {
    std::wstring url = kDefaultUrl;
    std::wstring workDirectory;
    std::wstring extractDirectory;
    std::wstring payloadName = kDefaultPayloadExe;
    std::wstring payloadArguments;
    std::wstring expectedSha256;
    std::wstring proxy;
    std::wstring logFile;
    int timeoutSeconds = 30;
    int retries = 3;
    bool waitForPayload = true;
    bool elevate = false;
    bool keepFiles = false;
    bool launch = true;
    bool quiet = false;
    bool verbose = false;
    bool showProgress = true;
    bool listOnly = false;
    bool insecureTls = false;
};

const wchar_t* kUsageTemplate =
    L"ShittimLogon 更新器 - 自动下载、解压并运行压缩包中的安装程序\n"
    L"\n"
    L"用法：%s [选项]\n"
    L"\n"
    L"获取压缩包\n"
    L"  --url <地址>         压缩包地址，支持 http(s):// 与 tcp://（默认 %s）\n"
    L"  --proxy <host:port>  通过指定代理下载（仅 http 模式有效）\n"
    L"  --timeout <秒>       单次网络操作超时，默认 30\n"
    L"  --retry <次数>       下载失败后的重试次数，默认 3\n"
    L"  --insecure           忽略 TLS 证书错误（不推荐）\n"
    L"  --sha256 <摘要>      校验下载包的 SHA-256，不一致则中止\n"
    L"\n"
    L"解压与运行\n"
    L"  --work-dir <目录>    工作目录，默认为临时目录下的随机子目录\n"
    L"  --extract-to <目录>  解压目标目录，默认为 <工作目录>\\payload\n"
    L"  --exe <文件名>       解压后要运行的程序，默认 %s\n"
    L"  --args <参数>        传递给目标程序的命令行参数\n"
    L"  --wait / --no-wait   是否等待目标程序结束，默认等待\n"
    L"  --elevate            以管理员权限启动目标程序（会触发 UAC）\n"
    L"  --no-launch          只下载并解压，不运行目标程序\n"
    L"  --list               仅列出压缩包内容后退出\n"
    L"  --keep               保留临时文件，便于排查问题\n"
    L"\n"
    L"输出\n"
    L"  --log-file <路径>    同时把日志写入文件\n"
    L"  --quiet              只输出警告与错误\n"
    L"  --verbose            输出调试信息\n"
    L"  --no-progress        关闭进度显示\n"
    L"  -h, --help           显示本帮助\n"
    L"\n"
    L"退出码\n"
    L"  0 成功   1 参数错误   2 下载失败   3 解压失败   4 未找到目标程序\n"
    L"  5 启动失败   6 目标程序返回非零   7 摘要不匹配   8 内部错误\n"
    L"\n"
    L"示例\n"
    L"  %s\n"
    L"  %s --elevate\n"
    L"  %s --url https://example.com/ShittimLogon.zip --exe install.exe --keep\n"
    L"  %s --url tcp://192.168.1.10:9000/ShittimLogon.zip\n";

void PrintUsage() {
    const std::wstring program = GetFileName(GetExecutablePath());
    WriteRawText(Format(kUsageTemplate, program.c_str(), kDefaultUrl, kDefaultPayloadExe,
                        program.c_str(), program.c_str(), program.c_str(), program.c_str()));
}

bool TakeValue(const std::vector<std::wstring>& arguments, std::size_t& index, bool hasInline,
               const std::wstring& inlineValue, std::wstring& value, std::wstring& error) {
    if (hasInline) {
        value = inlineValue;
        return true;
    }
    if (index + 1 >= arguments.size()) {
        error = L"选项缺少参数：" + arguments[index];
        return false;
    }
    value = arguments[++index];
    return true;
}

bool ParsePositiveInt(const std::wstring& text, int minimum, int maximum, int& value) {
    if (text.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const long parsed = wcstol(text.c_str(), &end, 10);
    if (end == text.c_str() || (end != nullptr && *end != L'\0')) {
        return false;
    }
    if (parsed < minimum || parsed > maximum) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool ParseOptions(const std::vector<std::wstring>& arguments, Options& options, bool& showHelp,
                  std::wstring& error) {
    showHelp = false;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& raw = arguments[index];
        if (raw.size() < 2 || raw[0] != L'-') {
            error = L"无法识别的参数：" + raw;
            return false;
        }

        std::wstring name = raw;
        std::wstring inlineValue;
        bool hasInline = false;
        const std::size_t equals = raw.find(L'=');
        if (equals != std::wstring::npos) {
            name = raw.substr(0, equals);
            inlineValue = raw.substr(equals + 1);
            hasInline = true;
        }
        const std::wstring key = ToLower(name);

        if (key == L"-h" || key == L"--help" || key == L"/?") {
            showHelp = true;
            return true;
        }
        if (key == L"--url") {
            if (!TakeValue(arguments, index, hasInline, inlineValue, options.url, error)) {
                return false;
            }
        } else if (key == L"--work-dir") {
            if (!TakeValue(arguments, index, hasInline, inlineValue, options.workDirectory, error)) {
                return false;
            }
        } else if (key == L"--extract-to") {
            if (!TakeValue(arguments, index, hasInline, inlineValue, options.extractDirectory,
                           error)) {
                return false;
            }
        } else if (key == L"--exe") {
            if (!TakeValue(arguments, index, hasInline, inlineValue, options.payloadName, error)) {
                return false;
            }
        } else if (key == L"--args") {
            if (!TakeValue(arguments, index, hasInline, inlineValue, options.payloadArguments,
                           error)) {
                return false;
            }
        } else if (key == L"--sha256") {
            if (!TakeValue(arguments, index, hasInline, inlineValue, options.expectedSha256,
                           error)) {
                return false;
            }
        } else if (key == L"--proxy") {
            if (!TakeValue(arguments, index, hasInline, inlineValue, options.proxy, error)) {
                return false;
            }
        } else if (key == L"--log-file") {
            if (!TakeValue(arguments, index, hasInline, inlineValue, options.logFile, error)) {
                return false;
            }
        } else if (key == L"--timeout") {
            std::wstring text;
            if (!TakeValue(arguments, index, hasInline, inlineValue, text, error)) {
                return false;
            }
            if (!ParsePositiveInt(text, 1, 3600, options.timeoutSeconds)) {
                error = L"--timeout 需要 1..3600 之间的整数";
                return false;
            }
        } else if (key == L"--retry") {
            std::wstring text;
            if (!TakeValue(arguments, index, hasInline, inlineValue, text, error)) {
                return false;
            }
            if (!ParsePositiveInt(text, 0, 20, options.retries)) {
                error = L"--retry 需要 0..20 之间的整数";
                return false;
            }
        } else if (key == L"--wait") {
            options.waitForPayload = true;
        } else if (key == L"--no-wait") {
            options.waitForPayload = false;
        } else if (key == L"--elevate") {
            options.elevate = true;
        } else if (key == L"--no-launch") {
            options.launch = false;
        } else if (key == L"--list") {
            options.listOnly = true;
        } else if (key == L"--keep") {
            options.keepFiles = true;
        } else if (key == L"--quiet") {
            options.quiet = true;
        } else if (key == L"--verbose") {
            options.verbose = true;
        } else if (key == L"--no-progress") {
            options.showProgress = false;
        } else if (key == L"--insecure") {
            options.insecureTls = true;
        } else {
            error = L"未知选项：" + raw;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
//  进度显示（限制刷新频率，避免拖慢下载）
// ---------------------------------------------------------------------------
class ProgressPrinter {
public:
    explicit ProgressPrinter(bool enabled)
        : enabled_(enabled), startedAt_(GetTickCount64Ms()), lastUpdate_(0) {}

    bool Enabled() const { return enabled_; }
    std::uint64_t ElapsedMs() const { return GetTickCount64Ms() - startedAt_; }

    void Update(const std::wstring& text, bool force = false) {
        if (!enabled_) {
            return;
        }
        const std::uint64_t now = GetTickCount64Ms();
        if (!force && now - lastUpdate_ < 100) {
            return;
        }
        lastUpdate_ = now;
        ShowProgress(text);
    }

    void Finish() {
        if (enabled_) {
            FinishProgress();
            enabled_ = false;
        }
    }

private:
    bool enabled_;
    std::uint64_t startedAt_;
    std::uint64_t lastUpdate_;
};

std::wstring FileNameFromUrl(const std::wstring& url) {
    std::wstring path = url;
    const std::size_t query = path.find_first_of(L"?#");
    if (query != std::wstring::npos) {
        path = path.substr(0, query);
    }
    std::wstring name = GetFileName(path);
    for (wchar_t& ch : name) {
        if (wcschr(L"<>:\"/\\|?*", ch) != nullptr) {
            ch = L'_';
        }
    }
    if (name.empty() || name == L"." || name == L"..") {
        name = L"package.zip";
    }
    return name;
}

// 判断地址是否走原始 TCP 传输（tcp://host[:port]/name）。
bool IsTcpUrl(const std::wstring& url) {
    const std::wstring prefix = L"tcp://";
    return url.size() > prefix.size() && ToLower(url.substr(0, prefix.size())) == prefix;
}

// 析构时删除临时工作目录（仅在满足清理条件时启用）。
struct CleanupGuard {
    std::wstring path;
    bool enabled = false;

    ~CleanupGuard() {
        if (enabled && !path.empty()) {
            const bool removed = RemoveDirectoryRecursive(path);
            if (!removed) {
                // 记录到调试日志即可，退出阶段不再打扰用户。
                LogDebug(Format(L"未能完全清理临时目录：%s", path.c_str()));
            }
        }
    }
};

// ---------------------------------------------------------------------------
//  主流程
// ---------------------------------------------------------------------------
int RunUpdater(const Options& options) {
    const std::uint64_t startedAt = GetTickCount64Ms();

    // ---- 1. 准备工作目录 -------------------------------------------------
    std::wstring workDirectory = options.workDirectory;
    bool ephemeralWorkDirectory = false;
    if (workDirectory.empty()) {
        const std::wstring base = JoinPath(GetTempDirectory(), L"ShittimLogonUpdater");
        if (!EnsureDirectoryExists(base) || !CreateUniqueDirectory(base, workDirectory)) {
            LogError(L"无法创建临时工作目录，请检查磁盘权限或使用 --work-dir 指定目录");
            return kExitInternalError;
        }
        ephemeralWorkDirectory = true;
    } else if (!EnsureDirectoryExists(workDirectory)) {
        LogError(Format(L"无法使用工作目录：%s", workDirectory.c_str()));
        return kExitInternalError;
    }

    std::wstring extractDirectory = options.extractDirectory;
    if (extractDirectory.empty()) {
        extractDirectory = JoinPath(workDirectory, L"payload");
    }

    CleanupGuard cleanup;
    const bool mayLeaveProcessRunning = options.launch && !options.waitForPayload;
    if (ephemeralWorkDirectory && !options.keepFiles && !mayLeaveProcessRunning) {
        cleanup.path = workDirectory;
        cleanup.enabled = true;
    }

    LogDebug(Format(L"工作目录：%s", workDirectory.c_str()));

    // ---- 2. 获取压缩包（HTTP 或原始 TCP）--------------------------------
    const bool tcpMode = IsTcpUrl(options.url);
    // HTTP 模式的本地文件名由 URL 决定；TCP 模式由服务端返回，
    // 因此先留空，传输成功后用实际落盘路径回填。
    std::wstring archivePath =
        tcpMode ? std::wstring() : JoinPath(workDirectory, FileNameFromUrl(options.url));
    ProgressPrinter downloadProgress(options.showProgress && !options.quiet);

    // 两种传输共用一套进度渲染，只是动词与总长度来源不同。
    const auto renderProgress = [&](std::uint64_t received, std::uint64_t total) {
        const double seconds = static_cast<double>(downloadProgress.ElapsedMs()) / 1000.0;
        const double speed = seconds > 0.05 ? static_cast<double>(received) / seconds : 0.0;

        std::wstring text;
        if (total > 0) {
            text = Format(L"[%s] %5.1f%%  %s / %s", tcpMode ? L"接收" : L"下载",
                          100.0 * static_cast<double>(received) / static_cast<double>(total),
                          FormatBytes(received).c_str(), FormatBytes(total).c_str());
            if (speed > 0.0) {
                text += Format(L"  %s/s", FormatBytes(static_cast<std::uint64_t>(speed)).c_str());
                text += L"  剩余 " +
                        FormatDuration((static_cast<double>(total) - static_cast<double>(received)) /
                                       speed);
            }
        } else {
            text = Format(L"[%s] %s", tcpMode ? L"接收" : L"下载", FormatBytes(received).c_str());
            if (speed > 0.0) {
                text += Format(L"  %s/s", FormatBytes(static_cast<std::uint64_t>(speed)).c_str());
            }
        }
        downloadProgress.Update(text);
    };

    std::uint64_t transferredBytes = 0;

    if (tcpMode) {
        TcpDownloadOptions transfer;
        std::string parseError;
        if (!ParseTcpUrl(options.url, transfer, parseError)) {
            LogError(Format(L"TCP 地址解析失败：%s", Utf8ToWide(parseError).c_str()));
            return kExitBadArguments;
        }
        // 地址不带文件名（如 tcp://host:port）时，由发送端提供它准备好的默认文件。
        transfer.destinationDirectory = workDirectory;
        transfer.preferredFileName = kDefaultPayloadArchive;
        transfer.connectTimeoutMs = options.timeoutSeconds * 1000;
        // 传输阶段允许更长的静默期：大文件在慢速链路上可能长时间持续传输。
        transfer.ioTimeoutMs = std::max(60000, options.timeoutSeconds * 1000);
        transfer.maxRetries = options.retries;
        transfer.onProgress = [&](std::uint64_t received, std::uint64_t total) -> bool {
            if (g_cancelled.load()) {
                return false;
            }
            if (downloadProgress.Enabled()) {
                renderProgress(received, total);
            }
            return true;
        };

        TcpDownloadResult transferResult;
        const bool transferred = TcpDownload(transfer, transferResult);
        downloadProgress.Finish();

        if (!transferred) {
            if (transferResult.cancelled || g_cancelled.load()) {
                LogWarn(L"传输已被取消");
                return kExitDownloadFailed;
            }
            LogError(Format(L"TCP 传输失败：%s", Utf8ToWide(transferResult.error).c_str()));
            return kExitDownloadFailed;
        }
        LogDebug(Format(L"TCP 传输的 CRC32 校验通过：%08X",
                        static_cast<unsigned>(transferResult.crc32)));
        // 以服务端返回的文件名作为后续解压的对象。
        archivePath = transferResult.savedPath;
        transferredBytes = transferResult.bytesWritten;
    } else {
        HttpDownloadOptions download;
        download.url = options.url;
        download.destinationPath = archivePath;
        download.userAgent = std::wstring(kAppName) + L"/" + kAppVersion;
        download.proxy = options.proxy;
        download.timeoutMs = options.timeoutSeconds * 1000;
        download.maxRetries = options.retries;
        download.allowInsecureTls = options.insecureTls;
        download.onProgress = [&](std::uint64_t received, std::uint64_t total,
                                 bool totalKnown) -> bool {
            if (g_cancelled.load()) {
                return false;
            }
            if (downloadProgress.Enabled()) {
                renderProgress(received, totalKnown ? total : 0);
            }
            return true;
        };

        const HttpDownloadResult downloadResult = HttpDownload(download);
        downloadProgress.Finish();

        if (!downloadResult.ok) {
            if (downloadResult.cancelled || g_cancelled.load()) {
                LogWarn(L"下载已被取消");
                return kExitDownloadFailed;
            }
            LogError(Format(L"下载失败：%s", Utf8ToWide(downloadResult.error).c_str()));
            return kExitDownloadFailed;
        }
        transferredBytes = downloadResult.bytesWritten;
    }

    if (transferredBytes == 0) {
        LogError(L"收到的内容为空，请检查地址是否正确");
        return kExitDownloadFailed;
    }

    // ---- 3. 完整性校验 --------------------------------------------------
    if (!options.expectedSha256.empty()) {
        std::string digest;
        std::string hashError;
        if (!ComputeFileSha256(archivePath, digest, hashError)) {
            LogError(Format(L"计算 SHA-256 失败：%s", Utf8ToWide(hashError).c_str()));
            return kExitInternalError;
        }
        if (!Sha256Equals(digest, WideToUtf8(options.expectedSha256))) {
            LogError(L"SHA-256 校验不通过，已中止：");
            LogError(Format(L"  期望：%s", options.expectedSha256.c_str()));
            LogError(Format(L"  实际：%s", Utf8ToWide(digest).c_str()));
            return kExitHashMismatch;
        }
        LogInfo(L"SHA-256 校验通过");
    }

    // ---- 4. 列出内容（可选） --------------------------------------------
    if (options.listOnly) {
        std::vector<ZipEntry> entries;
        std::string listError;
        if (!ListZipEntries(archivePath, entries, listError)) {
            LogError(Format(L"读取压缩包失败：%s", Utf8ToWide(listError).c_str()));
            return kExitExtractFailed;
        }
        WriteRawText(Format(L"压缩包共 %llu 个条目：\n",
                            static_cast<unsigned long long>(entries.size())));
        for (const ZipEntry& entry : entries) {
            WriteRawText(Format(L"  %s%s  (%s)\n", entry.relativePath.c_str(),
                                entry.directory ? L"\\" : L"",
                                FormatBytes(entry.uncompressedSize).c_str()));
        }
        return kExitSuccess;
    }

    // ---- 5. 解压 --------------------------------------------------------
    LogInfo(Format(L"解压到：%s", extractDirectory.c_str()));
    ProgressPrinter extractProgress(options.showProgress && !options.quiet);

    ZipExtractOptions extract;
    extract.zipPath = archivePath;
    extract.destinationRoot = extractDirectory;
    extract.overwrite = true;
    extract.onEntry = [&](const ZipEntry& entry, std::size_t index, std::size_t total) -> bool {
        if (g_cancelled.load()) {
            return false;
        }
        if (!entry.directory && extractProgress.Enabled()) {
            extractProgress.Update(Format(L"[解压] %llu/%llu  %s",
                                          static_cast<unsigned long long>(index + 1),
                                          static_cast<unsigned long long>(total),
                                          entry.relativePath.c_str()));
        }
        return true;
    };

    ZipExtractStats stats;
    std::string extractError;
    if (!ExtractZip(extract, stats, extractError)) {
        extractProgress.Finish();
        LogError(Format(L"解压失败：%s", Utf8ToWide(extractError).c_str()));
        return kExitExtractFailed;
    }
    extractProgress.Finish();
    LogInfo(Format(L"解压完成：%llu 个文件，%llu 个目录，共 %s",
                   static_cast<unsigned long long>(stats.files),
                   static_cast<unsigned long long>(stats.directories),
                   FormatBytes(stats.bytesWritten).c_str()));

    // ---- 6. 定位目标程序 ------------------------------------------------
    std::wstring payloadPath;
    if (!FindFileInDirectory(extractDirectory, options.payloadName, payloadPath)) {
        LogError(Format(L"在解压内容中未找到 %s", options.payloadName.c_str()));
        return kExitPayloadNotFound;
    }
    LogInfo(Format(L"找到目标程序：%s", payloadPath.c_str()));

    if (!options.launch) {
        LogInfo(L"已按 --no-launch 跳过运行步骤");
        return kExitSuccess;
    }

    // ---- 7. 启动目标程序 ------------------------------------------------
    LaunchOptions launch;
    launch.executable = payloadPath;
    launch.workingDirectory = GetDirectoryName(payloadPath);
    launch.arguments = options.payloadArguments;
    launch.wait = options.waitForPayload;
    launch.elevate = options.elevate;

    LogInfo(Format(L"启动 %s（工作目录：%s）%s", options.payloadName.c_str(),
                   launch.workingDirectory.c_str(),
                   options.elevate ? L"，已请求管理员权限" : L""));

    const LaunchResult launchResult = LaunchProcess(launch);
    if (!launchResult.ok) {
        LogError(Format(L"启动失败：%s", Utf8ToWide(launchResult.error).c_str()));
        return kExitLaunchFailed;
    }

    int exitCode = kExitSuccess;
    if (options.waitForPayload) {
        if (launchResult.timedOut) {
            LogWarn(L"等待目标程序超时");
        } else {
            LogInfo(Format(L"目标程序已退出，返回码：%u",
                           static_cast<unsigned>(launchResult.exitCode)));
            if (launchResult.exitCode != 0) {
                LogWarn(L"目标程序返回了非零退出码，请检查其安装日志");
                exitCode = kExitPayloadFailed;
            }
        }
    } else {
        LogInfo(L"已在后台启动目标程序，本程序即刻退出");
        if (ephemeralWorkDirectory && !options.keepFiles) {
            LogInfo(Format(L"由于目标程序可能仍在使用解压文件，临时目录予以保留：%s",
                           workDirectory.c_str()));
        }
    }

    LogInfo(Format(L"总耗时 %s",
                   FormatDuration(static_cast<double>(GetTickCount64Ms() - startedAt) / 1000.0)
                       .c_str()));
    return exitCode;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::vector<std::wstring> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    Options options;
    bool showHelp = false;
    std::wstring parseError;
    if (!ParseOptions(arguments, options, showHelp, parseError)) {
        WriteRawText(L"\n");
        LogError(parseError);
        WriteRawText(L"\n");
        PrintUsage();
        return kExitBadArguments;
    }
    if (showHelp) {
        PrintUsage();
        return kExitSuccess;
    }

    if (options.quiet) {
        SetLogLevel(LogLevel::Error);
    } else if (options.verbose) {
        SetLogLevel(LogLevel::Debug);
    }

    // 日志文件创建失败属于用户明确要求却未生效的情况，用错误级别保证不被 --quiet 吞掉。
    if (!options.logFile.empty() && !SetLogFilePath(options.logFile)) {
        LogError(Format(L"无法创建日志文件：%s", options.logFile.c_str()));
    }

    if (!options.quiet) {
        WriteRawText(Format(L"%s %s\n", kAppName, kAppVersion));
        WriteRawText(Format(L"  压缩包：%s\n", options.url.c_str()));
    }

    int exitCode = kExitInternalError;
    try {
        exitCode = RunUpdater(options);
    } catch (const std::exception& ex) {
        LogError(Format(L"发生未处理的异常：%s", Utf8ToWide(ex.what()).c_str()));
        exitCode = kExitInternalError;
    } catch (...) {
        LogError(L"发生未知异常");
        exitCode = kExitInternalError;
    }

    CoUninitialize();
    return exitCode;
}
