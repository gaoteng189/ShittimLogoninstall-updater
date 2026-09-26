// ---------------------------------------------------------------------------
//  入口
// ---------------------------------------------------------------------------
using System;
using System.Windows.Forms;

namespace ShittimLogonUpdater
{
    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }
}
