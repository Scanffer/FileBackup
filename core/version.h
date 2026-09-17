#pragma once

#include <string>
#include <vector>

namespace backup {

/// 本软件版本号。单一来源是 CMake 的 project(FileBackup VERSION ...)，
/// 经 FB_VERSION 编译宏注入（见 core/CMakeLists.txt）。
std::string version();

/// 一个第三方库的名字与版本。
struct LibraryVersion {
    std::string name;
    std::string version;
};

/// 本次构建**实际链接**进来的第三方库版本。
///
/// 用途有两个：
///   1. 排查「我这能编译你那报错」——先比对双方链接到的库版本；
///   2. 充当依赖接线自检。这些版本号取自链接期的真实符号（而非编译期宏），
///      因此任何一个库没接对，这里会直接链接失败，而不是等到 M2 才炸出来。
///
/// 注：nlohmann-json 是纯头文件库，没有可链接的符号，
/// 其版本只能取自编译期宏，这一点在返回项中如实标注。
std::vector<LibraryVersion> linked_libraries();

}  // namespace backup
