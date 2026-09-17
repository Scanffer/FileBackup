#include "fs_utils.h"

#include <algorithm>
#include <system_error>

namespace backup {

std::vector<FileEntry> list_files(const std::filesystem::path& root) {
    std::vector<FileEntry> files;

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return files;
    }

    const auto options =
        std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator it(root, options, ec);
    const std::filesystem::recursive_directory_iterator end;

    while (it != end) {
        const std::filesystem::directory_entry& item = *it;
        if (item.is_regular_file(ec)) {
            FileEntry entry;
            // 用 u8 版本：generic_string() 按本地代码页转换，中文会抛异常。
            entry.relative_path =
                std::filesystem::relative(item.path(), root, ec)
                    .generic_u8string();
            entry.size = item.file_size(ec);
            files.push_back(std::move(entry));
        }
        it.increment(ec);
    }

    std::sort(files.begin(), files.end(),
              [](const FileEntry& a, const FileEntry& b) {
                  return a.relative_path < b.relative_path;
              });
    return files;
}

}  // namespace backup
