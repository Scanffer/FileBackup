#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace backup {

/// 目录树中的一个文件条目。
///
/// M1 起步阶段只处理普通文件。符号链接、硬链接、FIFO、设备等
/// 留到 M3（README §2.2「文件类型支持」）再扩展 type 字段。
/// 现在就把结构体单独定义出来，是为了那时只加字段、不改调用方。
struct FileEntry {
    /// 相对 root 的路径，**UTF-8 编码**，统一以 '/' 分隔。
    ///
    /// 存 UTF-8 而不是宽字符，是因为它最终要写进 manifest 文件；
    /// UTF-8 是跨平台、可排序、可读的表示，宽字符编码则依平台而异。
    std::string relative_path;

    std::uintmax_t size = 0;  ///< 字节数
};

/// 递归列出 root 下的所有普通文件。
///
/// 返回结果按 relative_path 排序 —— 顺序必须固定，否则同一份输入
/// 每次生成的备份产物不同，归档不可复现，往返测试会随机失败。
///
/// root 不存在、不是目录、或无权访问时返回空列表，不抛异常。
///
/// ---- 调用方注意（Windows）----
/// root 必须能从宽字符构造。**不要**用窄字符串（如 argv 的 char*）去构造它：
/// MinGW 下 std::filesystem::path 从非 ASCII 窄字符串构造会直接抛异常，
/// 中文目录名（「我的文档」）会让程序崩溃。用 wmain 拿 wchar_t** 参数，
/// 再直接赋给 path 即可。详见 cli/main.cpp 中 wmain 的说明。
std::vector<FileEntry> list_files(const std::filesystem::path& root);

}  // namespace backup
