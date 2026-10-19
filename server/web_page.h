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

  /* 密码框和文本路径框共用同一套外观：高度、等宽字体、深色控件底。
     type=password 也要列进来，否则密码框会是浏览器默认的浅色样式。 */
  input[type="text"],
  input[type="password"] {
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
  input[type="text"]::placeholder,
  input[type="password"]::placeholder {
    color: #989898;
    font-family: var(--sans);
  }

  input[type="text"]:hover,
  input[type="password"]:hover { border-color: #4D4D4D; }

  input[type="text"]:focus-visible,
  input[type="password"]:focus-visible {
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

  /* ---- 次级按钮：密码显示/隐藏、填入示例、复制路径 ------------------- */

  .btn-secondary {
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

  .btn-secondary:hover { background: #3A3A3A; border-color: #4D4D4D; }
  .btn-secondary:active { background: #444444; }

  .btn-secondary:focus-visible {
    outline: none;
    border-color: var(--accent);
    box-shadow: 0 0 0 2px var(--accent-soft);
  }

  .btn-secondary:disabled { opacity: .55; cursor: default; }

  /* ---- 多行文本：六维筛选规则（JSON） --------------------------------- */

  textarea {
    width: 100%;
    /* 10rem 正好放得下占位里的六维示例，不用滚动就能看全。 */
    min-height: 10rem;
    padding: .45rem .625rem;
    font-family: var(--mono);
    font-size: .75rem;
    line-height: 1.5;
    color: var(--ink);
    background: var(--field);
    border: 1px solid var(--border-strong);
    border-radius: .25rem;
    resize: vertical;
    color-scheme: dark;
    tab-size: 2;
  }

  textarea::placeholder { color: #888888; }
  textarea:hover:not(:disabled) { border-color: #4D4D4D; }

  textarea:focus-visible {
    outline: none;
    border-color: var(--accent);
    box-shadow: 0 0 0 2px var(--accent-soft);
  }

  textarea:disabled { opacity: .6; }

  .filter-line {
    display: flex;
    align-items: center;
    gap: .5rem;
    flex-wrap: wrap;
  }

  .filter-line .hint { flex: 1 1 auto; min-width: 0; }

  .filter-status { color: var(--ink-3); }
  .filter-status.is-ok { color: #7EE787; }
  .filter-status.is-err { color: var(--err); }

  /* fieldset 只用来「一键禁用整张表单」，视觉上必须完全隐形。 */
  .fields {
    min-width: 0;
    margin: 0;
    padding: 0;
    border: 0;
  }

  .fields:disabled .lbl { color: #6E7681; }
  .fields:disabled .hint { opacity: .7; }

  /* 原生 disabled 控件在深色主题下几乎看不出变化，给整个表单加一点
     统一的不透明度，让「正在跑、别动」一眼可见。 */
  .fields:disabled input,
  .fields:disabled select,
  .fields:disabled textarea,
  .fields:disabled button { opacity: .55; }

  .tab:disabled,
  .tab:disabled:hover {
    color: #6E7681;
    background: none;
    cursor: default;
  }

  /* ---- 任务列表 / 结果面板 -------------------------------------------- */

  .panel {
    margin-top: 1.25rem;
    padding: .75rem .875rem;
    background: #232323;
    border: 1px solid var(--border-strong);
    border-radius: .375rem;
  }

  .panel-head {
    display: flex;
    align-items: center;
    gap: .5rem;
    min-width: 0;
    margin-bottom: .5rem;
  }

  .panel-title {
    margin: 0;
    font-size: .8125rem;
    font-weight: 600;
    color: var(--ink);
  }

  .link-btn {
    appearance: none;
    margin-left: auto;
    padding: .1rem .25rem;
    background: none;
    border: 0;
    border-radius: .2rem;
    font-family: inherit;
    font-size: .75rem;
    color: var(--accent);
    cursor: pointer;
  }

  .link-btn:hover { text-decoration: underline; }

  .link-btn:focus-visible {
    outline: 1px solid var(--accent);
    outline-offset: 1px;
  }

  .result-state {
    padding: .05rem .4rem;
    border-radius: .6rem;
    font-size: .6875rem;
    color: var(--ink-3);
    background: #2D2D2D;
  }

  .result-state.is-ok { color: #7EE787; background: rgba(46, 160, 67, .16); }
  .result-state.is-err { color: #FF7B72; background: rgba(248, 81, 73, .16); }

  .result-detail {
    margin: 0;
    font-size: .8125rem;
    color: var(--ink-2);
  }

  .result-error {
    margin: .35rem 0 0;
    font-size: .8125rem;
    color: var(--err);
    white-space: pre-wrap;
    word-break: break-word;
  }

  .result-path-line {
    display: flex;
    align-items: center;
    gap: .5rem;
    min-width: 0;
    margin: .55rem 0 0;
  }

  .result-path-label {
    flex: 0 0 auto;
    font-size: .75rem;
    color: var(--ink-3);
  }

  .result-path {
    flex: 1 1 auto;
    min-width: 0;
    overflow: hidden;
    padding: .3rem .45rem;
    background: var(--field);
    border: 1px solid var(--border-strong);
    border-radius: .25rem;
    font-family: var(--mono);
    font-size: .75rem;
    color: var(--ink);
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .task-list {
    display: flex;
    flex-direction: column;
    gap: .4rem;
    margin: 0;
    padding: 0;
    list-style: none;
  }

  .task-item {
    padding: .4rem .5rem .45rem;
    background: #252526;
    border: 1px solid var(--border);
    border-radius: .3rem;
  }

  .task-top {
    display: flex;
    align-items: center;
    gap: .5rem;
    min-width: 0;
  }

  .task-kind { font-size: .75rem; color: var(--ink-2); }

  .task-badge {
    padding: .02rem .38rem;
    border-radius: .55rem;
    font-size: .6875rem;
    color: var(--ink-3);
    background: #2D2D2D;
    white-space: nowrap;
  }

  .task-item[data-state="running"] .task-badge {
    color: #E3B341;
    background: rgba(204, 167, 0, .16);
  }

  .task-item[data-state="success"] .task-badge {
    color: #7EE787;
    background: rgba(46, 160, 67, .16);
  }

  .task-item[data-state="failed"] .task-badge {
    color: #FF7B72;
    background: rgba(248, 81, 73, .16);
  }

  .task-pct {
    margin-left: auto;
    font-family: var(--mono);
    font-size: .6875rem;
    color: var(--ink-3);
  }

  .task-mini {
    height: 3px;
    margin: .32rem 0 .28rem;
    overflow: hidden;
    background: #2B2B2B;
    border-radius: 2px;
  }

  .task-mini > i {
    display: block;
    width: 0;
    height: 100%;
    background: var(--busy);
    transition: width .2s ease;
  }

  .task-item[data-state="success"] .task-mini > i { background: var(--ok); }
  .task-item[data-state="failed"] .task-mini > i { background: var(--err); }

  .task-msg {
    margin: 0;
    overflow: hidden;
    font-size: .75rem;
    color: var(--ink-3);
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  /* ---- 底部状态栏：左边是这次操作的结果，右边是唯一的主动作 ---------- */

  .statusbar {
    flex: 0 0 auto;
    display: flex;
    flex-direction: column;
    border-top: 1px solid var(--border);
    background: var(--bar);
  }

  /* 运行中在状态栏顶部画一条细进度条；[hidden] 会把它整条藏掉。 */
  .statusbar-progress {
    flex: 0 0 auto;
    height: 3px;
    overflow: hidden;
    background: #2B2B2B;
  }

  .statusbar-progress-fill {
    width: 0;
    height: 100%;
    background: var(--accent);
    transition: width .2s ease;
  }

  .statusbar.is-busy .statusbar-progress-fill { background: var(--busy); }
  .statusbar.is-ok   .statusbar-progress-fill { background: var(--ok); }
  .statusbar.is-err  .statusbar-progress-fill { background: var(--err); }

  .statusbar-row {
    display: flex;
    align-items: center;
    gap: .5rem;
    padding: .625rem var(--gutter);
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

    /* 窄窗口下路径和「复制路径」放不下同一行，折成两行。 */
    .result-path-line { flex-wrap: wrap; }
    .result-copy { margin-left: auto; }
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
      <!-- 运行中把整个 fieldset 置 disabled，一次禁用所有字段控件；
           比逐个记「原来是否禁用」可靠，也天然覆盖以后新增的控件。 -->
      <fieldset class="fields" id="fields">

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
          <p class="hint" id="compress-hint">
            仅对单容器 .vbak 生效；级别越高越慢、压缩率通常越好。
          </p>
        </div>
      </div>

      <div class="row" id="row-encrypt" hidden>
        <span class="lbl">加密方式</span>
        <div class="control">
          <div class="compress-line">
            <select id="encrypt" aria-label="加密算法"></select>
          </div>
          <p class="hint" id="encrypt-hint">
            仅单容器 .vbak 支持加密；勾选上方「打包」后启用。
          </p>
        </div>
      </div>

      <div class="row" id="row-password" hidden>
        <label class="lbl" for="password">密码</label>
        <div class="control">
          <div class="field-line">
            <input type="password" id="password" autocomplete="new-password"
                   spellcheck="false" placeholder="加密密码（不会写入归档）">
            <button type="button" class="btn-secondary" id="password-toggle"
                    aria-pressed="false" title="显示 / 隐藏密码">显示</button>
          </div>
          <p class="hint" id="password-hint"></p>
        </div>
      </div>

      <div class="row" id="row-metadata">
        <span class="lbl">元数据</span>
        <div class="control">
          <label class="check">
            <input type="checkbox" id="metadata" checked>
            <span>保存 / 还原元数据（时间戳、Windows 属性等）</span>
          </label>
          <p class="hint" id="metadata-hint"></p>
        </div>
      </div>

      <div class="row" id="row-filter" hidden>
        <span class="lbl">筛选规则</span>
        <div class="control">
          <textarea id="filter" spellcheck="false"
                    aria-label="筛选规则 JSON"></textarea>
          <div class="filter-line">
            <button type="button" class="btn-secondary" id="filter-example">
              填入示例
            </button>
            <p class="hint filter-status" id="filter-status"
               role="status" aria-live="polite"></p>
          </div>
          <p class="hint" id="filter-hint"></p>
        </div>
      </div>

      </fieldset>

      <section class="panel" id="result-panel" hidden aria-live="polite">
        <div class="panel-head">
          <h2 class="panel-title" id="result-title">操作结果</h2>
          <span class="result-state" id="result-state"></span>
        </div>
        <p class="result-detail" id="result-detail" hidden></p>
        <p class="result-error" id="result-error" hidden></p>
        <div class="result-path-line" id="result-path-line" hidden>
          <span class="result-path-label">目标路径</span>
          <code class="result-path" id="result-path"></code>
          <button type="button" class="btn-secondary result-copy"
                  id="result-copy">复制路径</button>
        </div>
      </section>

      <section class="panel" id="tasks-panel" hidden aria-label="最近任务">
        <div class="panel-head">
          <h2 class="panel-title">最近任务</h2>
          <button type="button" class="link-btn" id="tasks-refresh">刷新</button>
        </div>
        <ul class="task-list" id="task-list"></ul>
      </section>

    </div>
  </main>

  <div class="statusbar" id="statusbar">
    <div class="statusbar-progress" id="run-progress" hidden>
      <div class="statusbar-progress-fill" id="run-progress-fill"></div>
    </div>
    <div class="statusbar-row">
      <span class="lamp" aria-hidden="true"></span>
      <p class="status-text" id="status-text" role="status"
         aria-live="polite"></p>
      <button type="submit" class="primary" id="go">开始备份</button>
    </div>
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

// ---- 算法清单 / 筛选规则校验 ---------------------------------------------
// 压缩、加密清单都从 /api/algorithms 拉取；只展示 available != false 的算法。
// 接口不可用时用本地兜底清单，页面仍然可用（只是加密选项可能不完整）。
const FALLBACK_COMPRESS = [
  { id: "none", label: "不压缩", has_level: false, self_made: false, available: true },
  { id: "deflate", label: "DEFLATE", has_level: true, self_made: true, available: true },
];
const FALLBACK_ENCRYPT = [
  { id: "none", label: "不加密", self_made: false, available: true },
  { id: "aes256cbc", label: "AES-256-CBC", self_made: true, available: true },
  { id: "chacha20poly1305", label: "ChaCha20-Poly1305", self_made: true, available: true },
  { id: "rc4", label: "RC4", self_made: true, available: true },
];

let compressAlgorithms = FALLBACK_COMPRESS.slice();
let encryptAlgorithms = FALLBACK_ENCRYPT.slice();

function findAlgorithm(list, id) {
  for (const algo of list) {
    if (algo.id === id) return algo;
  }
  return null;
}

function currentCompressionInfo() {
  return (
    findAlgorithm(compressAlgorithms, state.compression) || {
      id: "none",
      label: "不压缩",
      has_level: false,
      self_made: false,
    }
  );
}

function currentEncryptionInfo() {
  return (
    findAlgorithm(encryptAlgorithms, state.encryption) || {
      id: "none",
      label: "不加密",
      self_made: false,
    }
  );
}

/// 各压缩算法支持的级别范围。优先用后端给的 level_min/level_max，
/// 老接口没有这两个字段时按算法 ID 回退（DEFLATE 1–9、zstd 1–19）。
/// 级别不是"压缩几次"，而是搜索力度：越高越慢，通常文件越小。
function compressionLevelRange(info) {
  const min = Number(info && info.level_min);
  const max = Number(info && info.level_max);
  if (Number.isFinite(min) && Number.isFinite(max) && min >= 1 && max >= min) {
    return [min, max];
  }
  if (info && info.id === "deflate") return [1, 9];
  if (info && info.id === "zstd") return [1, 19];
  return [1, 1];
}

function syncCompressionControls() {
  const info = currentCompressionInfo();
  const enabled = state.mode === "backup" && state.pack && !state.busy;
  $("compress").disabled = !enabled;
  const level = $("level");
  const hasLevel = info.has_level === true;
  level.disabled = !enabled || !hasLevel;
  level.title = hasLevel ? "压缩级别" : "该算法没有压缩级别";

  const [minLevel, maxLevel] = compressionLevelRange(info);
  level.min = String(minLevel);
  level.max = String(maxLevel);
  const value = Number.parseInt(level.value, 10);
  if (!Number.isFinite(value)) {
    level.value = String(minLevel);
  } else {
    level.value = String(Math.min(maxLevel, Math.max(minLevel, value)));
  }

  // 提示文字直接写出当前算法的级别范围，用户不用去猜。
  const hint = $("compress-hint");
  if (state.mode !== "backup") {
    hint.textContent = "";
  } else if (!state.pack) {
    hint.textContent =
      "仅勾选「打包为单个 .vbak 归档文件」时生效；目录镜像模式按原样复制文件。";
  } else if (!hasLevel) {
    hint.textContent = (info.label || info.id) + " 没有压缩级别参数。";
  } else {
    hint.textContent =
      "级别范围 " + minLevel + "–" + maxLevel +
      "；级别越高越慢、压缩率通常越好。";
  }
}

function fillCompressionOptions() {
  const select = $("compress");
  select.innerHTML = "";
  for (const algo of compressAlgorithms) {
    const option = document.createElement("option");
    option.value = algo.id;
    let label = algo.label || algo.id;
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

function fillEncryptionOptions() {
  const select = $("encrypt");
  select.innerHTML = "";
  for (const algo of encryptAlgorithms) {
    const option = document.createElement("option");
    option.value = algo.id;
    let label = algo.label || algo.id;
    if (algo.self_made) label += "（自研）";
    option.textContent = label;
    select.appendChild(option);
  }
  if (!encryptAlgorithms.some((algo) => algo.id === state.encryption)) {
    state.encryption = "none";
  }
  select.value = state.encryption;
  syncEncryptionControls();
}

/// 加密方式与密码的可用性：只有「备份 + 打包 vbak」能选加密算法；
/// 还原归档时也需要密码（归档是否加密由后端判断，留空也能提交）。
function syncEncryptionControls() {
  const info = currentEncryptionInfo();
  const encryptEnabled = state.mode === "backup" && state.pack && !state.busy;
  const select = $("encrypt");
  select.disabled = !encryptEnabled;
  if (encryptEnabled && findAlgorithm(encryptAlgorithms, state.encryption)) {
    select.value = state.encryption;
  }

  const passwordEnabled = !state.busy && (
    (state.mode === "backup" && state.pack && state.encryption !== "none") ||
    (state.mode === "restore" && state.kind === "archive")
  );
  $("password").disabled = !passwordEnabled;
  $("password-toggle").disabled = !passwordEnabled;

  let encryptHint;
  if (!state.pack) {
    encryptHint = "仅单容器 .vbak 支持加密；勾选上方「打包」后启用。";
  } else if (state.encryption === "none") {
    encryptHint = "不加密：归档内容以明文保存。";
  } else {
    const label = info.label || state.encryption;
    encryptHint = label + (info.self_made ? "（自研）" : "") +
      "；需要设置密码，密码不会写入归档。";
    if (info.note) encryptHint += " " + info.note;
  }
  $("encrypt-hint").textContent = encryptHint;
  $("password-hint").textContent = state.mode === "backup"
    ? "加密密码；归档中不保存密码，还原时需要再次输入。"
    : "加密归档需要密码；未加密的归档可以留空。";
}

async function loadAlgorithms() {
  try {
    const resp = await fetch("/api/algorithms");
    if (!resp.ok) throw new Error("HTTP " + resp.status);
    const data = await resp.json();
    const compress = (data.compress || []).filter(
      (algo) => algo.available !== false
    );
    if (compress.length > 0) {
      compressAlgorithms = compress.some((algo) => algo.id === "none")
        ? compress
        : [{ id: "none", label: "不压缩", has_level: false, self_made: false }]
            .concat(compress);
    }
    const encrypt = (data.encrypt || []).filter(
      (algo) => algo.available !== false
    );
    if (encrypt.length > 0) {
      encryptAlgorithms = encrypt.some((algo) => algo.id === "none")
        ? encrypt
        : [{ id: "none", label: "不加密", self_made: false }].concat(encrypt);
    }
  } catch (err) {
    // 后端异常时保留兜底清单，不阻塞界面。
  }
  fillCompressionOptions();
  fillEncryptionOptions();
  if (!state.busy) render();
}

// ---- 六维筛选规则：JSON 文本域 + 实时校验 --------------------------------
// 规则格式见 core/filter.h：JSON 数组，每条规则可含
// path / type / name / time / size / user 六个维度，action 为 include 或
// exclude。前端只做结构校验（后端还会再校验一遍）。
const FILTER_DIMENSIONS = ["path", "type", "name", "time", "size", "user"];

const FILTER_EXAMPLE = [
  "[",
  '  {"action": "include", "path": "docs/**", "name": "*.pdf"},',
  '  {"action": "exclude", "type": "tmp,bak"},',
  '  {"action": "include", "size": ">= 1KB"},',
  '  {"action": "exclude", "time": "before 2020-01-01"},',
  '  {"action": "include", "user": "owner:alice"}',
  "]",
].join("\n");

function validateFilterText(text) {
  const trimmed = String(text == null ? "" : text).trim();
  if (!trimmed) {
    return {
      ok: true,
      empty: true,
      rules: null,
      message: "留空表示不筛选（包含全部文件）。",
    };
  }

  let parsed;
  try {
    parsed = JSON.parse(trimmed);
  } catch (err) {
    return {
      ok: false,
      empty: false,
      rules: null,
      message: "JSON 语法错误：" +
        (err && err.message ? err.message : "无法解析"),
    };
  }
  if (!Array.isArray(parsed)) {
    return {
      ok: false,
      empty: false,
      rules: null,
      message: "规则必须是 JSON 数组，例如 [ {...} ]。",
    };
  }
  if (parsed.length === 0) {
    return { ok: true, empty: true, rules: [], message: "空数组 = 不筛选。" };
  }

  let includes = 0;
  let excludes = 0;
  for (let i = 0; i < parsed.length; i += 1) {
    const rule = parsed[i];
    const label = "第 " + (i + 1) + " 条规则";
    if (!rule || typeof rule !== "object" || Array.isArray(rule)) {
      return {
        ok: false,
        empty: false,
        rules: null,
        message: label + "必须是对象。",
      };
    }
    if (rule.action !== undefined && rule.action !== "include" &&
        rule.action !== "exclude") {
      return {
        ok: false,
        empty: false,
        rules: null,
        message: label + "的 action 只能是 include 或 exclude。",
      };
    }
    const unknown = Object.keys(rule).filter(
      (key) => key !== "action" && FILTER_DIMENSIONS.indexOf(key) < 0
    );
    if (unknown.length > 0) {
      return {
        ok: false,
        empty: false,
        rules: null,
        message: label + "含有未知维度：" + unknown.join("、") +
          "（可用：" + FILTER_DIMENSIONS.join(" / ") + "）。",
      };
    }
    if (!FILTER_DIMENSIONS.some((dim) => rule[dim] !== undefined)) {
      return {
        ok: false,
        empty: false,
        rules: null,
        message: label + "至少要写一个维度：" +
          FILTER_DIMENSIONS.join(" / ") + "。",
      };
    }
    if (rule.action === "exclude") excludes += 1;
    else includes += 1;
  }

  return {
    ok: true,
    empty: false,
    rules: parsed,
    message: "规则合法：" + parsed.length + " 条（include " + includes +
      "，exclude " + excludes + "）。",
  };
}

function updateFilterStatus() {
  const result = validateFilterText($("filter").value);
  const status = $("filter-status");
  status.className = "hint filter-status" +
    (result.ok ? (result.empty ? "" : " is-ok") : " is-err");
  status.textContent = result.message;
  return result;
}

// mode: backup | restore；kind: archive | mirror（仅还原时有效）；pack：是否打包；
// encryption：加密算法；metadata：是否保存/还原元数据；busy：是否有任务在跑。
const state = {
  mode: "backup",
  kind: "archive",
  pack: false,
  compression: "deflate",
  encryption: "none",
  metadata: true,
  busy: false,
  currentVerb: "",
  currentTarget: "",
  currentTaskId: null,
  tasks: [],
  sse: null,
  sseWatchdog: 0,
  pollTimer: 0,
  pollFailures: 0,
  lastProgressSign: "",
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
      compressHidden: false,
      encryptHidden: false,
      // 密码行只在「选了加密算法」时出现；镜像模式没有加密，藏起来。
      passwordHidden: !(pack && state.encryption !== "none"),
      filterHidden: false,
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
    encryptHidden: true,
    // 还原归档需要密码输入；从备份目录还原（镜像）不需要。
    passwordHidden: state.kind !== "archive",
    filterHidden: true,
  };
}

// kind 为空字符串表示「闲置，还没跑过」—— 灯是灰的。
function setStatus(kind, text) {
  $("statusbar").className = "statusbar" + (kind ? " is-" + kind : "");
  $("status-text").textContent = text;
}

function syncMetadataHint() {
  if (state.mode === "backup") {
    $("metadata-hint").textContent = state.pack
      ? "把时间戳、Windows 属性等元数据写入 v4 归档；关闭后只备份文件内容。"
      : "把时间戳、Windows 属性等元数据写入 .backup-manifest.json；" +
        "关闭后只复制文件内容。";
  } else if (state.kind === "archive") {
    $("metadata-hint").textContent =
      "还原归档中保存的元数据（时间戳、属性等）；关闭后只写文件内容。";
  } else {
    $("metadata-hint").textContent =
      "读取备份目录中的 .backup-manifest.json 还原元数据；关闭后只写文件内容。";
  }
}

/// 筛选规则区：备份的两种模式都支持筛选（镜像模式写 .backup-manifest.json），
/// 只有还原模式不适用，整行隐藏。
function syncFilterHint() {
  const textarea = $("filter");
  const filterEnabled = state.mode === "backup" && !state.busy;
  textarea.disabled = !filterEnabled;
  $("filter-example").disabled = !filterEnabled;
  const hint = $("filter-hint");
  if (state.mode !== "backup") {
    hint.textContent = "";
  } else if (!state.pack) {
    hint.textContent =
      "镜像模式同样应用筛选：被排除的条目不复制，规则与元数据一起写入" +
      " .backup-manifest.json。";
  } else {
    hint.textContent =
      "JSON 数组；同一条规则内的多个维度是「且」，同一维度内逗号分隔是" +
      "「或」；include 为空时默认全选，命中 exclude 优先排除。";
  }
  updateFilterStatus();
}

function render(keepStatus) {
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
  $("row-encrypt").hidden = d.encryptHidden;
  $("row-password").hidden = d.passwordHidden;
  $("row-filter").hidden = d.filterHidden;

  $("metadata").checked = state.metadata;

  syncCompressionControls();
  syncEncryptionControls();
  syncMetadataHint();
  syncFilterHint();

  $("src-label").textContent = d.srcLabel;
  $("dst-label").textContent = d.dstLabel;
  $("src").placeholder = d.srcPlaceholder;
  $("dst").placeholder = d.dstPlaceholder;
  $("src-hint").textContent = d.srcHint;
  $("dst-hint").textContent = d.dstHint;

  $("go").textContent = state.busy
    ? "正在" + (state.currentVerb || d.verb) + "…"
    : "开始" + d.verb;
  $("go").disabled = state.busy;

  // 换模式或选项就把上一次的结果收起来，换回「接下来会发生什么」，
  // 免得上一轮的「完成」被误读成本轮的。任务运行中不动状态栏。
  if (!keepStatus) setStatus("", d.note);
}

for (const tab of document.querySelectorAll(".tab")) {
  tab.addEventListener("click", () => {
    if (state.busy) return;
    state.mode = tab.dataset.mode;
    render();
  });
}

for (const radio of document.querySelectorAll('input[name="kind"]')) {
  radio.addEventListener("change", () => {
    if (!radio.checked || state.busy) return;
    state.kind = radio.value;
    render();
  });
}

$("pack").addEventListener("change", (e) => {
  if (state.busy) return;
  state.pack = e.target.checked;
  render();
});

$("compress").addEventListener("change", (e) => {
  state.compression = e.target.value;
  syncCompressionControls();
});

$("encrypt").addEventListener("change", (e) => {
  state.encryption = e.target.value;
  render();
});

$("metadata").addEventListener("change", (e) => {
  state.metadata = e.target.checked;
  render();
});

// 密码显示/隐藏：把 input 的 type 在 password / text 之间切换。
$("password-toggle").addEventListener("click", () => {
  const input = $("password");
  const show = input.type === "password";
  input.type = show ? "text" : "password";
  $("password-toggle").textContent = show ? "隐藏" : "显示";
  $("password-toggle").setAttribute("aria-pressed", String(show));
});

$("filter").addEventListener("input", updateFilterStatus);

$("filter-example").addEventListener("click", () => {
  const textarea = $("filter");
  if (textarea.value.trim() &&
      !window.confirm("用示例覆盖当前的筛选规则？")) {
    return;
  }
  textarea.value = FILTER_EXAMPLE;
  updateFilterStatus();
  textarea.focus();
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

// ---- 异步任务：SSE 订阅 + 轮询降级 --------------------------------------
//
// 提交后后端立刻返回 task_id，任务在后台线程里跑；页面用 EventSource 订阅
// /api/tasks/{id}/stream 拿进度。EventSource 不可用、或长时间没有 data
// 事件时，退回 GET /api/tasks/{id} 轮询。两种来源最终都走 applyTask。

function normalizeState(rawState) {
  const raw = String(rawState == null ? "" : rawState).trim().toLowerCase();
  if (!raw) return "";
  if (["success", "succeeded", "ok", "done", "complete", "completed",
       "finished"].indexOf(raw) >= 0) {
    return "success";
  }
  if (["failed", "failure", "error", "cancelled", "canceled", "aborted",
       "timeout"].indexOf(raw) >= 0) {
    return "failed";
  }
  if (["running", "in_progress", "in-progress", "working", "processing",
       "active", "started"].indexOf(raw) >= 0) {
    return "running";
  }
  if (["queued", "pending", "waiting", "created", "new"].indexOf(raw) >= 0) {
    return "queued";
  }
  if (raw === "progress" || raw === "update") return "running";
  return raw;
}

function textOf(value) {
  if (value == null) return "";
  if (typeof value === "string") return value;
  if (typeof value === "number" || typeof value === "boolean") {
    return String(value);
  }
  return "";
}

/// error 字段可能是字符串，也可能是 {code,message} 对象。
function errorText(value) {
  if (value == null) return "";
  if (typeof value === "string") return value;
  if (typeof value === "object") {
    const message = textOf(value.message) || textOf(value.error) ||
      textOf(value.code);
    if (message) return message;
    try {
      return JSON.stringify(value);
    } catch (err) {
      return "";
    }
  }
  return String(value);
}

function findTask(id) {
  for (const task of state.tasks) {
    if (task.id === id) return task;
  }
  return null;
}

function upsertTask(task) {
  if (!task || !task.id) return;
  for (let i = 0; i < state.tasks.length; i += 1) {
    if (state.tasks[i].id === task.id) {
      const merged = {};
      for (const key of Object.keys(state.tasks[i])) {
        merged[key] = state.tasks[i][key];
      }
      for (const key of Object.keys(task)) merged[key] = task[key];
      state.tasks[i] = merged;
      return;
    }
  }
  state.tasks.unshift(task);
  if (state.tasks.length > 20) state.tasks.length = 20;
}

/// 把后端返回的任意任务 JSON 归一成界面用的结构，兼容：
/// id/task_id、state/status/type、progress 0–100 或 0–1、
/// message/stage/detail、error 字符串或对象、result 对象或字符串。
function normalizeTask(raw) {
  if (!raw || typeof raw !== "object") return null;
  const src = raw.task && typeof raw.task === "object" ? raw.task : raw;
  const request = src.request && typeof src.request === "object"
    ? src.request
    : {};

  let stateName = normalizeState(
    src.state != null ? src.state : (src.status != null ? src.status : src.type)
  );
  if (!stateName && src.done === true) stateName = "success";
  if (!stateName) stateName = src.error ? "failed" : "queued";

  const id = textOf(src.id != null ? src.id : src.task_id);
  if (!id) return null;

  let progress = Number(src.progress != null ? src.progress : src.percent);
  if (!Number.isFinite(progress)) {
    progress = Number.parseFloat(
      String(src.progress != null ? src.progress : "").replace("%", "")
    );
  }
  if (!Number.isFinite(progress) || progress < 0) progress = 0;
  if (progress > 100) progress = 100;
  if (stateName === "success") {
    progress = 100;
  } else if (progress > 0 && progress < 1) {
    progress = progress * 100;
  }
  progress = Math.round(progress);

  const known = findTask(id);
  const message = textOf(src.message) || textOf(src.stage) ||
    textOf(src.detail) || textOf(src.text);

  return {
    id: id,
    kind: textOf(src.kind) || (known ? known.kind : ""),
    state: stateName,
    terminal: stateName === "success" || stateName === "failed",
    progress: progress,
    message: message,
    error: errorText(src.error),
    result: src.result != null
      ? src.result
      : (src.results != null ? src.results : null),
    target: textOf(src.target) || textOf(src.dst) ||
      textOf(request.dst) || (known ? known.target : ""),
    createdAt: src.created_at != null
      ? src.created_at
      : (src.createdAt != null
          ? src.createdAt
          : (known ? known.createdAt : null)),
  };
}

function kindLabel(kind) {
  const raw = String(kind || "").toLowerCase();
  if (raw.indexOf("restore") >= 0 || raw.indexOf("unpack") >= 0) return "还原";
  if (raw.indexOf("backup") >= 0 || raw.indexOf("pack") >= 0) return "备份";
  return "任务";
}

function taskStateLabel(taskState) {
  if (taskState === "success") return "成功";
  if (taskState === "failed") return "失败";
  if (taskState === "running") return "进行中";
  return "排队中";
}

function formatBytes(value) {
  const num = Number(value);
  if (!Number.isFinite(num) || num < 0) return String(value);
  const units = ["B", "KB", "MB", "GB", "TB"];
  let size = num;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return (unit === 0 ? String(size) : size.toFixed(1)) + " " + units[unit];
}

function firstNumber(obj, keys) {
  for (const key of keys) {
    if (obj[key] == null) continue;
    const num = Number(obj[key]);
    if (Number.isFinite(num)) return num;
  }
  return null;
}

/// 成功结果：把 entries / bytes 显示成人话；后端加字段时也兼容。
function taskResultText(task) {
  const result = task && task.result;
  if (result == null) return "";
  if (typeof result === "string") return result;
  if (typeof result === "number") return String(result);
  if (typeof result !== "object") return "";

  const entries = firstNumber(result, [
    "entries", "entries_written", "files", "files_written", "count", "items",
  ]);
  const bytes = firstNumber(result, [
    "bytes", "bytes_written", "total_bytes", "size", "bytes_read",
  ]);
  const parts = [];
  if (entries != null) {
    parts.push(entries.toLocaleString("zh-CN") + " 个文件");
  }
  if (bytes != null) {
    parts.push(formatBytes(bytes) + "（" +
      bytes.toLocaleString("zh-CN") + " 字节）");
  }
  const skipped = firstNumber(result, ["skipped", "files_skipped"]);
  if (skipped != null && skipped > 0) parts.push("跳过 " + skipped + " 项");
  const filtered = firstNumber(result, ["filtered", "entries_filtered"]);
  if (filtered != null && filtered > 0) {
    parts.push("筛选排除 " + filtered + " 项");
  }
  if (parts.length > 0) return parts.join("，") + "。";

  return textOf(result.message) || textOf(result.detail);
}

function renderTasks() {
  const panel = $("tasks-panel");
  const list = $("task-list");
  const items = state.tasks.slice(0, 8);
  list.textContent = "";
  panel.hidden = items.length === 0;

  for (const task of items) {
    const item = document.createElement("li");
    item.className = "task-item";
    item.dataset.state = task.state;

    const top = document.createElement("div");
    top.className = "task-top";

    const kind = document.createElement("span");
    kind.className = "task-kind";
    kind.textContent = kindLabel(task.kind);

    const badge = document.createElement("span");
    badge.className = "task-badge";
    badge.textContent = taskStateLabel(task.state);

    const pct = document.createElement("span");
    pct.className = "task-pct";
    pct.textContent = task.state === "success"
      ? "100%"
      : (task.state === "failed" ? "—" : task.progress + "%");

    top.appendChild(kind);
    top.appendChild(badge);
    top.appendChild(pct);

    const mini = document.createElement("div");
    mini.className = "task-mini";
    const fill = document.createElement("i");
    fill.style.width = task.progress + "%";
    mini.appendChild(fill);

    const msg = document.createElement("p");
    msg.className = "task-msg";
    const summary = task.state === "failed"
      ? (task.error || task.message)
      : (taskResultText(task) || task.message);
    msg.textContent = summary || (task.state === "success" ? "完成" : "…");
    msg.title = summary || "";

    item.appendChild(top);
    item.appendChild(mini);
    item.appendChild(msg);
    list.appendChild(item);
  }
}

async function refreshTasks() {
  try {
    const resp = await fetch("/api/tasks", { cache: "no-store" });
    if (!resp.ok) return;
    const data = await resp.json();
    let rawList = null;
    if (Array.isArray(data)) rawList = data;
    else if (data && Array.isArray(data.tasks)) rawList = data.tasks;
    else if (data && Array.isArray(data.items)) rawList = data.items;
    if (!rawList) return;

    const refreshed = [];
    for (const raw of rawList) {
      const task = normalizeTask(raw);
      if (!task) continue;
      // 当前任务由 SSE / 轮询驱动，列表刷新时保留它的实时状态。
      if (task.id === state.currentTaskId) continue;
      refreshed.push(task);
      if (refreshed.length >= 20) break;
    }
    // 服务端列表是主来源，但刚结束/只在本地出现的任务不能因一次刷新被丢掉。
    const merged = refreshed.slice();
    for (const local of state.tasks) {
      if (merged.length >= 20) break;
      if (local.id === state.currentTaskId) continue;
      if (merged.some((task) => task.id === local.id)) continue;
      merged.push(local);
    }
    const current = state.currentTaskId ? findTask(state.currentTaskId) : null;
    state.tasks = merged;
    if (current) state.tasks.unshift(current);
    renderTasks();
  } catch (err) {
    // 任务接口不可用时静默跳过，不影响备份/还原主流程。
  }
}

function applyTask(raw) {
  const task = normalizeTask(raw);
  if (!task) return;
  upsertTask(task);
  renderTasks();
  if (task.id !== state.currentTaskId) return;
  updateRunProgress(task);
  if (task.terminal) finishTask(task);
}

function updateRunProgress(task) {
  const percent = Math.max(0, Math.min(100, task.progress));
  $("run-progress-fill").style.width = percent + "%";

  // 进度事件可能很密，状态栏文字只在内容变了才改。
  const signature = task.state + "|" + percent + "|" +
    task.message + "|" + task.error;
  if (signature === state.lastProgressSign) return;
  state.lastProgressSign = signature;

  const verb = state.currentVerb || describe().verb;
  let text = task.state === "queued"
    ? "排队中…"
    : "正在" + verb + "… " + percent + "%";
  if (task.message) text += " · " + task.message;
  setStatus("busy", text);
}

function finishTask(task) {
  const ok = task.state === "success";
  const verb = state.currentVerb || describe().verb;
  state.currentTaskId = null;
  stopTaskStreams();
  setBusy(false);

  showTaskResult(task, ok);
  if (ok) {
    setStatus("ok", describeSuccessLine(task, verb));
  } else {
    setStatus("err", describeFailureLine(task, verb));
  }
  refreshTasks();
}

function describeSuccessLine(task, verb) {
  const detail = taskResultText(task);
  if (detail) {
    return verb + "完成：" +
      (detail.slice(-1) === "。" ? detail : detail + "。");
  }
  if (task.message) return verb + "完成：" + task.message;
  return verb + "完成。";
}

function describeFailureLine(task, verb) {
  const reason = task.error || task.message || "未知错误。";
  return verb + "失败：" + reason;
}

function showTaskResult(task, ok) {
  const verb = state.currentVerb || describe().verb;
  const panel = $("result-panel");
  $("result-title").textContent = verb + (ok ? "完成" : "失败");

  const badge = $("result-state");
  badge.textContent = ok ? "成功" : "失败";
  badge.className = "result-state " + (ok ? "is-ok" : "is-err");

  const detail = ok ? taskResultText(task) : "";
  const detailEl = $("result-detail");
  detailEl.textContent = detail;
  detailEl.hidden = !detail;

  const error = ok ? "" : (task.error || task.message || "未知错误。");
  const errorEl = $("result-error");
  errorEl.textContent = error;
  errorEl.hidden = !error;

  const target = task.target || state.currentTarget || "";
  $("result-path-line").hidden = !target;
  $("result-path").textContent = target;
  $("result-copy").disabled = !target;
  $("result-copy").textContent = "复制路径";

  panel.hidden = false;
}

function stopTaskStreams() {
  if (state.sse) {
    try {
      state.sse.close();
    } catch (err) {
      // 已经关闭过，忽略。
    }
    state.sse = null;
  }
  if (state.sseWatchdog) {
    clearTimeout(state.sseWatchdog);
    state.sseWatchdog = 0;
  }
  if (state.pollTimer) {
    clearTimeout(state.pollTimer);
    state.pollTimer = 0;
  }
}

/// SSE 心跳是注释行（: ping），页面收不到；如果长时间没有任何 data 事件，
/// 说明流可能被中间层缓冲或已经半死，降级成轮询更可靠。
function resetSseWatchdog(taskId) {
  if (state.sseWatchdog) clearTimeout(state.sseWatchdog);
  state.sseWatchdog = setTimeout(() => {
    state.sseWatchdog = 0;
    if (state.currentTaskId !== taskId || !state.busy) return;
    if (state.sse) {
      try {
        state.sse.close();
      } catch (err) {
        // 忽略。
      }
      state.sse = null;
    }
    startPolling(taskId);
  }, 20000);
}

function handleStreamData(text, taskId) {
  if (!text) return;
  let data;
  try {
    data = JSON.parse(text);
  } catch (err) {
    return;
  }
  resetSseWatchdog(taskId);
  applyTask(data);
}

function subscribeTask(taskId) {
  stopTaskStreams();
  state.currentTaskId = taskId;
  state.pollFailures = 0;
  state.lastProgressSign = "";

  if (typeof EventSource !== "function") {
    startPolling(taskId);
    return;
  }

  let source;
  try {
    source = new EventSource(
      "/api/tasks/" + encodeURIComponent(taskId) + "/stream"
    );
  } catch (err) {
    startPolling(taskId);
    return;
  }
  state.sse = source;

  const onMessage = (event) => {
    if (state.currentTaskId !== taskId) return;
    if (event && typeof event.data === "string" && event.data) {
      handleStreamData(event.data, taskId);
    }
  };
  source.onmessage = onMessage;
  source.onopen = () => resetSseWatchdog(taskId);
  // 服务端目前只发未命名事件；命名事件也一并监听，以后改协议不会漏。
  for (const name of ["progress", "update", "task", "done", "complete",
                      "completed", "failed", "error"]) {
    source.addEventListener(name, onMessage);
  }
  source.onerror = () => {
    if (state.currentTaskId !== taskId) return;
    // 连接失败/被服务端关闭且任务还没终态：降级轮询。
    if (state.sse) {
      try {
        state.sse.close();
      } catch (err) {
        // 忽略。
      }
      state.sse = null;
    }
    startPolling(taskId);
  };
  resetSseWatchdog(taskId);
}

function startPolling(taskId) {
  if (state.pollTimer) {
    clearTimeout(state.pollTimer);
    state.pollTimer = 0;
  }
  state.pollFailures = 0;

  const tick = async () => {
    state.pollTimer = 0;
    if (state.currentTaskId !== taskId || !state.busy) return;
    try {
      const resp = await fetch(
        "/api/tasks/" + encodeURIComponent(taskId),
        { cache: "no-store" }
      );
      if (state.currentTaskId !== taskId || !state.busy) return;
      if (resp.status === 404) {
        failCurrentTask("任务不存在或已被清理。");
        return;
      }
      if (resp.ok) {
        state.pollFailures = 0;
        applyTask(await resp.json());
      } else {
        state.pollFailures += 1;
      }
    } catch (err) {
      state.pollFailures += 1;
    }
    if (state.currentTaskId !== taskId || !state.busy) return;
    if (state.pollFailures >= 8) {
      failCurrentTask("无法获取任务进度，请确认本地服务仍在运行。");
      return;
    }
    state.pollTimer = setTimeout(
      tick, state.pollFailures > 3 ? 2000 : 800
    );
  };
  state.pollTimer = setTimeout(tick, 500);
}

/// SSE / 轮询本身失败（连接断开、任务被清理）时的兜底收尾。
function failCurrentTask(message) {
  const task = normalizeTask({
    id: state.currentTaskId || "",
    kind: state.mode,
    state: "failed",
    progress: 0,
    error: message,
    target: state.currentTarget || "",
  });
  state.currentTaskId = null;
  stopTaskStreams();
  if (task) {
    upsertTask(task);
    renderTasks();
    finishTask(task);
  } else {
    setBusy(false);
    setStatus("err",
      (state.currentVerb || describe().verb) + "失败：" + message);
  }
}

/// 提交前后统一管理表单的禁用状态：fieldset.disabled 会级联到所有字段。
function setBusy(busy) {
  state.busy = busy;
  $("fields").disabled = busy;
  for (const tab of document.querySelectorAll(".tab")) tab.disabled = busy;

  const go = $("go");
  go.disabled = busy;
  go.textContent = busy
    ? "正在" + (state.currentVerb || describe().verb) + "…"
    : "开始" + describe().verb;

  $("run-progress").hidden = !busy;
  if (!busy) $("run-progress-fill").style.width = "0%";
}

/// 提交任务。新接口返回 {"task_id": "..."}；万一遇到还没迁移的同步接口，
/// 把返回文本当结果直接显示，不让界面卡死。
async function postTaskRequest(url, params) {
  const resp = await fetch(url, {
    method: "POST",
    headers: { Accept: "application/json" },
    // 必须用 URLSearchParams 拼 body，不要手写字符串拼接：
    //   1. 它会自动带上 Content-Type: application/x-www-form-urlencoded，
    //      httplib 靠这个才把字段填进 req.params；
    //   2. 它会把 + & = % 和所有非 ASCII 字节正确百分号编码。
    //      手写拼接的话，路径里一个字面 "+" 会被服务端解成空格。
    body: new URLSearchParams(params),
  });
  const text = (await resp.text()).trim();
  let data = null;
  if (text) {
    try {
      data = JSON.parse(text);
    } catch (err) {
      data = null;
    }
  }
  if (!resp.ok) {
    let reason = "";
    if (data && typeof data === "object") {
      reason = errorText(data.error) || textOf(data.message);
    }
    if (!reason && text && text.length <= 300) reason = text;
    throw new Error(reason || ("提交任务失败（HTTP " + resp.status + "）。"));
  }

  let taskId = "";
  if (data && typeof data === "object") {
    const nested = data.task && typeof data.task === "object"
      ? data.task
      : null;
    taskId = textOf(data.task_id) || textOf(data.taskId) ||
      textOf(data.id) ||
      (nested ? (textOf(nested.id) || textOf(nested.task_id)) : "");
  }
  if (taskId) return { taskId: taskId };
  if (text) return { legacyText: text };
  return { taskId: "" };
}

$("form").addEventListener("submit", async (e) => {
  e.preventDefault();
  if (state.busy) return;

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

  const passwordValue = $("password").value;
  let filterValue = null;

  if (state.mode === "backup") {
    if (state.pack && state.encryption !== "none" && !passwordValue) {
      setStatus("err", "已选择加密算法，请先填写密码。");
      $("password").focus();
      return;
    }
    const filterCheck = validateFilterText($("filter").value);
    if (!filterCheck.ok) {
      updateFilterStatus();
      setStatus("err", "筛选规则无效：" + filterCheck.message);
      $("filter").focus();
      return;
    }
    if (!filterCheck.empty) filterValue = JSON.stringify(filterCheck.rules);
  }

  // 字段名与语义和 CLI 的 --src / --dst / --pack / --archive 一一对应
  // （见 cli/main.cpp 的 backup / restore 分支），GUI 和 CLI 走的是同一批
  // backup:: 函数，行为不可能漂移。
  let url;
  const params = {};
  if (state.mode === "backup") {
    url = "/api/backup";
    params.src = src;
    params.dst = dst;
    params.metadata = state.metadata ? "1" : "0";
    // 筛选规则对打包和目录镜像两种模式都生效（见 http_server.cpp）。
    if (filterValue) params.filter = filterValue;
    if (state.pack) {
      params.pack = "vbak";
      const info = currentCompressionInfo();
      if (state.compression !== "none") {
        params.compress = state.compression;
        if (info.has_level) {
          const raw = Number.parseInt($("level").value, 10);
          const [minLevel, maxLevel] = compressionLevelRange(info);
          const level = Number.isFinite(raw)
            ? Math.min(maxLevel, Math.max(minLevel, raw))
            : minLevel;
          params.level = String(level);
        }
      }
      if (state.encryption !== "none") {
        params.encrypt = state.encryption;
        params.password = passwordValue;
      }
    }
  } else {
    url = "/api/restore";
    params.dst = dst;
    params.metadata = state.metadata ? "1" : "0";
    if (state.kind === "archive") {
      params.archive = src;
      if (passwordValue) params.password = passwordValue;
    } else {
      params.src = src;
    }
  }

  state.currentVerb = d.verb;
  state.currentTarget = dst;
  state.lastProgressSign = "";
  setBusy(true);
  setStatus("busy", "正在" + d.verb + "… 0%");

  try {
    const outcome = await postTaskRequest(url, params);
    if (!outcome.taskId) {
      // 老接口：同步返回结果文本，没有任务可订阅。当作一次成功显示，
      // 至少不会让「点了没反应」。
      const legacy = normalizeTask({
        id: "legacy-" + Date.now(),
        kind: state.mode,
        state: "success",
        progress: 100,
        result: outcome.legacyText || "",
        target: dst,
      });
      if (legacy) {
        upsertTask(legacy);
        renderTasks();
        finishTask(legacy);
      } else {
        setBusy(false);
        setStatus("ok", outcome.legacyText || "操作完成。");
      }
      return;
    }

    const pending = normalizeTask({
      id: outcome.taskId,
      kind: state.mode,
      state: "queued",
      progress: 0,
      message: "已提交，等待开始…",
      target: dst,
    });
    if (pending) upsertTask(pending);
    renderTasks();
    subscribeTask(outcome.taskId);
  } catch (err) {
    setBusy(false);
    setStatus("err", err && err.message
      ? err.message
      : "无法连接本地服务，请确认 FileBackup 仍在运行。");
  }
});

// ---- 结果区：复制目标路径 ------------------------------------------------
// 宿主目前只实现了窗口/选择框命令，没有「在资源管理器中打开」的命令，
// 所以先提供复制路径；等后端加了打开文件夹的命令再接上。
async function copyText(text) {
  try {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch (err) {
    // 落到 execCommand 兜底。
  }
  try {
    const area = document.createElement("textarea");
    area.value = text;
    area.setAttribute("readonly", "");
    area.style.position = "fixed";
    area.style.top = "-1000px";
    area.style.opacity = "0";
    document.body.appendChild(area);
    area.select();
    const ok = document.execCommand("copy");
    area.remove();
    return ok;
  } catch (err) {
    return false;
  }
}

$("result-copy").addEventListener("click", async () => {
  const text = $("result-path").textContent;
  if (!text) return;
  const button = $("result-copy");
  const ok = await copyText(text);
  button.textContent = ok ? "已复制" : "复制失败";
  setTimeout(() => {
    button.textContent = "复制路径";
  }, 1600);
});

$("tasks-refresh").addEventListener("click", () => {
  refreshTasks();
});

// ---- 启动 ----------------------------------------------------------------
$("filter").placeholder =
  "例如（六维：path / name / type / size / time / user）：\n" + FILTER_EXAMPLE;

fillCompressionOptions();
fillEncryptionOptions();
render();
loadAlgorithms();
refreshTasks();

// 打开窗口就能直接输入路径，不用先点一下输入框。
$("src").focus();
</script>
</body>
</html>)FBHTML";

}  // namespace backup
