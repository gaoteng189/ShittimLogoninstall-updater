// ---------------------------------------------------------------------------
//  主窗口：WinForms（.NET Framework 4.8）
//
//  为什么不用 WinUI 3：
//     本工程的 WinUI 3（手工集成 Windows App SDK）在运行时无法建立窗口内容岛，
//     布局管线永不启动，客户区一片空白。WinForms 走 GDI/DWM，不依赖
//     WinUI 的 Composition，实测可正常渲染。
// ---------------------------------------------------------------------------
using System;
using System.Drawing;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace ShittimLogonUpdater
{
    internal sealed class MainForm : Form
    {
        private readonly TextBox urlBox;
        private readonly Button startButton;
        private readonly Button cancelButton;
        private readonly ProgressBar progressBar;
        private readonly Label statusLabel;
        private readonly TextBox logBox;

        private CancellationTokenSource cancellation;
        private bool running;

        public MainForm()
        {
            Text = "ShittimLogon 更新器";
            ClientSize = new Size(780, 540);
            MinimumSize = new Size(660, 460);
            StartPosition = FormStartPosition.CenterScreen;
            Font = new Font("Microsoft YaHei UI", 9F);
            BackColor = Color.FromArgb(250, 250, 250);

            var title = new Label
            {
                Text = "ShittimLogon 更新器",
                Font = new Font("Microsoft YaHei UI", 18F),
                AutoSize = true,
                Location = new Point(22, 18),
            };

            var subtitle = new Label
            {
                Text = "自动拉取安装包 → 解压 → 运行其中的 install.exe",
                ForeColor = Color.FromArgb(100, 100, 100),
                AutoSize = true,
                Location = new Point(25, 64),
            };

            var urlLabel = new Label
            {
                Text = "拉取地址",
                AutoSize = true,
                Location = new Point(25, 104),
            };

            urlBox = new TextBox
            {
                Text = Defaults.Url,
                Location = new Point(98, 101),
                Width = 540,
                Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right,
            };

            startButton = new Button
            {
                Text = "开始",
                Location = new Point(650, 99),
                Size = new Size(108, 30),
                Anchor = AnchorStyles.Top | AnchorStyles.Right,
            };
            startButton.Click += OnStartClicked;

            cancelButton = new Button
            {
                Text = "取消",
                Location = new Point(650, 137),
                Size = new Size(108, 30),
                Enabled = false,
                Anchor = AnchorStyles.Top | AnchorStyles.Right,
            };
            cancelButton.Click += OnCancelClicked;

            statusLabel = new Label
            {
                Text = "就绪",
                AutoSize = true,
                Location = new Point(25, 150),
            };

            progressBar = new ProgressBar
            {
                Location = new Point(25, 176),
                Size = new Size(733, 22),
                Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right,
            };

            logBox = new TextBox
            {
                Multiline = true,
                ReadOnly = true,
                ScrollBars = ScrollBars.Vertical,
                Location = new Point(25, 214),
                Size = new Size(733, 300),
                Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right,
                BackColor = Color.White,
                Font = new Font("Consolas", 9F),
            };

            Controls.Add(title);
            Controls.Add(subtitle);
            Controls.Add(urlLabel);
            Controls.Add(urlBox);
            Controls.Add(startButton);
            Controls.Add(cancelButton);
            Controls.Add(statusLabel);
            Controls.Add(progressBar);
            Controls.Add(logBox);
        }

        private void OnStartClicked(object sender, EventArgs e)
        {
            if (running)
            {
                return;
            }

            string url = urlBox.Text.Trim();
            if (url.Length == 0)
            {
                SetStatus("请先填写拉取地址");
                return;
            }

            running = true;
            startButton.Enabled = false;
            urlBox.Enabled = false;
            cancelButton.Enabled = true;
            logBox.Clear();
            progressBar.Style = ProgressBarStyle.Continuous;
            progressBar.Value = 0;

            cancellation = new CancellationTokenSource();
            CancellationToken token = cancellation.Token;

            string workDirectory = Path.Combine(Path.GetTempPath(), "ShittimLogonUpdater");

            AppendLog("工作目录：" + workDirectory);

            Task.Run(() =>
            {
                try
                {
                    Updater.RunAll(
                        url,
                        workDirectory,
                        message => Post(() => AppendLog(message)),
                        (received, total) => Post(() => UpdateProgress(received, total)),
                        token);

                    Post(() => SetStatus("完成，安装程序已启动"));
                }
                catch (OperationCanceledException)
                {
                    Post(() => SetStatus("已取消"));
                }
                catch (Exception error)
                {
                    string message = error.Message;
                    Post(() =>
                    {
                        SetStatus("失败：" + message);
                        AppendLog("错误：" + message);
                    });
                }
                finally
                {
                    Post(() =>
                    {
                        running = false;
                        startButton.Enabled = true;
                        urlBox.Enabled = true;
                        cancelButton.Enabled = false;
                        if (cancellation != null)
                        {
                            cancellation.Dispose();
                            cancellation = null;
                        }
                    });
                }
            });
        }

        private void OnCancelClicked(object sender, EventArgs e)
        {
            if (cancellation != null)
            {
                cancellation.Cancel();
                SetStatus("正在取消…");
            }
        }

        private void UpdateProgress(long received, long total)
        {
            if (total <= 0)
            {
                progressBar.Style = ProgressBarStyle.Marquee;
                SetStatus("正在连接…");
                return;
            }

            if (progressBar.Style != ProgressBarStyle.Continuous)
            {
                progressBar.Style = ProgressBarStyle.Continuous;
            }

            int percent = (int)Math.Min(100L, received * 100L / total);
            progressBar.Value = percent;
            SetStatus(string.Format("下载中 {0}%　{1} / {2}",
                                    percent, FormatBytes(received), FormatBytes(total)));
        }

        private void SetStatus(string text)
        {
            statusLabel.Text = text;
        }

        private void AppendLog(string text)
        {
            logBox.AppendText(string.Format("[{0:HH:mm:ss}] {1}{2}",
                                            DateTime.Now, text, Environment.NewLine));
        }

        /// <summary>把回调切回 UI 线程执行。</summary>
        private void Post(Action action)
        {
            if (IsDisposed || !IsHandleCreated)
            {
                return;
            }
            try
            {
                BeginInvoke(action);
            }
            catch (InvalidOperationException)
            {
                // 窗口已在关闭过程中，忽略。
            }
        }

        private static string FormatBytes(long value)
        {
            string[] units = { "B", "KB", "MB", "GB" };
            double size = value;
            int unit = 0;
            while (size >= 1024 && unit < units.Length - 1)
            {
                size /= 1024;
                unit++;
            }
            return unit == 0
                ? string.Format("{0} {1}", value, units[unit])
                : string.Format("{0:0.0} {1}", size, units[unit]);
        }
    }
}
