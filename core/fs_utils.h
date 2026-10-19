#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "types.h"

namespace backup {

/// 目录树中的一个条目。
struct FileEntry {
    /// 相对 root 的路径，**UTF-8 编码**，统一以 '/' 分隔。
    ///
    /// 存 UTF-8 而不是宽字符，是因为它最终要写进 manifest / VBAK 索引；
    /// UTF-8 是跨平台、可排序、可读的表示，宽字符编码则依平台而异。
    std::string relative_path;

    EntryType type = EntryType::File;
    std::uintmax_t size = 0;      ///< 普通文件的字节数；其它类型为 0
    std::string link_target;      ///< 符号链接目标（UTF-8）
};

/// 递归扫描 root 下的条目。
///
/// 返回结果按 relative_path 排序 —— 顺序必须固定，否则同一份输入
/// 每次生成的备份产物不同，归档不可复现，往返测试会随机失败。
///
/// include_directories 决定是否把目录本身也放进结果（元数据还原、空目录
/// 还原需要它）。include_links 决定是否包含符号链接；链接本身不会被跟随。
///
/// root 不存在、不是目录、或无权访问时返回空列表，不抛异常。
///
/// ---- 调用方注意（Windows）----
/// root 必须能从宽字符构造。**不要**用窄字符串（如 argv 的 char*）去构造它：
/// MinGW 下 std::filesystem::path 从非 ASCII 窄字符串构造会直接抛异常，
/// 中文目录名（「我的文档」）会让程序崩溃。用 wmain 拿 wchar_t** 参数，
/// 再直接赋给 path 即可。详见 cli/main.cpp 中 wmain 的说明。
std::vector<FileEntry> scan_tree(const std::filesystem::path& root,
                                 bool include_directories = true,
                                 bool include_links = false);

/// 只列出普通文件（旧的 M1 接口，保留给 `scan` 调试命令）。
std::vector<FileEntry> list_files(const std::filesystem::path& root);

/// UTF-8 std::string -> std::filesystem::path。
///
/// 绝不能写 std::filesystem::path(s)：
/// MinGW 下窄字符串构造会按本地代码页（GBK）解释，中文直接抛
/// "Illegal byte sequence"。必须走 u8 通道。
///
/// 凡是「UTF-8 字符串 -> 路径」的地方都要走这里，不要各处自己写一份：
/// 归档格式、manifest 里存的都是 UTF-8，HTTP 层从浏览器收到的也是 UTF-8。
std::filesystem::path path_from_u8(const std::string& s);

}  // namespace backup
