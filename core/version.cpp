#include "version.h"

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

}  // namespace backup
