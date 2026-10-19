// FileBackup.exe —— 图形界面入口（Windows GUI 子系统）
//
// 和 CLI 分开成两个 exe 之后：
//   - 这里链接的是 -mwindows，进程没有控制台窗口，双击启动就是一个
//     干净的应用窗口；
//   - 所有业务逻辑仍然在 backup_core / backup_server 里，CLI 与 GUI
//     共享同一套实现。
//
// 支持少量调试参数（见 print_usage / README）：
//   --port <端口>   起始端口，默认 8080，被占用时自动往后试
//   --browser       用浏览器打开，不开原生 WebView2 窗口
//   --no-browser    只起 HTTP 服务，什么都不打开（curl / 截图用）
// 为了兼容旧的单 exe 调用方式，开头的 "gui" / "--gui" / "web" 会被忽略。

#include "http_server.h"

#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void show_error(const wchar_t* message) {
#ifdef _WIN32
    MessageBoxW(nullptr, message, L"FileBackup", MB_OK | MB_ICONERROR);
#endif
}

/// 解析 GUI 启动参数。参数不合法时弹错误框并返回 nullopt。
std::optional<backup::WebOptions> parse_web_options(int argc, wchar_t** argv) {
    backup::WebOptions options;

    int first = 1;
    if (argc > 1) {
        const std::wstring head = argv[1];
        if (head == L"gui" || head == L"--gui" || head == L"web") first = 2;
    }

    auto find = [&](const std::wstring& key) -> std::optional<std::wstring> {
        for (int i = first; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == key && i + 1 < argc) return std::wstring(argv[i + 1]);
            if (arg.size() > key.size() + 1 &&
                arg.compare(0, key.size(), key) == 0 &&
                arg[key.size()] == L'=') {
                return arg.substr(key.size() + 1);
            }
        }
        return std::nullopt;
    };

    if (const auto raw = find(L"--port")) {
        try {
            options.port = std::stoi(std::string(raw->begin(), raw->end()));
        } catch (const std::exception&) {
            show_error(L"无效的端口参数。\n\n用法：FileBackup.exe --port 8080");
            return std::nullopt;
        }
        if (options.port < 1 || options.port > 65535) {
            show_error(L"端口超出范围（1-65535）。");
            return std::nullopt;
        }
    }

    // --browser 强制走浏览器（有 F12，调界面时方便）。
    options.native_window = !find(L"--browser").has_value();
    // --no-browser 只起服务，不弹任何窗口（curl / 无头截图用）。
    options.open_browser = !find(L"--no-browser").has_value();

    return options;
}

}  // namespace

#ifdef _WIN32

int APIENTRY wWinMain(HINSTANCE /*instance*/, HINSTANCE /*prev_instance*/,
                      PWSTR /*command_line*/, int /*show*/) {
    // 必须在创建任何窗口、初始化 WebView2 之前设置，否则 125% 缩放下
    // WebView2 内容会被当成位图整体放大，文字发虚。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        show_error(L"无法解析命令行参数。");
        return 1;
    }

    const auto options = parse_web_options(argc, argv);
    LocalFree(argv);
    if (!options) return 1;

    return backup::run_web_server(*options);
}

#else

int main() {
    // 本项目的 GUI 依赖 Windows WebView2，非 Windows 平台请使用 CLI。
    return 1;
}

#endif
