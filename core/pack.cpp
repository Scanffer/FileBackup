#include "pack.h"
#include "fs_utils.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace backup {
namespace {

constexpr char kMagic[4] = {'V', 'B', 'A', 'K'};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::uint32_t kHeaderSize = 20;
constexpr std::size_t kCopyBufferSize = 64 * 1024;

// ---- 小端读写 ----
// 归档格式定死小端；不依赖主机字节序，跨平台读得出来。

void write_u16(std::ostream& os, std::uint16_t v) {
    char b[2] = {char(v & 0xFF), char((v >> 8) & 0xFF)};
    os.write(b, 2);
}

void write_u32(std::ostream& os, std::uint32_t v) {
    char b[4];
    for (int i = 0; i < 4; ++i) b[i] = char((v >> (8 * i)) & 0xFF);
    os.write(b, 4);
}

void write_u64(std::ostream& os, std::uint64_t v) {
    char b[8];
    for (int i = 0; i < 8; ++i) b[i] = char((v >> (8 * i)) & 0xFF);
    os.write(b, 8);
}

std::uint16_t read_u16(std::istream& is) {
    unsigned char b[2];
    is.read(reinterpret_cast<char*>(b), 2);
    return std::uint16_t(b[0]) | (std::uint16_t(b[1]) << 8);
}

std::uint32_t read_u32(std::istream& is) {
    unsigned char b[4];
    is.read(reinterpret_cast<char*>(b), 4);
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::uint32_t(b[i]) << (8 * i);
    return v;
}

std::uint64_t read_u64(std::istream& is) {
    unsigned char b[8];
    is.read(reinterpret_cast<char*>(b), 8);
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= std::uint64_t(b[i]) << (8 * i);
    return v;
}

/// UTF-8 std::string -> std::filesystem::path。
/// 同 backup.cpp：MinGW 下窄串构造会按 GBK 解释，中文崩，必须走 u8 通道。
std::filesystem::path path_from_u8(const std::string& s) {
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201907L
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
#else
    return std::filesystem::u8path(s);
#endif
}

}  // namespace

// ------------------------------------------------------------------
// 打包
// ------------------------------------------------------------------

PackResult pack_directory(const PackOptions& opts) {
    std::error_code ec;
    if (!std::filesystem::is_directory(opts.src, ec)) {
        throw std::runtime_error("源目录不存在或不是目录");
    }

    // 复用 fs_utils，保证和镜像模式同一套遍历、同一套排序。
    const std::vector<FileEntry> files = list_files(opts.src);

    std::ofstream out(opts.archive, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("无法创建归档文件");
    }

    // ---- Header ----
    out.write(kMagic, 4);
    write_u16(out, kFormatVersion);
    write_u16(out, 0);  // flags：M2/M3 再填
    write_u32(out, static_cast<std::uint32_t>(files.size()));
    write_u32(out, 0);            // reserved
    write_u32(out, kHeaderSize);  // data_off

    if (!out) {
        throw std::runtime_error("写归档头失败");
    }

    PackResult result;
    std::vector<char> buf(kCopyBufferSize);

    for (const FileEntry& entry : files) {
        const std::filesystem::path from =
            opts.src / path_from_u8(entry.relative_path);

        if (entry.relative_path.size() > 0xFFFF) {
            throw std::runtime_error("路径过长：" + entry.relative_path);
        }

        // ---- Entry header ----
        write_u16(out, static_cast<std::uint16_t>(entry.relative_path.size()));
        out.write(entry.relative_path.data(),
                  static_cast<std::streamsize>(entry.relative_path.size()));
        write_u64(out, entry.size);
        write_u32(out, 0);  // checksum：M4 启用

        if (!out) {
            throw std::runtime_error("写条目头失败：" + entry.relative_path);
        }

        // ---- Data：逐块拷贝 ----
        std::ifstream in(from, std::ios::binary);
        if (!in) {
            throw std::runtime_error("无法读取：" + entry.relative_path);
        }

        std::uint64_t written = 0;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize n = in.gcount();
            if (n > 0) {
                out.write(buf.data(), n);
                written += static_cast<std::uint64_t>(n);
            }
        }

        if (!out) {
            throw std::runtime_error("写数据失败：" + entry.relative_path);
        }
        if (written != entry.size) {
            throw std::runtime_error("读取时文件大小变化：" +
                                     entry.relative_path);
        }

        ++result.entries_written;
        result.bytes_written += written;
    }

    if (!out) {
        throw std::runtime_error("写归档失败");
    }
    return result;
}

// ------------------------------------------------------------------
// 解包
// ------------------------------------------------------------------

UnpackResult unpack_archive(const UnpackOptions& opts) {
    std::ifstream in(opts.archive, std::ios::binary);
    if (!in) {
        throw std::runtime_error("无法打开归档文件");
    }

    // ---- Header ----
    char magic[4];
    in.read(magic, 4);
    if (!in || std::memcmp(magic, kMagic, 4) != 0) {
        throw std::runtime_error("不是 VBAK 归档（magic 不匹配）");
    }

    const std::uint16_t version = read_u16(in);
    if (version != kFormatVersion) {
        throw std::runtime_error("不支持的归档版本：" +
                                 std::to_string(version));
    }
    (void)read_u16(in);  // flags
    const std::uint32_t entry_count = read_u32(in);
    (void)read_u32(in);  // reserved
    (void)read_u32(in);  // data_off

    if (!in) {
        throw std::runtime_error("归档头损坏");
    }

    std::error_code ec;
    std::filesystem::create_directories(opts.dst, ec);
    if (ec) {
        throw std::runtime_error("无法创建目标目录：" + ec.message());
    }

    UnpackResult result;
    std::vector<char> buf(kCopyBufferSize);

    for (std::uint32_t i = 0; i < entry_count; ++i) {
        // ---- Entry header ----
        const std::uint16_t path_len = read_u16(in);
        if (!in) {
            throw std::runtime_error("归档损坏（读路径长度失败）");
        }

        std::string rel(path_len, '\0');
        in.read(rel.data(), static_cast<std::streamsize>(path_len));
        const std::uint64_t data_size = read_u64(in);
        (void)read_u32(in);  // checksum

        if (!in) {
            throw std::runtime_error("归档损坏（读条目头失败）");
        }

        // ---- 路径安全检查（zip-slip 防御）----
        const std::filesystem::path rel_path = path_from_u8(rel);
        if (rel_path.is_absolute()) {
            throw std::runtime_error("归档包含绝对路径：" + rel);
        }
        for (const auto& part : rel_path) {
            if (part == "..") {
                throw std::runtime_error("归档包含非法路径（..）：" + rel);
            }
        }

        const std::filesystem::path to = opts.dst / rel_path;

        // ---- 跳过逻辑 ----
        if (!opts.overwrite && std::filesystem::exists(to, ec)) {
            // 跳过前必须把数据段读掉，否则下一条 entry 偏移错位。
            in.seekg(static_cast<std::streamoff>(data_size), std::ios::cur);
            if (!in) {
                throw std::runtime_error("归档损坏（跳过数据失败）：" + rel);
            }
            ++result.entries_read;
            result.bytes_read += data_size;
            continue;
        }

        std::filesystem::create_directories(to.parent_path(), ec);
        if (ec) {
            throw std::runtime_error("无法创建目录：" +
                                     to.parent_path().u8string() + "：" +
                                     ec.message());
        }

        // ---- 写文件 ----
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("无法写入：" + rel);
        }

        std::uint64_t remaining = data_size;
        while (remaining > 0) {
            const std::streamsize want = static_cast<std::streamsize>(
                std::min<std::uint64_t>(remaining, buf.size()));
            in.read(buf.data(), want);
            if (in.gcount() != want) {
                throw std::runtime_error("归档数据不完整：" + rel);
            }
            out.write(buf.data(), want);
            if (!out) {
                throw std::runtime_error("写文件失败：" + rel);
            }
            remaining -= static_cast<std::uint64_t>(want);
        }

        ++result.entries_read;
        result.bytes_read += data_size;
    }

    return result;
}

}  // namespace backup