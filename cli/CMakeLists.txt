# FileBackup.exe —— 命令行入口（供脚本调用 / 自动化测试，接口见 README §7.1）

add_executable(FileBackup
    main.cpp
)

target_link_libraries(FileBackup
    PRIVATE backup_core
)

# 产物应为单 exe，无需随附任何 DLL（README §3.4）。
# 由根 CMakeLists.txt 里 MINGW 分支的 -static 系列开关保证。
set_target_properties(FileBackup PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
