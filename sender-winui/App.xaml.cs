using Microsoft.UI.Xaml;

namespace ShittimLogonSender
{
    public partial class App : Application
    {
        // 必须保住强引用：一旦 Window 被 GC 回收，界面就没了。
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
