# FileBackup.exe —— 命令行入口（供脚本调用 / 自动化测试，接口见 README §7.1）

add_executable(FileBackup
    main.cpp
)

target_link_libraries(FileBackup
    PRIVATE backup_core
)

# 静态 triplet 下运行库已由 CMAKE_MSVC_RUNTIME_LIBRARY 统一为 /MT，
# 产物应为单 exe，无需随附任何 DLL（README §3.4）。
set_target_properties(FileBackup PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
