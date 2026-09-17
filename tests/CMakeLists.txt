# 单元测试（Catch2 v3）
#
# ⚠️ SETUP.md §6：TEST_CASE 的名字一律用 ASCII。
# Catch2 的中文测试名经 Windows ANSI 代码页往返后变乱码，
# 会导致 ctest 报 No test cases matched —— 现象是连最简单的用例都「失败」，
# 极易误判成框架没配好。中文请写在注释与断言消息里。

add_executable(fb_tests
    test_smoke.cpp
)

target_link_libraries(fb_tests
    PRIVATE
        backup_core
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(fb_tests)
