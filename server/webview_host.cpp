// WebView2 宿主窗口的实现。
//
// 编译期只需要 WebView2.h；运行时需要一个 WebView2Loader.dll，而那个 DLL
// 是嵌在本 exe 的资源段里的（见 third_party/webview2/loader.rc），
// 启动时释放到临时目录再按绝对路径加载 —— 这样交付形态仍然是单个 exe。
//
// MinGW 相关的两个坑，都在下面就地注释了：
//   1. wrl/client.h 只有 ComPtr，没有 Microsoft::WRL::Callback
//   2. 必须定义 UNICODE，否则 IDC_ARROW 之类的宏会展开成 ANSI 版本

#include "webview_host.h"

#ifdef _WIN32

// 必须在 windows.h 之前：否则 IDC_ARROW 等宏走 ANSI 版本，
// 传给 LoadCursorW 是类型不匹配。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <dwmapi.h>   // DwmSetWindowAttribute：让系统标题栏跟随页面切深色
#include <shobjidl.h>  // IFileOpenDialog / IFileSaveDialog：原生选择框

#include <objbase.h>  // CoInitializeEx

#include <cstdint>
#include <iostream>
#include <string>

#include "WebView2.h"

namespace backup::webview {
namespace {

// ---------------------------------------------------------------------------
// 把 lambda 包成 COM 回调的最小骨架
//
// 微软官方示例全用 Microsoft::WRL::Callback，但 MinGW 的 wrl/client.h
// 只有 247 行、只实现了 ComPtr，**没有 Callback**，只能自己写。
//
// 生命周期约定（和 WRL::Callback 一致）：引用计数从 1 开始，那是"我们手里
// 这一份"；交给 WebView2 之后立刻 Release 掉，它会在异步操作完成前自己 AddRef。
// ---------------------------------------------------------------------------
template <class I>
class ComHandler : public I {
public:
    explicit ComHandler(const IID& iid) : iid_(iid) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (IsEqualIID(riid, iid_) || IsEqualIID(riid, IID_IUnknown)) {
            *ppv = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

private:
    LONG refs_ = 1;
    IID iid_;
};

// ---------------------------------------------------------------------------
// 从自己的资源段里把 WebView2Loader.dll 释放出来
// ---------------------------------------------------------------------------
constexpr const wchar_t* kLoaderResName = L"WEBVIEW2LOADER";
constexpr const wchar_t* kLoaderFileName = L"WebView2Loader.dll";

/// FNV-1a 64 位。不是加密哈希 —— 这里只要一个"内容变了目录名就变"的短标识。
std::wstring HashHex(const void* data, std::size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    wchar_t buf[17]{};
    for (int i = 15; i >= 0; --i) {
        buf[i] = L"0123456789abcdef"[h & 0xF];
        h >>= 4;
    }
    return buf;
}

/// 把资源里的 loader 落到临时目录，返回绝对路径；失败返回空串。
///
/// 目录名带内容哈希有两个目的：
///   1. 将来换 SDK 版本会自动换新目录，不会和旧的打架
///   2. 同时开多个实例时大家算出同一个路径，一个写、其余直接复用，
///      不会撞上"文件已被占用，写不进去"
std::wstring ExtractLoader() {
    HRSRC res = FindResourceW(nullptr, kLoaderResName, RT_RCDATA);
    if (res == nullptr) return {};  // exe 里没嵌（比如构建时漏了资源对象）
    const DWORD size = SizeofResource(nullptr, res);
    HGLOBAL handle = LoadResource(nullptr, res);
    if (handle == nullptr || size == 0) return {};
    const void* bytes = LockResource(handle);
    if (bytes == nullptr) return {};

    wchar_t tmp_dir[MAX_PATH + 1]{};
    const DWORD n = GetTempPathW(MAX_PATH, tmp_dir);
    std::wstring root(tmp_dir, n);
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) {
        root.pop_back();
    }
    root += L"\\FileBackup";

    const std::wstring dir = root + L"\\wv2-" + HashHex(bytes, size);
    CreateDirectoryW(root.c_str(), nullptr);  // 已存在会失败，无所谓
    CreateDirectoryW(dir.c_str(), nullptr);

    const std::wstring dll = dir + L"\\" + kLoaderFileName;

    // 已经在了（多半是另一个实例刚释放的），直接复用。
    // 顺带校验大小，避免复用一个写了一半的文件。
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(dll.c_str(), GetFileExInfoStandard, &attr) != 0 &&
        attr.nFileSizeHigh == 0 && attr.nFileSizeLow == size) {
        return dll;
    }

    // 先写临时名再改名。直接写目标文件的话，两个实例同时启动会互相截断，
    // **写出一份半截的 DLL 然后被 LoadLibrary 加载**。
    // 改名失败通常意味着别人抢先写好了 —— 那正好，直接用。
    const std::wstring tmp_file =
        dll + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE f = CreateFileW(tmp_file.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const BOOL ok = WriteFile(f, bytes, size, &written, nullptr);
        CloseHandle(f);
        if (ok != 0 && written == size) {
            if (MoveFileExW(tmp_file.c_str(), dll.c_str(),
                            MOVEFILE_REPLACE_EXISTING) == 0) {
                DeleteFileW(tmp_file.c_str());
            }
        } else {
            DeleteFileW(tmp_file.c_str());
        }
    }
    return dll;
}

/// 释放并加载 loader，返回模块句柄；失败返回 nullptr。
HMODULE LoadLoader() {
    const std::wstring path = ExtractLoader();
    if (!path.empty()) {
        // 用绝对路径：%TEMP% 不在默认 DLL 搜索路径上，光靠文件名找不到。
        if (HMODULE m = LoadLibraryW(path.c_str())) return m;
        std::cerr << "WebView2Loader.dll 加载失败（错误码 " << GetLastError()
                  << "）\n";
    }
    // 兜底：exe 同目录里直接放一个（开发调试时方便）。
    HMODULE m = LoadLibraryW(kLoaderFileName);
    if (m == nullptr) {
        std::cerr << "找不到 WebView2Loader.dll"
                     "（既不在资源段里，也不在同目录）\n";
    }
    return m;
}

// ---------------------------------------------------------------------------
// 窗口与 WebView2 状态
//
// 用全局变量是有意的：这个程序同一时刻只可能有一个主窗口，把它塞进一个
// 结构体再层层传递，只会让窗口过程和两个异步回调都多拿一个参数，没有收益。
// ---------------------------------------------------------------------------
struct WindowState {
    HWND hwnd = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* webview = nullptr;
};
WindowState g_win;

/// PostQuitMessage 的退出码，用来区分"用户关窗"和"WebView2 起不来"。
constexpr int kExitInitFailed = 1;

// ---------------------------------------------------------------------------
// 原生文件 / 文件夹选择框
//
// 页面上的「浏览…」按钮发消息请求，宿主打开和资源管理器同款的选择框
// （IFileOpenDialog / IFileSaveDialog）。相比旧式的 SHBrowseForFolder，
// 它有路径栏、快速访问、可以直接粘路径，才像个正经桌面程序。
// 对话框是模态的；选完或取消后把结果 PostWebMessageAsString 回页面。
// ---------------------------------------------------------------------------

/// IShellItem -> 文件系统路径；失败返回空串。
std::wstring ShellItemPath(IShellItem* item) {
    if (item == nullptr) return {};
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) ||
        raw == nullptr) {
        return {};
    }
    std::wstring path(raw);
    CoTaskMemFree(raw);
    return path;
}

/// 文件 / 文件夹选择框。取消或失败返回空串。
std::wstring PickOpenPath(HWND owner, bool pick_folders, bool vbak_filter) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                reinterpret_cast<void**>(&dialog)))) {
        return {};
    }

    FILEOPENDIALOGOPTIONS options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (pick_folders) options |= FOS_PICKFOLDERS;
        dialog->SetOptions(options);
    }

    const COMDLG_FILTERSPEC filters[] = {
        {L"VBAK 归档文件", L"*.vbak"},
        {L"所有文件", L"*.*"},
    };
    if (vbak_filter) {
        dialog->SetFileTypes(static_cast<UINT>(ARRAYSIZE(filters)), filters);
        dialog->SetFileTypeIndex(1);
    }

    std::wstring result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            result = ShellItemPath(item);
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

/// 「另存为」选择框（打包时选 .vbak 输出路径）。取消返回空串。
std::wstring PickSavePath(HWND owner) {
    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_IFileSaveDialog,
                                reinterpret_cast<void**>(&dialog)))) {
        return {};
    }

    const COMDLG_FILTERSPEC filters[] = {
        {L"VBAK 归档文件", L"*.vbak"},
        {L"所有文件", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(ARRAYSIZE(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"vbak");
    dialog->SetFileName(L"backup.vbak");

    FILEOPENDIALOGOPTIONS options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                   FOS_OVERWRITEPROMPT;
        dialog->SetOptions(options);
    }

    std::wstring result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            result = ShellItemPath(item);
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

/// 把选择结果回给页面。取消时 path 为空，页面据此清掉等待状态。
void ReplyPicked(const std::wstring& path) {
    if (g_win.webview == nullptr) return;
    const std::wstring message = L"picked|" + path;
    g_win.webview->PostWebMessageAsString(message.c_str());
}

// ---- 页面 → 宿主的命令 ---------------------------------------------------
//
// 窗口没有系统标题栏（理由见 show_url 里的 kWindowStyle），所以拖拽、
// 最小化、最大化、关闭都得由页面发起；「浏览…」也要宿主的原生对话框。
// 页面用 window.chrome.webview.postMessage("...") 发字符串，这里翻译成
// 对应的 Win32 / COM 调用。消息只认固定几步，其它一律忽略。
class PageCommandHandler
    : public ComHandler<ICoreWebView2WebMessageReceivedEventHandler> {
public:
    explicit PageCommandHandler(HWND hwnd)
        : ComHandler(IID_ICoreWebView2WebMessageReceivedEventHandler),
          hwnd_(hwnd) {}

    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2* /*sender*/,
        ICoreWebView2WebMessageReceivedEventArgs* args) override {
        if (args == nullptr) return S_OK;

        LPWSTR raw = nullptr;
        if (FAILED(args->TryGetWebMessageAsString(&raw)) || raw == nullptr) {
            return S_OK;  // 页面只发字符串，JSON 消息不处理
        }
        const std::wstring command(raw);
        CoTaskMemFree(raw);

        if (command == L"pick:folder") {
            ReplyPicked(PickOpenPath(hwnd_, true, false));
        } else if (command == L"pick:file") {
            ReplyPicked(PickOpenPath(hwnd_, false, true));
        } else if (command == L"pick:save") {
            ReplyPicked(PickSavePath(hwnd_));
        } else if (command == L"window:drag") {
            // 系统拖拽窗口的标准做法：把这次按下当成拖拽标题栏。
            // 最大化状态下 Windows 会先还原再跟着鼠标走。
            ReleaseCapture();
            SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        } else if (command == L"window:minimize") {
            ShowWindow(hwnd_, SW_MINIMIZE);
        } else if (command == L"window:maximize") {
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        } else if (command == L"window:close") {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        return S_OK;
    }

private:
    HWND hwnd_;
};

// ---- 第二级回调：controller 建好了 ----
class ControllerReadyHandler
    : public ComHandler<
          ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    ControllerReadyHandler(HWND hwnd, std::wstring url)
        : ComHandler(
              IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler),
          hwnd_(hwnd),
          url_(std::move(url)) {}

    HRESULT STDMETHODCALLTYPE Invoke(
        HRESULT result, ICoreWebView2Controller* controller) override {
        if (FAILED(result) || controller == nullptr) {
            std::cerr << "创建 WebView2 controller 失败（0x" << std::hex
                      << static_cast<unsigned long>(result) << std::dec
                      << "）\n";
            PostQuitMessage(kExitInitFailed);
            return S_OK;
        }

        controller->AddRef();
        g_win.controller = controller;
        controller->get_CoreWebView2(&g_win.webview);

        // WebView2 默认底色是白色，页面第一帧画出来之前会露出来。设成与
        // 页面相同的 #1F1F1F，深色界面就不会在加载时闪白。
        // ICoreWebView2Controller2 只在较新的运行时上有，取不到就算了。
        ICoreWebView2Controller2* controller2 = nullptr;
        if (SUCCEEDED(controller->QueryInterface(
                IID_ICoreWebView2Controller2,
                reinterpret_cast<void**>(&controller2)))) {
            const COREWEBVIEW2_COLOR background = {255, 31, 31, 31};
            controller2->put_DefaultBackgroundColor(background);
            controller2->Release();
        }

        // 页面标题栏上的窗口按钮、拖拽和「浏览…」都通过 WebView2 消息
        // 回传（默认开启）。注册后 WebView2 持有引用，我们这份即可释放。
        EventRegistrationToken message_token{};
        auto* page_commands = new PageCommandHandler(hwnd_);
        g_win.webview->add_WebMessageReceived(page_commands, &message_token);
        page_commands->Release();

        RECT rc{};
        GetClientRect(hwnd_, &rc);
        g_win.controller->put_Bounds(rc);
        g_win.controller->put_IsVisible(TRUE);
        g_win.webview->Navigate(url_.c_str());

        // 到这一步才显示窗口。顺序很关键：窗口从头到尾是隐藏创建的，
        // 所以运行时缺失时用户看不到任何东西，而不是先闪一个白窗口再消失。
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        SetForegroundWindow(hwnd_);
        return S_OK;
    }

private:
    HWND hwnd_;
    std::wstring url_;
};

// ---- 第一级回调：WebView2 运行时环境建好了 ----
class EnvReadyHandler
    : public ComHandler<
          ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
    EnvReadyHandler(HWND hwnd, std::wstring url)
        : ComHandler(
              IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler),
          hwnd_(hwnd),
          url_(std::move(url)) {}

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                     ICoreWebView2Environment* env) override {
        if (FAILED(result) || env == nullptr) {
            // 最常见的原因就是这台机器没装 WebView2 运行时
            // （LTSC 版、或者被企业策略移除了 Edge）。
            std::cerr << "创建 WebView2 环境失败（0x" << std::hex
                      << static_cast<unsigned long>(result) << std::dec
                      << "），可能没装 WebView2 运行时\n";
            PostQuitMessage(kExitInitFailed);
            return S_OK;
        }
        auto* next = new ControllerReadyHandler(hwnd_, std::move(url_));
        env->CreateCoreWebView2Controller(hwnd_, next);
        next->Release();  // 交出去之后我们这份引用就用完了
        return S_OK;
    }

private:
    HWND hwnd_;
    std::wstring url_;
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            // 让 webview 跟着窗口一起变。漏了这段的话，拉大窗口会露出白边。
            if (g_win.controller != nullptr) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                g_win.controller->put_Bounds(rc);
            }
            // 把最大化/还原状态告诉页面：标题栏上的按钮图标要跟着换。
            if (g_win.webview != nullptr) {
                if (wp == SIZE_MAXIMIZED) {
                    g_win.webview->PostWebMessageAsString(L"window:maximized");
                } else if (wp == SIZE_RESTORED) {
                    g_win.webview->PostWebMessageAsString(L"window:restored");
                }
            }
            return 0;

        case WM_GETMINMAXINFO: {
            // 没有标题栏的 WS_POPUP 窗口默认会把最大化当成「铺满整块
            // 屏幕」，连任务栏一起盖住。这里改成按显示器工作区来最大化，
            // 同时给一个合理的拖动最小尺寸。
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            DefWindowProcW(hwnd, WM_GETMINMAXINFO, 0, lp);

            HMONITOR monitor =
                MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(monitor, &mi)) {
                info->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                info->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                info->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                info->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            }

            const UINT dpi = GetDpiForWindow(hwnd);
            info->ptMinTrackSize.x = MulDiv(720, static_cast<int>(dpi), 96);
            info->ptMinTrackSize.y = MulDiv(480, static_cast<int>(dpi), 96);
            return 0;
        }

        case WM_DPICHANGED: {
            // 窗口拖到另一块不同缩放比例的显示器时，按系统建议的新矩形
            // 调整窗口；页面内容由 WebView2 按新 DPI 重新渲染。
            const auto* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_DESTROY:
            // 释放顺序无所谓，但必须在窗口销毁时做掉，否则拖到进程退出
            // 就是一堆没释放的 COM 引用。
            if (g_win.webview) {
                g_win.webview->Release();
                g_win.webview = nullptr;
            }
            if (g_win.controller) {
                g_win.controller->Release();
                g_win.controller = nullptr;
            }
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

constexpr const wchar_t* kWindowClass = L"FileBackupMainWindow";

// 主窗口不带系统标题栏：窗口顶部的「标题栏」就是页面里的 appbar，
// 拖拽和最小化/最大化/关闭都由页面发给宿主执行（见 PageCommandHandler）。
// WS_THICKFRAME 保留系统原生缩放边框；WS_MINIMIZEBOX/WS_MAXIMIZEBOX 让
// 任务栏菜单和 ShowWindow 的最小化/最大化行为保持正常。
constexpr DWORD kWindowStyle =
    WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;

/// 注册窗口类。第二次调用会因为类已存在而失败，那是正常的，忽略即可。
void EnsureWindowClass() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // 页面是深色，窗口背景也要是深色：从窗口创建到 WebView2 画出第一帧
    // 之间有一段空窗期，背景若取系统默认值（浅色主题下是白色）就会闪一下。
    // 这个画刷随窗口类存活到进程结束，不需要单独释放。
    wc.hbrBackground = CreateSolidBrush(RGB(0x1F, 0x1F, 0x1F));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
}

/// WebView2 要一个可写的绝对路径放缓存和浏览器配置。
/// 用 %LOCALAPPDATA% 而不是 exe 同目录 —— 后者可能没写权限，
/// 也不该往程序目录里拉屎。
std::wstring UserDataFolder() {
    wchar_t local_appdata[MAX_PATH + 1]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local_appdata, MAX_PATH);
    return std::wstring(local_appdata) + L"\\FileBackup\\WebView2";
}

using CreateEnvFn = HRESULT(STDMETHODCALLTYPE*)(
    PCWSTR browserExecutableFolder, PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*
        environmentCreatedHandler);

}  // namespace

ShowResult show_url(const std::wstring& url, const std::wstring& title) {
    // WebView2 要求 STA。这条不满足的话 CreateCoreWebView2EnvironmentWithOptions
    // 会直接失败，而且报错信息很不直观。
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool own_com = SUCCEEDED(com_hr);
    if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE) {
        std::cerr << "COM 初始化失败\n";
        return ShowResult::Unavailable;
    }

    HMODULE loader = LoadLoader();
    if (loader == nullptr) return ShowResult::Unavailable;
    auto create_env = reinterpret_cast<CreateEnvFn>(
        GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions"));
    if (create_env == nullptr) {
        std::cerr << "WebView2Loader.dll 里没有预期的导出函数\n";
        return ShowResult::Unavailable;
    }

    // 隐藏创建。等 WebView2 真正就绪了才 ShowWindow（见上面回调里的注释）。
    EnsureWindowClass();

    // 先按 100% 兜底尺寸创建（隐藏），再按真实 DPI 调整大小。
    //
    // 这里不能用进程启动阶段能拿到的 DPI 值：本机 125% 缩放下，
    // GetDpiForSystem / GetDeviceCaps 都返回 96，GetDpiForWindow(桌面窗口)
    // 也不可靠；窗口一旦创建就关联到了显示器，此时 GetDpiForWindow(hwnd)
    // 返回的才是有效 DPI。算错的后果是窗口比 WebView2 渲染出来的内容
    // 小一圈，右侧被裁掉。
    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW, kWindowClass, title.c_str(), kWindowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 620, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        std::cerr << "CreateWindowExW 失败\n";
        if (own_com) CoUninitialize();
        return ShowResult::Unavailable;
    }
    const UINT dpi = GetDpiForWindow(hwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, 920 * dpi / 96, 620 * dpi / 96,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_win.hwnd = hwnd;

    // 窗口没有系统标题栏，但 DWM 仍会画外边框和阴影；深色属性保证在
    // 浅色系统主题下这条边框也不发白。属性 20 是 Windows 10 20H1+ 的
    // DWMWA_USE_IMMERSIVE_DARK_MODE；更早的预览版编号是 19，失败时兜底。
    // MinGW 的 dwmapi.h 不保证有对应枚举名，所以直接写字面量；失败不影响
    // 功能，只是边框跟随系统主题。
    const BOOL dark_titlebar = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark_titlebar,
                                     sizeof(dark_titlebar)))) {
        DwmSetWindowAttribute(hwnd, 19, &dark_titlebar, sizeof(dark_titlebar));
    }

    auto* env_handler = new EnvReadyHandler(hwnd, url);
    const HRESULT hr = create_env(nullptr, UserDataFolder().c_str(), nullptr,
                                  env_handler);
    env_handler->Release();
    if (FAILED(hr)) {
        std::cerr << "CreateCoreWebView2EnvironmentWithOptions 调用失败\n";
        DestroyWindow(hwnd);
        if (own_com) CoUninitialize();
        return ShowResult::Unavailable;
    }

    // 消息循环。**没有它，上面那两个异步回调永远不会被触发** ——
    // WebView2 的完成回调是通过窗口消息派发的，create_env 返回时什么都还没发生。
    MSG msg{};
    BOOL got;
    while ((got = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    const bool init_failed =
        (got == 0) && (msg.wParam == static_cast<WPARAM>(kExitInitFailed));

    // 起不来时窗口还留着（隐藏状态），得自己收掉；正常关窗的路径里
    // WM_DESTROY 已经把它销毁了。
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    g_win.hwnd = nullptr;

    if (own_com) CoUninitialize();
    return init_failed ? ShowResult::Unavailable : ShowResult::Closed;
}

}  // namespace backup::webview

#else  // 非 Windows

namespace backup::webview {

ShowResult show_url(const std::wstring&, const std::wstring&) {
    return ShowResult::Unavailable;  // 让调用方走浏览器兜底
}

}  // namespace backup::webview

#endif
