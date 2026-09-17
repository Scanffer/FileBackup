# 开发环境配置清单

> 文件备份软件 · 软件开发综合实验
> 本文档的目标：**任何一位组员（或助教）拿到这份清单，能从零把项目编译起来。**
>
> 所有版本号均已核实，可直接复制执行。最后核对日期：2026-09-10。

---

## 0. 快速开始（已装好环境的人看这里）

```bash
git clone <仓库地址> && cd FileBackup
. ./tools/devshell.ps1          # 进入 MSVC 编译环境（见 §3.5）
cmake --preset x64-static       # 配置（首次会自动下载编译依赖，约 10-20 分钟）
cmake --build --preset x64-release
./build/x64-static/FileBackup.exe --help
```

> `tools/devshell.ps1` 与 `CMakePresets.json` 由 M0 阶段创建。本文档 §3.5 与 §4 给出了它们的完整内容。

---

## 1. 工具清单

### 1.1 必需工具

| # | 工具 | 最低版本 | 本项目实测版本 | 用途 | 安装方式 |
|---|---|---|---|---|---|
| 1 | **MSVC** (VS Build Tools) | v143 (VS 2022) | **14.51 / VS 2026 v18** | C++ 编译器与链接器 | 见 §3.2 |
| 2 | **CMake** | 3.20 | **4.4.3** | 构建系统生成器 | §3.1 |
| 3 | **Ninja** | 1.10 | **1.13.2** | 构建系统（执行编译） | §3.1 |
| 4 | **vcpkg** | 锁定 commit | **`3723ec1`** | 第三方库管理 | `git clone`（见 §3.3） |
| 5 | **Git** | 2.30 | **2.53.0** | 版本控制 + 拉取 vcpkg | `winget install Git.Git` |
| 6 | **Python** | 3.8 | **3.14.7** | vcpkg 构建脚本 + `embed_web.py` | `winget install Python.Python.3.14` |

> **MSVC 版本说明**：VS 2022 (v143) 与 VS 2026 (v18, MSVC 14.5x) **均可**，本项目只要求 C++17，两者都完整支持。已有任一版本即可，不必重装。

**为什么 Python 是必需项**：不只是 vcpkg 的部分 port 用 Python 写构建脚本，本项目自己的 `tools/embed_web.py`（把前端编入 exe，见 README §3.4）也是 Python。**没有 Python 无法完成构建。**

### 1.2 可选工具（不影响构建，但强烈建议）

| 工具 | 用途 | 安装方式 |
|---|---|---|
| **VS Code** | 编辑器 | 已装 |
| ↳ **C/C++ 扩展** (`ms-vscode.cpptools`) | 补全、跳转、调试 | VS Code 扩展市场 |
| ↳ **CMake Tools** (`ms-vscode.cmake-tools`) | 在 IDE 内配置/构建/调试，并可视化 Catch2 测试结果 | VS Code 扩展市场 |
| ↳ **PowerShell** (`ms-vscode.powershell`) | 编辑 `tools/*.ps1` 脚本时语法高亮 | VS Code 扩展市场 |
| **Doxygen** | 从注释生成 API 文档（系统设计文档加分项） | `winget install Doxygen.Doxygen` |
| **PlantUML** 或 **draw.io** | 绘制用例图/类图/顺序图（三份文档必需） | PlantUML 需 Java；draw.io 为网页版 |

### 1.3 磁盘与系统要求

| | 需求 | 说明 |
|---|---|---|
| 操作系统 | Windows 10 21H2+ / Windows 11 | WebView2 运行时在 Win11 为系统自带 |
| 磁盘 | **约 10 GB 可用** | VS Build Tools 3–6 GB + vcpkg 及依赖 3–5 GB |
| 内存 | 8 GB+ | 编译 OpenSSL 时较吃内存 |

> ⚠️ **C 盘空间紧张的同学请注意**：vcpkg 本体与编译产物体积不小。建议按 §3.3 装到非系统盘（如 `D:\Development_Software\vcpkg`），并通过 §3.4 把二进制缓存也指向同一盘。

---

## 2. 第三方库清单

### 2.1 由 vcpkg 自动管理

**不要在仓库里提交这些库的源码**，它们由 `vcpkg.json` 声明、CMake 自动获取：

| 库 | 用途 | 对应扩展分 | 档位 | 在本项目中的角色 |
|---|---|---|---|---|
| **zlib** | DEFLATE 压缩 | 压缩解压 10 | **必选** | 通用压缩，压缩率高 |
| **lz4** | LZ4 压缩 | 压缩解压 10 | **必选** | 极快，适合大文件场景 |
| **zstd** | Zstandard 压缩 | 压缩解压 10 | **必选** | 压缩级别可调（1–19） |
| **openssl** | AES-256-CBC<br>ChaCha20-Poly1305<br>PBKDF2-SHA256 | 加密解密 20 | **必选** | 工业级加密与密钥派生 |
| **nlohmann-json** | JSON 解析 | — | **必选** | 前端通信、筛选规则下发、manifest 读写 |
| **catch2** | 单元测试框架 | — | **必选** | 测试报告（20 分）的支撑 |

> **这 6 个库一个都不能少。** 压缩与加密是必选功能，JSON 与测试框架贯穿必选部分全程。唯一可选的是 §2.2 中的 `webview`。

### 2.2 随仓库分发的单头文件库

放进 `third_party/`，**需要提交到 Git**（体积小、无构建步骤）：

| 库 | 版本 | 文件 | 档位 | 用途 |
|---|---|---|---|---|
| **cpp-httplib** | `v0.54.1` | `httplib.h` | **必选** | HTTP 服务，单头文件 |
| **webview** | `0.12.0` | `webview.h` | 可选（M9） | 桌面外壳，**项目后期再拉取，前期不需要** |
| **utf8**（如需要） | 最新 | `utf8.h` | 按需 | 路径 UTF-8 与宽字符互转 |

下载地址：
- cpp-httplib: <https://github.com/yhirose/cpp-httplib/releases/tag/v0.54.1>
- webview: <https://github.com/webview/webview/releases/tag/0.12.0>

---

## 3. 安装步骤

### 3.1 CMake 与 Ninja

装 **CMake ≥ 3.20**、**Ninja ≥ 1.10**，并保证两者都**在 PATH 中可用**（`cmake --version` / `ninja --version` 能直接跑通）。装法随意，winget 或便携包都行。

> ⚠️ **CMake 4.x 的兼容性风险（会影响 vcpkg 编译依赖）**
>
> CMake 4.x **移除了对 `cmake_minimum_required(VERSION < 3.5)` 的支持**。本项目自己用 `3.20`，不受影响；但 **vcpkg 在编译 zlib/lz4/zstd/openssl 时会调用它们各自的 CMake 脚本**，个别 port 可能因此报错：
>
> ```
> CMake Error: Compatibility with CMake < 3.5 has been removed
> ```
>
> **若遇到**，在 `CMakePresets.json` 的 `cacheVariables` 中加入临时兼容开关：
>
> ```json
> "CMAKE_POLICY_VERSION_MINIMUM": "3.5"
> ```
>
> 本项目锁定的 vcpkg 基线（2026-07 之后）中的 port 大多已适配 CMake 4，正常情况下不会触发。

### 3.2 MSVC (VS Build Tools)

需要 **MSVC 编译器**，即 Visual Studio 的 **"使用 C++ 的桌面开发"**（Desktop development with C++）工作负载。VS 2022 或 VS 2026 均可，本项目只要求 C++17。

> ⚠️ **只装 Build Tools 本体是不够的**——必须带上 C++ 工作负载，否则拿到的是一个不含编译器的空壳。安装包约 3–6 GB。

**已有 Visual Studio 的同学**：不必重装，确认工作负载已勾选即可，用 §5 第 5 条自查：

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
```

- 有输出（一条安装路径）→ 已装好，跳到 §3.3
- **无输出** → 缺少 C++ 工作负载，需要打开 Visual Studio Installer → 修改 → 勾选"使用 C++ 的桌面开发"

> ⚠️ **VS 不一定在 `C:\Program Files\` 下。** 它可能在 **`C:\Program Files (x86)\Microsoft Visual Studio\`**（本项目的实测环境就是如此，装在 `(x86)` 下的 `18\BuildTools`）。只搜 `C:\Program Files\` 会误判为"没装"。**用上面的 vswhere 命令判断，不要靠翻目录。**

### 3.3 vcpkg

**位置、版本、triplet 三条都要按要求来，否则后面要返工。** 装到非系统盘即可（如 `D:\Development_Software\vcpkg`），三台机器路径不必一致，但都要设 `VCPKG_ROOT`。

**必须锁定到 commit `3723ec118c8354290925feb58d021a9205a3e772`**：

```bash
git clone https://github.com/microsoft/vcpkg.git D:/Development_Software/vcpkg
cd D:/Development_Software/vcpkg
git checkout 3723ec118c8354290925feb58d021a9205a3e772
./bootstrap-vcpkg.bat -disableMetrics
```

> **为什么要锁定 commit**：vcpkg 是滚动更新的仓库，同一份 `vcpkg.json` 在不同时间 clone 会拉到不同版本的库，可能导致"我这能编译你那里报错"。锁定基线后，三人的依赖版本完全一致。这个 commit 会写进 `vcpkg.json` 的 `builtin-baseline` 字段。

### 3.4 配置环境变量

在 **系统环境变量** 或 **用户环境变量** 中添加（PowerShell 命令，需重开终端生效）：

```powershell
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "D:\Development_Software\vcpkg", "User")

# 关键：默认 triplet 必须是静态版本，否则出不了单 exe（见 README §3.4）
[Environment]::SetEnvironmentVariable("VCPKG_DEFAULT_TRIPLET", "x64-windows-static", "User")

# 二进制缓存：同一个已编译的包不再重复编译，换机器/重装时可复用
[Environment]::SetEnvironmentVariable("VCPKG_DEFAULT_BINARY_CACHE", "D:\Development_Software\vcpkg-cache", "User")

# 创建缓存目录（不创建会报警告）
New-Item -ItemType Directory -Force "D:\Development_Software\vcpkg-cache"
```

配置完成后**务必新开一个终端**，然后验证：

```powershell
echo $env:VCPKG_ROOT                # 应输出 D:\Development_Software\vcpkg
echo $env:VCPKG_DEFAULT_TRIPLET     # 应输出 x64-windows-static
```

> ⚠️ **`VCPKG_DEFAULT_TRIPLET` 是最容易漏、后果最重的一项。** 如果漏设导致依赖用动态方式编译，事后改成静态需要**重编全部依赖**（OpenSSL 一步约 10–20 分钟）。详见 README §3.4。

### 3.5 进入 MSVC 编译环境

MSVC 的 `cl.exe` **不在系统 PATH 里**（这是 VS 的有意设计，它同时支持 x86/x64/ARM）。用 Ninja 构建前必须先让 `cl` 可见——**每开一个新终端都要做一次**。

仓库内会提供 `tools/devshell.ps1`（M0 创建），执行它即可：

```powershell
. ./tools/devshell.ps1
```

它的内容如下，用 vswhere 自动定位 VS，比手写路径可靠：

```powershell
# tools/devshell.ps1 —— 仓库内统一入口，自动定位 VS 后进入编译环境
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $install) { Write-Error "未找到含 C++ 工作负载的 Visual Studio"; exit 1 }
& "$install\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
```

> 也可以直接在开始菜单搜索 **"x64 Native Tools Command Prompt for VS"** 打开。

### 3.6 网络要求

**构建全程必须能访问 GitHub**——vcpkg 的依赖（zlib/lz4/zstd/openssl）全部要从 GitHub 拉源码并本地编译。

直连不通时请自行配置代理，**git 和 `HTTP_PROXY`/`HTTPS_PROXY` 环境变量两处都要配**（vcpkg 读环境变量，clone 读 git 配置）。判断是否需要代理：

```powershell
try { (Invoke-WebRequest "https://github.com" -Method Head -TimeoutSec 10 -UseBasicParsing).StatusCode }
catch { "不通，需要代理" }
```

> ⚠️ **配了代理就要保证代理在运行。** 代理软件没开时，所有网络操作都会失败（包括 pip/npm/git）。另外，走代理的下载速度实测可快几十倍（CMake 包：直连 ~95 KB/s vs 代理 4 MB/s），vcpkg 首次装依赖时请确认代理是开着的。

---

## 4. 构建项目

### 4.1 `CMakePresets.json`（由 M0 创建，仓库内提交）

这是**保证三人构建参数一致**的关键文件，内容如下：

```json
{
  "version": 3,
  "cmakeMinimumRequired": { "major": 3, "minor": 20, "patch": 0 },
  "configurePresets": [
    {
      "name": "x64-static",
      "displayName": "MSVC x64 静态链接（单 exe 交付）",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/x64-static",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
        "VCPKG_TARGET_TRIPLET": "x64-windows-static",
        "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreaded$<$<CONFIG:Debug>:Debug>"
      }
    },
    {
      "name": "x64-debug",
      "inherits": "x64-static",
      "displayName": "MSVC x64 静态链接（Debug）",
      "binaryDir": "${sourceDir}/build/x64-debug",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" }
    }
  ],
  "buildPresets": [
    { "name": "x64-release", "configurePreset": "x64-static" },
    { "name": "x64-debug", "configurePreset": "x64-debug" }
  ],
  "testPresets": [
    { "name": "x64-release", "configurePreset": "x64-static", "output": { "outputOnFailure": true } }
  ]
}
```

**四个字段各自的作用**（理解后可自行排错）：

| 字段 | 作用 | 漏掉/写错的后果 |
|---|---|---|
| `CMAKE_TOOLCHAIN_FILE` | 让 CMake 认识 vcpkg | `find_package(ZLIB)` 找不到包 |
| `VCPKG_TARGET_TRIPLET` | 指定静态链接 | 出 8 个 DLL，无法单 exe 交付 |
| `CMAKE_MSVC_RUNTIME_LIBRARY` | 自身代码也用 `/MT` | `LNK2038: RuntimeLibrary 不匹配` |
| `generator: Ninja` | 用 Ninja 而非 MSBuild | 构建慢，输出啰嗦 |

### 4.2 `vcpkg.json`（由 M0 创建，仓库内提交）

```json
{
  "name": "file-backup",
  "version": "0.1.0",
  "dependencies": ["zlib", "lz4", "zstd", "openssl", "nlohmann-json", "catch2"],
  "builtin-baseline": "3723ec118c8354290925feb58d021a9205a3e772"
}
```

> `builtin-baseline` 与 §3.3 中 checkout 的 commit **必须一致**。这样即使有人忘了 `git checkout`，CMake 也会按基线拉取正确版本。

### 4.3 构建命令

```powershell
# ① 进入编译环境（每次新终端都要做）
. ./tools/devshell.ps1

# ② 配置 —— 首次会自动下载并编译全部依赖，约 10-20 分钟
cmake --preset x64-static

# ③ 构建 —— 之后每次改代码只走这一步，秒级完成
cmake --build --preset x64-release

# ④ 运行测试
ctest --preset x64-release
```

**构建产物位置**：`build/x64-static/FileBackup.exe`

---

## 5. 环境验证清单

逐条执行，**全绿才算环境配好**：

| # | 验证项 | 命令 | 期望结果 |
|---|---|---|---|
| 1 | Git | `git --version` | `git version 2.30+` |
| 2 | Python | `python --version` | `Python 3.8+` |
| 3 | CMake | `cmake --version` | `cmake version 3.20+`（须已在 PATH 中，见 §3.1） |
| 4 | Ninja | `ninja --version` | `1.10+`（须已在 PATH 中，见 §3.1） |
| 5 | VS 已装 C++ 工作负载 | `& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath` | 输出一条安装路径 |
| 6 | vcpkg 本体 | `Test-Path "$env:VCPKG_ROOT\vcpkg.exe"` | `True` |
| 7 | **vcpkg triplet** | `$env:VCPKG_DEFAULT_TRIPLET` | **`x64-windows-static`** |
| 8 | vcpkg 基线 | `git -C $env:VCPKG_ROOT rev-parse HEAD` | `3723ec118c8354290925feb58d021a9205a3e772` |
| 9 | 编译器可用 | 执行 §3.5 后 `cl 2>&1 \| Select-String "Version"` | `... Version 19.4x` |
| 10 | 端到端 | `cmake --preset x64-static; cmake --build --preset x64-release` | 生成 `FileBackup.exe` |
| 11 | 网络（仅直连不通时需要） | 见 §3.6 的判断命令 | 能返回 `200` |

**第 7 项是最容易被忽略、后果最重的一项**，请务必确认。第 11 项只在直连 GitHub 不通时才需要配。

---

## 6. 常见问题

### ❌ `'cl' 不是内部或外部命令`

没有进入 MSVC 编译环境。执行 §3.5 的方式 A 或 B。**注意这是每个新终端都要做一次的操作。**

### ❌ `LNK2038: 检测到"RuntimeLibrary"的不匹配: 值"MT_StaticRelease"不匹配值"MD_DynamicRelease"`

自身代码用了动态运行时（`/MD`），但 vcpkg 静态 triplet 的库是用 `/MT` 编译的。

**解决**：确认 `CMakePresets.json` 中有 `CMAKE_MSVC_RUNTIME_LIBRARY`，然后**删除 `build/` 目录重新配置**（缓存变量改了必须重新配置，否则不生效）。

### ❌ `error C2001: 字符串字面量中的换行符` / `C3688: 文本后缀无效`

源码里有中文注释或字符串，而 MSVC 默认按**系统代码页**（简体中文 Windows 是 GBK）解析源文件，把 UTF-8 字节读错了，于是字符串字面量被从中间截断。

**解决**：在 `CMakeLists.txt` 中全局加 `/utf-8`：

```cmake
if (MSVC)
    add_compile_options(/utf-8)
endif()
```

> **这个开关是必须的，不是可选项。** 项目里必然有中文注释，不加它，任何含中文的源文件都可能编译失败。

### ❌ `ctest` 报 `No test cases matched "..."`

Catch2 的 `TEST_CASE` 名字里含中文时会触发。`catch_discover_tests` 把测试名注册进 CTest，运行时再作为过滤参数回传，中文经 Windows ANSI 代码页往返后变成乱码，于是匹配不到任何用例——**现象是连最简单的 `REQUIRE(1 + 1 == 2)` 都"失败"**，很容易误判成测试框架没配好。

**解决**：`TEST_CASE` 的名字**一律用 ASCII**，中文写在注释和断言消息里：

```cpp
// ❌ 错误：TEST_CASE("zlib 压缩解压往返", "[zlib]")
TEST_CASE("zlib roundtrip", "[zlib]") { ... }   // ✅ 正确
```

### ❌ `Could not find a package configuration file provided by "ZLIB"`

CMake 没找到 vcpkg。检查：
1. `$env:VCPKG_ROOT` 是否正确设置且已重开终端
2. `CMakePresets.json` 中 `CMAKE_TOOLCHAIN_FILE` 是否指向 `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`
3. 首次配置时 CMake 会自动安装依赖，**需要联网**

### ❌ `error: building <包>:x64-windows-static failed`

vcpkg 编译依赖失败。常见原因：
- **Python 没装或不在 PATH** —— vcpkg 部分 port 依赖它
- **网络问题** —— vcpkg 需从 GitHub 下载源码，国内直连通常不通，见 **§3.6 网络与代理**
- 查看详细日志：`$env:VCPKG_ROOT\buildtrees\<包名>\*.log`

### ❌ `Compatibility with CMake < 3.5 has been removed`

CMake 4.x 与某个旧 port 的构建脚本不兼容。在 `CMakePresets.json` 的 `cacheVariables` 中加入：

```json
"CMAKE_POLICY_VERSION_MINIMUM": "3.5"
```

详见 §3.1 的 CMake 4.x 说明。

### ❌ `error: while loading vcpkg.json: ... builtin-baseline`

`vcpkg.json` 中记录的基线 commit 在你本地的 vcpkg 仓库里不存在——通常是 §3.3 的 `git checkout` 没做，或 vcpkg 仓库拉取不完整。解决：

```powershell
git -C $env:VCPKG_ROOT fetch --all --tags
git -C $env:VCPKG_ROOT checkout 3723ec118c8354290925feb58d021a9205a3e772
```

### ⚠️ 配置很慢 / 每次都重新编译依赖

二进制缓存没生效。检查 `VCPKG_DEFAULT_BINARY_CACHE` 是否设置且目录存在（§3.4）。

### ⚠️ 磁盘空间不足

`build/` 和 vcpkg 的 `buildtrees/`（编译中间产物）最占空间。vcpkg 的中间产物可安全清理：

```powershell
Remove-Item -Recurse -Force "$env:VCPKG_ROOT\buildtrees"
```

**不要**删除 `$env:VCPKG_ROOT\installed`——那是已编译好的库。

---

## 7. 版本锁定表

**三人必须使用一致的关键版本**，否则会出现"我这儿能编译你那儿报错"：

| 组件 | 锁定值 | 一致性由谁保证 |
|---|---|---|
| vcpkg 基线 | `3723ec118c8354290925feb58d021a9205a3e772` | `vcpkg.json` 的 `builtin-baseline` |
| vcpkg triplet | `x64-windows-static` | `CMakePresets.json` 的 `VCPKG_TARGET_TRIPLET` |
| C++ 标准 | C++17 | `CMakeLists.txt` 的 `CMAKE_CXX_STANDARD` |
| 编译器 | MSVC v143+ | 各自安装，允许小版本差异 |
| CMake | ≥ 3.20 | 各自安装，允许差异 |

**不锁定的项是有意为之**：编译器和 CMake 的小版本差异不会影响构建结果，强制统一反而增加组员负担。真正会导致构建不一致的是**依赖库版本**，所以只锁 vcpkg 基线。

---

*最后核对：2026-09-10 · 发现清单有误或环境变化，请更新本文档并同步到群里*
