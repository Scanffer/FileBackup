// FileBackup —— 命令行入口
//
// 接口设计见 README §7.1。当前建立命令契约（子命令名、参数风格、退出码），
// 具体实现在后续里程碑填入：M1 做 backup/restore，M6 做 list/verify。
//
// 分层原则（README §4）：本文件只做参数解析与输出，不包含任何备份逻辑。
// 业务逻辑全部在 backup_core 里，以便被 CLI 与 server 共同链接。

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "fs_utils.h"
#include "version.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
// MinGW 的 <cstdint> 链路里已经定义过 NOMINMAX，直接再定义会触发
// "redefined" 警告，所以先判断一下。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

void print_usage(std::ostream& os) {
    os <<
        "FileBackup —— 文件备份软件\n"
        "\n"
        "用法：\n"
        "  FileBackup <命令> [选项]\n"
        "\n"
        "命令：\n"
        "  backup    备份目录树到指定位置\n"
        "  restore   将备份内容还原到指定位置\n"
        "  list      列出归档内容\n"
        "  verify    校验归档完整性\n"
        "\n"
        "通用选项：\n"
        "  -h, --help       显示本帮助\n"
        "  -v, --version    显示版本\n"
        "\n"
        "开发调试（M1 期间用于检查目录遍历，非 README §7.1 的正式接口）：\n"
        "  scan <目录>      列出该目录树下的所有文件\n"
        "\n"
        "示例（详见 README §7.1）：\n"
        "  FileBackup backup  --src ./docs --dst ./bak/docs.vbak \\\n"
        "                     --pack vbak --compress zstd --level 15 \\\n"
        "                     --encrypt aes256cbc --password ******\n"
        "  FileBackup restore --archive ./bak/docs.vbak --dst ./restored\n"
        "  FileBackup list    --archive ./bak/docs.vbak\n"
        "  FileBackup verify  --archive ./bak/docs.vbak\n"
        "\n"
        "两种输出模式（README §2.4）：\n"
        "  目录镜像  备份到目录          —— 基础必做，M1\n"
        "  单容器    备份到单个归档文件  —— 备选扩展，M6\n";
}

void print_version() {
    std::cout << "FileBackup " << backup::version() << "\n";
}

/// 尚未实现的子命令给出明确提示，而不是静默失败。
/// 对应里程碑标注出来，方便演示时解释进度。
void report_not_implemented(const std::wstring& command) {
    std::string milestone;
    if (command == L"backup" || command == L"restore") {
        milestone = "M1（目录镜像备份 / 还原）";
    } else {
        milestone = "M6（打包解包 / 单容器格式）";
    }

    std::cerr << "命令 '" << narrow_ascii(command) << "' 尚未实现。\n"
              << "计划于 " << milestone << " 交付，见 README §10 里程碑计划。\n";
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
// 代价：MinGW 下链接时要用 -municode（见根 CMakeLists.txt）。
// 将来若移植到 Linux，把 wmain 换回 main 即可，其余代码不用动。
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    enable_utf8_console();

    if (argc < 2) {
        print_usage(std::cerr);
        return 1;
    }

    const std::wstring command = argv[1];

    if (command == L"-h" || command == L"--help") {
        print_usage(std::cout);
        return 0;
    }

    if (command == L"-v" || command == L"--version") {
        print_version();
        return 0;
    }

    // 开发调试命令：验证 core 的目录遍历是否接进来了。
    // 不属于 README §7.1 的正式 CLI 契约，M1 完成备份功能后可移除。
    if (command == L"scan") {
        if (argc < 3) {
            std::cerr << "用法：FileBackup scan <目录>\n";
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
        std::cout << "\n共 " << files.size() << " 个文件，" << total << " 字节。\n";
        return 0;
    }

    if (command == L"backup" || command == L"restore" ||
        command == L"list" || command == L"verify") {
        report_not_implemented(command);
        return 2;
    }

    std::cerr << "未知命令：" << narrow_ascii(command) << "\n\n";
    print_usage(std::cerr);
    return 1;
}
