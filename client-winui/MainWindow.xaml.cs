using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace ShittimLogonUpdater
{
    public sealed partial class MainWindow : Window
    {
        private readonly ObservableCollection<string> _log = new ObservableCollection<string>();

        // 仅在传输进行中存在，完成后置回 null
        private CancellationTokenSource? _cancellation;

        public MainWindow()
        {
            InitializeComponent();

            Title = "ShittimLogon 更新器";

            // Win11 云母背景。主题不显式设置，即 ElementTheme.Default，跟随系统明暗。
            SystemBackdrop = new MicaBackdrop { Kind = MicaKind.Base };

            // 自定义标题栏：让出系统标题栏，由 AppTitleBar 顶上。
            ExtendsContentIntoTitleBar = true;
            SetTitleBar(AppTitleBar);

            UrlBox.Text = Defaults.Url;
            LogList.ItemsSource = _log;

            AppWindow.Resize(new Windows.Graphics.SizeInt32(860, 700));
        }

        // -------------------------------------------------------------------
        //  后台线程只能通过 DispatcherQueue 碰控件
        // -------------------------------------------------------------------
        private void Post(Action action)
        {
            DispatcherQueue.TryEnqueue(() => action());
        }

        private void Log(string message)
        {
            _log.Add(string.Format("[{0:HH:mm:ss}] {1}", DateTime.Now, message));
            LogList.ScrollIntoView(_log[_log.Count - 1]);
        }

        private void UpdateProgress(long done, long total)
        {
            // total 未知时用不确定进度条
            if (total <= 0)
            {
                Progress.IsIndeterminate = true;
                StatusText.Text = done > 0 ? FormatBytes(done) : "准备中…";
                return;
            }

            Progress.IsIndeterminate = false;
            Progress.Value = done * 100.0 / total;
            StatusText.Text = string.Format("下载中 {0:0}%　{1} / {2}",
                done * 100.0 / total,
                FormatBytes(done),
                FormatBytes(total));
        }

        private static string FormatBytes(long bytes)
        {
            string[] units = { "B", "KB", "MB", "GB" };
            double value = bytes;
            int unit = 0;
            while (value >= 1024 && unit < units.Length - 1)
            {
                value /= 1024;
                unit++;
            }
            return string.Format("{0:0.#} {1}", value, units[unit]);
        }

        // -------------------------------------------------------------------
        //  开始
        // -------------------------------------------------------------------
        private async void OnStartClicked(object sender, RoutedEventArgs e)
        {
            string url = UrlBox.Text.Trim();
            if (url.Length == 0)
            {
                StatusText.Text = "请先填写拉取地址";
                return;
            }

            _cancellation = new CancellationTokenSource();
            StartButton.IsEnabled = false;
            CancelButton.IsEnabled = true;
            UrlBox.IsEnabled = false;
            _log.Clear();

            string workDirectory = Path.Combine(Path.GetTempPath(), "ShittimLogonUpdater");
            Log("工作目录：" + workDirectory);

            try
            {
                await Task.Run(() => Updater.RunAll(
                    url,
                    workDirectory,
                    message => Post(() => Log(message)),
                    (done, total) => Post(() => UpdateProgress(done, total)),
                    _cancellation.Token));

                StatusText.Text = "完成，安装程序已启动";
            }
            catch (OperationCanceledException)
            {
                Log("已取消");
                StatusText.Text = "已取消";
            }
            catch (Exception error)
            {
                Log("失败：" + error.Message);
                StatusText.Text = "失败：" + error.Message;
            }
            finally
            {
                Progress.IsIndeterminate = false;
                StartButton.IsEnabled = true;
                CancelButton.IsEnabled = false;
                UrlBox.IsEnabled = true;
                _cancellation = null;
            }
        }

        private void OnCancelClicked(object sender, RoutedEventArgs e)
        {
            var pending = _cancellation;
            if (pending != null && !pending.IsCancellationRequested)
            {
                pending.Cancel();
                Log("正在取消…");
            }
        }
    }
}
