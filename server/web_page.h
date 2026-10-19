#pragma once

namespace backup {

/// 前端页面，编译期直接嵌进 exe。
///
/// 用 raw string literal 而不是构建期脚本生成（对比 README §8 的
/// tools/embed_web.py）：效果一样是「单 exe，运行时不依赖 web/ 目录」，
/// 但不用把 Python 拉进构建链 —— 这个项目一直在避免被环境问题卡住。
///
/// 代价：HTML 的语法高亮不如单独的 .html 文件。等页面稳定下来、需要拆成
/// 多个文件时，再用纯 CMake 的 file(READ)/file(WRITE) 生成这个头文件即可，
/// 不必引入 Python。
///
/// 文件必须保持 **UTF-8 无 BOM**：BOM 落在 raw literal 里会污染响应体。
/// 定界符选 FBHTML，因为 HTML 里不可能出现 ")FBHTML\"" 这个序列。
///
///
/// ---- 界面设计说明 -------------------------------------------------------
///
/// 这是一个本机桌面工具，不是网页，更不是宣传页。版式按 Windows 桌面程序
/// 的惯例组织：标题栏一行、模式标签页一行、中间左对齐的表单、底部状态栏
/// 一行。界面只服务一件事：让用户填好两个路径、看清这次操作成没成。
///
/// 几条刻意的取舍：
///
///   - **不做居中大标题、不做卡片容器、不做装饰性图形。** 打开软件的
///     目的是干活，不是阅读落地页；把这些东西塞进来只会让表单变小。
///   - **颜色只编码状态和操作入口。** 带色块的只有主按钮、选中标签的
///     强调线、状态灯：灰＝闲置、琥珀＝进行中、绿＝完成、红＝失败。
///     不做渐变，不给整块背景铺色。
///   - **表单用左对齐的双列。** 标签右对齐、控件左对齐，和系统属性对话框
///     一致；用户的视线只需要在一条竖线上移动。
///   - **路径用等宽字体。** 这是功能性的：反斜杠、冒号、尾部空格必须一眼
///     看得出来，不能和普通说明文字混在一起。
///   - **控件尽量用系统原生外观。** 复选框、单选框都保留原生形态与键盘
///     行为；按钮只改颜色和尺寸。自定义控件越多，越像"套壳网页"。
///   - **系统标题栏不画，标题栏由页面承担。** 原生标题栏和页内标题会
///     重复显示产品名；现在窗口只保留可缩放边框，最小化/最大化/关闭
///     按钮放在页内标题栏右侧，拖动标题栏移动窗口（页面消息 → 宿主
///     Win32 调用）。浏览器回退模式下这几个按钮自动隐藏，窗口边框交给
///     浏览器提供，页面不假设自己一定是独立窗口。
///   - **路径既可以直接粘贴，也可以从资源管理器选择。** 输入框旁的
///     「浏览…」由宿主打开 Windows 原生的文件夹 / 归档文件 / 另存为
///     对话框（IFileOpenDialog / IFileSaveDialog），选完把绝对路径填回
///     输入框。浏览器回退模式拿不到真实路径，按钮自动隐藏。
///
/// 字体直接使用系统 UI 字体（Segoe UI／微软雅黑 UI），不引入 Web 字体、
/// 不依赖网络。
///
/// 配色固定为深色，对齐 VS Code 的 Dark Modern（编辑器同色 #1F1F1F、
/// 栏位 #181818、强调色 #0078D4）。不跟随系统明暗、也不提供浅色版：
/// 这是一个常驻在编辑器旁边的本地工具，同一套深色更统一，也避免两套
/// 配色长期失同步。原生窗口的外边框和 WebView2 的默认底色也已同步切暗
/// （见 server/webview_host.cpp），页面加载瞬间不会闪白。
inline constexpr char kIndexHtml[] = R"FBHTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="dark">
<title>FileBackup 文件备份</title>
<style>
  /* 配色对齐 VS Code Dark Modern：内容底 #1F1F1F、栏位 #181818、
     控件底 #313131、正文 #CCCCCC、说明 #9D9D9D、强调色 #0078D4。 */
  :root {
    color-scheme: dark;

    --surface: #1F1F1F;   /* 内容区（VS Code editor.background） */
    --bar:     #181818;   /* 标题栏 / 标签栏 / 状态栏（sideBar、statusBar） */
    --field:   #313131;   /* 输入框、复选框底（input、checkbox.background） */
    --border:  #2B2B2B;   /* 细分隔线 */
    --border-strong: #3C3C3C;  /* 控件描边 */

    --ink:     #CCCCCC;   /* 正文 */
    --ink-2:   #CCCCCC;   /* 标签、状态文字 */
    --ink-3:   #9D9D9D;   /* 说明文字（descriptionForeground） */

    --accent:        #0078D4;
    --accent-hover:  #026EC1;
    --accent-active: #005A9E;
    --accent-soft:   rgba(0, 120, 212, .38);

    --ok:   #2EA043;
    --busy: #CCA700;
    --err:  #F85149;

    --sans: "Segoe UI Variable Text", "Segoe UI", "Microsoft YaHei UI",
            "Microsoft YaHei", "PingFang SC", sans-serif;
    --mono: "Cascadia Mono", Consolas, "Sarasa Mono SC",
            "Noto Sans Mono CJK SC", monospace;

    --label-w: 5.5rem;
    --gutter:  1.5rem;
  }

  * { box-sizing: border-box; }

  ::selection {
    background: #264F78;  /* VS Code 的选中色 */
    color: #FFFFFF;
  }

  /* 滚动条按 VS Code 的样式画：窄、无箭头、深灰滑块。 */
  ::-webkit-scrollbar { width: 10px; height: 10px; }
  ::-webkit-scrollbar-track { background: var(--surface); }
  ::-webkit-scrollbar-thumb {
    background: #424242;
    border: 2px solid var(--surface);
  }
  ::-webkit-scrollbar-thumb:hover { background: #4F4F4F; }

  /* 必须显式写：.radios 等设了 display:flex，会盖掉 hidden 属性自带的
     display:none —— 这是「切模式时该藏的东西没藏住」的经典原因。 */
  [hidden] { display: none !important; }

  html, body { height: 100%; }

  body {
    margin: 0;
    display: flex;
    flex-direction: column;
    min-width: 24rem;
    background: var(--surface);
    color: var(--ink);
    font-family: var(--sans);
    font-size: .8125rem;
    line-height: 1.5;
    -webkit-font-smoothing: antialiased;
  }

  /* ---- 标题栏：一行，只有产品名和一句本机声明 ------------------------- */

  /* ---- 标题栏：窗口没有系统标题栏，这一条就是标题栏 ------------------- */

  .appbar {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: .6rem;
    height: 2.5rem;
    padding: 0 0 0 var(--gutter);
    border-bottom: 1px solid var(--border);
    background: var(--bar);
    user-select: none;
  }

  .logo {
    display: grid;
    place-items: center;
    width: 1.25rem;
    height: 1.25rem;
    border-radius: .25rem;
    background: var(--accent);
    color: #fff;
  }

  .brand {
    margin: 0;
    font-size: .8125rem;
    font-weight: 600;
    letter-spacing: 0;
  }

  .brand-sub {
    padding-left: .5rem;
    margin-left: .1rem;
    border-left: 1px solid var(--border);
    font-size: .75rem;
    font-weight: 400;
    color: var(--ink-3);
  }

  .appbar-note {
    margin-left: auto;
    padding-right: .75rem;
    font-size: .75rem;
    color: var(--ink-3);
    white-space: nowrap;
  }

  /* 最小化/最大化/关闭。尺寸和悬停色按 Windows 11 标题栏按钮来：
     宽 46px、悬停 #2A2A2A、关闭悬停变红。控件区贴到窗口右边缘，
     所以用负的右外边距抵消掉 appbar 的 padding。

     浏览器回退模式下没有这条标题栏（窗口边框由浏览器提供），
     JS 会给 .win-controls 加上 hidden。 */
  .win-controls {
    display: flex;
    align-self: stretch;
    margin-left: .5rem;
  }

  .win-btn {
    display: grid;
    place-items: center;
    width: 2.875rem;
    height: 100%;
    padding: 0;
    background: none;
    border: 0;
    color: var(--ink);
    cursor: default;
  }

  .win-btn:hover { background: #2A2A2A; }
  .win-btn:active { background: #333333; }

  .win-btn:focus-visible {
    outline: none;
    background: #2A2A2A;
    box-shadow: inset 0 0 0 1px var(--accent);
  }

  .win-btn.close:hover { background: #C42B1C; color: #FFFFFF; }
  .win-btn.close:active { background: #B2271A; color: #FFFFFF; }

  .win-btn svg { display: block; }
  .win-btn .ico-restore { display: none; }
  body.is-maximized .win-btn .ico-max { display: none; }
  body.is-maximized .win-btn .ico-restore { display: block; }

  /* ---- 模式标签页 ------------------------------------------------------ */

  .tabs {
    flex: 0 0 auto;
    display: flex;
    gap: .25rem;
    padding: 0 calc(var(--gutter) - .75rem);
    /* 用内阴影画分隔线：选中标签的底色能盖住它，看起来和内容区连成一片，
       和 VS Code 的标签页是同一个做法。 */
    box-shadow: inset 0 -1px 0 var(--border);
    background: var(--bar);
  }

  .tab {
    appearance: none;
    margin: 0;
    padding: 0 .75rem;
    height: 2.25rem;
    background: none;
    border: 0;
    font-family: inherit;
    font-size: .8125rem;
    color: var(--ink-3);
    cursor: pointer;
    transition: color .12s, background-color .12s;
  }

  .tab:hover { color: var(--ink); background: var(--surface); }

  /* 选中标签：底色并入内容区，顶部一条强调色。 */
  .tab[aria-selected="true"] {
    color: #FFFFFF;
    background: var(--surface);
    box-shadow: inset 0 2px 0 var(--accent);
    font-weight: 600;
  }

  /* ---- 表单 ------------------------------------------------------------ */

  form {
    flex: 1 1 auto;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }

  main {
    flex: 1 1 auto;
    min-height: 0;
    overflow: auto;
    padding: 1.5rem var(--gutter) 1.25rem;
  }

  .form { max-width: 44rem; }

  /* 双列：左边是字段名，右边是控件和说明。标签右对齐，和系统属性对话框
     的排版一致，用户的视线只需要沿一条竖线上下移动。 */
  .row {
    display: grid;
    grid-template-columns: var(--label-w) minmax(0, 1fr);
    column-gap: .875rem;
    align-items: start;
    margin-bottom: 1.125rem;
  }

  .lbl {
    padding-top: .4rem;
    text-align: right;
    font-size: .8125rem;
    color: var(--ink-2);
    white-space: nowrap;
  }

  .control {
    display: flex;
    flex-direction: column;
    gap: .35rem;
    min-width: 0;
  }

  input[type="text"] {
    width: 100%;
    height: 2rem;
    padding: 0 .625rem;
    font-family: var(--mono);
    font-size: .8125rem;
    color: var(--ink);
    background: var(--field);
    border: 1px solid var(--border-strong);
    border-radius: .25rem;
  }

  /* 占位文字是给人看的说明，不是路径，所以用正文字体。 */
  input[type="text"]::placeholder {
    color: #989898;
    font-family: var(--sans);
  }

  input[type="text"]:hover { border-color: #4D4D4D; }

  input[type="text"]:focus-visible {
    outline: none;
    border-color: var(--accent);
    box-shadow: 0 0 0 2px var(--accent-soft);
  }

  /* ---- 压缩方式（仅打包模式显示） ------------------------------------ */

  .compress-line {
    display: flex;
    gap: .5rem;
    align-items: stretch;
  }

  select,
  input[type="number"] {
    height: 2rem;
    padding: 0 .5rem;
    font-family: inherit;
    font-size: .8125rem;
    color: var(--ink);
    background: var(--field);
    border: 1px solid var(--border-strong);
    border-radius: .25rem;
    color-scheme: dark;
  }

  select { flex: 1 1 auto; min-width: 0; }
  input[type="number"] { flex: 0 0 5rem; width: 5rem; }

  select:hover,
  input[type="number"]:hover { border-color: #4D4D4D; }

  select:focus-visible,
  input[type="number"]:focus-visible {
    outline: none;
    border-color: var(--accent);
    box-shadow: 0 0 0 2px var(--accent-soft);
  }

  select:disabled,
  input[type="number"]:disabled { opacity: .6; }

  .hint {
    margin: 0;
    font-size: .75rem;
    color: var(--ink-3);
  }

  /* 路径输入 + 「浏览…」。按钮只在 WebView2 宿主模式下显示 —— 浏览器
     回退模式拿不到真实绝对路径，弹原生选择框也没有意义。 */
  .field-line {
    display: flex;
    gap: .5rem;
    align-items: stretch;
  }

  .field-line input { flex: 1 1 auto; min-width: 0; }

  .browse {
    flex: 0 0 auto;
    height: 2rem;
    padding: 0 .875rem;
    background: var(--field);
    border: 1px solid var(--border-strong);
    border-radius: .25rem;
    font-family: inherit;
    font-size: .8125rem;
    color: var(--ink);
    cursor: pointer;
  }

  .browse:hover { background: #3A3A3A; border-color: #4D4D4D; }
  .browse:active { background: #444444; }

  .browse:focus-visible {
    outline: none;
    border-color: var(--accent);
    box-shadow: 0 0 0 2px var(--accent-soft);
  }

  .radios {
    display: flex;
    gap: 1.25rem;
    min-height: 2rem;
    align-items: center;
  }

  .radio, .check {
    display: inline-flex;
    align-items: center;
    gap: .4rem;
    color: var(--ink-2);
    cursor: pointer;
    user-select: none;
  }

  .radio input, .check input {
    width: .875rem;
    height: .875rem;
    margin: 0;
    accent-color: var(--accent);
  }

  .check { min-height: 2rem; }

  /* ---- 底部状态栏：左边是这次操作的结果，右边是唯一的主动作 ---------- */

  .statusbar {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: .5rem;
    padding: .625rem var(--gutter);
    border-top: 1px solid var(--border);
    background: var(--bar);
  }

  .lamp {
    flex: 0 0 auto;
    width: .5rem;
    height: .5rem;
    border-radius: 50%;
    background: #6E7681;
  }

  .status-text {
    flex: 1 1 auto;
    min-width: 0;
    margin: 0;
    font-size: .75rem;
    color: var(--ink-2);
    white-space: pre-wrap;
    word-break: break-word;
  }

  .primary {
    flex: 0 0 auto;
    appearance: none;
    height: 2rem;
    margin-left: .75rem;
    padding: 0 1.125rem;
    background: var(--accent);
    border: 1px solid var(--accent);
    border-radius: .25rem;
    font-family: inherit;
    font-size: .8125rem;
    font-weight: 600;
    color: #fff;
    cursor: pointer;
  }

  .primary:hover {
    background: var(--accent-hover);
    border-color: var(--accent-hover);
  }

  .primary:active {
    background: var(--accent-active);
    border-color: var(--accent-active);
  }

  .primary:disabled {
    background: #2B2B2B;
    border-color: #3C3C3C;
    color: #6E7681;
    cursor: default;
  }

  .primary:focus-visible {
    outline: 2px solid var(--accent);
    outline-offset: 2px;
  }

  .statusbar.is-busy .lamp { background: var(--busy); }
  .statusbar.is-ok   .lamp { background: var(--ok); }
  .statusbar.is-err  .lamp { background: var(--err); }
  .statusbar.is-err  .status-text { color: var(--err); }

  /* ---- 窄窗口：标签移到控件上方，其余保持不变 ------------------------- */

  @media (max-width: 40rem) {
    :root { --gutter: 1rem; }

    .appbar-note { display: none; }

    .row {
      grid-template-columns: 1fr;
      row-gap: .35rem;
    }

    .lbl {
      padding-top: 0;
      text-align: left;
    }

    main { padding-top: 1rem; }
  }

  @media (prefers-reduced-motion: reduce) {
    * { transition: none !important; animation: none !important; }
  }
</style>
</head>
<body>

<header class="appbar" id="appbar">
  <span class="logo" aria-hidden="true">
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none">
      <path d="M1.9 4.6h4.1l1.2 1.5h6.9v6.2a1.3 1.3 0 0 1-1.3 1.3H3.2a1.3 1.3 0 0 1-1.3-1.3V4.6Z"
            stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"/>
      <path d="M8.4 8.3v3.1M7 10.2l1.4 1.4L9.8 10.2"
            stroke="currentColor" stroke-width="1.3"
            stroke-linecap="round" stroke-linejoin="round"/>
    </svg>
  </span>
  <h1 class="brand">FileBackup<span class="brand-sub">文件备份</span></h1>
  <span class="appbar-note">本地操作 · 数据不会上传</span>

  <div class="win-controls" hidden>
    <button type="button" class="win-btn" id="win-min"
            title="最小化" aria-label="最小化">
      <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden="true">
        <path d="M0 5h10" stroke="currentColor" stroke-width="1"/>
      </svg>
    </button>
    <button type="button" class="win-btn" id="win-max"
            title="最大化" aria-label="最大化">
      <svg class="ico-max" width="10" height="10" viewBox="0 0 10 10"
           aria-hidden="true">
        <path d="M0.5 0.5h9v9h-9z" fill="none" stroke="currentColor"
              stroke-width="1"/>
      </svg>
      <svg class="ico-restore" width="10" height="10" viewBox="0 0 10 10"
           aria-hidden="true">
        <path d="M2.5 2.5v-2h7v7h-2" fill="none" stroke="currentColor"
              stroke-width="1"/>
        <path d="M0.5 2.5h7v7h-7z" fill="none" stroke="currentColor"
              stroke-width="1"/>
      </svg>
    </button>
    <button type="button" class="win-btn close" id="win-close"
            title="关闭" aria-label="关闭">
      <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden="true">
        <path d="M0.5 0.5l9 9M9.5 0.5l-9 9" stroke="currentColor"
              stroke-width="1"/>
      </svg>
    </button>
  </div>
</header>

<nav class="tabs" role="tablist" aria-label="操作模式">
  <button type="button" class="tab" role="tab"
          data-mode="backup" aria-selected="true">备份</button>
  <button type="button" class="tab" role="tab"
          data-mode="restore" aria-selected="false">还原</button>
</nav>

<form id="form" novalidate>
  <main>
    <div class="form">

      <div class="row" id="row-kind" hidden>
        <span class="lbl">还原来源</span>
        <div class="control">
          <div class="radios">
            <label class="radio">
              <input type="radio" name="kind" value="archive" checked>
              <span>.vbak 归档文件</span>
            </label>
            <label class="radio">
              <input type="radio" name="kind" value="mirror">
              <span>备份目录</span>
            </label>
          </div>
        </div>
      </div>

      <div class="row">
        <label class="lbl" for="src" id="src-label">源目录</label>
        <div class="control">
          <div class="field-line">
            <input type="text" id="src" name="src" autocomplete="off"
                   spellcheck="false" placeholder="例如 D:\我的文档">
            <button type="button" class="browse" data-target="src"
                    title="在资源管理器中选择" hidden>浏览…</button>
          </div>
          <p class="hint" id="src-hint"></p>
        </div>
      </div>

      <div class="row">
        <label class="lbl" for="dst" id="dst-label">目标位置</label>
        <div class="control">
          <div class="field-line">
            <input type="text" id="dst" name="dst" autocomplete="off"
                   spellcheck="false" placeholder="例如 D:\备份">
            <button type="button" class="browse" data-target="dst"
                    title="在资源管理器中选择" hidden>浏览…</button>
          </div>
          <p class="hint" id="dst-hint"></p>
        </div>
      </div>

      <div class="row" id="row-pack">
        <span class="lbl">输出方式</span>
        <div class="control">
          <label class="check">
            <input type="checkbox" id="pack">
            <span>打包为单个 .vbak 归档文件</span>
          </label>
          <p class="hint">所有文件合并为一个文件，便于保存和传输；还原时再展开。</p>
        </div>
      </div>

      <div class="row" id="row-compress" hidden>
        <span class="lbl">压缩方式</span>
        <div class="control">
          <div class="compress-line">
            <select id="compress" aria-label="压缩算法"></select>
            <input type="number" id="level" min="1" max="19" value="6"
                   title="压缩级别" aria-label="压缩级别">
          </div>
          <p class="hint">
            仅对单容器 .vbak 生效；级别越高越慢、压缩率通常越好。
          </p>
        </div>
      </div>

    </div>
  </main>

  <div class="statusbar" id="statusbar">
    <span class="lamp" aria-hidden="true"></span>
    <p class="status-text" id="status-text" role="status" aria-live="polite"></p>
    <button type="submit" class="primary" id="go">开始备份</button>
  </div>
</form>

<script>
"use strict";

const $ = (id) => document.getElementById(id);

// WebView2 宿主模式下窗口没有系统标题栏（见 server/webview_host.cpp），
// 标题栏和窗口按钮由页面承担；浏览器回退模式（--browser）没有
// window.chrome.webview，这些按钮要隐藏，窗口边框交给浏览器自己画。
const webviewHost = window.chrome && window.chrome.webview;
const hasNativeChrome = !!(webviewHost && webviewHost.postMessage);
document.querySelector(".win-controls").hidden = !hasNativeChrome;

// 浏览器回退模式拿不到真实绝对路径，原生选择框也没有意义，直接隐藏。
const browseButtons = document.querySelectorAll(".browse");
for (const button of browseButtons) {
  button.hidden = !hasNativeChrome;
}

function postWindowCommand(command) {
  if (hasNativeChrome) webviewHost.postMessage(command);
}

// ---- 压缩算法下拉框 ------------------------------------------------------
// 清单从 /api/algorithms 拉取；只展示 available=true 的算法。拉不到时用
// 一个最小兜底清单，页面仍然可用。
let compressAlgorithms = [
  { id: "none", label: "不压缩", has_level: false, self_made: false },
  { id: "deflate", label: "DEFLATE", has_level: true, self_made: true },
];

function currentCompressionInfo() {
  return (
    compressAlgorithms.find((algo) => algo.id === state.compression) || {
      id: "none",
      label: "不压缩",
      has_level: false,
      self_made: false,
    }
  );
}

function syncCompressionControls() {
  const info = currentCompressionInfo();
  const level = $("level");
  level.disabled = !info.has_level;
  level.title = info.has_level ? "压缩级别" : "该算法没有压缩级别";
}

function fillCompressionOptions() {
  const select = $("compress");
  select.innerHTML = "";
  for (const algo of compressAlgorithms) {
    const option = document.createElement("option");
    option.value = algo.id;
    let label = algo.label;
    if (algo.self_made) label += "（自研）";
    else if (algo.id !== "none") label += "（库）";
    option.textContent = label;
    select.appendChild(option);
  }
  if (!compressAlgorithms.some((algo) => algo.id === state.compression)) {
    // 默认优先选 DEFLATE（可用时），备份工具的合理默认是压一下。
    state.compression = compressAlgorithms.some(
      (algo) => algo.id === "deflate"
    )
      ? "deflate"
      : (compressAlgorithms[0] ? compressAlgorithms[0].id : "none");
  }
  select.value = state.compression;
  syncCompressionControls();
}

async function loadAlgorithms() {
  try {
    const resp = await fetch("/api/algorithms");
    if (!resp.ok) throw new Error("HTTP " + resp.status);
    const data = await resp.json();
    const list = (data.compress || []).filter(
      (algo) => algo.available !== false
    );
    if (list.length > 0) compressAlgorithms = list;
  } catch (err) {
    // 后端异常时保留兜底清单，不阻塞界面。
  }
  fillCompressionOptions();
}

// mode: backup | restore；kind: archive | mirror（仅还原时有效）；pack：是否打包
const state = {
  mode: "backup",
  kind: "archive",
  pack: false,
  compression: "deflate",
  level: 6,
};

// 当前状态下界面上的每个词都从这里取，避免同一个动作在标签、按钮和状态栏
// 里叫三个不同的名字。
function describe() {
  if (state.mode === "backup") {
    const pack = state.pack;
    return {
      verb: pack ? "打包" : "备份",
      note: pack
        ? "就绪：将把源目录打包为单个 .vbak 归档文件。"
        : "就绪：将把源目录完整复制到目标目录，已存在的文件会跳过。",
      srcLabel: "源目录",
      srcPlaceholder: "例如 D:\\我的文档",
      srcHint: "要备份的文件夹，其下所有文件都会按原目录结构保存。",
      dstLabel: pack ? "归档文件" : "目标位置",
      dstPlaceholder: pack ? "例如 D:\\备份\\docs.vbak" : "例如 D:\\备份",
      dstHint: pack
        ? "生成单个 .vbak 文件；所在目录不存在时会自动创建。"
        : "目标目录会得到源目录的完整副本；已存在的文件会跳过。",
      kindHidden: true,
      packHidden: false,
      compressHidden: !pack,
    };
  }

  const archive = state.kind === "archive";
  return {
    verb: archive ? "解包" : "还原",
    note: archive
      ? "就绪：将把归档中的文件展开到目标目录。"
      : "就绪：将把备份目录中的副本写回目标目录。",
    srcLabel: archive ? "归档文件" : "备份目录",
    srcPlaceholder: archive ? "例如 D:\\备份\\docs.vbak" : "例如 D:\\备份",
    srcHint: archive ? "要展开的 .vbak 归档文件。" : "之前备份得到的目录副本。",
    dstLabel: "还原到",
    dstPlaceholder: "例如 D:\\还原结果",
    dstHint: "文件会恢复到该目录；子目录不存在时会自动创建。",
    kindHidden: false,
    packHidden: true,
    compressHidden: true,
  };
}

// kind 为空字符串表示「闲置，还没跑过」—— 灯是灰的。
function setStatus(kind, text) {
  $("statusbar").className = "statusbar" + (kind ? " is-" + kind : "");
  $("status-text").textContent = text;
}

function render() {
  const d = describe();

  for (const tab of document.querySelectorAll(".tab")) {
    tab.setAttribute("aria-selected", String(tab.dataset.mode === state.mode));
  }
  for (const radio of document.querySelectorAll('input[name="kind"]')) {
    radio.checked = radio.value === state.kind;
  }

  $("row-kind").hidden = d.kindHidden;
  $("row-pack").hidden = d.packHidden;
  $("row-compress").hidden = d.compressHidden;
  syncCompressionControls();

  $("src-label").textContent = d.srcLabel;
  $("dst-label").textContent = d.dstLabel;
  $("src").placeholder = d.srcPlaceholder;
  $("dst").placeholder = d.dstPlaceholder;
  $("src-hint").textContent = d.srcHint;
  $("dst-hint").textContent = d.dstHint;
  $("go").textContent = "开始" + d.verb;

  // 换模式或选项就把上一次的结果收起来，换回「接下来会发生什么」，
  // 免得上一轮的「完成」被误读成本轮的。
  setStatus("", d.note);
}

for (const tab of document.querySelectorAll(".tab")) {
  tab.addEventListener("click", () => {
    state.mode = tab.dataset.mode;
    render();
  });
}

for (const radio of document.querySelectorAll('input[name="kind"]')) {
  radio.addEventListener("change", () => {
    if (!radio.checked) return;
    state.kind = radio.value;
    render();
  });
}

$("pack").addEventListener("change", (e) => {
  state.pack = e.target.checked;
  render();
});

$("compress").addEventListener("change", (e) => {
  state.compression = e.target.value;
  syncCompressionControls();
});

$("level").addEventListener("input", (e) => {
  const value = Number.parseInt(e.target.value, 10);
  if (!Number.isNaN(value)) state.level = value;
});

// ---- 窗口按钮与标题栏拖拽（仅 WebView2 宿主模式）------------------------

if (hasNativeChrome) {
  $("win-min").addEventListener("click", () =>
    postWindowCommand("window:minimize"));
  $("win-max").addEventListener("click", () =>
    postWindowCommand("window:maximize"));
  $("win-close").addEventListener("click", () =>
    postWindowCommand("window:close"));

  // 按下标题栏后，位移超过几个像素才真正开始拖窗口 —— 这样单击、双击
  // 不会被拖拽抢走；超过阈值再让宿主进入系统的移动循环。
  let dragOrigin = null;

  $("appbar").addEventListener("mousedown", (e) => {
    if (e.button !== 0 || e.target.closest(".win-controls")) return;
    dragOrigin = { x: e.screenX, y: e.screenY };
  });

  document.addEventListener("mousemove", (e) => {
    if (dragOrigin === null) return;
    if (Math.abs(e.screenX - dragOrigin.x) +
            Math.abs(e.screenY - dragOrigin.y) < 4) {
      return;
    }
    dragOrigin = null;
    postWindowCommand("window:drag");
  });

  document.addEventListener("mouseup", () => {
    dragOrigin = null;
  });

  // 双击标题栏 = 最大化/还原。
  $("appbar").addEventListener("dblclick", (e) => {
    if (e.target.closest(".win-controls")) return;
    postWindowCommand("window:maximize");
  });

  // ---- 原生「浏览…」选择框 -----------------------------------------------
  //
  // 页面只告诉宿主「要选文件夹 / 文件 / 保存位置」，真正的资源管理器对话框
  // 由宿主打开（见 server/webview_host.cpp 的 PageCommandHandler）；选完
  // 宿主回一条 picked|<路径>，取消时路径为空。
  let pendingPickTarget = null;

  function pickKindFor(target) {
    if (state.mode === "backup") {
      if (target === "src") return "folder";
      return state.pack ? "save" : "folder";
    }
    if (target === "src") return state.kind === "archive" ? "file" : "folder";
    return "folder";
  }

  for (const button of browseButtons) {
    button.addEventListener("click", () => {
      pendingPickTarget = button.dataset.target;
      postWindowCommand("pick:" + pickKindFor(pendingPickTarget));
    });
  }

  // 宿主在窗口最大化/还原时推消息过来，按钮图标跟着换。
  webviewHost.addEventListener("message", (e) => {
    // 「浏览…」的选择结果（取消时路径为空）。
    if (typeof e.data === "string" && e.data.startsWith("picked|")) {
      const path = e.data.slice("picked|".length);
      if (pendingPickTarget !== null) {
        if (path) $(pendingPickTarget).value = path;
        pendingPickTarget = null;
      }
      return;
    }
    if (e.data === "window:maximized") {
      document.body.classList.add("is-maximized");
    } else if (e.data === "window:restored") {
      document.body.classList.remove("is-maximized");
    }
  });
}

$("form").addEventListener("submit", async (e) => {
  e.preventDefault();

  const d = describe();
  const src = $("src").value.trim();
  const dst = $("dst").value.trim();

  // 本地先挡一道，报错话说清楚是哪个框。服务端也会校验（400），
  // 但那边只能说「参数为空：src」，对用户没有意义。
  if (!src) {
    setStatus("err", "请先填写「" + d.srcLabel + "」。");
    $("src").focus();
    return;
  }
  if (!dst) {
    setStatus("err", "请先填写「" + d.dstLabel + "」。");
    $("dst").focus();
    return;
  }

  // 字段名与语义和 CLI 的 --src / --dst / --pack / --archive 一一对应
  // （见 cli/main.cpp 的 backup / restore 分支），GUI 和 CLI 走的是同一批
  // backup:: 函数，行为不可能漂移。
  let url, params;
  if (state.mode === "backup") {
    url = "/api/backup";
    params = { src, dst };
    if (state.pack) {
      params.pack = "vbak";
      if (state.compression !== "none") {
        params.compress = state.compression;
        const info = currentCompressionInfo();
        if (info.has_level) {
          const raw = Number.parseInt($("level").value, 10);
          const level = Number.isFinite(raw) ? raw : state.level;
          params.level = String(Math.min(19, Math.max(1, level)));
        }
      }
    }
  } else {
    url = "/api/restore";
    params = { dst };
    if (state.kind === "archive") params.archive = src;
    else params.src = src;
  }

  const go = $("go");
  go.disabled = true;
  setStatus("busy", "正在" + d.verb + "，请稍候…");

  try {
    // 必须用 URLSearchParams 拼 body，不要手写字符串拼接：
    //   1. 它会自动带上 Content-Type: application/x-www-form-urlencoded，
    //      httplib 靠这个才把字段填进 req.params；
    //   2. 它会把 + & = % 和所有非 ASCII 字节正确百分号编码。
    //      手写拼接的话，路径里一个字面 "+" 会被服务端解成空格。
    const r = await fetch(url, {
      method: "POST",
      body: new URLSearchParams(params),
    });
    const text = (await r.text()).trim();
    setStatus(r.ok ? "ok" : "err", text || (r.ok ? "操作完成。" : "操作失败。"));
  } catch (err) {
    setStatus("err", "无法连接本地服务，请确认 FileBackup 仍在运行。");
  } finally {
    go.disabled = false;
  }
});

render();
loadAlgorithms();

// 打开窗口就能直接输入路径，不用先点一下输入框。
$("src").focus();
</script>
</body>
</html>)FBHTML";

}  // namespace backup
