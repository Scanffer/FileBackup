#pragma once

#include <filesystem>
#include <string>

namespace backup_test {

namespace fs = std::filesystem;

/// 测试用的临时根目录。
///
/// 优先用系统临时目录；但某些 Windows 安全软件/沙箱会禁止新编译的
/// exe 在 %TEMP% 下创建目录（Permission denied），所以先探测一次，
/// 失败则退回当前工作目录下的 .filebackup_test_tmp（通常位于 build/，
/// 可写且被 .gitignore 忽略）。
inline const fs::path& temp_root() {
    static const fs::path root = [] {
        std::error_code ec;
        const fs::path system_tmp = fs::temp_directory_path(ec);
        if (!ec) {
            const fs::path probe = system_tmp / "filebackup_tmp_probe";
            std::error_code probe_ec;
            if (fs::create_directories(probe, probe_ec) && !probe_ec) {
                fs::remove(probe, probe_ec);
                return system_tmp;
            }
        }

        const fs::path fallback = fs::current_path() / ".filebackup_test_tmp";
        std::error_code fallback_ec;
        fs::create_directories(fallback, fallback_ec);
        return fallback;
    }();
    return root;
}

/// 创建（并清空）一个测试专用目录。
inline fs::path fresh_dir(const std::string& name) {
    const fs::path dir = temp_root() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

}  // namespace backup_test
