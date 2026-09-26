using System;
using System.Collections.ObjectModel;
using System.IO;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace ShittimLogonSender
{
    public sealed partial class MainWindow : Window
    {
        private const int DefaultPort = 50304;
        private const int MaxLogLines = 500;

        private readonly ObservableCollection<string> _log = new ObservableCollection<string>();
        private FileServer? _server;

        public MainWindow()
        {
            InitializeComponent();

            Title = "ShittimLogon 发送端";

            // 与客户端一致：Win11 云母背景、自定义标题栏、主题跟随系统
            SystemBackdrop = new MicaBackdrop { Kind = MicaKind.Base };
            ExtendsContentIntoTitleBar = true;
            SetTitleBar(AppTitleBar);

            LogList.ItemsSource = _log;
            PortBox.Value = DefaultPort;

            // 默认分发 exe 同目录的压缩包，与 C++ 版行为一致
            string beside = Path.Combine(AppContext.BaseDirectory, "ShittimLogon.zip");
            if (File.Exists(beside))
            {
                FileBox.Text = beside;
            }

            // 关窗时一定要把监听停掉，否则进程会挂在那儿
            Closed += (sender, args) => ShutdownServer();

            AppWindow.Resize(new Windows.Graphics.SizeInt32(900, 780));
        }

        // -------------------------------------------------------------------
        //  界面日志与统计
        // -------------------------------------------------------------------
        private void Log(string message)
        {
            _log.Add(string.Format("[{0:HH:mm:ss}] {1}", DateTime.Now, message));
            while (_log.Count > MaxLogLines)
            {
                _log.RemoveAt(0);
            }
            LogList.ScrollIntoView(_log[_log.Count - 1]);
        }

        private void UpdateStats(ServerEvent evt)
        {
            ConnectionsText.Text = evt.ActiveClients.ToString();
            TransfersText.Text = evt.CompletedTransfers.ToString();
            BytesText.Text = FileServer.FormatBytes(evt.TotalBytesSent);
        }

        // -------------------------------------------------------------------
        //  服务端事件（来自后台线程，必须切回 UI 线程）
        // -------------------------------------------------------------------
        private void OnServerEvent(ServerEvent evt)
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                switch (evt.Kind)
                {
                    case ServerEventKind.TransferProgress:
                        // 进度只更新那一行，不进日志，否则会刷屏
                        TransferText.Text = evt.Message;
                        break;

                    case ServerEventKind.TransferStarted:
                        TransferText.Text = evt.Message;
                        Log(evt.Message);
                        break;

                    case ServerEventKind.TransferCompleted:
                        TransferText.Text = string.Empty;
                        Log(evt.Message);
                        break;

                    case ServerEventKind.StatsChanged:
                        break;

                    default:
                        if (evt.Message.Length > 0)
                        {
                            Log(evt.Message);
                        }
                        break;
                }

                UpdateStats(evt);
            });
        }

        // -------------------------------------------------------------------
        //  选择文件
        // -------------------------------------------------------------------
        private async void OnBrowseClicked(object sender, RoutedEventArgs e)
        {
            var picker = new Windows.Storage.Pickers.FileOpenPicker();
            picker.FileTypeFilter.Add(".zip");
            picker.FileTypeFilter.Add("*");

            // unpackaged 应用必须把 picker 关联到窗口，否则会直接抛异常
            var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
            WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);

            var file = await picker.PickSingleFileAsync();
            if (file != null)
            {
                FileBox.Text = file.Path;
            }
        }

        // -------------------------------------------------------------------
        //  开始 / 停止监听
        // -------------------------------------------------------------------
        private void OnStartClicked(object sender, RoutedEventArgs e)
        {
            string path = FileBox.Text.Trim();
            if (path.Length == 0)
            {
                StatusText.Text = "请先选择要分发的文件";
                return;
            }
            if (!File.Exists(path))
            {
                StatusText.Text = "文件不存在：" + path;
                return;
            }

            int port = (int)PortBox.Value;
            if (port < 1 || port > 65535)
            {
                StatusText.Text = "端口必须在 1 到 65535 之间";
                return;
            }

            string directory = Path.GetDirectoryName(path) ?? AppContext.BaseDirectory;
            var server = new FileServer(directory, Path.GetFileName(path), port, OnServerEvent);

            string error;
            if (!server.Start(out error))
            {
                StatusText.Text = "启动失败：" + error;
                Log("启动失败：" + error);
                return;
            }

            _server = server;
            StartButton.IsEnabled = false;
            StopButton.IsEnabled = true;
            FileBox.IsEnabled = false;
            BrowseButton.IsEnabled = false;
            PortBox.IsEnabled = false;

            StatusText.Text = string.Format("监听中　0.0.0.0:{0}　共享目录 {1}", port, directory);
            Log(string.Format("开始监听 0.0.0.0:{0}，共享目录 {1}", port, directory));
            Log(string.Format("分发文件：{0}（{1}）", path, FileServer.FormatBytes(new FileInfo(path).Length)));
        }

        private void OnStopClicked(object sender, RoutedEventArgs e)
        {
            ShutdownServer();
            StatusText.Text = "未监听";
            Log("已停止监听");
        }

        private void ShutdownServer()
        {
            var server = _server;
            _server = null;
            if (server != null)
            {
                server.Stop();
            }

            StartButton.IsEnabled = true;
            StopButton.IsEnabled = false;
            FileBox.IsEnabled = true;
            BrowseButton.IsEnabled = true;
            PortBox.IsEnabled = true;
            TransferText.Text = string.Empty;
        }
    }
}
