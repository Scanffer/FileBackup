# 单元测试。测试文件用 GLOB 收集：各模块 agent 新增 tests/test_*.cpp 后
# 下次配置会自动纳入，不需要改这个文件。
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
)

add_executable(backup_tests ${TEST_SOURCES})

target_link_libraries(backup_tests
    PRIVATE backup_core
    PRIVATE fb_catch2
)

target_include_directories(backup_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/core
)

add_test(NAME backup_tests COMMAND backup_tests)
