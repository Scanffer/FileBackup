#include "pack.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "algorithm.h"
#include "fs_utils.h"

namespace backup {
namespace {

constexpr char kMagic[4] = {'V', 'B', 'A', 'K'};
constexpr std::uint16_t kFormatVersion = 2;
constexpr std::uint32_t kHeaderSizeV2 = 24;
constexpr std::uint32_t kHeaderSizeV1 = 20;

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

Bytes read_exact(std::istream& in, std::uint64_t size, const char* what) {
    if (size > 4ull * 1024 * 1024 * 1024) {
        throw std::runtime_error(std::string("归档声明的大小过大：") + what);
    }
    Bytes data(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(data.data()),
                static_cast<std::streamsize>(size));
        if (in.gcount() != static_cast<std::streamsize>(size)) {
            throw std::runtime_error(std::string("归档数据不完整：") + what);
        }
    }
    return data;
}

Bytes read_file(const std::filesystem::path& path,
                const std::string& relative) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法读取：" + relative);
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) throw std::runtime_error("无法读取：" + relative);
    in.seekg(0, std::ios::beg);
    Bytes data(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(data.data()),
                static_cast<std::streamsize>(size));
        if (in.gcount() != static_cast<std::streamsize>(size)) {
            throw std::runtime_error("读取时文件大小变化：" + relative);
        }
    }
    return data;
}

/// 检查归档条目里的相对路径，拒绝绝对路径和 ".."（防 zip-slip）。
std::filesystem::path safe_relative_path(const std::string& rel) {
    if (rel.empty()) throw std::runtime_error("归档包含空路径");
    const std::filesystem::path path = path_from_u8(rel);
    if (path.is_absolute()) {
        throw std::runtime_error("归档包含绝对路径：" + rel);
    }
    for (const auto& part : path) {
        if (part == "..") {
            throw std::runtime_error("归档包含非法路径（..）：" + rel);
        }
    }
    return path;
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

    std::unique_ptr<ICompressor> compressor;
    if (opts.compression != CompressionId::None) {
        compressor = make_compressor(opts.compression);
        if (!compressor) {
            throw std::runtime_error(std::string("压缩算法尚未实现：") +
                                     to_string(opts.compression));
        }
    }
    const int level =
        clamp_compression_level(opts.compression, opts.compression_level);

    const std::vector<FileEntry> files = list_files(opts.src);

    std::ofstream out(opts.archive, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("无法创建归档文件");
    }

    // ---- Header (v2) ----
    out.write(kMagic, 4);
    write_u16(out, kFormatVersion);
    write_u16(out, 0);  // flags：加密等后续版本启用
    out.put(static_cast<char>(opts.compression));
    out.put('\0');
    out.put('\0');
    out.put('\0');
    write_u32(out, static_cast<std::uint32_t>(files.size()));
    write_u32(out, kHeaderSizeV2);

    if (!out) throw std::runtime_error("写归档头失败");

    PackResult result;
    for (const FileEntry& entry : files) {
        const std::filesystem::path from =
            opts.src / path_from_u8(entry.relative_path);

        if (entry.relative_path.size() > 0xFFFF) {
            throw std::runtime_error("路径过长：" + entry.relative_path);
        }

        Bytes original = read_file(from, entry.relative_path);
        const std::uint32_t checksum =
            crc32(original.data(), original.size());

        std::uint8_t stored_comp = 0;
        Bytes stored = original;
        if (compressor && !original.empty()) {
            std::string compress_error;
            Bytes compressed =
                compressor->compress(original, level, compress_error);
            if (!compress_error.empty()) {
                throw std::runtime_error("压缩失败：" +
                                         entry.relative_path + "：" +
                                         compress_error);
            }
            // 压不动就存原数据，别让归档反而变大。
            if (compressed.size() < original.size()) {
                stored = std::move(compressed);
                stored_comp = static_cast<std::uint8_t>(opts.compression);
            }
        }

        // ---- Entry header ----
        write_u16(out, static_cast<std::uint16_t>(entry.relative_path.size()));
        out.write(entry.relative_path.data(),
                  static_cast<std::streamsize>(entry.relative_path.size()));
        write_u64(out, static_cast<std::uint64_t>(original.size()));
        write_u64(out, static_cast<std::uint64_t>(stored.size()));
        write_u32(out, checksum);
        out.put(static_cast<char>(stored_comp));

        if (!out) {
            throw std::runtime_error("写条目头失败：" + entry.relative_path);
        }

        if (!stored.empty()) {
            out.write(reinterpret_cast<const char*>(stored.data()),
                      static_cast<std::streamsize>(stored.size()));
        }
        if (!out) {
            throw std::runtime_error("写数据失败：" + entry.relative_path);
        }

        ++result.entries_written;
        result.bytes_written += original.size();
    }

    if (!out) throw std::runtime_error("写归档失败");
    return result;
}

// ------------------------------------------------------------------
// 解包
// ------------------------------------------------------------------

UnpackResult unpack_archive(const UnpackOptions& opts) {
    std::ifstream in(opts.archive, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开归档文件");

    in.seekg(0, std::ios::end);
    const std::uint64_t archive_size =
        static_cast<std::uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);

    // ---- Header ----
    char magic[4];
    in.read(magic, 4);
    if (!in || std::memcmp(magic, kMagic, 4) != 0) {
        throw std::runtime_error("不是 VBAK 归档（magic 不匹配）");
    }

    const std::uint16_t version = read_u16(in);
    if (version != 1 && version != 2) {
        throw std::runtime_error("不支持的归档版本：" +
                                 std::to_string(version));
    }
    (void)read_u16(in);  // flags

    if (version == 2) {
        (void)in.get();  // 归档默认 comp_id（条目里另有记录）
        (void)in.get();  // reserved
        (void)in.get();  // reserved
        (void)in.get();  // reserved
    }
    const std::uint32_t entry_count = read_u32(in);
    if (version == 1) {
        (void)read_u32(in);  // reserved
    }
    (void)read_u32(in);  // data_off

    if (!in) throw std::runtime_error("归档头损坏");

    std::error_code ec;
    std::filesystem::create_directories(opts.dst, ec);
    if (ec) throw std::runtime_error("无法创建目标目录：" + ec.message());

    UnpackResult result;

    for (std::uint32_t i = 0; i < entry_count; ++i) {
        const std::uint16_t path_len = read_u16(in);
        if (!in || path_len == 0) {
            throw std::runtime_error("归档损坏（读路径长度失败）");
        }

        std::string rel(path_len, '\0');
        in.read(rel.data(), static_cast<std::streamsize>(path_len));

        std::uint64_t orig_size = 0;
        std::uint64_t stored_size = 0;
        std::uint32_t checksum = 0;
        std::uint8_t entry_comp = 0;

        if (version == 2) {
            orig_size = read_u64(in);
            stored_size = read_u64(in);
            checksum = read_u32(in);
            entry_comp = static_cast<std::uint8_t>(in.get());
        } else {
            orig_size = read_u64(in);
            stored_size = orig_size;
            (void)read_u32(in);  // v1 的 checksum 占位
        }

        if (!in) throw std::runtime_error("归档损坏（读条目头失败）");
        if (stored_size > archive_size) {
            throw std::runtime_error("归档损坏（条目大小异常）：" + rel);
        }

        const std::filesystem::path rel_path = safe_relative_path(rel);
        const std::filesystem::path to = opts.dst / rel_path;

        const Bytes stored = read_exact(in, stored_size, rel.c_str());

        if (!opts.overwrite && std::filesystem::exists(to, ec)) {
            ++result.entries_read;
            result.bytes_read += orig_size;
            continue;
        }

        Bytes data;
        if (entry_comp != 0) {
            auto compressor = make_compressor(
                static_cast<CompressionId>(entry_comp));
            if (!compressor) {
                throw std::runtime_error(
                    std::string("归档使用了未知压缩算法：") +
                    std::to_string(entry_comp));
            }
            std::string decompress_error;
            data = compressor->decompress(
                stored, static_cast<std::size_t>(orig_size),
                decompress_error);
            if (!decompress_error.empty()) {
                throw std::runtime_error("解压失败：" + rel + "：" +
                                         decompress_error);
            }
        } else {
            if (stored.size() != orig_size) {
                throw std::runtime_error("归档条目大小不一致：" + rel);
            }
            data = stored;
        }

        if (version == 2) {
            const std::uint32_t actual = crc32(data.data(), data.size());
            if (actual != checksum) {
                throw std::runtime_error("CRC32 校验失败：" + rel);
            }
        }

        std::filesystem::create_directories(to.parent_path(), ec);
        if (ec) {
            throw std::runtime_error("无法创建目录：" +
                                     to.parent_path().u8string() + "：" +
                                     ec.message());
        }

        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("无法写入：" + rel);
        if (!data.empty()) {
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
        }
        if (!out) throw std::runtime_error("写文件失败：" + rel);

        ++result.entries_read;
        result.bytes_read += orig_size;
    }

    return result;
}

}  // namespace backup
