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
inline constexpr char kIndexHtml[] = R"FBHTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FileBackup 文件备份</title>
<style>
  :root { color-scheme: light dark; }
  body { font-family: system-ui, "Microsoft YaHei", sans-serif;
         max-width: 46rem; margin: 2rem auto; padding: 0 1rem; line-height: 1.6; }
  h1 { font-size: 1.35rem; margin: 0 0 1rem; }
  fieldset { border: 1px solid #8884; border-radius: 6px;
             margin: 1rem 0; padding: .6rem 1rem 1rem; }
  legend { padding: 0 .4rem; font-weight: 600; font-size: .9rem; }
  label { display: block; font-size: .9rem; }
  .field { margin-top: .7rem; }
  input[type=text] { width: 100%; box-sizing: border-box; padding: .45rem .5rem;
                     font-family: inherit; font-size: .95rem;
                     border: 1px solid #8886; border-radius: 4px; }
  .row { display: flex; gap: 1.2rem; align-items: center; flex-wrap: wrap; }
  .row label { display: flex; gap: .35rem; align-items: center; margin: 0; }
  button { margin-top: 1rem; padding: .5rem 1.6rem; font-size: .95rem;
           font-family: inherit; cursor: pointer; border-radius: 5px;
           border: 1px solid transparent; background: #0a7; color: #fff; }
  button:disabled { opacity: .5; cursor: default; }
  #out { white-space: pre-wrap; word-break: break-all; background: #8881;
         border-radius: 6px; padding: .8rem; min-height: 2.6rem; margin-top: 1rem;
         font-family: ui-monospace, Consolas, monospace; font-size: .9rem; }
  .hint { font-weight: 400; opacity: .65; }
  .hidden { display: none; }
</style>
</head>
<body>
<h1>FileBackup 文件备份</h1>

<form id="form">
  <div class="row">
    <strong>操作：</strong>
    <label><input type="radio" name="mode" value="backup" checked> 备份</label>
    <label><input type="radio" name="mode" value="restore"> 还原</label>
  </div>

  <fieldset id="panel-backup">
    <legend>备份</legend>
    <div class="field">
      <label for="backup-src">源目录</label>
      <input type="text" id="backup-src" placeholder="D:\我的文档">
    </div>
    <div class="field">
      <label for="backup-dst">目标 <span id="dst-hint" class="hint">（目录）</span></label>
      <input type="text" id="backup-dst" placeholder="D:\备份">
    </div>
    <div class="field">
      <label class="row"><input type="checkbox" id="pack"> 打包成单个 .vbak 文件</label>
    </div>
  </fieldset>

  <fieldset id="panel-restore" class="hidden">
    <legend>还原</legend>
    <div class="field">
      <label>来源类型</label>
      <div class="row">
        <label><input type="radio" name="restore_kind" value="archive" checked> .vbak 归档文件</label>
        <label><input type="radio" name="restore_kind" value="mirror"> 备份目录</label>
      </div>
    </div>
    <div class="field">
      <label for="restore-src">来源路径</label>
      <input type="text" id="restore-src" placeholder="D:\备份\docs.vbak">
    </div>
    <div class="field">
      <label for="restore-dst">还原到</label>
      <input type="text" id="restore-dst" placeholder="D:\还原结果">
    </div>
  </fieldset>

  <button type="submit" id="go">开始</button>
</form>

<pre id="out">就绪。</pre>

<script>
"use strict";
const $ = (id) => document.getElementById(id);

function currentMode() {
  return document.querySelector("input[name=mode]:checked").value;
}

function syncPanels() {
  const isBackup = currentMode() === "backup";
  $("panel-backup").classList.toggle("hidden", !isBackup);
  $("panel-restore").classList.toggle("hidden", isBackup);
}

document.querySelectorAll("input[name=mode]").forEach(
  (r) => r.addEventListener("change", syncPanels));
syncPanels();

$("pack").addEventListener("change", () => {
  $("dst-hint").textContent = $("pack").checked ? "（.vbak 文件）" : "（目录）";
});

$("form").addEventListener("submit", async (e) => {
  e.preventDefault();

  let url, params;
  if (currentMode() === "backup") {
    url = "/api/backup";
    params = { src: $("backup-src").value, dst: $("backup-dst").value };
    if ($("pack").checked) params.pack = "vbak";
  } else {
    url = "/api/restore";
    const kind = document.querySelector("input[name=restore_kind]:checked").value;
    params = { dst: $("restore-dst").value };
    // 归档文件走 archive 字段，备份目录走 src 字段 —— 与 CLI 的
    // --archive / --src 一一对应，参数语义完全一致。
    if (kind === "archive") params.archive = $("restore-src").value;
    else params.src = $("restore-src").value;
  }

  const go = $("go");
  go.disabled = true;
  $("out").textContent = "处理中…";

  try {
    // 必须用 URLSearchParams 拼 body，不要手写字符串拼接：
    //   1. 它会自动带上 Content-Type: application/x-www-form-urlencoded，
    //      httplib 靠这个才把字段填进 req.params；
    //   2. 它会把 + & = % 和所有非 ASCII 字节正确百分号编码。
    //      手写拼接的话，路径里一个字面 "+" 会被服务端解成空格。
    const r = await fetch(url, { method: "POST", body: new URLSearchParams(params) });
    const text = await r.text();
    $("out").textContent = (r.ok ? "" : "[" + r.status + "] ") + text;
  } catch (err) {
    $("out").textContent = "请求失败：" + err;
  } finally {
    go.disabled = false;
  }
});
</script>
</body>
</html>)FBHTML";

}  // namespace backup
