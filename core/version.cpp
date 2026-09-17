#include "version.h"

#include <openssl/crypto.h>

#include <lz4.h>
#include <zlib.h>
#include <zstd.h>

#include <nlohmann/json.hpp>

namespace backup {

std::string version() {
#ifdef FB_VERSION
    return FB_VERSION;
#else
    // 不通过 CMake 构建时会走到这里（例如 IDE 直接索引单文件，
    // 缺 FB_VERSION 编译宏）。正常情况下不应出现。
    return "0.0.0-unknown";
#endif
}

std::vector<LibraryVersion> linked_libraries() {
    return {
        // 下列调用都是对外部符号的真实引用。链接器因此必须去解析它们 ——
        // 这正是 M0 想要的效果：库没接对、triplet 选错、运行库口径不一致
        // （/MT vs /MD），都会在这里以链接错误的形式暴露，
        // 而不是潜伏到 M2 用压缩功能时才炸出来（README §十一）。
        {"zlib", zlibVersion()},
        {"lz4", LZ4_versionString()},
        {"zstd", ZSTD_versionString()},
        {"openssl", OpenSSL_version(OPENSSL_VERSION)},

        // 纯头文件库：版本号只能来自编译期宏，无链接期符号可查。
        {"nlohmann-json",
         std::to_string(NLOHMANN_JSON_VERSION_MAJOR) + "." +
             std::to_string(NLOHMANN_JSON_VERSION_MINOR) + "." +
             std::to_string(NLOHMANN_JSON_VERSION_PATCH)},
    };
}

}  // namespace backup
