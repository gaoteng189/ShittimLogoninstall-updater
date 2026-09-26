// ---------------------------------------------------------------------------
//  程序集信息与 Win32 版本资源
//
//  csc 会把下面这些特性写进 exe 的「属性 → 详细信息」面板。字段与 C++ 版
//  res\version.rc 保持一致，三个客户端版本的文件属性看起来是一样的。
//
//  WinUI 3 版是新的主推客户端，因此版本号走 2.x，与 .NET Framework 版
//  （1.1.0.1）和 C++ 控制台版（1.0.0.0）区分开。
// ---------------------------------------------------------------------------

using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

// ---------------------------------------------------------------------------
//  平台声明
//
//  这一条不能省。csproj 里设了 GenerateAssemblyInfo=false（因为版本信息在下面
//  手工管理），于是 SDK 不会自动生成 SupportedOSPlatform 特性，平台兼容性
//  分析器就会认为代码 "reachable on all platforms"，把每一个 WinUI 调用都
//  报成 CA1416 —— 三十条纯粹无意义的噪音。手工补上即可全部消除。
// ---------------------------------------------------------------------------
[assembly: SupportedOSPlatform("windows10.0.17763.0")]

[assembly: AssemblyTitle("ShittimLogon 更新器")]
[assembly: AssemblyDescription("ShittimLogon 更新器（下载、解压并运行安装程序）")]
[assembly: AssemblyCompany("ShittimLogon")]
[assembly: AssemblyProduct("ShittimLogon Updater")]
[assembly: AssemblyCopyright("Copyright (C) 2026 ShittimLogon")]
[assembly: AssemblyVersion("2.0.0.0")]
[assembly: AssemblyFileVersion("2.0.0.0")]
[assembly: AssemblyInformationalVersion("2.0.0.0")]
[assembly: ComVisible(false)]
