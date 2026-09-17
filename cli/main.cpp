// FileBackup —— 命令行入口
//
// 接口设计见 README §7.1。M0 只建立命令契约（子命令名、参数风格、退出码），
// 具体实现在后续里程碑填入：M1 做 backup/restore，M6 做 list/verify。
//
// 分层原则（README §4）：本文件只做参数解析与输出，不包含任何备份逻辑。
// 业务逻辑全部在 backup_core 里，以便被 CLI 与 server 共同链接。

#include <iostream>
#include <string>
#include <vector>

#include "version.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

/// Windows 控制台默认使用系统代码页（简体中文为 GBK），
/// 而源码以 UTF-8 编译（CMakeLists.txt 的 /utf-8）。
/// 两者不一致时中文会显示成乱码，因此在启动时切换代码页。
void enable_utf8_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
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
        "  -v, --version    显示版本与已链接的第三方库版本\n"
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

    std::cout << "\n已链接的第三方库：\n";
    for (const auto& lib : backup::linked_libraries()) {
        std::cout << "  " << lib.name << " " << lib.version << "\n";
    }
}

/// 尚未实现的子命令给出明确提示，而不是静默失败。
/// 对应里程碑标注出来，方便演示时解释进度。
void report_not_implemented(const std::string& command) {
    std::string milestone;
    if (command == "backup" || command == "restore") {
        milestone = "M1（目录镜像备份 / 还原）";
    } else {
        milestone = "M6（打包解包 / 单容器格式）";
    }

    std::cerr << "命令 '" << command << "' 尚未实现。\n"
              << "计划于 " << milestone << " 交付，见 README §10 里程碑计划。\n";
}

}  // namespace

int main(int argc, char** argv) {
    enable_utf8_console();

    if (argc < 2) {
        print_usage(std::cerr);
        return 1;
    }

    const std::string command = argv[1];

    if (command == "-h" || command == "--help") {
        print_usage(std::cout);
        return 0;
    }

    if (command == "-v" || command == "--version") {
        print_version();
        return 0;
    }

    if (command == "backup" || command == "restore" ||
        command == "list" || command == "verify") {
        report_not_implemented(command);
        return 2;
    }

    std::cerr << "未知命令：" << command << "\n\n";
    print_usage(std::cerr);
    return 1;
}
