#include "backup.h"
#include "fs_utils.h"

#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace backup {
namespace {

/// UTF-8 std::string -> std::filesystem::path。
///
/// 绝不能写 std::filesystem::path(s)：
/// MinGW 下窄字符串构造会按本地代码页（GBK）解释，中文直接抛
/// "Illegal byte sequence"。必须走 u8 通道。
std::filesystem::path path_from_u8(const std::string& s) {
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
#else
    return std::filesystem::u8path(s);
#endif
}

}  // namespace

MirrorResult mirror_directory(const MirrorOptions& opts) {
    std::error_code ec;

    if (!std::filesystem::is_directory(opts.src, ec)) {
        throw std::runtime_error("源目录不存在或不是目录");
    }

    std::filesystem::create_directories(opts.dst, ec);
    if (ec) {
        throw std::runtime_error("无法创建目标目录：" + ec.message());
    }

    const std::vector<FileEntry> files = list_files(opts.src);

    MirrorResult result;
    for (const FileEntry& entry : files) {
        const std::filesystem::path rel = path_from_u8(entry.relative_path);
        const std::filesystem::path from = opts.src / rel;
        const std::filesystem::path to   = opts.dst / rel;

        std::filesystem::create_directories(to.parent_path(), ec);
        if (ec) {
            throw std::runtime_error("无法创建目录：" +
                                     to.parent_path().u8string() + "：" +
                                     ec.message());
        }

        if (opts.overwrite) {
            // 先删掉目标（若存在），避免依赖 copy_file 的 overwrite_existing，
            // 那个选项在 MinGW/libstdc++ 上行为不稳，会遇到已存在文件仍报
            // "File exists"。remove 对不存在的文件返回 false 但不设错误码。
            std::error_code ignore;
            std::filesystem::remove(to, ignore);
        }

        ec.clear();
        std::filesystem::copy_file(from, to,
                                std::filesystem::copy_options::none, ec);
        if (ec) {
            // 目标存在时一律算「跳过」，不管 overwrite 是什么。
            // 因为 overwrite=true 的路径上我们已经 remove 过了，走到这里
            // 要么是权限问题，要么是 overwrite=false 的预期跳过。
            if (ec == std::errc::file_exists) {
                ++result.files_skipped;
                continue;
            }
            throw std::runtime_error("复制失败：" + entry.relative_path +
                                    "：" + ec.message());
        }

        ++result.files_copied;
        result.bytes_copied += entry.size;
    }

    return result;
}

}  // namespace backup