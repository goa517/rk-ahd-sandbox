/* 多路摄像头预览前端：WebRTC 拉流 + 自由编排 + 参数控制 */
"use strict";

const RESOLUTION_PRESETS = [
  [640, 360], [1280, 720], [1920, 1080], [2560, 1440],
];

const state = {
  channels: [],            // /api/channels 返回
  stats: new Map(),        // id -> ChannelStats
  pcs: new Map(),          // id -> RTCPeerConnection
  receivers: new Map(),    // id -> RTCRtpReceiver（读 RTP 时间戳用）
  selected: null,
  layout: loadLayout(),    // id -> {col, row, order}
  expanded: null,
};

/* ---------- 时钟对时（板端墙上时钟 = performance.now() + offsetMs） ---------- */
const clockSync = { offsetMs: 0, ready: false };

async function syncClock() {
  let best = null;
  for (let i = 0; i < 5; i++) {
    try {
      const t0 = performance.now();
      const res = await fetch("/api/clock", { cache: "no-store" });
      const { server_us } = await res.json();
      const t1 = performance.now();
      const rtt = t1 - t0;
      if (!best || rtt < best.rtt) best = { rtt, offsetMs: server_us / 1000 - (t0 + t1) / 2 };
    } catch { /* 忽略单次失败 */ }
  }
  if (best) {
    clockSync.offsetMs = best.offsetMs;
    clockSync.ready = true;
  }
}

function boardNowMs() { return performance.now() + clockSync.offsetMs; }

state.lastLat = new Map();   // id -> 端到端延迟 EMA（ms），按帧到达事件更新
state.lastRtpTs = new Map(); // id -> 最近收到的 RTP 时间戳（90kHz，已解绕）

/* 高频轮询（20ms）RTP 时间戳变化：检测到新帧到达的时刻计算延迟，
   避免低频采样与帧到达锁相导致的 ±33ms 读数跳变 */
function pollLatency() {
  if (!clockSync.ready) return;
  for (const [id, recv] of state.receivers) {
    if (typeof recv.getSynchronizationSources !== "function") continue;
    const srcs = recv.getSynchronizationSources();
    const rtpTs = srcs?.length ? srcs[0].rtpTimestamp : null;
    if (rtpTs == null) continue;
    const ts = unwrapTs90(rtpTs, boardNowMs() * 90);
    if (state.lastRtpTs.get(id) === ts) continue; // 无新帧
    state.lastRtpTs.set(id, ts);
    const lat = Math.max(0, boardNowMs() - ts / 90);
    const prev = state.lastLat.get(id);
    state.lastLat.set(id, prev == null ? lat : prev * 0.6 + lat * 0.4);
  }
}

/* 32bit RTP 时间戳（90kHz）按参考点解绕 */
function unwrapTs90(ts, ref) {
  const MOD = 4294967296;
  let t = ts;
  while (t - ref > MOD / 2) t -= MOD;
  while (ref - t > MOD / 2) t += MOD;
  return t;
}

function fmtCapTime(ms) {
  const d = new Date(ms);
  const p = (n, w = 2) => String(n).padStart(w, "0");
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
}

/* 每窗格悬浮信息：采集时间 + 端到端延迟；顶栏显示可见通道延迟中位数 */
function updateOverlays() {
  const lats = [];
  for (const ch of state.channels) {
    const pane = paneEl(ch.id);
    const infoEl = pane?.querySelector(".overlay-info");
    if (!infoEl) continue;
    if (!ch.online) { infoEl.style.display = "none"; continue; }
    infoEl.style.display = "";
    const timeEl = infoEl.querySelector(".cap-time");
    const latEl = infoEl.querySelector(".latency");

    const ts = state.lastRtpTs.get(ch.id);
    if (ts == null) {
      timeEl.textContent = "--:--:--.---";
      latEl.textContent = "";
      latEl.className = "latency";
      continue;
    }
    timeEl.textContent = fmtCapTime(ts / 90);
    const lat = state.lastLat.get(ch.id);
    if (lat == null) continue;
    lats.push(lat);
    if (state.selected === ch.id) {
      const dl = document.getElementById("d-lat");
      if (dl) dl.textContent = `${lat.toFixed(0)} ms`;
    }
    latEl.textContent = `${lat.toFixed(0)}ms`;
    latEl.className = "latency " + (lat < 150 ? "lat-ok" : lat < 300 ? "lat-warn" : "lat-bad");
  }
  const medEl = document.getElementById("latency-med");
  if (lats.length) {
    lats.sort((a, b) => a - b);
    medEl.textContent = `延迟 ${lats[Math.floor(lats.length / 2)].toFixed(0)}ms`;
  } else {
    medEl.textContent = "延迟 --ms";
  }
}

function loadLayout() {
  try { return JSON.parse(localStorage.getItem("mcp-layout") || "{}"); }
  catch { return {}; }
}
function saveLayout() {
  localStorage.setItem("mcp-layout", JSON.stringify(state.layout));
}
function layoutOf(id) {
  if (!state.layout[id]) state.layout[id] = { col: 4, row: 1, order: 99 };
  return state.layout[id];
}

/* ---------- 初始化 ---------- */
async function init() {
  await refreshChannels();
  renderGrid();
  connectStats();
  bindToolbar();
  syncClock();
  setInterval(syncClock, 30000);
  setInterval(pollLatency, 20);
  setInterval(updateOverlays, 100);
  setInterval(refreshChannelsAndStreams, 10000);
  setInterval(updateRTT, 5000);
  setInterval(reportRtcStats, 5000);
  adjustRowHeight();
  window.addEventListener("resize", adjustRowHeight);
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && state.expanded) toggleExpand(state.expanded);
  });
}

function adjustRowHeight() {
  const grid = document.getElementById("grid");
  const h = Math.max(160, Math.floor((grid.clientHeight - 6 * 4) / 3));
  grid.style.gridAutoRows = h + "px";
}

async function refreshChannels() {
  const res = await fetch("/api/channels");
  state.channels = await res.json();
  state.channels.forEach((ch, i) => {
    const l = layoutOf(ch.id);
    // 拼接画面默认占更大格子（仅首次布局，用户可拖拽覆盖）
    if (ch.type === "stitch" && l.order === 99 && l.col === 4 && l.row === 1) {
      l.col = 6; l.row = 2;
    }
    if (l.order === 99) l.order = i;
  });
}

/* 周期刷新：发现新上线通道则起播，下线则断开 */
async function refreshChannelsAndStreams() {
  const prevOnline = new Map(state.channels.map((c) => [c.id, c.online]));
  await refreshChannels();
  for (const ch of state.channels) {
    const was = prevOnline.get(ch.id);
    if (ch.online && !was) startStream(ch);
    if (!ch.online && was) stopStream(ch.id);
  }
  syncPanes();
  refreshDetails();
}

/* ---------- 网格渲染 ---------- */
function renderGrid() {
  const grid = document.getElementById("grid");
  grid.innerHTML = "";
  const sorted = [...state.channels].sort((a, b) => layoutOf(a.id).order - layoutOf(b.id).order);
  sorted.forEach((ch, idx) => { layoutOf(ch.id).order = idx; });
  for (const ch of sorted) grid.appendChild(buildPane(ch));
  saveLayout();
}

function buildPane(ch) {
  const l = layoutOf(ch.id);
  const pane = document.createElement("div");
  pane.className = "pane";
  pane.dataset.id = ch.id;
  pane.draggable = true;
  pane.style.gridColumn = `span ${l.col}`;
  pane.style.gridRow = `span ${l.row}`;
  pane.style.order = l.order;

  const video = document.createElement("video");
  video.autoplay = true;
  video.muted = true;
  video.playsInline = true;
  pane.appendChild(video);

  const top = document.createElement("div");
  top.className = "overlay-top";
  top.innerHTML = `<span class="name"></span><span class="live"></span>`;
  pane.appendChild(top);

  const pill = document.createElement("div");
  pill.className = "status-pill";
  pill.textContent = "连接中…";
  pane.appendChild(pill);

  const tools = document.createElement("div");
  tools.className = "tools";
  const btnExpand = document.createElement("button");
  btnExpand.textContent = "⤢";
  btnExpand.title = "放大 / 还原（Esc）";
  btnExpand.addEventListener("click", (e) => { e.stopPropagation(); toggleExpand(ch.id); });
  tools.appendChild(btnExpand);
  pane.appendChild(tools);

  const info = document.createElement("div");
  info.className = "overlay-info";
  info.innerHTML = `<span class="cap-time">--:--:--.---</span><span class="latency"></span>`;
  pane.appendChild(info);

  const handle = document.createElement("div");
  handle.className = "resize-handle";
  pane.appendChild(handle);
  bindResize(pane, handle, ch.id);

  bindDrag(pane, ch.id);

  pane.addEventListener("click", () => selectChannel(ch.id));
  pane.addEventListener("dblclick", () => toggleExpand(ch.id));

  syncPane(ch);
  return pane;
}

/* 按最新状态同步 pane 内容（不重建 DOM，避免视频重载） */
function syncPanes() {
  for (const ch of state.channels) syncPane(ch);
}

function syncPane(ch) {
  const pane = paneEl(ch.id);
  if (!pane) return;
  const st = state.stats.get(ch.id);
  const nameEl = pane.querySelector(".name");
  const liveEl = pane.querySelector(".live");
  nameEl.textContent = ch.name || ch.id;

  let placeholder = pane.querySelector(".placeholder");
  if (ch.online) {
    if (placeholder) placeholder.remove();
    const fps = st ? ` · ${st.fps.toFixed(0)}fps` : "";
    const kbps = st && st.kbps > 0 ? ` · ${(st.kbps / 1000).toFixed(1)}Mbps` : "";
    liveEl.textContent = `${ch.width}×${ch.height}@${ch.fps}${kbps}${fps}`;
  } else {
    liveEl.textContent = "";
    if (!placeholder) {
      placeholder = document.createElement("div");
      placeholder.className = "placeholder";
      pane.insertBefore(placeholder, pane.firstChild?.nextSibling);
    }
    const reason = ch.enabled ? (ch.last_error ? "设备异常" : "未连接") : "未启用";
    placeholder.innerHTML = `<div class="icon">📷</div><div>${escapeHtml(ch.name || ch.id)}</div><div class="muted">${reason}</div>`;
  }
}

function paneEl(id) {
  return document.querySelector(`.pane[data-id="${CSS.escape(id)}"]`);
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

/* ---------- 拖拽换位 ---------- */
function bindDrag(pane, id) {
  pane.addEventListener("dragstart", (e) => {
    e.dataTransfer.setData("text/plain", id);
    pane.classList.add("dragging");
  });
  pane.addEventListener("dragend", () => pane.classList.remove("dragging"));
  pane.addEventListener("dragover", (e) => { e.preventDefault(); pane.classList.add("drag-over"); });
  pane.addEventListener("dragleave", () => pane.classList.remove("drag-over"));
  pane.addEventListener("drop", (e) => {
    e.preventDefault();
    pane.classList.remove("drag-over");
    const srcId = e.dataTransfer.getData("text/plain");
    if (!srcId || srcId === id) return;
    const a = layoutOf(srcId), b = layoutOf(id);
    [a.order, b.order] = [b.order, a.order];
    saveLayout();
    renderGrid();
    reattachVideos();
  });
}

/* renderGrid 重建 DOM 后，把既有视频流挂回新 video 元素 */
function reattachVideos() {
  for (const [id, entry] of state.streamEls || []) {
    const pane = paneEl(id);
    if (pane) {
      const v = pane.querySelector("video");
      v.srcObject = entry.stream;
    }
  }
}

/* ---------- 缩放手柄 ---------- */
function bindResize(pane, handle, id) {
  handle.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    e.stopPropagation();
    handle.setPointerCapture(e.pointerId);
    const grid = document.getElementById("grid");
    const gridRect = grid.getBoundingClientRect();
    const cellW = (gridRect.width - 6 * 13) / 12;
    const cellH = parseFloat(getComputedStyle(grid).gridAutoRows) || 200;
    const startRect = pane.getBoundingClientRect();
    const l = layoutOf(id);

    const onMove = (ev) => {
      const col = clamp(Math.round((ev.clientX - startRect.left) / cellW), 1, 12);
      const row = clamp(Math.round((ev.clientY - startRect.top) / cellH), 1, 4);
      pane.style.gridColumn = `span ${col}`;
      pane.style.gridRow = `span ${row}`;
      l.col = col; l.row = row;
    };
    const onUp = () => {
      handle.removeEventListener("pointermove", onMove);
      handle.removeEventListener("pointerup", onUp);
      saveLayout();
    };
    handle.addEventListener("pointermove", onMove);
    handle.addEventListener("pointerup", onUp);
  });
}

function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

/* ---------- 单路放大（纯前端） ---------- */
function toggleExpand(id) {
  const pane = paneEl(id);
  if (!pane) return;
  if (state.expanded === id) {
    pane.classList.remove("expanded");
    state.expanded = null;
  } else {
    if (state.expanded) paneEl(state.expanded)?.classList.remove("expanded");
    pane.classList.add("expanded");
    state.expanded = id;
  }
}

/* ---------- WebRTC 拉流 ---------- */
state.streamEls = new Map(); // id -> {stream}

async function startStream(ch) {
  stopStream(ch.id);
  const pane = paneEl(ch.id);
  pane?.classList.add("connecting");

  const pc = new RTCPeerConnection();
  state.pcs.set(ch.id, pc);

  pc.addTransceiver("video", { direction: "recvonly" });
  pc.ontrack = (ev) => {
    state.streamEls.set(ch.id, { stream: ev.streams[0] });
    state.receivers.set(ch.id, ev.receiver);
    const v = paneEl(ch.id)?.querySelector("video");
    if (v) v.srcObject = ev.streams[0];
    paneEl(ch.id)?.classList.remove("connecting");
  };
  pc.onconnectionstatechange = () => {
    if (["failed", "closed"].includes(pc.connectionState)) {
      stopStream(ch.id);
      const cur = state.channels.find((c) => c.id === ch.id);
      if (cur?.online) setTimeout(() => startStream(cur), 2000);
    }
  };

  try {
    await pc.setLocalDescription(await pc.createOffer());
    await waitIceComplete(pc);
    const res = await fetch(`/api/whep/${encodeURIComponent(ch.id)}`, {
      method: "POST",
      headers: { "Content-Type": "application/sdp" },
      body: pc.localDescription.sdp,
    });
    if (!res.ok) throw new Error(await res.text());
    await pc.setRemoteDescription({ type: "answer", sdp: await res.text() });
  } catch (err) {
    console.error("拉流失败", ch.id, err);
    stopStream(ch.id);
    pane?.classList.remove("connecting");
  }
}

function stopStream(id) {
  const pc = state.pcs.get(id);
  if (pc) { pc.close(); state.pcs.delete(id); }
  state.streamEls.delete(id);
  state.receivers.delete(id);
  state.lastRtpTs.delete(id);
  state.lastLat.delete(id);
  const v = paneEl(id)?.querySelector("video");
  if (v) v.srcObject = null;
}

function waitIceComplete(pc) {
  return new Promise((resolve) => {
    if (pc.iceGatheringState === "complete") return resolve();
    const timer = setTimeout(resolve, 3000); // 兜底，避免极端情况下卡死
    pc.addEventListener("icegatheringstatechange", () => {
      if (pc.iceGatheringState === "complete") { clearTimeout(timer); resolve(); }
    });
  });
}

/* ---------- 统计（SSE） ---------- */
function connectStats() {
  const es = new EventSource("/ws/stats");
  es.onopen = () => setConn(true);
  es.onerror = () => setConn(false);
  es.onmessage = (ev) => {
    const st = JSON.parse(ev.data);
    setConn(true);
    document.getElementById("online-count").textContent = `${st.online}/${st.total} 在线`;
    document.getElementById("enc-load").textContent = `编码负载 ${st.encoder_load_pct.toFixed(0)}%`;
    if (st.system) {
      const s = st.system;
      document.getElementById("sys-info").textContent =
        `${s.platform} · ${s.cpu} ${s.cores}核 · CPU ${s.cpu_pct.toFixed(0)}% · ${s.temp_c.toFixed(1)}°C · 内存 ${(s.mem_used_mb / 1024).toFixed(1)}/${(s.mem_total_mb / 1024).toFixed(1)}GB`;
      document.getElementById("net-rx").textContent = (s.net_rx_kbps / 1000).toFixed(1);
      document.getElementById("net-tx").textContent = (s.net_tx_kbps / 1000).toFixed(1);
    }
    state.stats = new Map(st.channels.map((c) => [c.id, c]));
    syncPanes();
    refreshDetails();
  };
}

function refreshDetails() {
  if (!state.selected) return;
  fillDetails(state.channels.find((c) => c.id === state.selected));
}

function setConn(ok) {
  const dot = document.getElementById("conn-dot");
  dot.className = "dot " + (ok ? "dot-ok" : "dot-bad");
  document.getElementById("conn-text").textContent = ok ? "已连接" : "连接断开";
}

/* ---------- 浏览器侧 WebRTC 统计（诊断用，上报板端 + 面板显示） ---------- */
state.rtcStats = new Map(); // id -> 最近一次统计

async function collectRtcStats(id) {
  const pc = state.pcs.get(id);
  if (!pc) return null;
  let inb = null, pair = null;
  try {
    (await pc.getStats()).forEach((r) => {
      if (r.type === "inbound-rtp" && r.kind === "video") inb = r;
      if (r.type === "candidate-pair" && r.state === "succeeded" && r.currentRoundTripTime != null) pair = r;
    });
  } catch { return null; }
  if (!inb) return null;
  return {
    id,
    lost: inb.packetsLost ?? 0,
    recv: inb.packetsReceived ?? 0,
    jitter_ms: +((inb.jitter ?? 0) * 1000).toFixed(1),
    jb_ms: +(((inb.jitterBufferDelay ?? 0) / (inb.jitterBufferEmittedCount || 1)) * 1000).toFixed(1),
    jb_target_ms: +(((inb.jitterBufferTargetDelay ?? 0) / (inb.jitterBufferEmittedCount || 1)) * 1000).toFixed(1),
    decode_ms: +(((inb.totalDecodeTime ?? 0) / (inb.framesDecoded || 1)) * 1000).toFixed(1),
    dropped: inb.framesDropped ?? 0,
    pli: inb.pliCount ?? 0,
    keyframes: inb.keyFramesDecoded ?? 0,
    rtt_ms: pair ? +(pair.currentRoundTripTime * 1000).toFixed(1) : null,
  };
}

async function reportRtcStats() {
  for (const id of state.pcs.keys()) {
    const s = await collectRtcStats(id);
    if (!s) continue;
    state.rtcStats.set(id, s);
    fetch("/api/debug/rtc-stats", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(s),
    }).catch(() => {});
  }
  refreshDetails();
}

/* RTT 中位数（候选对 RTT，近似链路延迟） */
async function updateRTT() {
  const rtts = [];
  for (const pc of state.pcs.values()) {
    try {
      const report = await pc.getStats();
      report.forEach((r) => {
        if (r.type === "candidate-pair" && r.state === "succeeded" && r.currentRoundTripTime != null) {
          rtts.push(r.currentRoundTripTime * 1000);
        }
      });
    } catch { /* ignore */ }
  }
  if (rtts.length) {
    rtts.sort((a, b) => a - b);
    const med = rtts[Math.floor(rtts.length / 2)];
    document.getElementById("net-rtt").textContent = `RTT ${med.toFixed(0)}ms`;
  }
}

/* ---------- 参数面板 ---------- */
function selectChannel(id) {
  state.selected = id;
  document.querySelectorAll(".pane.selected").forEach((p) => p.classList.remove("selected"));
  paneEl(id)?.classList.add("selected");
  openPanel();
  fillPanel();
}

function openPanel() { document.getElementById("panel").classList.remove("collapsed"); }
function closePanel() { document.getElementById("panel").classList.add("collapsed"); }

/* ---------- 通道详情（摄像头 / 采集 / 编码 / 传输） ---------- */
function detailRows(rows) {
  return rows
    .filter(([, v]) => v != null && v !== "")
    .map(([k, v]) => `<dt>${escapeHtml(k)}</dt><dd>${escapeHtml(String(v))}</dd>`)
    .join("");
}

function fillDetails(ch) {
  const el = document.getElementById("panel-details");
  if (!ch) { el.hidden = true; el.innerHTML = ""; return; }
  el.hidden = false;
  const st = state.stats.get(ch.id);
  const lat = state.lastLat.get(ch.id);

  const camRows = [
    ["设备", ch.camera],
    ["驱动", ch.driver && ch.bus ? `${ch.driver} · ${ch.bus}` : ch.driver],
    ["节点", ch.device],
    ["状态", ch.online ? "在线" : `离线${ch.last_error ? `（${ch.last_error}）` : ""}`],
  ];
  const capRows = ch.type === "stitch"
    ? [
        ["拼接源", ch.online ? "4 路 AHD 原始帧 tap（dma-buf 零拷贝）" : null],
        ["合成", ch.online ? "RGA 裁剪/旋转/缩放 → NV12 画布" : null],
        ["源帧率", ch.source_fps ? `${ch.source_fps} fps` : null],
      ]
    : [
        ["格式", ch.format ? `${ch.format}${ch.stride ? ` · stride ${ch.stride}` : ""}` : null],
        ["源帧率", ch.source_fps ? `${ch.source_fps} fps` : null],
        ["缓冲", ch.online ? "16 × dma-buf（零拷贝直送编码器）" : null],
        ["采集时间戳", ch.online ? "ISP 出帧时刻（墙上时钟）" : null],
      ];
  const encRows = [
    ["编码器", "rkvenc 硬件 H.265 · CBR"],
    ["输出", `${ch.width}×${ch.height}@${ch.fps} · ${(ch.bitrate_kbps / 1000).toFixed(1)} Mbps · GOP ${ch.gop}`],
    st && ch.online ? ["实测", `${st.fps.toFixed(1)} fps · ${(st.kbps / 1000).toFixed(2)} Mbps`] : null,
    ["观众", String(st?.viewers ?? ch.viewers ?? 0)],
  ];
  const rs = state.rtcStats.get(ch.id);
  const netRows = rs ? [
    ["丢包", `${rs.lost} / ${rs.recv}（${rs.recv ? ((rs.lost / (rs.recv + rs.lost)) * 100).toFixed(2) : 0}%）`],
    ["网络抖动", `${rs.jitter_ms} ms`],
    ["抖动缓冲", `${rs.jb_ms} ms（目标 ${rs.jb_target_ms} ms）`],
    ["解码", `${rs.decode_ms} ms/帧 · 丢弃 ${rs.dropped} 帧`],
    ["RTT", rs.rtt_ms != null ? `${rs.rtt_ms} ms` : null],
  ] : [];

  el.innerHTML =
    `<div class="detail-group">摄像头</div><dl>${detailRows(camRows)}</dl>` +
    `<div class="detail-group">采集管线</div><dl>${detailRows(capRows)}</dl>` +
    `<div class="detail-group">编码 / 传输</div><dl>${detailRows(encRows)}</dl>` +
    (netRows.length ? `<div class="detail-group">网络 / 解码（浏览器侧）</div><dl>${detailRows(netRows)}</dl>` : "") +
    (ch.online
      ? `<div class="detail-group">延迟</div><dl><dt>端到端</dt><dd id="d-lat">${lat != null ? lat.toFixed(0) + " ms" : "--"}</dd><dt>说明</dt><dd class="muted">采集 → 浏览器收到，10Hz 刷新</dd></dl>`
      : "");
}

function fillPanel() {
  const ch = state.channels.find((c) => c.id === state.selected);
  const form = document.getElementById("panel-form");
  const empty = document.getElementById("panel-empty");
  fillDetails(ch);
  if (!ch || !ch.online || (ch.type !== "raw" && ch.type !== "ahd" && ch.type !== "stitch")) {
    form.hidden = true;
    empty.hidden = false;
    empty.textContent = ch ? `「${ch.name}」离线或类型暂不支持调整` : "点击选择一个在线通道";
    document.getElementById("panel-title").textContent = ch ? ch.name : "参数";
    return;
  }
  form.hidden = false;
  empty.hidden = true;
  document.getElementById("panel-title").textContent = ch.name;

  const resSel = document.getElementById("f-res");
  resSel.innerHTML = "";
  // AHD 输入源最高 1080p，rkcif 无缩放器，超过输入会被驱动 clamp
  const maxSrcW = ch.type === "ahd" ? 1920 : Infinity;
  for (const [w, h] of RESOLUTION_PRESETS) {
    if (w > maxSrcW) continue;
    const opt = document.createElement("option");
    opt.value = `${w}x${h}`;
    opt.textContent = `${w} × ${h}`;
    if (w === ch.width && h === ch.height) opt.selected = true;
    resSel.appendChild(opt);
  }
  if (![...resSel.options].some((o) => o.selected)) {
    const opt = document.createElement("option");
    opt.value = `${ch.width}x${ch.height}`;
    opt.textContent = `${ch.width} × ${ch.height}（当前）`;
    opt.selected = true;
    resSel.appendChild(opt);
  }

  const fps = document.getElementById("f-fps");
  fps.max = ch.source_fps || 30;
  fps.value = ch.fps;
  document.getElementById("f-fps-val").textContent = `${ch.fps} fps`;

  const br = document.getElementById("f-br");
  br.value = ch.bitrate_kbps;
  document.getElementById("f-br-val").textContent = `${(ch.bitrate_kbps / 1000).toFixed(1)} Mbps`;

  document.getElementById("f-gop").value = ch.gop;
}

async function applyParams() {
  const ch = state.channels.find((c) => c.id === state.selected);
  if (!ch) return;
  const [w, h] = document.getElementById("f-res").value.split("x").map(Number);
  const body = {
    width: w,
    height: h,
    fps: Number(document.getElementById("f-fps").value),
    bitrate_kbps: Number(document.getElementById("f-br").value),
    gop: Number(document.getElementById("f-gop").value),
  };
  const btn = document.getElementById("f-apply");
  btn.disabled = true;
  try {
    const res = await fetch(`/api/channels/${encodeURIComponent(ch.id)}`, {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    if (!res.ok) throw new Error(await res.text());
    const info = await res.json();
    Object.assign(ch, info);
    toast(`「${ch.name}」参数已生效`);
    syncPane(ch);
  } catch (err) {
    toast(`参数设置失败: ${err.message}`, true);
  } finally {
    btn.disabled = false;
  }
}

function toast(msg, isErr = false) {
  const el = document.createElement("div");
  el.className = "toast" + (isErr ? " err" : "");
  el.textContent = msg;
  document.getElementById("toast-root").appendChild(el);
  setTimeout(() => el.remove(), 2200);
}

/* ---------- 工具栏 ---------- */
function bindToolbar() {
  document.querySelectorAll("#bottombar [data-preset]").forEach((btn) => {
    btn.addEventListener("click", () => applyPreset(btn.dataset.preset));
  });
  document.getElementById("btn-panel").addEventListener("click", () => {
    document.getElementById("panel").classList.toggle("collapsed");
  });
  document.getElementById("panel-close").addEventListener("click", closePanel);
  document.getElementById("btn-fullscreen").addEventListener("click", () => {
    if (document.fullscreenElement) document.exitFullscreen();
    else document.documentElement.requestFullscreen();
  });
  document.getElementById("f-apply").addEventListener("click", applyParams);
  document.getElementById("f-fps").addEventListener("input", (e) => {
    document.getElementById("f-fps-val").textContent = `${e.target.value} fps`;
  });
  document.getElementById("f-br").addEventListener("input", (e) => {
    document.getElementById("f-br-val").textContent = `${(e.target.value / 1000).toFixed(1)} Mbps`;
  });
}

function applyPreset(name) {
  const sorted = [...state.channels].sort((a, b) => layoutOf(a.id).order - layoutOf(b.id).order);
  if (name === "grid3") {
    sorted.forEach((ch) => Object.assign(layoutOf(ch.id), { col: 4, row: 1 }));
  } else if (name === "grid2") {
    sorted.forEach((ch) => Object.assign(layoutOf(ch.id), { col: 6, row: 1 }));
  } else if (name === "focus") {
    const focusId = state.selected || sorted.find((c) => c.online)?.id || sorted[0]?.id;
    sorted.forEach((ch) => {
      if (ch.id === focusId) Object.assign(layoutOf(ch.id), { col: 8, row: 2 });
      else Object.assign(layoutOf(ch.id), { col: 4, row: 1 });
    });
  }
  saveLayout();
  renderGrid();
  reattachVideos();
}

/* 首次渲染后对在线通道起播 */
async function bootstrapStreams() {
  for (const ch of state.channels) {
    if (ch.online) startStream(ch);
  }
}

init().then(bootstrapStreams);
