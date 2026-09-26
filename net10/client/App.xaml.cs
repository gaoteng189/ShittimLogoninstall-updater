using Microsoft.UI.Xaml;

namespace ShittimLogonUpdater
{
    public partial class App : Application
    {
        // 必须保住强引用：一旦 Window 被 GC 回收，界面就没了。
        // 可空：真正的赋值发生在 OnLaunched 里，构造函数阶段确实是 null。
        private Window? _window;

        public App()
        {
            InitializeComponent();
        }

        protected override void OnLaunched(LaunchActivatedEventArgs args)
        {
            _window = new MainWindow();
            _window.Activate();
        }
    }
}
