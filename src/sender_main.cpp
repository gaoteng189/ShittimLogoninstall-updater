// ---------------------------------------------------------------------------
//  ShittimLogon 发送端
//
//  通过原始 TCP 把指定目录下的文件提供给客户端拉取，无需部署 HTTP 服务。
//  与 ShittimLogonUpdater 配合使用：
//      ShittimLogonUpdater.exe --url tcp://<服务器地址>:9000/ShittimLogon.zip
// ---------------------------------------------------------------------------
#include "updater/file_server.h"
#include "updater/logger.h"
#include "updater/tcp_protocol.h"
#include "updater/util.h"

#include <atomic>
#include <cstdlib>
#include <exception>

namespace {

using namespace updater;

std::atomic<bool> g_stopRequested{false};

BOOL WINAPI ConsoleControlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stopRequested.store(true);
            return TRUE;
        default:
            return FALSE;
    }
}

struct Options {
    std::wstring rootDirectory;
    std::wstring fileName = kDefaultPayloadArchive;
    std::wstring bindAddress = L"0.0.0.0";
    std::wstring logFile;
    std::uint16_t port = tcp::kDefaultPort;
    int ioTimeoutSeconds = 120;
    bool singleShot = false;
    bool allowListing = false;
    bool quiet = false;
    bool verbose = false;
};

const wchar_t* kUsageTemplate =
    L"ShittimLogon 发送端 - 通过原始 TCP 提供文件下载服务\n"
    L"\n"
    L"用法：%s [选项]\n"
    L"\n"
    L"默认行为：自动把本程序所在目录下的 %s 准备好，监听 %u 端口等待客户端拉取。\n"
    L"\n"
    L"  --file <文件名>     要分发的文件，默认 %s（相对 --root）\n"
    L"  --root <目录>       文件所在目录，默认本程序所在目录\n"
    L"  --port <端口>       监听端口，默认 %u\n"
    L"  --bind <地址>       绑定地址，默认 0.0.0.0（填 :: 可监听 IPv6）\n"
    L"  --once              完成一次传输后自动退出\n"
    L"  --list              允许客户端查询文件列表（默认关闭）\n"
    L"  --io-timeout <秒>   单连接读写超时，默认 120\n"
    L"  --log-file <路径>   同时把日志写入文件\n"
    L"  --quiet             只输出警告与错误\n"
    L"  --verbose           输出调试信息\n"
    L"  -h, --help          显示本帮助\n"
    L"\n"
    L"客户端调用方式（地址不带文件名时，服务端提供上面准备好的文件）：\n"
    L"  ShittimLogonUpdater.exe --url tcp://<本机地址>:%u\n"
    L"\n"
    L"示例：\n"
    L"  %s\n"
    L"  %s --port 9100 --once\n";

void PrintUsage() {
    const std::wstring program = GetFileName(GetExecutablePath());
    WriteRawText(Format(kUsageTemplate, program.c_str(), kDefaultPayloadArchive,
                        static_cast<unsigned>(tcp::kDefaultPort), kDefaultPayloadArchive,
                        static_cast<unsigned>(tcp::kDefaultPort), program.c_str(),
                        program.c_str()));
}

bool ParsePositiveInt(const std::wstring& text, int minimum, int maximum, int& value) {
    if (text.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const long parsed = wcstol(text.c_str(), &end, 10);
    if (end == text.c_str() || (end != nullptr && *end != L'\0') || parsed < minimum ||
        parsed > maximum) {
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

        const auto takeValue = [&](std::wstring& out) -> bool {
            if (hasInline) {
                out = inlineValue;
                return true;
            }
            if (index + 1 >= arguments.size()) {
                error = L"选项缺少参数：" + raw;
                return false;
            }
            out = arguments[++index];
            return true;
        };

        const std::wstring key = ToLower(name);
        if (key == L"-h" || key == L"--help" || key == L"/?") {
            showHelp = true;
            return true;
        }
        if (key == L"--root") {
            if (!takeValue(options.rootDirectory)) {
                return false;
            }
        } else if (key == L"--file") {
            if (!takeValue(options.fileName)) {
                return false;
            }
        } else if (key == L"--bind") {
            if (!takeValue(options.bindAddress)) {
                return false;
            }
        } else if (key == L"--log-file") {
            if (!takeValue(options.logFile)) {
                return false;
            }
        } else if (key == L"--port") {
            std::wstring text;
            if (!takeValue(text)) {
                return false;
            }
            int port = 0;
            if (!ParsePositiveInt(text, 1, 65535, port)) {
                error = L"--port 需要 1..65535 之间的整数";
                return false;
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (key == L"--io-timeout") {
            std::wstring text;
            if (!takeValue(text)) {
                return false;
            }
            if (!ParsePositiveInt(text, 1, 86400, options.ioTimeoutSeconds)) {
                error = L"--io-timeout 需要 1..86400 之间的整数";
                return false;
            }
        } else if (key == L"--once") {
            options.singleShot = true;
        } else if (key == L"--list") {
            options.allowListing = true;
        } else if (key == L"--quiet") {
            options.quiet = true;
        } else if (key == L"--verbose") {
            options.verbose = true;
        } else {
            error = L"未知选项：" + raw;
            return false;
        }
    }
    return true;
}

int RunSender(Options options) {
    // 默认分发本程序所在目录：把发送端和压缩包放在一起即可直接双击运行。
    if (options.rootDirectory.empty()) {
        options.rootDirectory = GetExecutableDirectory();
        if (options.rootDirectory.empty()) {
            options.rootDirectory = L".";
        }
    }

    LogDebug(Format(L"根目录：%s", options.rootDirectory.c_str()));
    LogDebug(Format(L"待分发文件：%s", options.fileName.c_str()));
    LogDebug(Format(L"绑定：%s:%u", options.bindAddress.c_str(),
                    static_cast<unsigned>(options.port)));

    FileServerOptions serverOptions;
    serverOptions.rootDirectory = options.rootDirectory;
    serverOptions.defaultFileName = options.fileName;
    serverOptions.bindAddress = options.bindAddress;
    serverOptions.port = options.port;
    serverOptions.singleShot = options.singleShot;
    serverOptions.allowListing = options.allowListing;
    serverOptions.ioTimeoutMs = options.ioTimeoutSeconds * 1000;

    std::string error;
    if (!RunFileServer(serverOptions, g_stopRequested, error)) {
        LogError(Format(L"服务启动失败：%s", Utf8ToWide(error).c_str()));
        return 1;
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // 发送端只使用 Winsock 与文件 API，无需初始化 COM。
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);

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
        return 1;
    }
    if (showHelp) {
        PrintUsage();
        return 0;
    }

    if (options.quiet) {
        SetLogLevel(LogLevel::Error);
    } else if (options.verbose) {
        SetLogLevel(LogLevel::Debug);
    }

    if (!options.logFile.empty() && !SetLogFilePath(options.logFile)) {
        LogError(Format(L"无法创建日志文件：%s", options.logFile.c_str()));
    }

    if (!options.quiet) {
        WriteRawText(L"ShittimLogon 发送端 1.0.0\n");
    }

    int exitCode = 0;
    try {
        exitCode = RunSender(options);
    } catch (const std::exception& ex) {
        LogError(Format(L"发生未处理的异常：%s", Utf8ToWide(ex.what()).c_str()));
        exitCode = 1;
    } catch (...) {
        LogError(L"发生未知异常");
        exitCode = 1;
    }

    return exitCode;
}
