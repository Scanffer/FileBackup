// FileBackup-cli —— 命令行入口
//
// 和图形界面是**两个独立的 exe**：
//   FileBackup.exe      GUI（Windows 子系统，双击启动，无控制台）
//   FileBackup-cli.exe  CLI（控制台子系统，可被脚本调用、能捕获输出）
//
// 两个程序共享 core 备份引擎；GUI 通过 backup_server 暴露 HTTP 接口。
// 这样既不会有"双击 GUI 弹出一个终端窗口"的问题，CLI 的 stdout/stderr
// 也仍然是标准控制台语义。
//
// 分层原则（README §4）：本文件只做参数解析与输出，不包含任何备份逻辑。

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "algorithm.h"
#include "backup.h"
#include "fs_utils.h"
#include "pack.h"
#include "version.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
// MinGW 的 <cstdint> 链路里已经定义过 NOMINMAX，直接再定义会触发
// "redefined" 警告，所以先判断一下。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // ShellExecuteW —— 拉起同目录的 GUI 程序
#endif

namespace {

/// Windows 控制台默认使用系统代码页（简体中文为 GBK），而本程序内部
/// 一律用 UTF-8，两者不一致时中文会显示成乱码。启动时切到 UTF-8。
void enable_utf8_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

/// 命令名只可能是 ASCII（那是我们自己定的 CLI 契约），所以宽转窄是安全的。
///
/// 注意：**这个函数绝不能用来转路径**。路径里出现中文是完全正常的，
/// 而窄字符串在 MinGW 下无法构造 std::filesystem::path（见 wmain 的说明）。
std::string narrow_ascii(const std::wstring& w) {
    return std::string(w.begin(), w.end());
}

/// 宽字符串 -> UTF-8。密码可能包含中文/非 ASCII，不能用 narrow_ascii。
std::string wide_to_utf8(const std::wstring& w) {
#ifdef _WIN32
    const int size = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                         static_cast<int>(w.size()), nullptr,
                                         0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
#else
    return std::string(w.begin(), w.end());
#endif
}

std::optional<std::wstring> find_opt(int argc, wchar_t** argv, int start,
                                     const std::wstring& key) {
    for (int i = start; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == key && i + 1 < argc) {
            return std::wstring(argv[i + 1]);
        }
        if (a.size() > key.size() + 1 && a.compare(0, key.size(), key) == 0 &&
            a[key.size()] == L'=') {
            return a.substr(key.size() + 1);
        }
    }
    return std::nullopt;
}

/// 启动同目录下的图形界面程序。
bool launch_gui_app() {
#ifdef _WIN32
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return false;

    const std::filesystem::path gui =
        std::filesystem::path(self).parent_path() / L"FileBackup.exe";
    std::error_code ec;
    if (!std::filesystem::exists(gui, ec)) {
        std::cerr << "找不到图形界面程序：" << gui.u8string()
                  << "\n请确认 FileBackup.exe 与 FileBackup-cli.exe 在同一目录。\n";
        return false;
    }

    const HINSTANCE r = ShellExecuteW(nullptr, L"open", gui.c_str(), nullptr,
                                      nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        std::cerr << "启动图形界面失败（错误码 "
                  << reinterpret_cast<INT_PTR>(r) << "）\n";
        return false;
    }
    std::cout << "已启动图形界面：" << gui.u8string() << "\n";
    return true;
#else
    std::cerr << "当前平台没有图形界面程序。\n";
    return false;
#endif
}

void print_usage(std::ostream& os) {
    os << "FileBackup-cli —— 文件备份软件（命令行）\n"
          "\n"
          "用法：\n"
          "  FileBackup-cli <命令> [选项]\n"
          "\n"
          "命令：\n"
          "  backup    备份目录树到指定位置\n"
          "  restore   将备份内容还原到指定位置\n"
          "  list      列出归档内容（尚未实现）\n"
          "  verify    校验归档完整性（尚未实现）\n"
          "  gui       启动图形界面（同目录的 FileBackup.exe）\n"
          "\n"
          "通用选项：\n"
          "  -h, --help       显示本帮助\n"
          "  -v, --version    显示版本\n"
          "\n"
          "开发调试：\n"
          "  scan <目录>      列出该目录树下的所有文件\n"
          "\n"
          "示例：\n"
          "  FileBackup-cli backup  --src ./docs --dst ./bak/docs.vbak \\\n"
          "                         --pack vbak --compress deflate --level 6 \\\n"
          "                         --encrypt aes256cbc --password 123456\n"
          "  FileBackup-cli restore --archive ./bak/docs.vbak --dst ./restored \\\n"
          "                         --password 123456\n"
          "\n"
          "图形界面是独立程序 FileBackup.exe，双击启动；调试用参数：\n"
          "  FileBackup.exe --port 8080 --browser --no-browser\n";
}

void print_version() {
    std::cout << "FileBackup " << backup::version() << "（CLI）\n";
}

/// 尚未实现的子命令给出明确提示，而不是静默失败。
void report_not_implemented(const std::wstring& command) {
    std::cerr << "命令 '" << narrow_ascii(command) << "' 尚未实现。\n"
              << "归档 list/verify 会在 VBAK v2 完整归档模块完成后提供。\n";
}

}  // namespace

// ---- 为什么是 wmain 而不是 main ----
//
// Windows 上 main() 拿到的 argv 是**系统代码页**编码的（简体中文为 GBK）。
// 而 MinGW 的 std::filesystem::path 无法从非 ASCII 的窄字符串构造 ——
// 会抛 "Cannot convert character sequence: Illegal byte sequence"，
// 且改全局 locale 也没用（libstdc++ 没实现走 ANSI 代码页的转换）。
// 后果：用户目录叫「我的文档」这种再正常不过的情况会直接崩溃。
//
// wmain 拿到的 argv 是 UTF-16，path 从宽字符串构造是原生支持的，
// 不需要任何手工转码。内部一律用宽字符构造路径，输出时用
// generic_u8string() 转成 UTF-8 交给已切到 CP_UTF8 的控制台。
//
// 代价：MinGW 下链接时要用 -municode（见 cli/CMakeLists.txt）。
// 将来若移植到 Linux，把 wmain 换回 main 即可，其余代码不用动。
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    enable_utf8_console();

    if (argc < 2) {
        print_usage(std::cout);
        return 0;
    }

    const std::wstring command = argv[1];

    // 兼容旧习惯：从 CLI 里也能把 GUI 拉起来。
    if (command == L"gui" || command == L"--gui" || command == L"web") {
        return launch_gui_app() ? 0 : 1;
    }

    if (command == L"-h" || command == L"--help") {
        print_usage(std::cout);
        return 0;
    }

    if (command == L"-v" || command == L"--version") {
        print_version();
        return 0;
    }

    // 开发调试命令：验证 core 的目录遍历是否接进来了。
    if (command == L"scan") {
        if (argc < 3) {
            std::cerr << "用法：FileBackup-cli scan <目录>\n";
            return 1;
        }

        // 直接用宽字符参数构造 path，不做任何转码 —— 这是中文路径能work的关键。
        const std::filesystem::path root = argv[2];
        const std::vector<backup::FileEntry> files = backup::list_files(root);

        std::uintmax_t total = 0;
        for (const backup::FileEntry& file : files) {
            std::cout << file.size << "\t" << file.relative_path << "\n";
            total += file.size;
        }
        std::cout << "\n共 " << files.size() << " 个文件，" << total
                  << " 字节。\n";
        return 0;
    }

    if (command == L"backup") {
        const auto src = find_opt(argc, argv, 2, L"--src");
        const auto dst = find_opt(argc, argv, 2, L"--dst");
        const auto pack = find_opt(argc, argv, 2, L"--pack");
        const auto compress = find_opt(argc, argv, 2, L"--compress");
        const auto level = find_opt(argc, argv, 2, L"--level");
        const auto encrypt = find_opt(argc, argv, 2, L"--encrypt");
        const auto password = find_opt(argc, argv, 2, L"--password");

        if (!src || !dst) {
            std::cerr << "用法：FileBackup-cli backup --src <目录> --dst <目标>\n"
                         "      加 --pack vbak 打包成单文件归档，可再指定\n"
                         "      --compress deflate --level 6\n"
                         "      --encrypt aes256cbc --password <密码>\n";
            return 1;
        }

        try {
            if (pack) {
                if (*pack != L"vbak") {
                    std::cerr << "不支持的打包格式：" << narrow_ascii(*pack)
                              << "\n";
                    return 1;
                }
                backup::PackOptions o;
                o.src = *src;
                o.archive = *dst;  // 打包时 --dst 是归档文件路径

                if (compress) {
                    const auto id = backup::compression_from_string(
                        narrow_ascii(*compress));
                    if (!id) {
                        std::cerr << "不支持的压缩算法："
                                  << narrow_ascii(*compress) << "\n";
                        return 1;
                    }
                    o.compression = *id;
                }
                if (level) {
                    try {
                        o.compression_level = std::stoi(narrow_ascii(*level));
                    } catch (const std::exception&) {
                        std::cerr << "无效的压缩级别："
                                  << narrow_ascii(*level) << "\n";
                        return 1;
                    }
                    if (o.compression_level < 1 ||
                        o.compression_level > 19) {
                        std::cerr << "压缩级别超出范围（1-19）："
                                  << o.compression_level << "\n";
                        return 1;
                    }
                }
                if (encrypt) {
                    const auto id = backup::encryption_from_string(
                        narrow_ascii(*encrypt));
                    if (!id) {
                        std::cerr << "不支持的加密算法："
                                  << narrow_ascii(*encrypt) << "\n";
                        return 1;
                    }
                    o.encryption = *id;
                }
                if (password) o.password = wide_to_utf8(*password);
                if (o.encryption != backup::EncryptionId::None &&
                    o.password.empty()) {
                    std::cerr << "使用 --encrypt 时必须提供非空 --password。\n";
                    return 1;
                }
                if (o.encryption == backup::EncryptionId::None && password) {
                    std::cerr << "--password 只在 --encrypt 时生效。\n";
                    return 1;
                }

                const backup::PackResult r = backup::pack_directory(o);
                std::cout << "打包完成（" << backup::to_string(o.compression);
                if (o.encryption != backup::EncryptionId::None) {
                    std::cout << "，加密 " << backup::to_string(o.encryption);
                }
                std::cout << "）：" << r.entries_written << " 个文件，"
                          << r.bytes_written << " 字节。\n";
            } else {
                if (compress || level || encrypt || password) {
                    std::cerr << "--compress / --level / --encrypt / "
                                 "--password 只在 --pack vbak 模式下生效。\n";
                    return 1;
                }
                backup::MirrorOptions o;
                o.src = *src;
                o.dst = *dst;
                const backup::MirrorResult r = backup::mirror_directory(o);
                std::cout << "完成：" << r.files_copied << " 个文件，"
                          << r.bytes_copied << " 字节。\n";
                if (r.files_skipped > 0) {
                    std::cout << "跳过 " << r.files_skipped
                              << " 个（已存在）。\n";
                }
            }
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "失败：" << e.what() << "\n";
            return 3;
        }
    }

    if (command == L"restore") {
        const auto archive = find_opt(argc, argv, 2, L"--archive");
        const auto src = find_opt(argc, argv, 2, L"--src");
        const auto dst = find_opt(argc, argv, 2, L"--dst");
        const auto password = find_opt(argc, argv, 2, L"--password");

        if (!dst || (!archive && !src)) {
            std::cerr
                << "用法：FileBackup-cli restore --archive <归档> --dst <目录>\n"
                   "      或 FileBackup-cli restore --src <备份目录> --dst "
                   "<目录>\n"
                   "      加密归档需加 --password <密码>\n";
            return 1;
        }
        if (!archive && password) {
            std::cerr << "--password 只在解包加密归档时使用。\n";
            return 1;
        }

        try {
            if (archive) {
                backup::UnpackOptions o;
                o.archive = *archive;
                o.dst = *dst;
                if (password) o.password = wide_to_utf8(*password);
                const backup::UnpackResult r = backup::unpack_archive(o);
                std::cout << "解包完成：" << r.entries_read << " 个文件，"
                          << r.bytes_read << " 字节。\n";
            } else {
                backup::MirrorOptions o;
                o.src = *src;
                o.dst = *dst;
                const backup::MirrorResult r = backup::mirror_directory(o);
                std::cout << "还原完成：" << r.files_copied << " 个文件，"
                          << r.bytes_copied << " 字节。\n";
            }
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "失败：" << e.what() << "\n";
            return 3;
        }
    }

    if (command == L"list" || command == L"verify") {
        report_not_implemented(command);
        return 2;
    }

    std::cerr << "未知命令：" << narrow_ascii(command) << "\n\n";
    print_usage(std::cerr);
    return 1;
}
