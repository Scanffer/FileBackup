# FileBackup.exe —— 唯一入口，CLI 与 Web GUI 共用（README §7.1 / §3.4）
#
# 无参数双击 → 拉起 Web GUI；带子命令 → 走 CLI。
# GUI 的服务端逻辑在 backup_server 里，这里只是把它链进来 ——
# 所以最终仍然只有一个 exe，不需要额外分发任何文件。

add_executable(FileBackup
    main.cpp
)

target_link_libraries(FileBackup
    PRIVATE backup_core
    PRIVATE backup_server
)

# 产物应为单 exe，无需随附任何 DLL（README §3.4）。
# 由根 CMakeLists.txt 里 MINGW 分支的 -static 系列开关保证。
set_target_properties(FileBackup PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
