#include "algorithm.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

// ===========================================================================
// 自研 DEFLATE 压缩 / 解压（RFC 1951，外加 RFC 1950 的 zlib 头）
//
// 压缩端：
//   - LZ77：32KB 窗口 + 3 字节哈希链，贪心匹配；
//   - 输出固定 Huffman 编码的 DEFLATE 块（BTYPE=01）；
//   - 外层套 zlib 头尾（CMF/FLG + Adler-32），方便用 `python -c
//     "import zlib"` 交叉验证，也能直接给其他 zlib 实现解开。
//
// 解压端：
//   - 支持 stored / 固定 Huffman / 动态 Huffman 三种块（BTYPE 0/1/2），
//     所以不仅能解自己压的流，也能解标准 zlib 产生的流；
//   - 校验 zlib 头中的 FCHECK、Adler-32 和调用方给的原始大小。
//
// 这是 2.1 必选扩展里第一个「完全自研」的压缩算法。RLE / LZ4 / LZW 会
// 在后续按同样口径自己实现。
// ===========================================================================

namespace backup {
namespace {

// ---------------------------------------------------------------------------
// 常量表（DEFLATE 规范里的长度/距离编码）
// ---------------------------------------------------------------------------

constexpr int kWindowSize = 32768;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kHashBits = 15;
constexpr int kHashSize = 1 << kHashBits;
constexpr std::size_t kBlockInput = 128 * 1024;  // 每块最多 128KB 原始数据

constexpr int kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577};
constexpr int kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// ---------------------------------------------------------------------------
// 位流读写
//
// DEFLATE 的普通字段按 LSB 优先；Huffman 码本身按 MSB 优先写入。
// ---------------------------------------------------------------------------

class BitWriter {
public:
    void put_bits(std::uint32_t value, int count) {
        for (int i = 0; i < count; ++i) {
            bitbuf_ |= ((value >> i) & 1u) << bitcount_;
            if (++bitcount_ == 8) {
                out_.push_back(static_cast<std::uint8_t>(bitbuf_));
                bitbuf_ = 0;
                bitcount_ = 0;
            }
        }
    }

    /// Huffman 码按 MSB 先出。
    void put_code(std::uint32_t code, int length) {
        for (int i = length - 1; i >= 0; --i) put_bits((code >> i) & 1u, 1);
    }

    void align_byte() {
        if (bitcount_ == 0) return;
        out_.push_back(static_cast<std::uint8_t>(bitbuf_));
        bitbuf_ = 0;
        bitcount_ = 0;
    }

    Bytes take() {
        align_byte();
        return std::move(out_);
    }

private:
    Bytes out_;
    std::uint32_t bitbuf_ = 0;
    int bitcount_ = 0;
};

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    bool read_bits(int count, std::uint32_t& value) {
        if (count < 0 || count > 24) return false;
        if (bit_ + static_cast<std::size_t>(count) > size_ * 8) return false;
        value = 0;
        for (int i = 0; i < count; ++i) {
            const std::uint32_t bit =
                (data_[bit_ >> 3] >> (bit_ & 7)) & 1u;
            value |= bit << i;
            ++bit_;
        }
        return true;
    }

    void align_byte() { bit_ = (bit_ + 7) & ~static_cast<std::size_t>(7); }

    bool byte_aligned() const { return (bit_ & 7) == 0; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t bit_ = 0;
};

// ---------------------------------------------------------------------------
// Huffman 解码树
// ---------------------------------------------------------------------------

class HuffmanTree {
public:
    /// 用 code lengths 建树；返回 false 表示长度表不合法（超长/冲突）。
    bool build(const std::uint8_t* lengths, int count) {
        nodes_.assign(1, Node{-1, -1, -1});
        empty_ = true;

        std::array<int, 16> bl_count{};
        for (int sym = 0; sym < count; ++sym) {
            const int len = lengths[sym];
            if (len < 0 || len > 15) return false;
            if (len > 0) {
                ++bl_count[len];
                empty_ = false;
            }
        }
        if (empty_) return true;

        std::array<std::uint32_t, 16> next_code{};
        std::uint32_t code = 0;
        for (int bits = 1; bits <= 15; ++bits) {
            code = (code + static_cast<std::uint32_t>(bl_count[bits - 1])) << 1;
            next_code[bits] = code;
        }

        for (int sym = 0; sym < count; ++sym) {
            const int len = lengths[sym];
            if (len == 0) continue;
            const std::uint32_t sym_code = next_code[len]++;
            if (sym_code >= (1u << len)) return false;  // 长度分配超额

            int node = 0;
            for (int i = len - 1; i >= 0; --i) {
                const int bit = static_cast<int>((sym_code >> i) & 1u);
                int next = nodes_[node].child[bit];
                if (next < 0) {
                    nodes_.push_back(Node{-1, -1, -1});
                    next = static_cast<int>(nodes_.size()) - 1;
                    nodes_[node].child[bit] = next;
                }
                node = next;
            }
            if (nodes_[node].symbol >= 0) return false;  // 两个符号同码
            nodes_[node].symbol = sym;
        }
        return true;
    }

    /// 解出一个符号；流结束/非法码返回 -1。
    int decode(BitReader& reader) const {
        if (empty_) return -1;
        int node = 0;
        for (int depth = 0; depth < 15; ++depth) {
            std::uint32_t bit = 0;
            if (!reader.read_bits(1, bit)) return -1;
            node = nodes_[node].child[bit];
            if (node < 0) return -1;
            if (nodes_[node].symbol >= 0) return nodes_[node].symbol;
        }
        return -1;
    }

    bool empty() const { return empty_; }

private:
    struct Node {
        int child[2];
        int symbol;
    };
    std::vector<Node> nodes_;
    bool empty_ = true;
};

// ---------------------------------------------------------------------------
// 固定 Huffman 码（RFC 1951 §3.2.6）
// ---------------------------------------------------------------------------

void fixed_litlen_code(int symbol, std::uint32_t& code, int& length) {
    if (symbol <= 143) {
        code = 0x30u + static_cast<std::uint32_t>(symbol);
        length = 8;
    } else if (symbol <= 255) {
        code = 0x190u + static_cast<std::uint32_t>(symbol - 144);
        length = 9;
    } else if (symbol <= 279) {
        code = static_cast<std::uint32_t>(symbol - 256);
        length = 7;
    } else {
        code = 0xC0u + static_cast<std::uint32_t>(symbol - 280);
        length = 8;
    }
}

HuffmanTree make_fixed_litlen_tree() {
    std::uint8_t lengths[288];
    for (int i = 0; i <= 143; ++i) lengths[i] = 8;
    for (int i = 144; i <= 255; ++i) lengths[i] = 9;
    for (int i = 256; i <= 279; ++i) lengths[i] = 7;
    for (int i = 280; i <= 287; ++i) lengths[i] = 8;
    HuffmanTree tree;
    tree.build(lengths, 288);
    return tree;
}

HuffmanTree make_fixed_dist_tree() {
    std::uint8_t lengths[32];
    for (int i = 0; i < 32; ++i) lengths[i] = 5;
    HuffmanTree tree;
    tree.build(lengths, 32);
    return tree;
}

// ---------------------------------------------------------------------------
// Adler-32（zlib 尾部校验）
// ---------------------------------------------------------------------------

std::uint32_t adler32(const Bytes& data) {
    constexpr std::uint32_t kMod = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t byte : data) {
        a = (a + byte) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

// ---------------------------------------------------------------------------
// 压缩实现
// ---------------------------------------------------------------------------

class DeflateCompressor final : public ICompressor {
public:
    CompressionId id() const override { return CompressionId::Deflate; }

    Bytes compress(const Bytes& input, int level, std::string& error) override {
        error.clear();

        level = std::clamp(level, 1, 9);
        const int max_chain = 8 + level * 18;  // 26 .. 170

        BitWriter writer;

        // zlib 头：CMF=0x78（deflate, 32KB 窗口），FLG 的 FLEVEL 按等级填，
        // FCHECK 补到 (CMF<<8 | FLG) % 31 == 0。
        const std::uint32_t flevel =
            level <= 1 ? 0u : (level <= 5 ? 1u : (level <= 7 ? 2u : 3u));
        const std::uint32_t base = (0x78u << 8) | (flevel << 6);
        const std::uint32_t rem = base % 31;
        const std::uint32_t fcheck = rem == 0 ? 0 : 31 - rem;
        const std::uint8_t flg =
            static_cast<std::uint8_t>((flevel << 6) | fcheck);
        writer.put_bits(0x78, 8);
        writer.put_bits(flg, 8);

        const std::size_t n = input.size();
        std::vector<std::int64_t> head(kHashSize, -1);
        std::vector<std::int64_t> prev(kWindowSize, -1);

        auto hash_at = [&](std::size_t pos) -> std::uint32_t {
            const std::uint32_t v =
                static_cast<std::uint32_t>(input[pos]) |
                (static_cast<std::uint32_t>(input[pos + 1]) << 8) |
                (static_cast<std::uint32_t>(input[pos + 2]) << 16);
            return (v * 2654435761u) >> (32 - kHashBits);
        };

        auto insert = [&](std::size_t pos) {
            if (pos + kMinMatch > n) return;
            const std::uint32_t h = hash_at(pos);
            prev[pos & (kWindowSize - 1)] = head[h];
            head[h] = static_cast<std::int64_t>(pos);
        };

        auto find_match = [&](std::size_t pos, int& match_len) -> int {
            match_len = 0;
            if (pos + kMinMatch > n) return 0;

            const std::uint32_t h = hash_at(pos);
            std::int64_t candidate = head[h];
            insert(pos);

            const std::size_t limit =
                std::min<std::size_t>(kMaxMatch, n - pos);
            int best_len = kMinMatch - 1;
            int best_dist = 0;
            int chain = max_chain;

            while (candidate >= 0 && chain-- > 0) {
                const std::size_t c = static_cast<std::size_t>(candidate);
                if (c >= pos) break;  // 环形链被覆盖，防止死循环
                const std::size_t dist = pos - c;
                if (dist > kWindowSize) break;

                // 用当前最优长度做快速排除。
                if (input[c + static_cast<std::size_t>(best_len)] !=
                    input[pos + static_cast<std::size_t>(best_len)]) {
                    const std::int64_t next = prev[c & (kWindowSize - 1)];
                    if (next >= candidate) break;
                    candidate = next;
                    continue;
                }

                std::size_t len = 0;
                while (len < limit && input[c + len] == input[pos + len]) {
                    ++len;
                }
                if (static_cast<int>(len) > best_len) {
                    best_len = static_cast<int>(len);
                    best_dist = static_cast<int>(dist);
                    if (len >= limit) break;
                }

                const std::int64_t next = prev[c & (kWindowSize - 1)];
                if (next >= candidate) break;
                candidate = next;
            }

            if (best_len >= kMinMatch) {
                match_len = best_len;
                return best_dist;
            }
            return 0;
        };

        auto emit_fixed = [&](int symbol) {
            std::uint32_t code = 0;
            int length = 0;
            fixed_litlen_code(symbol, code, length);
            writer.put_code(code, length);
        };

        auto emit_match = [&](int length, int distance) {
            int len_index = 28;
            for (int i = 0; i < 28; ++i) {
                if (length < kLengthBase[i + 1]) {
                    len_index = i;
                    break;
                }
            }
            emit_fixed(257 + len_index);
            writer.put_bits(
                static_cast<std::uint32_t>(length - kLengthBase[len_index]),
                kLengthExtra[len_index]);

            int dist_index = 29;
            for (int i = 0; i < 30; ++i) {
                const int span = 1 << kDistExtra[i];
                if (distance < kDistBase[i] + span) {
                    dist_index = i;
                    break;
                }
            }
            writer.put_code(static_cast<std::uint32_t>(dist_index), 5);
            writer.put_bits(
                static_cast<std::uint32_t>(distance - kDistBase[dist_index]),
                kDistExtra[dist_index]);
        };

        std::size_t pos = 0;
        if (n == 0) {
            writer.put_bits(1, 1);  // BFINAL
            writer.put_bits(1, 2);  // BTYPE = 固定 Huffman
            emit_fixed(256);        // 块结束
        } else {
            while (true) {
                const std::size_t block_end =
                    std::min(n, pos + kBlockInput);
                const bool is_last = block_end >= n;

                writer.put_bits(is_last ? 1u : 0u, 1);
                writer.put_bits(1, 2);  // 固定 Huffman 块

                while (pos < block_end) {
                    int match_len = 0;
                    const int match_dist = find_match(pos, match_len);
                    if (match_dist > 0 && match_len >= kMinMatch) {
                        emit_match(match_len, match_dist);
                        // 当前位置已由 find_match 插入，补插匹配段其余位置。
                        for (int i = 1; i < match_len; ++i) {
                            insert(pos + static_cast<std::size_t>(i));
                        }
                        pos += static_cast<std::size_t>(match_len);
                    } else {
                        emit_fixed(static_cast<int>(input[pos]));
                        ++pos;
                    }
                }

                emit_fixed(256);  // 块结束
                if (is_last) break;
            }
        }

        Bytes out = writer.take();
        const std::uint32_t checksum = adler32(input);
        out.push_back(static_cast<std::uint8_t>(checksum >> 24));
        out.push_back(static_cast<std::uint8_t>(checksum >> 16));
        out.push_back(static_cast<std::uint8_t>(checksum >> 8));
        out.push_back(static_cast<std::uint8_t>(checksum));
        return out;
    }

    Bytes decompress(const Bytes& input, std::size_t expected_size,
                     std::string& error) override {
        error.clear();
        if (input.size() < 2) {
            error = "DEFLATE 流太短";
            return {};
        }

        // ---- zlib 头 ----
        const std::uint8_t cmf = input[0];
        const std::uint8_t flg = input[1];
        if ((cmf & 0x0F) != 8) {
            error = "不是 zlib/DEFLATE 数据";
            return {};
        }
        if ((cmf >> 4) > 7) {
            error = "zlib 窗口大小超出规范";
            return {};
        }
        if (((static_cast<std::uint32_t>(cmf) << 8) + flg) % 31 != 0) {
            error = "zlib 头校验失败";
            return {};
        }
        if ((flg & 0x20) != 0) {
            error = "不支持带预置字典的 zlib 流";
            return {};
        }
        if (input.size() < 6) {
            error = "zlib 流不完整";
            return {};
        }

        BitReader reader(input.data() + 2, input.size() - 2);
        Bytes out;
        out.reserve(std::min<std::size_t>(expected_size, 64u * 1024u * 1024u));

        const HuffmanTree fixed_litlen = make_fixed_litlen_tree();
        const HuffmanTree fixed_dist = make_fixed_dist_tree();

        bool is_final = false;
        while (!is_final) {
            std::uint32_t final_bit = 0;
            std::uint32_t btype = 0;
            if (!reader.read_bits(1, final_bit) ||
                !reader.read_bits(2, btype)) {
                error = "DEFLATE 块头损坏";
                return {};
            }
            is_final = final_bit != 0;

            if (btype == 0) {
                // ---- 非压缩块 ----
                reader.align_byte();
                std::uint32_t len = 0;
                std::uint32_t nlen = 0;
                if (!reader.read_bits(16, len) || !reader.read_bits(16, nlen)) {
                    error = "stored 块头损坏";
                    return {};
                }
                if ((len ^ 0xFFFFu) != nlen) {
                    error = "stored 块长度校验失败";
                    return {};
                }
                for (std::uint32_t i = 0; i < len; ++i) {
                    std::uint32_t byte = 0;
                    if (!reader.read_bits(8, byte)) {
                        error = "stored 块数据不完整";
                        return {};
                    }
                    out.push_back(static_cast<std::uint8_t>(byte));
                }
                continue;
            }

            HuffmanTree dynamic_litlen;
            HuffmanTree dynamic_dist;
            const HuffmanTree* litlen_tree = &fixed_litlen;
            const HuffmanTree* dist_tree = &fixed_dist;

            if (btype == 2) {
                // ---- 动态 Huffman 块 ----
                std::uint32_t hlit = 0, hdist = 0, hclen = 0;
                if (!reader.read_bits(5, hlit) ||
                    !reader.read_bits(5, hdist) ||
                    !reader.read_bits(4, hclen)) {
                    error = "动态 Huffman 块头损坏";
                    return {};
                }
                hlit += 257;
                hdist += 1;
                hclen += 4;
                if (hlit > 286 || hdist > 30) {
                    error = "动态 Huffman 表大小非法";
                    return {};
                }

                static constexpr int kOrder[19] = {
                    16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                    11, 4, 12, 3, 13, 2, 14, 1, 15};
                std::uint8_t cl_lengths[19] = {};
                for (std::uint32_t i = 0; i < hclen; ++i) {
                    std::uint32_t v = 0;
                    if (!reader.read_bits(3, v)) {
                        error = "HCLEN 表损坏";
                        return {};
                    }
                    cl_lengths[kOrder[i]] = static_cast<std::uint8_t>(v);
                }

                HuffmanTree cl_tree;
                if (!cl_tree.build(cl_lengths, 19)) {
                    error = "码长 Huffman 表非法";
                    return {};
                }

                std::vector<std::uint8_t> lengths(hlit + hdist, 0);
                std::size_t index = 0;
                while (index < lengths.size()) {
                    const int sym = cl_tree.decode(reader);
                    if (sym < 0) {
                        error = "码长流损坏";
                        return {};
                    }
                    if (sym < 16) {
                        lengths[index++] = static_cast<std::uint8_t>(sym);
                    } else if (sym == 16) {
                        std::uint32_t extra = 0;
                        if (!reader.read_bits(2, extra)) {
                            error = "码长重复流损坏";
                            return {};
                        }
                        if (index == 0) {
                            error = "码长重复没有前值";
                            return {};
                        }
                        const std::uint8_t repeat = lengths[index - 1];
                        const int count = 3 + static_cast<int>(extra);
                        for (int i = 0; i < count && index < lengths.size();
                             ++i) {
                            lengths[index++] = repeat;
                        }
                    } else if (sym == 17) {
                        std::uint32_t extra = 0;
                        if (!reader.read_bits(3, extra)) {
                            error = "码长零重复流损坏";
                            return {};
                        }
                        const int count = 3 + static_cast<int>(extra);
                        for (int i = 0; i < count && index < lengths.size();
                             ++i) {
                            lengths[index++] = 0;
                        }
                    } else if (sym == 18) {
                        std::uint32_t extra = 0;
                        if (!reader.read_bits(7, extra)) {
                            error = "码长零重复流损坏";
                            return {};
                        }
                        const int count = 11 + static_cast<int>(extra);
                        for (int i = 0; i < count && index < lengths.size();
                             ++i) {
                            lengths[index++] = 0;
                        }
                    } else {
                        error = "非法码长符号";
                        return {};
                    }
                }

                if (!dynamic_litlen.build(lengths.data(),
                                          static_cast<int>(hlit))) {
                    error = "字面量 Huffman 表非法";
                    return {};
                }
                if (!dynamic_dist.build(lengths.data() + hlit,
                                        static_cast<int>(hdist))) {
                    error = "距离 Huffman 表非法";
                    return {};
                }
                litlen_tree = &dynamic_litlen;
                dist_tree = &dynamic_dist;
            } else if (btype != 1) {
                error = "非法 DEFLATE 块类型";
                return {};
            }

            // ---- 解码符号 ----
            while (true) {
                const int sym = litlen_tree->decode(reader);
                if (sym < 0) {
                    error = "字面量/长度流损坏";
                    return {};
                }
                if (sym < 256) {
                    out.push_back(static_cast<std::uint8_t>(sym));
                    continue;
                }
                if (sym == 256) break;
                if (sym > 285) {
                    error = "非法长度符号";
                    return {};
                }

                const int len_index = sym - 257;
                std::uint32_t extra = 0;
                if (!reader.read_bits(kLengthExtra[len_index], extra)) {
                    error = "长度附加位损坏";
                    return {};
                }
                const std::size_t length =
                    static_cast<std::size_t>(kLengthBase[len_index]) + extra;

                const int dist_sym = dist_tree->decode(reader);
                if (dist_sym < 0 || dist_sym > 29) {
                    error = "非法距离符号";
                    return {};
                }
                std::uint32_t dist_extra = 0;
                if (!reader.read_bits(kDistExtra[dist_sym], dist_extra)) {
                    error = "距离附加位损坏";
                    return {};
                }
                const std::size_t distance =
                    static_cast<std::size_t>(kDistBase[dist_sym]) +
                    dist_extra;
                if (distance == 0 || distance > out.size()) {
                    error = "距离超出已解压数据范围";
                    return {};
                }

                // 逐字节复制：允许源/目标重叠（DEFLATE 的经典情况）。
                const std::size_t start = out.size() - distance;
                for (std::size_t i = 0; i < length; ++i) {
                    out.push_back(out[start + i]);
                }
            }
        }

        // ---- zlib 尾：Adler-32 ----
        reader.align_byte();
        std::uint32_t checksum = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint32_t byte = 0;
            if (!reader.read_bits(8, byte)) {
                error = "zlib 尾部不完整";
                return {};
            }
            checksum = (checksum << 8) | byte;
        }

        if (out.size() != expected_size) {
            error = "解压后大小与预期不符";
            return {};
        }
        if (adler32(out) != checksum) {
            error = "Adler-32 校验失败";
            return {};
        }
        return out;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 算法注册表与工厂
// ---------------------------------------------------------------------------

const char* to_string(CompressionId id) {
    switch (id) {
        case CompressionId::None: return "none";
        case CompressionId::Deflate: return "deflate";
        case CompressionId::Lz4: return "lz4";
        case CompressionId::Zstd: return "zstd";
        case CompressionId::Rle: return "rle";
        case CompressionId::Lzw: return "lzw";
    }
    return "none";
}

std::optional<CompressionId> compression_from_string(const std::string& s) {
    if (s == "none") return CompressionId::None;
    if (s == "deflate") return CompressionId::Deflate;
    if (s == "lz4") return CompressionId::Lz4;
    if (s == "zstd") return CompressionId::Zstd;
    if (s == "rle") return CompressionId::Rle;
    if (s == "lzw") return CompressionId::Lzw;
    return std::nullopt;
}

const std::vector<AlgorithmInfo>& compression_algorithms() {
    static const std::vector<AlgorithmInfo> list = {
        {"none", "不压缩", false, false, ""},
        {"deflate", "DEFLATE", true, true,
         "自研：LZ77 + 固定 Huffman，输出标准 zlib 流"},
        {"lz4", "LZ4", false, true, "自研，LZ4 block 格式兼容"},
        {"zstd", "Zstandard", true, false, "第三方库（附加算法）"},
        {"rle", "RLE", false, true, "自研游程编码"},
        {"lzw", "LZW", false, true, "自研字典编码"},
    };
    return list;
}

int clamp_compression_level(CompressionId id, int level) {
    switch (id) {
        case CompressionId::Zstd: return std::clamp(level, 1, 19);
        case CompressionId::Deflate: return std::clamp(level, 1, 9);
        default: return level;
    }
}

std::unique_ptr<ICompressor> make_compressor(CompressionId id) {
    switch (id) {
        case CompressionId::Deflate:
            return std::make_unique<DeflateCompressor>();
        default:
            return nullptr;
    }
}

}  // namespace backup
