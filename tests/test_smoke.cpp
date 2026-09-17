// M0 接线自检
//
// 这些用例**不测业务逻辑**（M0 尚无业务逻辑），只确认两件事：
//   1. Catch2 v3 与 CTest 的 CMake 接线是通的；
//   2. backup_core 与六个 vcpkg 依赖的链接是通的。
//
// 第 2 点不是形式主义：Catch2WithMain 会把预先用 /MT 编译好的 Catch2 静态库
// 拉进本可执行文件。如果我们自己的代码用 /MD，这里就会报
// LNK2038: RuntimeLibrary 不匹配 —— 那正是 README §十一 列为
// 「链接失败」级风险、SETUP.md §6 专门写了排查步骤的那个坑。
// 让它在这里失败，比等到 M2 用压缩功能时才炸出来便宜得多。
//
// ⚠️ TEST_CASE 的名字一律用 ASCII，理由见本目录 CMakeLists.txt 顶部说明。

#include <catch2/catch_test_macros.hpp>

#include "version.h"

TEST_CASE("backup_core links and reports a version", "[m0]") {
    CHECK_FALSE(backup::version().empty());
}

TEST_CASE("all third-party libraries are linked", "[m0]") {
    const auto libraries = backup::linked_libraries();

    // zlib / lz4 / zstd / openssl / nlohmann-json
    REQUIRE(libraries.size() == 5);

    for (const auto& library : libraries) {
        INFO("library: " << library.name);  // 失败时指出是哪个库
        CHECK_FALSE(library.name.empty());
        CHECK_FALSE(library.version.empty());
    }
}
