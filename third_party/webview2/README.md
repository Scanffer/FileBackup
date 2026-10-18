# WebView2 SDK（vendored）

来源：NuGet 包 `Microsoft.Web.WebView2`，版本 **1.0.4258.31**（稳定版）。
包本身有 10.2 MB，其中 90% 是 .NET / UWP / ARM 的东西，我们只取了下面三个文件。

| 文件 | 大小 | 用途 |
|---|---|---|
| `WebView2.h` | 2.95 MB | COM 接口定义。**编译期**用，不进 exe |
| `WebView2EnvironmentOptions.h` | 19 KB | `ICoreWebView2EnvironmentOptions` 的实现类。目前没用上，留着备用 |
| `WebView2Loader.dll` | 190 KB | 运行时定位并加载 WebView2 运行时的引导 DLL。**会被嵌进 exe** |

`loader.rc` 是本项目自己写的（不是 SDK 的一部分），负责把那个 DLL 编进资源段。

## 关于那几个没取的文件

包里还有 `WebView2Experimental.h`（225 KB，实验性 API）、`ClusterEnvironmentOptions.h`
和 `WebView2LoaderStatic.lib`（11 MB，MSVC 静态库）。都用不上：

- 实验性 API 不稳定，不用
- 静态库那条路 **MinGW 走不通** —— 它的 `__declspec(selectany)` 编成 COMDAT 段
  `.rdata[IID_X]`，而 MinGW 编成 `.rdata$IID_X`，两边段名规则不同，链接器会报
  一堆 `multiple definition of IID_*`。用导入库或显式链接都没这个问题

## 许可

按微软 WebView2 SDK 的许可条款分发（NuGet 包里带 `LICENSE.txt`）。
本目录没有随附那份文件 —— 如果要正式对外发布，应当把它一并放进来。
