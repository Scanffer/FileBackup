#pragma once

#include <string>

namespace backup {

/// 本软件版本号。单一来源是 CMake 的 project(FileBackup VERSION ...)，
/// 经 FB_VERSION 编译宏注入（见 core/CMakeLists.txt）。
std::string version();

}  // namespace backup
