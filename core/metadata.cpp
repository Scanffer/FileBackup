#include "metadata.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#endif

namespace backup {
namespace {

// ===========================================================================
// Base64（元数据 JSON 里的 ACL 用）
// ===========================================================================

const char* const kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const Bytes& data) {
    std::string encoded;
    encoded.reserve((data.size() + 2) / 3 * 4);
    std::size_t index = 0;
    while (index + 2 < data.size()) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(data[index]) << 16) |
            (static_cast<std::uint32_t>(data[index + 1]) << 8) |
            static_cast<std::uint32_t>(data[index + 2]);
        encoded.push_back(kBase64Alphabet[(triple >> 18) & 0x3Fu]);
        encoded.push_back(kBase64Alphabet[(triple >> 12) & 0x3Fu]);
        encoded.push_back(kBase64Alphabet[(triple >> 6) & 0x3Fu]);
        encoded.push_back(kBase64Alphabet[triple & 0x3Fu]);
        index += 3;
    }
    const std::size_t remaining = data.size() - index;
    if (remaining == 1) {
        const std::uint32_t triple = static_cast<std::uint32_t>(data[index]) << 16;
        encoded.push_back(kBase64Alphabet[(triple >> 18) & 0x3Fu]);
        encoded.push_back(kBase64Alphabet[(triple >> 12) & 0x3Fu]);
        encoded.push_back('=');
        encoded.push_back('=');
    } else if (remaining == 2) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(data[index]) << 16) |
            (static_cast<std::uint32_t>(data[index + 1]) << 8);
        encoded.push_back(kBase64Alphabet[(triple >> 18) & 0x3Fu]);
        encoded.push_back(kBase64Alphabet[(triple >> 12) & 0x3Fu]);
        encoded.push_back(kBase64Alphabet[(triple >> 6) & 0x3Fu]);
        encoded.push_back('=');
    }
    return encoded;
}

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::optional<Bytes> base64_decode(const std::string& text) {
    Bytes decoded;
    decoded.reserve(text.size() / 4 * 3 + 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t sextets = 0;
    std::size_t padding = 0;
    for (char c : text) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        if (c == '=') {
            ++padding;
            continue;
        }
        if (padding != 0) return std::nullopt;  // '=' 之后不允许再出现数据
        const int value = base64_value(c);
        if (value < 0) return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        ++sextets;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFFu));
        }
    }
    if (padding > 2) return std::nullopt;
    if (padding == 0) {
        // 允许省略 '='，但 4n+1 个字符无法解码。
        if (sextets % 4 == 1) return std::nullopt;
    } else if ((sextets + padding) % 4 != 0) {
        return std::nullopt;
    }
    return decoded;
}

// ===========================================================================
// 小端二进制读写
// ===========================================================================

void append_u8(Bytes& out, std::uint8_t value) { out.push_back(value); }

void append_u16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void append_u32(Bytes& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void append_u64(Bytes& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void append_i64(Bytes& out, std::int64_t value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "int64 与 uint64 的位宽必须一致");
    std::memcpy(&bits, &value, sizeof(bits));
    append_u64(out, bits);
}

void append_string(Bytes& out, const std::string& text) {
    append_u32(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

void append_bytes(Bytes& out, const Bytes& data) {
    append_u32(out, static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
}

class ByteReader {
public:
    explicit ByteReader(const Bytes& data) : data_(data) {}

    bool failed() const { return failed_; }
    std::size_t position() const { return position_; }
    std::size_t remaining() const {
        return failed_ ? 0 : data_.size() - position_;
    }

    bool read_u8(std::uint8_t& out) {
        if (!require(1)) return false;
        out = data_[position_++];
        return true;
    }

    bool read_u16(std::uint16_t& out) {
        if (!require(2)) return false;
        out = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(data_[position_]) |
            (static_cast<std::uint16_t>(data_[position_ + 1]) << 8));
        position_ += 2;
        return true;
    }

    bool read_u32(std::uint32_t& out) {
        if (!require(4)) return false;
        out = 0;
        for (int index = 0; index < 4; ++index) {
            out |= static_cast<std::uint32_t>(data_[position_ + index])
                   << (index * 8);
        }
        position_ += 4;
        return true;
    }

    bool read_u64(std::uint64_t& out) {
        if (!require(8)) return false;
        out = 0;
        for (int index = 0; index < 8; ++index) {
            out |= static_cast<std::uint64_t>(data_[position_ + index])
                   << (index * 8);
        }
        position_ += 8;
        return true;
    }

    bool read_i64(std::int64_t& out) {
        std::uint64_t bits = 0;
        if (!read_u64(bits)) return false;
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    bool read_raw(std::size_t length, Bytes& out) {
        if (!require(length)) return false;
        out.assign(data_.begin() + static_cast<std::ptrdiff_t>(position_),
                   data_.begin() + static_cast<std::ptrdiff_t>(position_ + length));
        position_ += length;
        return true;
    }

    bool read_string(std::string& out) {
        std::uint32_t length = 0;
        if (!read_u32(length)) return false;
        if (static_cast<std::size_t>(length) > remaining()) return false;
        out.assign(reinterpret_cast<const char*>(data_.data()) + position_, length);
        position_ += length;
        return true;
    }

    bool read_bytes(Bytes& out) {
        std::uint32_t length = 0;
        if (!read_u32(length)) return false;
        return read_raw(static_cast<std::size_t>(length), out);
    }

    bool read_magic(std::uint8_t (&magic)[4]) {
        if (!require(4)) return false;
        for (int index = 0; index < 4; ++index) {
            magic[index] = data_[position_ + static_cast<std::size_t>(index)];
        }
        position_ += 4;
        return true;
    }

private:
    bool require(std::size_t length) {
        if (failed_) return false;
        if (length > data_.size() - position_) {
            failed_ = true;
            return false;
        }
        return true;
    }

    const Bytes& data_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

// ===========================================================================
// 二进制格式（version 1）
//
//   offset  size  字段
//   0       4     magic "META"
//   4       1     version = 1
//   5       1     reserved = 0
//   6       2     reserved = 0
//   8       4     mode        (u32)
//   12      4     uid         (u32)
//   16      4     gid         (u32)
//   20      4     win_attrs   (u32, FILE_ATTRIBUTE_*)
//   24      8     mtime_ns    (i64)
//   32      8     atime_ns    (i64)
//   40      8     ctime_ns    (i64)
//   48      4     owner_len   (u32) + owner UTF-8
//   ...           group_len   (u32) + group UTF-8
//   ...           acl_len     (u32) + ACL 自相对安全描述符字节
//
// 小端；所有长度/偏移在读侧做边界检查，版本不匹配或多余字节直接拒绝。
// ===========================================================================

const std::uint8_t kMetadataMagic[4] = {'M', 'E', 'T', 'A'};
const std::uint8_t kMetadataVersion = 1;
constexpr std::size_t kMetadataFixedSize = 48;

// ===========================================================================
// Windows 实现
// ===========================================================================

#if defined(_WIN32)

constexpr std::int64_t kFiletimeEpochOffset = 116444736000000000LL;

std::string utf8_from_wide(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide_from_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size);
    return result;
}

/// 转成宽字符路径；过长时加 `\\?\` 前缀（GetFileSecurity 等 API 对长度敏感）。
std::wstring native_path(const std::filesystem::path& path) {
    std::wstring wide = path.wstring();
    if (wide.rfind(L"\\\\?\\", 0) == 0) return wide;
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (!ec) wide = absolute.wstring();
    if (wide.size() < MAX_PATH) return wide;
    if (wide.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + wide.substr(2);
    return L"\\\\?\\" + wide;
}

std::string win_error_text(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text;
    if (length != 0 && buffer != nullptr) {
        text = utf8_from_wide(std::wstring(buffer, length));
        while (!text.empty() &&
               (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
            text.pop_back();
        }
    } else {
        text = "Win32 错误码 " + std::to_string(code);
    }
    if (buffer != nullptr) LocalFree(buffer);
    return text;
}

std::string path_for_error(const std::filesystem::path& path) {
    return utf8_from_wide(path.wstring());
}

/// FILETIME(1601-01-01 起的 100ns) -> Unix epoch 纳秒。
std::int64_t filetime_to_ns(const FILETIME& file_time) {
    ULARGE_INTEGER value;
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    const std::uint64_t ticks = value.QuadPart;
    if (ticks >= static_cast<std::uint64_t>(kFiletimeEpochOffset)) {
        const std::uint64_t delta =
            ticks - static_cast<std::uint64_t>(kFiletimeEpochOffset);
        constexpr std::uint64_t kMaxTicks =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 100);
        if (delta > kMaxTicks) return std::numeric_limits<std::int64_t>::max();
        return static_cast<std::int64_t>(delta) * 100;
    }
    const std::uint64_t delta =
        static_cast<std::uint64_t>(kFiletimeEpochOffset) - ticks;
    constexpr std::uint64_t kMaxTicks =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 100);
    if (delta > kMaxTicks) return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t>(delta) * 100;
}

/// Unix epoch 纳秒 -> FILETIME。超出 Windows 可表示范围返回 false。
bool ns_to_filetime(std::int64_t nanoseconds, FILETIME& out_file_time) {
    std::int64_t ticks = 0;
    if (nanoseconds >= 0) {
        ticks = nanoseconds / 100;
    } else {
        ticks = -(((-nanoseconds) + 99) / 100);  // 向负无穷取整
    }
    const std::int64_t total = ticks + kFiletimeEpochOffset;
    if (total < 0) return false;
    ULARGE_INTEGER value;
    value.QuadPart = static_cast<std::uint64_t>(total);
    out_file_time.dwLowDateTime = value.LowPart;
    out_file_time.dwHighDateTime = value.HighPart;
    return true;
}

/// SID -> `DOMAIN\account`（无域则只有账户名）；失败返回空串。
std::string sid_to_account_name(PSID sid) {
    if (sid == nullptr) return {};
    DWORD name_length = 0;
    DWORD domain_length = 0;
    SID_NAME_USE use = SidTypeUnknown;
    LookupAccountSidW(nullptr, sid, nullptr, &name_length, nullptr, &domain_length,
                      &use);
    if (name_length == 0) return {};
    std::wstring name(name_length, L'\0');
    std::wstring domain(domain_length == 0 ? 1 : domain_length, L'\0');
    DWORD name_length_second = name_length;
    DWORD domain_length_second = static_cast<DWORD>(domain.size());
    if (!LookupAccountSidW(nullptr, sid, name.data(), &name_length_second,
                           domain.data(), &domain_length_second, &use)) {
        return {};
    }
    name.resize(name_length_second);
    if (domain_length_second > 0) {
        domain.resize(domain_length_second);
    } else {
        domain.clear();
    }
    const std::string account = utf8_from_wide(name);
    if (account.empty()) return {};
    const std::string domain_text = utf8_from_wide(domain);
    if (domain_text.empty()) return account;
    return domain_text + "\\" + account;
}

/// GetNamedSecurityInfoW 等 API 的历史签名要求 LPWSTR，但不会修改内容。
LPWSTR mutable_path(const std::wstring& native) {
    return const_cast<LPWSTR>(native.c_str());
}

/// 读取属主/属组；失败时保持空串（best-effort）。
void query_owner_group(const std::wstring& native, std::string& owner,
                       std::string& group) {
    PSID owner_sid = nullptr;
    PSID group_sid = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetNamedSecurityInfoW(
        mutable_path(native), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION, &owner_sid,
        &group_sid, nullptr, nullptr, &descriptor);
    if (result != ERROR_SUCCESS) return;
    owner = sid_to_account_name(owner_sid);
    group = sid_to_account_name(group_sid);
    if (descriptor != nullptr) LocalFree(descriptor);
}

/// 读取自相对安全描述符原始字节（OWNER+GROUP+DACL）；失败返回空。
Bytes query_acl(const std::wstring& native) {
    const SECURITY_INFORMATION information =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION;
    DWORD needed = 0;
    GetFileSecurityW(native.c_str(), information, nullptr, 0, &needed);
    if (needed == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return {};
    Bytes buffer(static_cast<std::size_t>(needed));
    DWORD received = 0;
    if (!GetFileSecurityW(native.c_str(), information, buffer.data(), needed,
                          &received) ||
        received == 0) {
        return {};
    }
    buffer.resize(static_cast<std::size_t>(received));
    return buffer;
}

/// SetFileAttributesW 支持写回的属性位（其余位有的是只读的伪属性）。
DWORD settable_attribute_mask() {
    return FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN |
           FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
           FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_READONLY |
           FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_TEMPORARY;
}

std::optional<Metadata> read_metadata_win(const std::filesystem::path& path,
                                          std::string& error) {
    const std::wstring native = native_path(path);
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = "无法读取 \"" + path_for_error(path) + "\": " +
                win_error_text(GetLastError());
        return std::nullopt;
    }

    Metadata meta;
    meta.win_attrs = attributes;
    const bool is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool read_only = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    if (is_directory) {
        meta.mode = read_only ? 0555u : 0755u;
    } else {
        meta.mode = read_only ? 0444u : 0644u;
    }

    HANDLE handle = CreateFileW(
        native.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "无法打开 \"" + path_for_error(path) + "\": " +
                win_error_text(GetLastError());
        return std::nullopt;
    }
    FILETIME creation{};
    FILETIME access{};
    FILETIME write{};
    const BOOL times_ok = GetFileTime(handle, &creation, &access, &write);
    const DWORD times_error = times_ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!times_ok) {
        error = "读取时间戳失败: " + win_error_text(times_error);
        return std::nullopt;
    }
    meta.ctime_ns = filetime_to_ns(creation);
    meta.atime_ns = filetime_to_ns(access);
    meta.mtime_ns = filetime_to_ns(write);

    // 安全信息属于 best-effort：读不到不影响基础元数据。
    query_owner_group(native, meta.owner, meta.group);
    meta.acl = query_acl(native);

    error.clear();
    return meta;
}

struct AccountLookup {
    std::vector<std::uint8_t> sid_storage;
    PSID sid = nullptr;
};

/// 账户名 -> SID。失败返回 false；成功时 sid 指向 sid_storage。
bool lookup_account_name(const std::string& name, AccountLookup& lookup) {
    const std::wstring wide_name = wide_from_utf8(name);
    if (wide_name.empty()) return false;
    DWORD sid_length = 0;
    DWORD domain_length = 0;
    SID_NAME_USE use = SidTypeUnknown;
    LookupAccountNameW(nullptr, wide_name.c_str(), nullptr, &sid_length, nullptr,
                       &domain_length, &use);
    if (sid_length == 0) return false;
    lookup.sid_storage.resize(sid_length);
    std::wstring domain(domain_length == 0 ? 1 : domain_length, L'\0');
    DWORD sid_length_second = sid_length;
    DWORD domain_length_second = static_cast<DWORD>(domain.size());
    if (!LookupAccountNameW(nullptr, wide_name.c_str(),
                            lookup.sid_storage.data(), &sid_length_second,
                            domain.data(), &domain_length_second, &use)) {
        return false;
    }
    lookup.sid = lookup.sid_storage.data();
    return true;
}

bool apply_metadata_win(const std::filesystem::path& path, const Metadata& meta,
                        std::string& error) {
    const std::wstring native = native_path(path);
    DWORD current_attributes = GetFileAttributesW(native.c_str());
    if (current_attributes == INVALID_FILE_ATTRIBUTES) {
        error = "无法访问 \"" + path_for_error(path) + "\": " +
                win_error_text(GetLastError());
        return false;
    }

    // 1) 计算目标属性：win_attrs 优先，mode 的写位控制只读属性。
    const DWORD writable_mask = settable_attribute_mask();
    DWORD desired_attributes = (meta.win_attrs != 0)
                                   ? (meta.win_attrs & writable_mask)
                                   : (current_attributes & writable_mask);
    if (meta.mode != 0) {
        if ((meta.mode & 0200u) != 0) {
            desired_attributes &= ~FILE_ATTRIBUTE_READONLY;
        } else {
            desired_attributes |= FILE_ATTRIBUTE_READONLY;
        }
    }
    if (desired_attributes == 0) desired_attributes = FILE_ATTRIBUTE_NORMAL;

    // 2) 先去掉只读，否则无法打开写属性句柄、不能设置 ACL。
    if ((current_attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        DWORD writable_attributes =
            current_attributes & writable_mask & ~FILE_ATTRIBUTE_READONLY;
        if (writable_attributes == 0) writable_attributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileAttributesW(native.c_str(), writable_attributes)) {
            error = "清除只读属性失败: " + win_error_text(GetLastError());
            return false;
        }
    }

    // 3) 时间戳（ctime -> 创建时间，atime -> 最后访问，mtime -> 最后写入）。
    if (meta.mtime_ns != 0 || meta.atime_ns != 0 || meta.ctime_ns != 0) {
        FILETIME creation{};
        FILETIME access{};
        FILETIME write{};
        FILETIME* creation_ptr = nullptr;
        FILETIME* access_ptr = nullptr;
        FILETIME* write_ptr = nullptr;
        if (meta.ctime_ns != 0) {
            if (!ns_to_filetime(meta.ctime_ns, creation)) {
                error = "ctime 超出 Windows 可表示范围";
                return false;
            }
            creation_ptr = &creation;
        }
        if (meta.atime_ns != 0) {
            if (!ns_to_filetime(meta.atime_ns, access)) {
                error = "atime 超出 Windows 可表示范围";
                return false;
            }
            access_ptr = &access;
        }
        if (meta.mtime_ns != 0) {
            if (!ns_to_filetime(meta.mtime_ns, write)) {
                error = "mtime 超出 Windows 可表示范围";
                return false;
            }
            write_ptr = &write;
        }
        HANDLE handle = CreateFileW(
            native.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            error = "打开文件以设置时间戳失败: " + win_error_text(GetLastError());
            return false;
        }
        const BOOL times_ok =
            SetFileTime(handle, creation_ptr, access_ptr, write_ptr);
        const DWORD times_error = times_ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(handle);
        if (!times_ok) {
            error = "设置时间戳失败: " + win_error_text(times_error);
            return false;
        }
    }

    // 4) 属主/属组与 ACL 是 best-effort：权限不足只记 warning，不中断整体还原。
    std::string warnings;
    auto add_warning = [&warnings](const std::string& text) {
        if (!warnings.empty()) warnings += "; ";
        warnings += text;
    };

    if (!meta.owner.empty() || !meta.group.empty()) {
        AccountLookup owner_lookup;
        AccountLookup group_lookup;
        const bool owner_resolved =
            !meta.owner.empty() && lookup_account_name(meta.owner, owner_lookup);
        const bool group_resolved =
            !meta.group.empty() && lookup_account_name(meta.group, group_lookup);
        if (!meta.owner.empty() && !owner_resolved) {
            add_warning("属主账户 \"" + meta.owner + "\" 无法解析，已跳过");
        }
        if (!meta.group.empty() && !group_resolved) {
            add_warning("属组账户 \"" + meta.group + "\" 无法解析，已跳过");
        }
        if (owner_resolved || group_resolved) {
            SECURITY_INFORMATION information = 0;
            if (owner_resolved) information |= OWNER_SECURITY_INFORMATION;
            if (group_resolved) information |= GROUP_SECURITY_INFORMATION;
            const DWORD result = SetNamedSecurityInfoW(
                mutable_path(native), SE_FILE_OBJECT, information,
                owner_resolved ? owner_lookup.sid : nullptr,
                group_resolved ? group_lookup.sid : nullptr, nullptr, nullptr);
            if (result != ERROR_SUCCESS) {
                add_warning("属主/属组应用失败(" + win_error_text(result) + ")");
            }
        }
    }

    if (!meta.acl.empty()) {
        void* descriptor = const_cast<std::uint8_t*>(meta.acl.data());
        if (!SetFileSecurityW(native.c_str(), DACL_SECURITY_INFORMATION,
                              descriptor)) {
            add_warning("ACL 应用失败(" + win_error_text(GetLastError()) +
                        ")，已跳过");
        }
    }

    // 5) 最后落属性（恢复只读等）。
    if (meta.win_attrs != 0 || meta.mode != 0) {
        if (!SetFileAttributesW(native.c_str(), desired_attributes)) {
            error = "设置文件属性失败: " + win_error_text(GetLastError());
            return false;
        }
    }

    if (warnings.empty()) {
        error.clear();
    } else {
        error = "部分元数据未能应用: " + warnings;
    }
    return true;
}

std::string file_owner_name_win(const std::filesystem::path& path) {
    const std::wstring native = native_path(path);
    PSID owner_sid = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetNamedSecurityInfoW(
        mutable_path(native), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner_sid, nullptr, nullptr, nullptr, &descriptor);
    if (result != ERROR_SUCCESS) return {};
    const std::string name = sid_to_account_name(owner_sid);
    if (descriptor != nullptr) LocalFree(descriptor);
    return name;
}

#endif  // _WIN32

// ===========================================================================
// JSON 读取工具
// ===========================================================================

bool json_read_u32(const nlohmann::json& object, const char* key,
                   std::uint32_t& out) {
    if (!object.contains(key) || object[key].is_null()) return true;
    const nlohmann::json& value = object[key];
    if (value.is_number_unsigned()) {
        const std::uint64_t number = value.get<std::uint64_t>();
        if (number > std::numeric_limits<std::uint32_t>::max()) return false;
        out = static_cast<std::uint32_t>(number);
        return true;
    }
    if (value.is_number_integer()) {
        const std::int64_t number = value.get<std::int64_t>();
        if (number < 0 ||
            static_cast<std::uint64_t>(number) >
                std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        out = static_cast<std::uint32_t>(number);
        return true;
    }
    return false;
}

bool json_read_i64(const nlohmann::json& object, const char* key,
                   std::int64_t& out) {
    if (!object.contains(key) || object[key].is_null()) return true;
    const nlohmann::json& value = object[key];
    if (value.is_number_unsigned()) {
        const std::uint64_t number = value.get<std::uint64_t>();
        if (number >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        out = static_cast<std::int64_t>(number);
        return true;
    }
    if (value.is_number_integer()) {
        out = value.get<std::int64_t>();
        return true;
    }
    return false;
}

bool json_read_string(const nlohmann::json& object, const char* key,
                      std::string& out) {
    if (!object.contains(key) || object[key].is_null()) return true;
    if (!object[key].is_string()) return false;
    out = object[key].get<std::string>();
    return true;
}

}  // namespace

// ===========================================================================
// 公开接口
// ===========================================================================

std::optional<Metadata> read_metadata(const std::filesystem::path& path,
                                      std::string& error) {
#if defined(_WIN32)
    return read_metadata_win(path, error);
#else
    (void)path;
    error = "当前平台未实现元数据读取（仅支持 Windows）";
    return std::nullopt;
#endif
}

bool apply_metadata(const std::filesystem::path& path, const Metadata& meta,
                    std::string& error) {
#if defined(_WIN32)
    return apply_metadata_win(path, meta, error);
#else
    (void)path;
    (void)meta;
    error = "当前平台未实现元数据还原（仅支持 Windows）";
    return false;
#endif
}

Bytes metadata_to_bytes(const Metadata& meta) {
    Bytes out;
    out.reserve(kMetadataFixedSize + meta.owner.size() + meta.group.size() +
                meta.acl.size() + 12);
    out.insert(out.end(), std::begin(kMetadataMagic), std::end(kMetadataMagic));
    append_u8(out, kMetadataVersion);
    append_u8(out, 0);
    append_u16(out, 0);
    append_u32(out, meta.mode);
    append_u32(out, meta.uid);
    append_u32(out, meta.gid);
    append_u32(out, meta.win_attrs);
    append_i64(out, meta.mtime_ns);
    append_i64(out, meta.atime_ns);
    append_i64(out, meta.ctime_ns);
    append_string(out, meta.owner);
    append_string(out, meta.group);
    append_bytes(out, meta.acl);
    return out;
}

std::optional<Metadata> metadata_from_bytes(const Bytes& bytes,
                                            std::string& error) {
    if (bytes.size() < kMetadataFixedSize) {
        error = "元数据块过短（" + std::to_string(bytes.size()) + " 字节）";
        return std::nullopt;
    }
    ByteReader reader(bytes);

    std::uint8_t magic[4] = {0, 0, 0, 0};
    if (!reader.read_magic(magic) || std::memcmp(magic, kMetadataMagic, 4) != 0) {
        error = "元数据块 magic 不匹配（不是 META 块）";
        return std::nullopt;
    }
    std::uint8_t version = 0;
    if (!reader.read_u8(version)) {
        error = "元数据块版本字段缺失";
        return std::nullopt;
    }
    if (version != kMetadataVersion) {
        error = "不支持的元数据版本 " + std::to_string(version) +
                "（当前支持 " + std::to_string(kMetadataVersion) + "）";
        return std::nullopt;
    }
    std::uint8_t reserved8 = 0;
    std::uint16_t reserved16 = 0;
    if (!reader.read_u8(reserved8) || !reader.read_u16(reserved16)) {
        error = "元数据块保留字段缺失";
        return std::nullopt;
    }

    Metadata meta;
    if (!reader.read_u32(meta.mode) || !reader.read_u32(meta.uid) ||
        !reader.read_u32(meta.gid) || !reader.read_u32(meta.win_attrs) ||
        !reader.read_i64(meta.mtime_ns) || !reader.read_i64(meta.atime_ns) ||
        !reader.read_i64(meta.ctime_ns)) {
        error = "元数据块固定字段截断";
        return std::nullopt;
    }
    if (!reader.read_string(meta.owner)) {
        error = "元数据块 owner 字段越界";
        return std::nullopt;
    }
    if (!reader.read_string(meta.group)) {
        error = "元数据块 group 字段越界";
        return std::nullopt;
    }
    if (!reader.read_bytes(meta.acl)) {
        error = "元数据块 acl 字段越界";
        return std::nullopt;
    }
    if (reader.failed()) {
        error = "元数据块边界检查失败";
        return std::nullopt;
    }
    if (reader.position() != bytes.size()) {
        error = "元数据块尾部有多余的 " +
                std::to_string(bytes.size() - reader.position()) + " 字节";
        return std::nullopt;
    }
    error.clear();
    return meta;
}

nlohmann::json metadata_to_json(const Metadata& meta) {
    nlohmann::json value = nlohmann::json::object();
    value["mode"] = meta.mode;
    value["uid"] = meta.uid;
    value["gid"] = meta.gid;
    value["owner"] = meta.owner;
    value["group"] = meta.group;
    value["mtime_ns"] = meta.mtime_ns;
    value["atime_ns"] = meta.atime_ns;
    value["ctime_ns"] = meta.ctime_ns;
    value["win_attrs"] = meta.win_attrs;
    value["acl"] = base64_encode(meta.acl);
    return value;
}

std::optional<Metadata> metadata_from_json(const nlohmann::json& json) {
    if (!json.is_object()) return std::nullopt;

    Metadata meta;
    if (!json_read_u32(json, "mode", meta.mode) ||
        !json_read_u32(json, "uid", meta.uid) ||
        !json_read_u32(json, "gid", meta.gid) ||
        !json_read_u32(json, "win_attrs", meta.win_attrs)) {
        return std::nullopt;
    }
    if (!json_read_string(json, "owner", meta.owner) ||
        !json_read_string(json, "group", meta.group)) {
        return std::nullopt;
    }

    // 新键 *_ns 优先；兼容旧 manifest 的 mtime/atime/ctime。
    bool times_ok = false;
    if (json.contains("mtime_ns") || json.contains("atime_ns") ||
        json.contains("ctime_ns")) {
        times_ok = json_read_i64(json, "mtime_ns", meta.mtime_ns) &&
                   json_read_i64(json, "atime_ns", meta.atime_ns) &&
                   json_read_i64(json, "ctime_ns", meta.ctime_ns);
    } else {
        times_ok = json_read_i64(json, "mtime", meta.mtime_ns) &&
                   json_read_i64(json, "atime", meta.atime_ns) &&
                   json_read_i64(json, "ctime", meta.ctime_ns);
    }
    if (!times_ok) return std::nullopt;

    if (json.contains("acl") && !json["acl"].is_null()) {
        const nlohmann::json& acl = json["acl"];
        if (acl.is_string()) {
            const std::optional<Bytes> decoded = base64_decode(acl.get<std::string>());
            if (!decoded.has_value()) return std::nullopt;
            meta.acl = *decoded;
        } else if (acl.is_array()) {
            // 容忍直接写字节数组的 manifest。
            Bytes bytes;
            bytes.reserve(acl.size());
            for (const nlohmann::json& item : acl) {
                if (!item.is_number_integer() && !item.is_number_unsigned()) {
                    return std::nullopt;
                }
                const std::int64_t value = item.get<std::int64_t>();
                if (value < 0 || value > 255) return std::nullopt;
                bytes.push_back(static_cast<std::uint8_t>(value));
            }
            meta.acl = std::move(bytes);
        } else {
            return std::nullopt;
        }
    }
    return meta;
}

std::string file_owner_name(const std::filesystem::path& path) {
#if defined(_WIN32)
    return file_owner_name_win(path);
#else
    (void)path;
    return {};
#endif
}

}  // namespace backup
