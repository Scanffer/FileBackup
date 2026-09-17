#!/usr/bin/env python3
"""把 web/ 下的静态文件转成 C++ 源文件，供单 exe 交付时从内存托管。

对应 README §3.4「阻止单 exe 的四件事」中的第 ③ 件：

    web/ 前端文件需在运行时从磁盘读取
      → 构建期用 tools/embed_web.py 转成 C++ 字节数组编入 exe，
        HTTP 服务改为从内存响应

不嵌入的话，exe 旁边必须跟着一个 web/ 目录，拷到演示机器上少一个文件就白屏 ——
而单 exe 交付的全部意义就是让这类翻车在物理上不可能发生。

M0 只提供脚本，**不接入构建**。真正的接入发生在 M8 单 exe 打包阶段。

用法：
    python tools/embed_web.py
    python tools/embed_web.py --web-dir web --out-dir build/generated

脚本对空目录是安全的：web/ 里没有文件时生成合法的空表，不报错。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HEADER_NAME = "web_assets.h"
SOURCE_NAME = "web_assets.cpp"

HEADER_TEMPLATE = """\
// 由 tools/embed_web.py 自动生成，请勿手工编辑。
// 重新生成：python tools/embed_web.py
#pragma once

#include <cstddef>
#include <string_view>

namespace backup::web {

/// 一个被嵌入可执行文件的静态资源。
struct EmbeddedFile {
    std::string_view path;        ///< 相对 web/ 的路径，以 '/' 分隔
    const unsigned char* data;    ///< 文件内容（非空终止）
    std::size_t size;             ///< 字节数
};

/// 全部嵌入资源。web/ 为空时 kFiles 为 nullptr、kFileCount 为 0。
extern const EmbeddedFile* const kFiles;
extern const std::size_t kFileCount;

}  // namespace backup::web
"""

SOURCE_PREAMBLE = """\
// 由 tools/embed_web.py 自动生成，请勿手工编辑。
// 重新生成：python tools/embed_web.py
#include "{header}"

namespace backup::web {{
namespace {{
"""

# 注意：本模板**不经过 str.format()**（见 main 中直接写入），
# 因此花括号是字面量，不能像 SOURCE_EPILOGUE 那样写成 {{ }}。
SOURCE_EPILOGUE_EMPTY = """\
}  // namespace

// web/ 下没有文件（M0 的初始状态）。生成合法的空表而不是零长数组 ——
// C++ 不允许零长数组，这里若硬生成会编译失败。
const EmbeddedFile* const kFiles = nullptr;
const std::size_t kFileCount = 0;

}  // namespace backup::web
"""

SOURCE_EPILOGUE = """\
}}  // namespace

const EmbeddedFile kFileTable[] = {{
{table}
}};

const EmbeddedFile* const kFiles = kFileTable;
const std::size_t kFileCount = sizeof(kFileTable) / sizeof(kFileTable[0]);

}}  // namespace backup::web
"""

BYTES_PER_LINE = 12


def cpp_string_literal(text: str) -> str:
    """把路径转成 C++ 字符串字面量（转义反斜杠与引号）。"""
    escaped = text.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def cpp_identifier(relative_path: str) -> str:
    """由相对路径造一个合法的 C++ 标识符。"""
    identifier = "".join(
        ch if (ch.isalnum() or ch == "_") else "_" for ch in relative_path
    )
    if identifier and identifier[0].isdigit():
        identifier = "_" + identifier
    return "kData_" + identifier


def emit_byte_array(identifier: str, data: bytes) -> str:
    """生成 unsigned char 数组定义。

    用十六进制字节列表而不是字符串字面量：后者要处理转义，
    且 C++ 的 \\x 转义会贪婪地吞掉后续十六进制字符（"\\x41B" 不等于 'A'+'B'），
    是个容易埋雷的地方。生成的代码冗长些，但不会出错。
    """
    if not data:
        # C++ 不允许零长数组，而 0 字节文件本身是合法的（例如占位的 .nojekyll）。
        # 用一个字节占位，表里记录的长度是 0，因此这个字节不会被读到。
        return (
            "// 空文件：占位 1 字节；表中记录的长度为 0，该字节不会被读取。\n"
            f"const unsigned char {identifier}[] = {{0x00}};"
        )

    lines = [f"const unsigned char {identifier}[] = {{"]
    for offset in range(0, len(data), BYTES_PER_LINE):
        chunk = data[offset : offset + BYTES_PER_LINE]
        lines.append("    " + " ".join(f"0x{byte:02X}," for byte in chunk))
    lines.append("};")
    return "\n".join(lines)


def collect_files(web_dir: Path) -> list[tuple[str, bytes]]:
    """递归收集 web_dir 下的文件，返回 (相对路径, 内容) 列表。

    相对路径统一用 '/' 分隔，与 HTTP 请求路径一致。
    结果排序，保证同一份输入每次生成相同的输出 —— 否则构建不可复现。
    """
    if not web_dir.is_dir():
        return []

    files = []
    for path in sorted(web_dir.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(web_dir)
        # 跳过点文件。.gitkeep 这类是版本控制的占位物，不是前端资源；
        # 嵌进去不仅没意义，而且它通常是 0 字节，会生成零长数组
        # （C++ 不允许零长数组，编译直接失败）。
        if any(part.startswith(".") for part in relative.parts):
            continue
        files.append((relative.as_posix(), path.read_bytes()))
    return files


def main() -> int:
    parser = argparse.ArgumentParser(
        description="把 web/ 静态文件转成 C++ 源文件（单 exe 交付用）"
    )
    repo_root = Path(__file__).resolve().parent.parent
    parser.add_argument(
        "--web-dir",
        type=Path,
        default=repo_root / "web",
        help="前端目录（默认 <仓库>/web）",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=repo_root / "build" / "generated",
        help="生成目录（默认 <仓库>/build/generated）",
    )
    args = parser.parse_args()

    files = collect_files(args.web_dir)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    # 不调用 .format()：HEADER_TEMPLATE 里全是 C++ 的字面花括号，
    # 会被 format 当成字段名而抛 "unexpected '{' in field name"。
    (args.out_dir / HEADER_NAME).write_text(HEADER_TEMPLATE, encoding="utf-8")

    if not files:
        source = SOURCE_PREAMBLE.format(header=HEADER_NAME) + SOURCE_EPILOGUE_EMPTY
    else:
        arrays = "\n\n".join(
            emit_byte_array(cpp_identifier(rel), data) for rel, data in files
        )
        # 长度取 Python 侧的真实字节数，而不是 C++ 的 sizeof：
        # 空文件走的是 1 字节占位数组，sizeof 会得出 1，与真实长度不符。
        table = "\n".join(
            f"    {{{cpp_string_literal(rel)}, {cpp_identifier(rel)}, {len(data)}}},"
            for rel, data in files
        )
        source = (
            SOURCE_PREAMBLE.format(header=HEADER_NAME)
            + "\n"
            + arrays
            + "\n"
            + SOURCE_EPILOGUE.format(table=table)
        )

    (args.out_dir / SOURCE_NAME).write_text(source, encoding="utf-8")

    # Console output is deliberately ASCII: Windows consoles default to the
    # system code page (GBK on a Simplified Chinese system) and would render
    # Chinese here as mojibake. Python source itself is UTF-8, so the docstring
    # above is fine -- it is only the stdout path that is a problem.
    total_bytes = sum(len(data) for _, data in files)
    if not files:
        print(
            f"embed_web: no files under {args.web_dir}; "
            f"emitted an empty table so the build still compiles."
        )
    else:
        print(
            f"embed_web: embedded {len(files)} file(s), "
            f"{total_bytes} byte(s) total."
        )
    print(f"embed_web: output written to {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
