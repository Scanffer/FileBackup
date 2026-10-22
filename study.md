# FileBackup 技术栈总结（study.md）

> 本文档记录 **当前实际代码** 的技术栈、架构、关键实现与已知限制，用于学习、答辩准备和后续维护。
> 项目版本：0.1.0（CMake `project(FileBackup VERSION 0.1.0)`）
> 最近代码状态：`7f2cc67 feat(gui): 方案B 1040×720 双栏工作台 + 单列回退`
> 更新日期：2026-10-22

---

## 1. 项目定位与交付形态

FileBackup 是一个 **Windows 本地数据备份软件**：

- 目录镜像备份 / 还原
- 单容器 `.vbak` 打包 / 解包
- 自研压缩（DEFLATE / LZ4 / RLE / LZW）
- 自研加密（AES-256-CBC / ChaCha20-Poly1305 / RC4）
- 六维自定义筛选
- Windows 元数据保存 / 还原
- CLI + GUI + 本地 REST API + 可嵌入 WebView2 前端

交付为 **两个独立的静态 exe**：

| 程序 | 子系统 | 入口 | 用途 |
|---|---|---|---|
| `FileBackup.exe` | Windows GUI | `wWinMain`（GUI） | 图形界面、WebView2 窗口、HTTP 服务 |
| `FileBackup-cli.exe` | Windows CUI | `wmain`（控制台） | 命令行备份/还原/scan/list |

两者共享 `backup_core`，都不依赖 MinGW 运行库 DLL；GUI 的工具链额外把 `WebView2Loader.dll` 编进资源段。

---

## 2. 技术栈总览

| 层面 | 技术 / 组件 |
|---|---|
| 主语言 | **C++17**（`CMAKE_CXX_EXTENSIONS OFF`） |
| C 依赖编译 | C11（zstd 等第三方 C 库） |
| 构建系统 | **CMake 4.4.3**（最低 3.20）+ **Ninja 1.13.2** |
| 编译器 | **MinGW-w64 GCC 15.1.0**（x86_64-w64-mingw32，SEH） |
| 链接 | `-static -static-libgcc -static-libstdc++`，静态 CRT/运行库 |
| GUI 内核 | **Microsoft WebView2**（Chromium/Edge 内核） |
| GUI 宿主 | 原生 Win32；无边框窗口、自绘标题栏、DWM 深色边框 |
| 前端 | 原生 **HTML + CSS + JavaScript**（无框架、无 npm、无 CDN） |
| HTTP 库 | **cpp-httplib 0.54.1**（单头文件） |
| JSON 库 | **nlohmann/json 3.11.3**（单头文件） |
| 测试框架 | **Catch2 2.13.10** + CTest |
| 第三方压缩（备用） | zstd 1.5.7（源码 vendored，静态链接，但当前未开放为可选算法） |
| WebView2 SDK | 1.0.4258.31（vendored 头文件 + Loader DLL） |
| 包管理 | **无 vcpkg / Conan**；第三方源码直接 vendored 进 `third_party/` |
| 目标平台 | Windows 10/11 x64（WebView2 Runtime 需系统已安装；Win11 自带） |
| 数据库 | 无 |
| 网络服务 | 仅本机 loopback HTTP：`127.0.0.1`，端口 8080 起自动顺延 |

---

## 3. 构建工具链与工程组织

### 3.1 CMake 层级

```text
CMakeLists.txt（根，C/C++ 标准、MinGW 检查、静态链接、子目录顺序）
├── third_party/      INTERFACE / zstd 静态库
├── core/             backup_core（静态库，备份引擎）
├── server/           backup_server（静态库，HTTP + 任务 + WebView2）
├── cli/              FileBackup-cli.exe
├── gui/              FileBackup.exe（Windows GUI，资源段 + WebView2Loader）
└── tests/            backup_tests.exe（Catch2）
```

### 3.2 关键 CMake 技术点

- **MinGW 强校验**：如果 CMake 选中 MSVC，直接 `FATAL_ERROR`，避免链接阶段才暴露奇怪错误。
- **静态链接**：
  ```cmake
  add_link_options(-static -static-libgcc -static-libstdc++)
  ```
  目标是“拷贝 exe 到干净机器即可运行”。
- **`-municode` 只给 CLI**：CLI 用 `wmain` 拿 UTF-16 参数；测试/其他程序仍用普通 `main`，避免入口冲突。
- **GUI 子系统**：
  ```cmake
  target_link_options(FileBackupGui PRIVATE -mwindows -municode)
  ```
  GUI 无控制台窗口；CLI 保持 console 子系统，stdout 可被脚本捕获。
- **windres 资源嵌入**：`gui/CMakeLists.txt` 把 `WebView2Loader.dll` 编成 `RCDATA` 资源对象，再链入 GUI exe；运行时释放到 `%TEMP%` 并加载。
- **`compile_commands.json`**：给 VS Code C/C++ 插件使用。
- **测试文件 GLOB**：`tests/CMakeLists.txt` 用 `file(GLOB CONFIGURE_DEPENDS test_*.cpp)`，新增测试自动纳入。

### 3.3 构建命令

```powershell
cmake --preset mingw-release
cmake --build --preset mingw-release
ctest --test-dir build/mingw-release --output-on-failure
```

构建产物：

```text
build/mingw-release/FileBackup.exe
build/mingw-release/FileBackup-cli.exe
build/mingw-release/tests/backup_tests.exe
```

---

## 4. 总体架构

```text
┌──────────────────────────── FileBackup.exe（一个进程）────────────────────────────┐
│                                                                                   │
│  WebView2 / 浏览器                                                               │
│    HTML + CSS + JS（内嵌在 server/web_page.h）                                    │
│        │ fetch / EventSource（REST + SSE）                                        │
│        ▼                                                                          │
│  backup_server（cpp-httplib，127.0.0.1）                                          │
│        │ 直接函数调用                                                             │
│        ▼                                                                          │
│  backup_core                                                                      │
│    ├─ fs_utils：目录扫描 / UTF-8 路径工具                                         │
│    ├─ backup：目录镜像备份 / 还原                                                 │
│    ├─ pack：VBAK 容器打包 / 解包 / 归档信息                                       │
│    ├─ compress：DEFLATE / LZ4 / RLE / LZW                                         │
│    ├─ crypto：SHA-256 / HMAC / PBKDF2 / AES / ChaCha20-Poly1305 / RC4             │
│    ├─ filter：六维筛选                                                             │
│    ├─ metadata：Windows 元数据 + ACL                                               │
│    └─ archive：统一归档接口（当前仍为骨架，GUI/CLI 直接使用 pack/backup）          │
│                                                                                   │
└───────────────────────────────────────────────────────────────────────────────────┘

FileBackup-cli.exe（另一个进程）
  参数解析 ──直接调用──> backup_core（不经过 HTTP）
```

**分层原则：**

- `core` 不知道 HTTP/UI，也不依赖前端概念；
- `server` 只负责 HTTP/任务/WebView2 宿主，不包含备份逻辑；
- `cli` 只做参数解析与输出；
- GUI 与 CLI 共享同一个 core。

**进程模型：**

- GUI 启动时先启动 HTTP 服务（后台线程），再进入窗口消息循环；
- WebView2 打开 `http://127.0.0.1:<port>/`；
- 前端资源在内存中由 `GET /` 返回，不读磁盘 web 目录；
- 备份/还原请求进入单 worker 异步任务队列，SSE 推送进度。

---

## 5. 核心模块与技术细节

### 5.1 文件系统与路径（core/fs_utils）

- 使用 `std::filesystem` 递归扫描，`skip_permission_denied` 容错。
- **UTF-8 / UTF-16 处理**：
  - Windows 下用 `wmain` 获取 UTF-16 参数；
  - 内部路径用 `std::filesystem::path`（宽字符构造）；
  - 归档/manifest/HTTP 中统一存 UTF-8；
  - `path_from_u8()` 专门处理 UTF-8 → path，避免 MinGW 按 GBK 解释中文路径。
- 条目按相对路径排序，保证归档可复现。

### 5.2 VBAK 归档格式

自研单容器格式，当前主写 **v4**，兼容读取 v1/v2/v3/v4。

**v4 Header 64 字节：**

```text
magic "VBAK"        4
version u16 = 4
flags u16           bit0=加密, bit1=条目带元数据
comp_id u8          归档默认压缩算法
enc_id u8           加密算法
reserved u16
entry_count u32
data_off u32
kdf_iterations u32  PBKDF2 迭代次数（未加密时 0）
salt u8[16]         PBKDF2 随机盐
reserved2 u8[24]
```

**每个 Entry：**

```text
path_len u16 + path（UTF-8）
orig_size u64
stor_size u64（含 AEAD tag / CBC padding）
crc32 u32（原始内容）
comp_id u8（该条目实际使用的压缩算法）
[若加密] iv u8[16]（实际使用前 iv_size 字节）
meta_len u32 + MetaBlock（metadata_to_bytes 的版本化二进制）
data stor_size 字节
```

**关键约定：**

- 每条目先压缩、再加密；每条目独立 IV；
- 密码不写入归档；PBKDF2 派生一次密钥供所有条目使用；
- 不可压缩数据回退到原样存储（`comp_id=0`）；
- 解包校验路径，拒绝绝对路径和 `..`（zip-slip 防护）；
- CRC32 校验原始内容；
- 解压时 `expected_size` 必须准确，防止越界/炸弹。

### 5.3 压缩算法（全部自研）

| 算法 | 实现要点 |
|---|---|
| **DEFLATE** | LZ77 32KB 窗口 + 3 字节哈希链/贪心；固定 Huffman；zlib 封装 + Adler-32。inflate 支持 stored/fixed/dynamic Huffman；级别 1–9（默认 6） |
| **LZ4** | 标准 LZ4 block 格式：token、255 扩展、2 字节小端 offset、末尾 literal 序列；4KB 哈希表 + 贪心匹配 |
| **RLE** | 自定义 packet：控制字节 `<0x80` 为 literal run，`>=0x80` 为 repeat run |
| **LZW** | 9→16 位可变码长，CLEAR/END，16 位字典满重置，LSB 优先打包，支持 KwKwK |

接口统一为 `ICompressor`：

```cpp
class ICompressor {
    virtual CompressionId id() const = 0;
    virtual Bytes compress(const Bytes& input, int level, std::string& error) = 0;
    virtual Bytes decompress(const Bytes& input, std::size_t expected_size,
                             std::string& error) = 0;
};
```

注册表 `make_compressor()` 提供 `deflate / lz4 / rle / lzw`；`none` 表示不压缩。
**zstd**：源码已 vendored 并静态链接，但工厂未实现，`/api/algorithms` 中 `available=false`，属于备用/扩展算法。

### 5.4 加密算法（全部自研）

| 算法 | 说明 |
|---|---|
| **SHA-256** | 自研，含长度边界/标准测试向量 |
| **HMAC-SHA256** | RFC 4231 测试向量 |
| **PBKDF2-HMAC-SHA256** | 密钥派生，VBAK 使用 100000 次迭代 + 16 字节随机盐；标准向量验证 |
| **AES-256-CBC** | 自研 AES-256、CBC、PKCS#7；NIST SP 800-38A 向量 |
| **ChaCha20-Poly1305** | RFC 8439；tag 附在密文尾，解密前常数时间验签 |
| **RC4** | 自研；用 `SHA256(key||iv)` 生成流密钥；无认证，靠 CRC32 兜底 |

其他：

- `random_bytes` 用 Windows **BCryptGenRandom**（仅随机源，不属于算法库）；
- 不依赖 OpenSSL、libsodium 或 Windows CNG 的 AES/ChaCha 实现；
- 密码只在内存中传给 `PackOptions` / `UnpackOptions`，不写日志、不进任务 JSON。

**已知取舍：**

- RC4 无认证：错误密码/篡改可能静默解出垃圾，随后 CRC32 失败；
- AES-CBC 无独立 MAC：错密钥主要靠 PKCS#7 填充 + CRC32 兜底；
- ChaCha 生产路径无 AAD：tag 不覆盖归档头/IV 元信息；
- VBAK 解包失败会抛异常，CLI 返回非零退出码。

### 5.5 六维筛选（core/filter）

规则以 JSON 数组下发：

```json
[
  {"action":"include", "path":"docs/**", "type":"pdf,docx", "size":"< 50MB"},
  {"action":"include", "path":"pics/**", "type":"category:image", "time":"last 30 days"},
  {"action":"exclude", "name":"~$*"},
  {"action":"exclude", "path":"**/node_modules/**"}
]
```

语义：

- 一条规则内多个维度 **AND**；
- 同一维度多值 **OR**；
- 先 include 后 exclude；没有 include 规则时默认全选；
- empty filter 恒 true。

支持：

| 维度 | 能力 |
|---|---|
| path | glob `*` `?` `**`；路径统一 `/`；Windows 大小写不敏感 |
| type | 扩展名列表；`category:image/video/audio/document/archive/code/text/executable/font` |
| name | 文件名 glob |
| time | `last N s/m/h/d/w`、`before/after YYYY-MM-DD`、`A..B` |
| size | `< <= > >= =`、`A..B`，单位 B/KB/MB/GB/TB（1024 进制） |
| user | `uid:N`、`owner:name`、裸名（Windows 下用 owner 账户名） |

集成情况：

- 容器打包：扫描后过滤，未命中文件不读取/压缩/加密；
- 镜像备份：过滤后复制，被排除项不写入目标；
- CLI：`--filter '<JSON>'` / `--filter-file rules.json`；
- REST：`filter` 参数（JSON 字符串）；
- GUI：简单规则行 + JSON 高级模式双向同步。

### 5.6 元数据（core/metadata）

使用 Windows 安全/文件 API：

- `mode` / 只读属性映射
- `owner` / `group`（`GetNamedSecurityInfo` + `LookupAccountSid`）
- `mtime` / `atime` / `ctime`（FILETIME 100ns → Unix ns）
- Windows 文件属性（隐藏/只读/归档等）
- ACL：`GetFileSecurityW` / `SetFileSecurityW`，自相对安全描述符原始字节

存储：

- VBAK v4 每条目 **MetaBlock**（`metadata_to_bytes` 版本化二进制）；
- 目录镜像写入 `.backup-manifest.json`（逐文件元数据 + JSON）；
- 还原时按 MetaBlock / manifest 应用；权限不足或跨机 ACL 失败降级为 warning，不中断。

**限制：**

- 目录本身/空目录元数据当前不完整；
- ACL 跨机/权限不足时 best-effort；
- 非 NTFS 文件系统时间戳精度可能只有 2 秒。

### 5.7 HTTP 服务与异步任务（server）

技术：

- **cpp-httplib 0.54.1**：单头文件 HTTP 服务，监听 loopback；
- **nlohmann/json 3.11.3**：请求/响应/任务状态/筛选规则/归档信息；
- **C++11 线程 / mutex / condition_variable**：单 worker 任务队列；
- **SSE（Server-Sent Events）**：任务进度推送。

主要端点：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 内嵌 HTML 页面 |
| GET | `/api/status` | 服务状态/版本/任务数 |
| GET | `/api/algorithms` | 压缩/加密/打包算法及级别范围 |
| GET | `/api/archive/info?path=` | VBAK 头信息/条目数/算法/加密/元数据 |
| POST | `/api/backup` | 异步备份（容器/镜像）→ `task_id` |
| POST | `/api/restore` | 异步还原 → `task_id` |
| GET | `/api/tasks` | 任务列表 |
| GET | `/api/tasks/{id}` | 单任务快照 |
| GET | `/api/tasks/{id}/stream` | SSE 进度（`data: JSON\n\n`） |
| GET | `/api/fs/list?path=` | 目录树数据源（盘符/目录/文件） |

任务模型：

- 单 worker 串行执行所有备份/还原（避免并发写同一目标）；
- 状态：`queued → running → succeeded/failed`；
- `progress` 来自 core 的 `ProgressFn`（每处理完一个条目上报），完成 100；
- 密码只在内存，任务 JSON/错误信息不包含密码。

### 5.8 GUI（WebView2 + 前端）

- **宿主**：Win32 无边框窗口 + WebView2
  - 自绘标题栏（最小化/最大化/关闭、拖拽）
  - DWM 深色边框；`PerMonitorV2` DPI 感知
  - 原生 `IFileOpenDialog` / `IFileSaveDialog` 文件选择
  - `PageCommandHandler` 处理页面命令（`window:*`、`pick:*`、`shell:open-folder`）
  - WebView2Loader.dll 从资源段释放并加载
- **前端**：原生 HTML/CSS/JS（内嵌 raw string）
  - 深色 VS Code 风格；无框架、无 CDN、无外部资源
  - 备份/还原 Tab；异步任务；真实进度；SSE/轮询降级
  - 方案 B：1040×720 双栏工作台，`≥1000px` 两栏独立滚动，`<1000px` 回退单栏
  - 高级选项默认折叠；筛选简单规则/JSON 双模式；目录树；打开目标文件夹/复制路径
  - 运行中禁用表单；结果显示条目数/字节数/目标路径
- 默认窗口：**1040×720 DIP**，最小 **900×600**；工作区不足时自动夹到 90% 并居中。

### 5.9 CLI（FileBackup-cli.exe）

- 控制台子系统，`wmain` 获取 UTF-16 参数；
- 启动时 `SetConsoleOutputCP(CP_UTF8)`，中文输出正常；
- 命令：
  - `backup`：镜像 / `--pack vbak`、`--compress`、`--level`、`--encrypt`、`--password`、`--filter`、`--filter-file`、`--no-metadata`
  - `restore`：`--archive` / `--src`、`--password`、`--no-metadata`
  - `list --archive <vbak>`：格式/算法/加密/元数据 + 逐条目列表
  - `scan`、`gui`、`--version`、`--help`
- `verify` 尚未实现。

---

## 6. 第三方依赖（全部 vendored）

| 依赖 | 版本 | 许可证（上游） | 用途 |
|---|---|---|---|
| cpp-httplib | 0.54.1 | MIT | HTTP 服务 |
| nlohmann/json | 3.11.3 | MIT | JSON |
| Catch2 | 2.13.10 | BSL-1.0 | 单元测试 |
| zstd | 1.5.7 | BSD/GPLv2 双许可 | 备用压缩库；静态链接，但当前工厂未实现 |
| WebView2 SDK | 1.0.4258.31 | 微软 WebView2 SDK 许可 | WebView2 头文件 + Loader DLL |
| WebView2 Runtime | 系统安装 | Microsoft | Win11 自带；Win10 需安装 |

构建不依赖 vcpkg/Conan；`third_party/` 直接放源码/头文件。第三方 target 都用 INTERFACE/静态库显式隔离，避免大头上污染全局编译。

---

## 7. 测试与质量

- **Catch2 + ctest**：`tests/test_*.cpp`，共 **88 个测试用例 / 10164 条断言**（当前构建）。
- 覆盖：
  - DEFLATE / LZ4 / RLE / LZW 往返、边界、随机/损坏数据
  - SHA/HMAC/PBKDF2/AES/ChaCha/RC4 标准测试向量、错误密码、篡改
  - VBAK 打包/解包、加密、筛选、元数据、inspect_archive、截断归档
  - 镜像备份/还原、manifest、元数据
  - 六维筛选语义与 JSON 循环
- `tests/test_tmp.h`：`%TEMP%` 不可写时回退到 `.filebackup_test_tmp`（受 Windows 安全软件/沙箱影响的兜底）。
- 额外端到端验证（人工/脚本）：
  - CLI 5 压缩模式 × 4 加密模式 = 20 组矩阵
  - HTTP 异步任务 + SSE + 加密 + 筛选 + 元数据
  - GUI fake DOM 回归 + headless 截图 + 真实 WebView2 窗口（从外部部署目录启动）

---

## 8. 目录结构

```text
FileBackup/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── SETUP.md
├── core/                 # backup_core
│   ├── algorithm.h       # 算法接口/ID/注册表
│   ├── types.h           # Bytes/EntryType 等
│   ├── fs_utils.*        # 扫描/UTF-8 路径
│   ├── backup.*          # 目录镜像备份/还原
│   ├── pack.*            # VBAK 打包/解包/归档信息
│   ├── compress.cpp      # DEFLATE/LZ4/RLE/LZW
│   ├── crypto.cpp        # 哈希/对称加密/PBKDF2/Base64
│   ├── filter.*          # 六维筛选
│   ├── metadata.*        # Windows 元数据
│   ├── progress.h        # Progress/ProgressFn
│   ├── archive.*         # 统一归档接口（骨架）
│   └── version.*
├── server/               # backup_server
│   ├── http_server.*     # REST 路由
│   ├── tasks.*           # 异步任务管理器
│   ├── web_page.h        # 内嵌前端（HTML/CSS/JS）
│   ├── webview_host.*    # Win32 + WebView2 宿主
│   └── CMakeLists.txt
├── cli/                  # FileBackup-cli.exe
├── gui/                  # FileBackup.exe（GUI 入口 + windres）
├── tests/                # Catch2 测试
├── third_party/          # httplib/json/catch2/zstd/webview2
└── build/                # 构建产物（gitignore）
```

---

## 9. 运行与部署注意事项

### 9.1 运行依赖

- Windows 10/11 x64；
- GUI 需要 **WebView2 Runtime**（Win11 自带；Win10 需安装）；
- 两个 exe 都不依赖 MinGW DLL（`libstdc++-6.dll`、`libgcc_s_seh-1.dll`、`libwinpthread-1.dll` 都不需要）；
- WebView2Loader.dll 已嵌入 GUI exe。

### 9.2 未签名 exe 与 SmartScreen

学生项目没有代码签名证书，首次运行可能弹：

> Windows 已保护你的电脑：阻止了无法识别的应用启动

处理：点「更多信息」→「仍要运行」；或在部署机器上先手动放行。

### 9.3 DSH 工作区 Low 完整性注意

当前开发环境（DSH）会给工作区目录打 **Low Mandatory Integrity Label**：

```text
Mandatory Label\Low Mandatory Level:(OI)(CI)(NW)
```

从该目录启动的进程可能是 Low 完整性，WebView2 会失败并返回 `0x800700AA`。**不要把运行物直接放在 DSH 工作区 `build/` 里运行**；构建后复制到普通目录（如 `D:\FileBackupDeploy` 或桌面），从那里启动。源码可以继续放工作区。

---

## 10. 关键设计取舍

| 取舍 | 原因 |
|---|---|
| 压缩/加密全部自研 | 课程大纲规定“第三方库直接实现的功能扩展分按 50% 计”；自研可拿满 |
| VBAK 而不是 tar | 需要逐条目算法/加密/元数据/CRC，tar 不适合 |
| MinGW 而不是 MSVC | 课程环境要求/可用性；静态链接 + vendored 依赖，避免 DLL 问题 |
| WebView2 而不是 Qt | 无 Qt 依赖；HTML/CSS 做深色 UI 更灵活；Win11 自带运行时 |
| HTTP 服务内置 GUI 进程 | 本地单用户，避免独立后端/Node/CORS/端口管理；前端资源可内嵌 |
| 两个 exe | GUI 无控制台、CLI 可脚本化；比“一个 console exe 再隐藏控制台”语义更干净 |
| 单 worker 异步任务 | 文件操作串行化，避免并发写同一目标；状态可推送 |
| 把 WebView2Loader 嵌入资源段 | 保持“两个 exe 无外部 DLL”的交付形态 |

---

## 11. 已知限制 / 待完善

### 功能

- CLI `verify` 尚未实现；`list` 已实现。
- `archive.cpp` 仍是统一归档接口骨架，GUI/CLI 直接调用 `pack`/`backup`，没有完全收口。
- 空目录与目录本身元数据：容器模式只存文件；镜像用 `list_files`，空目录不保留。
- USTAR/tar 打包、定时备份、实时备份、网络备份、特殊文件类型未实现（P1）。
- zstd 已 vendored 但未接入 `make_compressor`，GUI/API 不可选。

### 健壮性 / 体验

- WebView2 初始化失败目前静默退回浏览器；GUI 子系统无控制台，用户可能以为“没反应”。
- WebView2 用户数据目录固定为 `%LOCALAPPDATA%\FileBackup\WebView2`，多实例/旧版可能争抢。
- 任务 `progress` 按条目数递增；大文件内部没有更细粒度进度。
- 任务不可取消；关闭窗口会等待当前任务结束。
- 大文件按整体读入内存，未流式处理。
- RC4 无认证、AES-CBC 无 MAC、ChaCha 无 AAD（见 5.4）。
- ACL/属主跨机还原可能降级为 warning；非 NTFS 时间戳精度有限。

### 工程

- 版本号仍为 0.1.0，未做最终版本 bump。
- README/SETUP 部分内容仍在向“单 exe / 旧算法计划”迁移，需要最终同步。
- `cpplint`/编译警告清理、从零干净构建验收尚未做。
- 第三方依赖版本在本文档记录，但未生成正式 `THIRD_PARTY_NOTICES`。

---

## 12. 学习 / 答辩重点

建议按这些主题准备：

1. **架构分层**：为什么 core/server/cli/gui 这样拆；为什么 GUI 里跑 HTTP 服务。
2. **两个 exe**：GUI/CLI 的子系统、入口（`wWinMain`/`wmain`）、如何共享 core；为什么要拆。
3. **VBAK 格式**：能画出 v4 header/entry 布局；解释 CRC、压缩、加密、元数据、IV/salt 的位置。
4. **DEFLATE 原理**：LZ77 + Huffman、zlib 封装；LZ4/RLE/LZW 的差异。
5. **加密原理**：AES-CBC/ChaCha20-Poly1305/RC4 的差异、PBKDF2、随机盐/IV、认证 tag。
6. **筛选语义**：规则内 AND、同维度 OR、include/exclude、glob、时间/尺寸表达式。
7. **元数据/ACL**：Windows API、VBAK v4 MetaBlock、镜像 manifest、权限不足降级。
8. **并发与进度**：单 worker 任务队列、SSE、`ProgressFn`、线程安全。
9. **WebView2/HTTP**：页面怎么加载、REST 端点、为什么不需要 Node/Vite。
10. **安全边界**：未签名 exe/SmartScreen、Windows 完整性级别、zip-slip、密码不落盘。

---

## 13. 参考文件

- [CMakeLists.txt](CMakeLists.txt)
- [CMakePresets.json](CMakePresets.json)
- [core/algorithm.h](core/algorithm.h)
- [core/pack.h](core/pack.h)
- [core/filter.h](core/filter.h)
- [core/metadata.h](core/metadata.h)
- [server/http_server.cpp](server/http_server.cpp)
- [server/tasks.h](server/tasks.h)
- [server/webview_host.cpp](server/webview_host.cpp)
- [server/web_page.h](server/web_page.h)
- [cli/main.cpp](cli/main.cpp)
- [gui/main.cpp](gui/main.cpp)
- [tests/](tests/)
- [third_party/CMakeLists.txt](third_party/CMakeLists.txt)
- [third_party/webview2/README.md](third_party/webview2/README.md)
