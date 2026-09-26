using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

// ---------------------------------------------------------------------------
//  平台声明
//
//  不能省：csproj 里设了 GenerateAssemblyInfo=false（版本信息在下面手工管理），
//  SDK 就不会自动生成 SupportedOSPlatform 特性，平台兼容性分析器会把每一个
//  WinUI 调用都报成 CA1416。手工补上即可全部消除。
// ---------------------------------------------------------------------------
[assembly: SupportedOSPlatform("windows10.0.17763.0")]

[assembly: AssemblyTitle("ShittimLogon 发送端")]
[assembly: AssemblyDescription("ShittimLogon 发送端（TCP 文件服务，按需分发安装包）")]
[assembly: AssemblyCompany("ShittimLogon")]
[assembly: AssemblyProduct("ShittimLogon Updater")]
[assembly: AssemblyCopyright("Copyright (C) 2026 ShittimLogon")]
[assembly: AssemblyVersion("2.0.0.0")]
[assembly: AssemblyFileVersion("2.0.0.0")]
[assembly: AssemblyInformationalVersion("2.0.0.0")]
[assembly: ComVisible(false)]
