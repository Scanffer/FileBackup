# FileBackup-cli.exe —— 命令行入口（控制台子系统）
#
# 和 GUI 分开成两个程序：
#   FileBackup.exe      gui/   链接 backup_server，-mwindows，无控制台
#   FileBackup-cli.exe  cli/   只链接 backup_core，控制台，可被脚本调用
#
# 这样双击 GUI 不会弹终端；CLI 的 stdout/stderr 仍然是标准控制台语义，
# 管道、重定向、`$LASTEXITCODE` 都正常。两者共享同一套备份引擎。

add_executable(FileBackupCli
    main.cpp
)

target_link_libraries(FileBackupCli
    PRIVATE backup_core
)

if(WIN32)
    # ShellExecuteW：CLI 的 `gui` 子命令用它拉起同目录的 FileBackup.exe。
    target_link_libraries(FileBackupCli PRIVATE shell32)
endif()

# -municode：用 wmain 而不是 main，拿到 UTF-16 参数（中文路径）。
if(MINGW)
    target_link_options(FileBackupCli PRIVATE -municode)
endif()

# 产物和 GUI 放在同一目录，方便互相同目录查找。
set_target_properties(FileBackupCli PROPERTIES
    OUTPUT_NAME "FileBackup-cli"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
