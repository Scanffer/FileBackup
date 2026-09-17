# third_party —— 随仓库分发的单头文件库

这些库体积小、无构建步骤，按 [SETUP.md §2.2](../SETUP.md) 的要求**提交到 Git**，不走 vcpkg。

| 库 | 版本 | 文件 | 档位 | 状态 |
|---|---|---|---|---|
| **cpp-httplib** | `v0.54.1` | `httplib.h` | 必选 | ✅ 已就位 |
| **webview** | `0.12.0` | `webview.h` | 可选（M9） | ⬜ 尚未拉取 |
| **utf8** | 最新 | `utf8.h` | 按需 | ⬜ 尚未拉取 |

---

## cpp-httplib（已就位）

HTTP 服务，供 `server/` 使用（M4 接入）。

```
来源：https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.54.1/httplib.h
大小：784467 字节
SHA256：5933C14B2D0F45212925ED18CA579841F5FCE717F431FC20CEC712423E905B10
```

文件内自带版本标识，可自查：

```
#define CPPHTTPLIB_VERSION "0.54.1"
```

## webview（M9 再拉取）

SETUP.md §2.2 明确：「**项目后期再拉取，前期不需要**」。

它只被 `desktop/main.cpp`（可选的桌面外壳，M9）使用，**不在关键路径上**。
README §11 把「webview 桌面外壳集成受阻」列为仅影响 P2 加分：失败就退回浏览器模式交付，
前后端零改动。因此现在不拉取，避免把一个纯加分项变成所有人的构建依赖。

需要时：

```
https://raw.githubusercontent.com/webview/webview/0.12.0/webview.h
```

## utf8（按需）

仅在需要做路径的 UTF-8 ↔ 宽字符互转时引入。当前 MSVC 编译已启用 `/utf-8`
（见根 `CMakeLists.txt`），多数场景不需要额外的头文件库。
