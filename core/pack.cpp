#include "pack.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "algorithm.h"
#include "fs_utils.h"
#include "metadata.h"

namespace backup {
namespace {

constexpr char kMagic[4] = {'V', 'B', 'A', 'K'};
constexpr std::uint16_t kVersion1 = 1;
constexpr std::uint16_t kVersion2 = 2;
constexpr std::uint16_t kVersion3 = 3;
constexpr std::uint16_t kVersion4 = 4;
constexpr std::uint32_t kHeaderV1 = 20;
constexpr std::uint32_t kHeaderV2 = 20;
constexpr std::uint32_t kHeaderV3 = 64;
constexpr std::uint32_t kHeaderV4 = 64;
constexpr std::uint32_t kKdfIterations = 100000;
constexpr std::uint8_t kFlagsEncrypted = 0x01;
constexpr std::uint8_t kFlagsMetadata = 0x02;

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

Bytes read_bytes(std::istream& in, std::size_t size, const char* what) {
    Bytes data(size);
    if (size > 0) {
        in.read(reinterpret_cast<char*>(data.data()),
                static_cast<std::streamsize>(size));
        if (in.gcount() != static_cast<std::streamsize>(size)) {
            throw std::runtime_error(std::string("归档数据不完整：") + what);
        }
    }
    return data;
}

Bytes read_exact(std::istream& in, std::uint64_t size, const char* what) {
    if (size > 8ull * 1024 * 1024 * 1024) {
        throw std::runtime_error(std::string("归档声明的大小过大：") + what);
    }
    return read_bytes(in, static_cast<std::size_t>(size), what);
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

/// std::filesystem 的 file_clock 没有跨实现的标准纪元转换；
/// 这里用「当前时刻」求偏移，误差在毫秒级以内，足够筛选「最近 N 天」。
std::int64_t file_mtime_ns(const std::filesystem::path& path) {
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;

    using namespace std::chrono;
    const auto file_now = std::filesystem::file_time_type::clock::now();
    const auto sys_now = system_clock::now();
    const auto offset =
        sys_now.time_since_epoch() -
        duration_cast<system_clock::duration>(file_now.time_since_epoch());
    const auto since_epoch =
        duration_cast<system_clock::duration>(ft.time_since_epoch()) + offset;
    return duration_cast<nanoseconds>(since_epoch).count();
}

std::string filename_u8(const std::string& relative_path) {
    const std::filesystem::path path = path_from_u8(relative_path);
    return path.filename().generic_u8string();
}

/// 只有规则里真的出现 user 维度时才去读属主（Windows 安全 API 很慢）。
bool filter_uses_user(const Filter& filter) {
    if (filter.empty()) return false;
    const nlohmann::json rules = filter.to_json();
    if (!rules.is_array()) return false;
    for (const auto& rule : rules) {
        if (rule.is_object() && rule.contains("user")) return true;
    }
    return false;
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

void require_password_if_encrypted(EncryptionId id,
                                   const std::string& password,
                                   const char* what) {
    if (id == EncryptionId::None) return;
    if (password.empty()) {
        throw std::runtime_error(std::string(what) + "需要密码");
    }
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
    require_password_if_encrypted(opts.encryption, opts.password, "加密打包");

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

    std::unique_ptr<ICipher> cipher;
    Bytes key;
    Bytes salt;
    if (opts.encryption != EncryptionId::None) {
        cipher = make_cipher(opts.encryption);
        if (!cipher) {
            throw std::runtime_error(std::string("加密算法尚未实现：") +
                                     to_string(opts.encryption));
        }
        salt = random_bytes(16);
        if (salt.size() != 16) {
            throw std::runtime_error("随机盐生成失败");
        }
        std::string kdf_error;
        key = derive_key(opts.password, salt, kKdfIterations,
                         cipher->key_size(), kdf_error);
        if (!kdf_error.empty() || key.size() != cipher->key_size()) {
            throw std::runtime_error("密钥派生失败：" + kdf_error);
        }
        if (cipher->iv_size() > 16) {
            throw std::runtime_error("加密算法 IV 长度超出归档格式限制");
        }
    }

    std::vector<FileEntry> files = list_files(opts.src);
    if (!opts.filter.empty()) {
        const bool needs_owner = filter_uses_user(opts.filter);
        std::vector<FileEntry> kept;
        kept.reserve(files.size());
        for (const FileEntry& entry : files) {
            const std::filesystem::path from =
                opts.src / path_from_u8(entry.relative_path);
            FilterContext ctx;
            ctx.relative_path = entry.relative_path;
            ctx.name = filename_u8(entry.relative_path);
            ctx.size = entry.size;
            ctx.mtime_ns = file_mtime_ns(from);
            ctx.type = EntryType::File;
            ctx.uid = 0;
            if (needs_owner) ctx.owner = file_owner_name(from);
            if (opts.filter.match(ctx)) kept.push_back(entry);
        }
        files = std::move(kept);
    }

    std::ofstream out(opts.archive, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("无法创建归档文件");

    const bool encrypted = opts.encryption != EncryptionId::None;
    const std::uint16_t version = kVersion4;
    std::uint8_t flags = encrypted ? kFlagsEncrypted : 0;
    if (opts.keep_metadata) flags |= kFlagsMetadata;

    // ---- Header ----
    out.write(kMagic, 4);
    write_u16(out, version);
    write_u16(out, flags);
    out.put(static_cast<char>(opts.compression));
    {
        out.put(static_cast<char>(opts.encryption));
        out.put('\0');
        out.put('\0');
        write_u32(out, static_cast<std::uint32_t>(files.size()));
        write_u32(out, kHeaderV4);
        write_u32(out, encrypted ? kKdfIterations : 0);
        std::uint8_t salt_field[16] = {};
        if (!salt.empty()) std::memcpy(salt_field, salt.data(), salt.size());
        out.write(reinterpret_cast<const char*>(salt_field), 16);
        for (int i = 0; i < 24; ++i) out.put('\0');
    }
    if (!out) throw std::runtime_error("写归档头失败");

    PackResult result;
    for (const FileEntry& entry : files) {
        const std::filesystem::path from =
            opts.src / path_from_u8(entry.relative_path);

        if (entry.relative_path.size() > 0xFFFF) {
            throw std::runtime_error("路径过长：" + entry.relative_path);
        }

        Bytes metadata_bytes;
        if (opts.keep_metadata) {
            std::string meta_error;
            const auto meta = read_metadata(from, meta_error);
            if (meta) metadata_bytes = metadata_to_bytes(*meta);
        }

        const Bytes original = read_file(from, entry.relative_path);
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
            if (compressed.size() < original.size()) {
                stored = std::move(compressed);
                stored_comp = static_cast<std::uint8_t>(opts.compression);
            }
        }

        Bytes iv;
        if (cipher) {
            iv = random_bytes(cipher->iv_size());
            if (iv.size() != cipher->iv_size()) {
                throw std::runtime_error("随机 IV 生成失败");
            }
            std::string encrypt_error;
            stored = cipher->encrypt(stored, key, iv, encrypt_error);
            if (!encrypt_error.empty()) {
                throw std::runtime_error("加密失败：" +
                                         entry.relative_path + "：" +
                                         encrypt_error);
            }
        }

        // ---- Entry ----
        write_u16(out, static_cast<std::uint16_t>(entry.relative_path.size()));
        out.write(entry.relative_path.data(),
                  static_cast<std::streamsize>(entry.relative_path.size()));
        write_u64(out, static_cast<std::uint64_t>(original.size()));
        write_u64(out, static_cast<std::uint64_t>(stored.size()));
        write_u32(out, checksum);
        out.put(static_cast<char>(stored_comp));
        if (encrypted) {
            std::uint8_t iv_field[16] = {};
            if (!iv.empty()) {
                std::memcpy(iv_field, iv.data(), iv.size());
            }
            out.write(reinterpret_cast<const char*>(iv_field), 16);
        }
        if (version == kVersion4) {
            write_u32(out, static_cast<std::uint32_t>(metadata_bytes.size()));
            if (!metadata_bytes.empty()) {
                out.write(
                    reinterpret_cast<const char*>(metadata_bytes.data()),
                    static_cast<std::streamsize>(metadata_bytes.size()));
            }
        }
        if (!stored.empty()) {
            out.write(reinterpret_cast<const char*>(stored.data()),
                      static_cast<std::streamsize>(stored.size()));
        }
        if (!out) {
            throw std::runtime_error("写条目失败：" + entry.relative_path);
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

    char magic[4];
    in.read(magic, 4);
    if (!in || std::memcmp(magic, kMagic, 4) != 0) {
        throw std::runtime_error("不是 VBAK 归档（magic 不匹配）");
    }

    const std::uint16_t version = read_u16(in);
    if (version != kVersion1 && version != kVersion2 && version != kVersion3 &&
        version != kVersion4) {
        throw std::runtime_error("不支持的归档版本：" +
                                 std::to_string(version));
    }
    const std::uint16_t flags = read_u16(in);
    (void)flags;  // bit0/bit1 目前仅作记录，实际以条目字段为准

    std::uint8_t comp_id_header = 0;
    std::uint8_t enc_id_header = 0;
    std::uint8_t salt_bytes[16] = {};
    std::uint32_t kdf_iterations = 0;

    if (version >= kVersion3) {
        comp_id_header = static_cast<std::uint8_t>(in.get());
        enc_id_header = static_cast<std::uint8_t>(in.get());
        (void)in.get();  // reserved
        (void)in.get();  // reserved
    } else if (version == kVersion2) {
        comp_id_header = static_cast<std::uint8_t>(in.get());
        (void)in.get();
        (void)in.get();
        (void)in.get();
    }
    (void)comp_id_header;  // 条目里另有记录，这里只做展示

    const std::uint32_t entry_count = read_u32(in);
    if (version == kVersion1) (void)read_u32(in);  // reserved
    (void)read_u32(in);                            // data_off

    if (version >= kVersion3) {
        kdf_iterations = read_u32(in);
        in.read(reinterpret_cast<char*>(salt_bytes), 16);
        for (int i = 0; i < 24; ++i) (void)in.get();
    }
    if (!in) throw std::runtime_error("归档头损坏");

    std::unique_ptr<ICipher> cipher;
    Bytes key;
    if (version >= kVersion3 && enc_id_header != 0) {
        if (opts.password.empty()) {
            throw std::runtime_error("归档已加密，需要密码");
        }
        if (kdf_iterations == 0 || kdf_iterations > 10000000) {
            throw std::runtime_error("归档的 KDF 迭代次数非法");
        }
        cipher = make_cipher(static_cast<EncryptionId>(enc_id_header));
        if (!cipher) {
            throw std::runtime_error(
                std::string("归档使用了未知加密算法：") +
                std::to_string(enc_id_header));
        }
        if (cipher->iv_size() > 16) {
            throw std::runtime_error("归档的 IV 长度超出格式限制");
        }
        const Bytes salt(salt_bytes, salt_bytes + 16);
        std::string kdf_error;
        key = derive_key(opts.password, salt, kdf_iterations,
                         cipher->key_size(), kdf_error);
        if (!kdf_error.empty() || key.size() != cipher->key_size()) {
            throw std::runtime_error("密钥派生失败：" + kdf_error);
        }
    }

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
        std::uint8_t iv_field[16] = {};
        std::uint32_t meta_len = 0;

        if (version >= kVersion2) {
            orig_size = read_u64(in);
            stored_size = read_u64(in);
            checksum = read_u32(in);
            entry_comp = static_cast<std::uint8_t>(in.get());
            if (version >= kVersion3 && enc_id_header != 0) {
                in.read(reinterpret_cast<char*>(iv_field), 16);
            }
        } else {
            orig_size = read_u64(in);
            stored_size = orig_size;
            (void)read_u32(in);  // v1 的 checksum 占位
        }

        if (!in) throw std::runtime_error("归档损坏（读条目头失败）");
        if (stored_size > archive_size) {
            throw std::runtime_error("归档损坏（条目大小异常）：" + rel);
        }

        Bytes metadata_bytes;
        if (version == kVersion4) {
            meta_len = read_u32(in);
            if (!in || meta_len > archive_size) {
                throw std::runtime_error("归档损坏（元数据长度异常）：" + rel);
            }
            if (meta_len > 0) {
                metadata_bytes = read_bytes(in, meta_len, "metadata");
            }
        }

        const std::filesystem::path rel_path = safe_relative_path(rel);
        const std::filesystem::path to = opts.dst / rel_path;

        Bytes stored = read_exact(in, stored_size, rel.c_str());

        if (cipher) {
            const Bytes iv(iv_field, iv_field + cipher->iv_size());
            std::string decrypt_error;
            stored = cipher->decrypt(stored, key, iv, decrypt_error);
            if (!decrypt_error.empty()) {
                throw std::runtime_error("解密失败（密码错误或归档被篡改）：" +
                                         rel + "：" + decrypt_error);
            }
        }

        Bytes data;
        if (entry_comp != 0) {
            auto compressor =
                make_compressor(static_cast<CompressionId>(entry_comp));
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
            data = std::move(stored);
        }

        if (version >= kVersion2) {
            const std::uint32_t actual = crc32(data.data(), data.size());
            if (actual != checksum) {
                throw std::runtime_error("CRC32 校验失败：" + rel);
            }
        }

        if (!opts.overwrite && std::filesystem::exists(to, ec)) {
            ++result.entries_read;
            result.bytes_read += orig_size;
            continue;
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
        out.close();
        if (!out) throw std::runtime_error("写文件失败：" + rel);

        if (opts.restore_metadata && !metadata_bytes.empty()) {
            std::string meta_error;
            const auto meta = metadata_from_bytes(metadata_bytes, meta_error);
            if (meta) {
                std::string apply_error;
                // 权限不足/跨机 ACL 等失败只降级为忽略，不能中断还原。
                (void)apply_metadata(to, *meta, apply_error);
            }
        }

        ++result.entries_read;
        result.bytes_read += orig_size;
    }

    return result;
}

}  // namespace backup
