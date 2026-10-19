#include "fs_utils.h"

#include <algorithm>
#include <system_error>

namespace backup {

const char* to_string(EntryType type) {
    switch (type) {
        case EntryType::File: return "file";
        case EntryType::Directory: return "directory";
        case EntryType::Symlink: return "symlink";
        case EntryType::Hardlink: return "hardlink";
        case EntryType::Fifo: return "fifo";
        case EntryType::Device: return "device";
        case EntryType::Socket: return "socket";
        case EntryType::Other: return "other";
    }
    return "other";
}

namespace {

/// 把 directory_entry 归类成 EntryType（不跟随符号链接）。
EntryType classify(const std::filesystem::directory_entry& item,
                   std::error_code& ec) {
    const std::filesystem::file_status status = item.symlink_status(ec);
    if (ec) return EntryType::Other;

    using std::filesystem::is_block_file;
    using std::filesystem::is_character_file;
    using std::filesystem::is_directory;
    using std::filesystem::is_fifo;
    using std::filesystem::is_regular_file;
    using std::filesystem::is_socket;
    using std::filesystem::is_symlink;

    if (is_symlink(status)) return EntryType::Symlink;
    if (is_regular_file(status)) return EntryType::File;
    if (is_directory(status)) return EntryType::Directory;
    if (is_fifo(status)) return EntryType::Fifo;
    if (is_block_file(status) || is_character_file(status)) {
        return EntryType::Device;
    }
    if (is_socket(status)) return EntryType::Socket;
    return EntryType::Other;
}

}  // namespace

std::vector<FileEntry> scan_tree(const std::filesystem::path& root,
                                 bool include_directories,
                                 bool include_links) {
    std::vector<FileEntry> entries;

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return entries;
    }

    const auto options =
        std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator it(root, options, ec);
    const std::filesystem::recursive_directory_iterator end;

    while (it != end) {
        const std::filesystem::directory_entry& item = *it;

        std::error_code item_ec;
        const EntryType type = classify(item, item_ec);

        bool keep = true;
        if (type == EntryType::Directory && !include_directories) keep = false;
        if (is_link(type) && !include_links) keep = false;

        if (keep) {
            FileEntry entry;
            entry.type = type;

            std::error_code rel_ec;
            // 用 u8 版本：generic_string() 按本地代码页转换，中文会抛异常。
            entry.relative_path =
                std::filesystem::relative(item.path(), root, rel_ec)
                    .generic_u8string();

            if (type == EntryType::File) {
                std::error_code size_ec;
                entry.size = item.file_size(size_ec);
            } else if (type == EntryType::Symlink) {
                std::error_code link_ec;
                entry.link_target =
                    std::filesystem::read_symlink(item.path(), link_ec)
                        .generic_u8string();
            }

            if (!rel_ec) entries.push_back(std::move(entry));
        }

        std::error_code inc_ec;
        it.increment(inc_ec);
        if (inc_ec) break;
    }

    std::sort(entries.begin(), entries.end(),
              [](const FileEntry& a, const FileEntry& b) {
                  return a.relative_path < b.relative_path;
              });
    return entries;
}

std::vector<FileEntry> list_files(const std::filesystem::path& root) {
    std::vector<FileEntry> files = scan_tree(root, false, false);
    files.erase(std::remove_if(files.begin(), files.end(),
                               [](const FileEntry& e) {
                                   return e.type != EntryType::File;
                               }),
                files.end());
    // 扫描函数已按路径排序；过滤不会破坏顺序。
    return files;
}

std::filesystem::path path_from_u8(const std::string& s) {
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
#else
    return std::filesystem::u8path(s);
#endif
}

}  // namespace backup
