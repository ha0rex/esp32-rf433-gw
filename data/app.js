(() => {
  const $ = (sel) => document.querySelector(sel);
  const $$ = (sel) => Array.from(document.querySelectorAll(sel));

  const state = {
    page: "dashboard",
    status: null,
    ws: null,
    pulses: [],
    scanPoints: [],
    lastRecord: null,
    setupMode: false,
  };

  const titles = {
    dashboard: "Dashboard",
    spectrum: "Spectrum",
    liverx: "Live RX",
    recorder: "Recorder",
    signals: "Saved Signals",
    remotes: "Devices",
    actions: "Actions",
    homekit: "HomeKit",
    transmitter: "Transmitter",
    settings: "Settings",
    setup: "Wi-Fi Setup",
  };

  let savedSignalsCache = [];
  let editingRemoteId = null;
  let editingActionId = null;
  let actionsCache = [];
  let actionStatusTimer = null;
  let recPoll = null;
  let remotesCache = [];
  let lastBySignal = {};
  let pressLog = [];
  let pairingLocked = false; // sticky save-step after successful pair
  let weatherPollTimer = null;
  let pickedWeather = null;

  function formatAge(ms) {
    if (ms == null) return "";
    if (ms < 1000) return "just now";
    if (ms < 60000) return `${Math.round(ms / 1000)}s ago`;
    return `${Math.round(ms / 60000)}m ago`;
  }

  function updateListenUi(listening) {
    ["signals", "remotes"].forEach((page) => {
      const dot = $(`#${page}-listen-dot`);
      const label = $(`#${page}-listen-label`);
      if (!dot || !label) return;
      dot.classList.toggle("on", !!listening);
      label.textContent = listening ? "Listening for presses" : "Not listening (radio busy?)";
    });
  }

  function flashPress(ev) {
    const label = ev.matched
      ? (ev.buttonName || ev.signalName || "matched")
      : `unmatched (${ev.pulseCount || 0} pulses, ${ev.rssi} dBm)`;
    ["signals-last-press", "remotes-last-press"].forEach((id) => {
      const el = $("#" + id);
      if (el) el.textContent = ev.matched
        ? `Last: ${label}${ev.homekitFired ? " · HomeKit fired" : ""}`
        : `Heard RF but no match · ${ev.pulseCount || 0} pulses · ${ev.rssi} dBm`;
    });
    ["signals-listen-dot", "remotes-listen-dot"].forEach((id) => {
      const d = $("#" + id);
      if (!d) return;
      d.classList.add("flash");
      setTimeout(() => d.classList.remove("flash"), 700);
    });

    pressLog.unshift(ev);
    if (pressLog.length > 25) pressLog.pop();
    renderPressLog();

    if (ev.matched && ev.signalId) {
      lastBySignal[ev.signalId] = { ageMs: 0, rssi: ev.rssi, buttonName: ev.buttonName, homekitFired: ev.homekitFired };
      const sigItem = document.querySelector(`[data-signal-id="${CSS.escape(ev.signalId)}"]`);
      if (sigItem) {
        sigItem.classList.add("pressed");
        const age = sigItem.querySelector(".press-age");
        if (age) age.textContent = "just now";
        setTimeout(() => sigItem.classList.remove("pressed"), 1200);
      }
      document.querySelectorAll(`[data-btn-signal="${CSS.escape(ev.signalId)}"]`).forEach((chip) => {
        chip.classList.add("pressed");
        setTimeout(() => chip.classList.remove("pressed"), 1200);
      });
    }
  }

  function renderPressLog() {
    const log = $("#press-log");
    if (!log) return;
    if (!pressLog.length) { log.hidden = true; return; }
    log.hidden = false;
    log.innerHTML = pressLog.slice(0, 12).map((e) => {
      if (e.matched) {
        return `<div class="ok">● ${escapeHtml(e.buttonName || e.signalName || "button")}` +
          `${e.homekitFired ? " → HomeKit" : ""} · ${e.rssi} dBm</div>`;
      }
      return `<div class="miss">○ unmatched · ${e.pulseCount || 0} pulses · ${e.rssi} dBm</div>`;
    }).join("");
  }

  async function refreshPresses() {
    try {
      const data = await api("/api/presses");
      updateListenUi(data.listening);
      lastBySignal = data.lastBySignal || {};
      if (Array.isArray(data.presses) && data.presses.length && !pressLog.length) {
        pressLog = data.presses.slice(0, 12);
        renderPressLog();
        const last = data.presses[0];
        if (last) {
          const el1 = $("#signals-last-press");
          const el2 = $("#remotes-last-press");
          const txt = last.matched
            ? `Last: ${last.buttonName || last.signalName} (${formatAge(last.ageMs)})`
            : `Last unmatched (${formatAge(last.ageMs)})`;
          if (el1) el1.textContent = txt;
          if (el2) el2.textContent = txt;
        }
      }
      // refresh age labels on visible lists
      document.querySelectorAll("[data-signal-id]").forEach((el) => {
        const id = el.getAttribute("data-signal-id");
        const info = lastBySignal[id];
        const age = el.querySelector(".press-age");
        if (age) age.textContent = info ? formatAge(info.ageMs) : "";
      });
    } catch (_) {}
  }

  async function api(path, opts = {}) {
    const res = await fetch(path, {
      headers: { "Content-Type": "application/json", ...(opts.headers || {}) },
      ...opts,
    });
    let data = null;
    try { data = await res.json(); } catch (_) { data = null; }
    if (!res.ok) throw new Error((data && data.error) || res.statusText);
    return data;
  }

  function showPage(name) {
    state.page = name;
    $$(".page").forEach((p) => { p.hidden = true; });
    const el = $(`#page-${name}`);
    if (el) el.hidden = false;
    $$(".nav-btn").forEach((b) => b.classList.toggle("active", b.dataset.page === name));
    $("#page-title").textContent = titles[name] || name;
    $("#sidebar").classList.remove("open");
    if (name === "recorder") {
      const busy = $("#rec-cancel") && !$("#rec-cancel").hidden;
      if (!busy && !pairingLocked && !state.lastRecord) resetRecorderUi(false);
    }
    if (name === "remotes") {
      loadRemotes().catch(console.warn);
      refreshPresses().catch(console.warn);
    }
    if (name === "actions") loadActions().catch(console.warn);
    if (name === "homekit") loadHomeKit().catch(console.warn);
    if (name === "signals") {
      loadSignals().catch(console.warn);
      refreshPresses().catch(console.warn);
    }
    if (name === "settings") refreshOta(false).catch(console.warn);
  }

  function stopRecPoll() {
    if (recPoll) { clearInterval(recPoll); recPoll = null; }
  }

  function resetRecorderUi(keepMessage) {
    stopRecPoll();
    pairingLocked = false;
    state.lastRecord = null;
    $("#rec-result").hidden = true;
    $("#rec-actions").hidden = true;
    $("#rec-btn").hidden = false;
    $("#rec-another").hidden = true;
    $("#rec-cancel").hidden = true;
    $("#rec-btn").textContent = "Start pairing";
    $("#rec-name").value = "";
    $("#rec-rssi-fill").style.width = "0%";
    $("#rec-live-rssi").textContent = "— dBm";
    $("#rec-presses").textContent = "0";
    setPairSteps("idle", 0, false, 2);
    if (!keepMessage) {
      $("#rec-msg").textContent = "Hold your remote close, then start pairing and hold the button down.";
    }
  }

  function fmtUptime(s) {
    s = Number(s) || 0;
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const sec = s % 60;
    return `${h}h ${m}m ${sec}s`;
  }

  function drawWaveform(canvas, pulses, zoom = 1, offsetPct = 0) {
    const ctx = canvas.getContext("2d");
    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = "#0a1220";
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = "#243352";
    ctx.beginPath();
    ctx.moveTo(0, h / 2);
    ctx.lineTo(w, h / 2);
    ctx.stroke();

    if (!pulses || !pulses.length) return;

    const total = pulses.reduce((a, p) => a + Math.abs(p), 0) || 1;
    const viewUs = total / zoom;
    const startUs = (total - viewUs) * (offsetPct / 100);
    const endUs = startUs + viewUs;

    ctx.strokeStyle = "#3dd6c6";
    ctx.lineWidth = 2;
    ctx.beginPath();
    let t = 0;
    let xPrev = 0;
    let y = h * 0.75;
    let started = false;

    for (const p of pulses) {
      const dur = Math.abs(p);
      const high = p > 0;
      const t0 = t;
      const t1 = t + dur;
      t = t1;
      if (t1 < startUs || t0 > endUs) continue;

      const x0 = ((Math.max(t0, startUs) - startUs) / viewUs) * w;
      const x1 = ((Math.min(t1, endUs) - startUs) / viewUs) * w;
      const yLevel = high ? h * 0.25 : h * 0.75;

      if (!started) {
        ctx.moveTo(x0, yLevel);
        started = true;
      } else {
        ctx.lineTo(x0, y);
      }
      ctx.lineTo(x0, yLevel);
      ctx.lineTo(x1, yLevel);
      y = yLevel;
      xPrev = x1;
    }
    ctx.stroke();
  }

  function drawScan(canvas, points, peakMHz) {
    const ctx = canvas.getContext("2d");
    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = "#0a1220";
    ctx.fillRect(0, 0, w, h);

    if (!points || points.length < 2) {
      ctx.fillStyle = "#8fa3c8";
      ctx.fillText("No scan data yet", 16, 24);
      return;
    }

    const sorted = [...points].sort((a, b) => a.f - b.f);
    const fMin = sorted[0].f;
    const fMax = sorted[sorted.length - 1].f || fMin + 0.01;
    const rMin = -110;
    const rMax = -20;

    // grid
    ctx.strokeStyle = "#1b2a45";
    ctx.fillStyle = "#8fa3c8";
    ctx.font = "12px sans-serif";
    for (let r = rMin; r <= rMax; r += 20) {
      const y = h - ((r - rMin) / (rMax - rMin)) * (h - 20) - 10;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
      ctx.fillText(`${r}`, 4, y - 2);
    }

    ctx.beginPath();
    ctx.strokeStyle = "#5b8cff";
    ctx.lineWidth = 2;
    sorted.forEach((p, i) => {
      const x = ((p.f - fMin) / (fMax - fMin)) * (w - 20) + 10;
      const y = h - ((p.r - rMin) / (rMax - rMin)) * (h - 20) - 10;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.stroke();

    // peaks above threshold-ish
    const thr = Number($("#scan-thr").value) || -70;
    sorted.forEach((p) => {
      if (p.r >= thr) {
        const x = ((p.f - fMin) / (fMax - fMin)) * (w - 20) + 10;
        const y = h - ((p.r - rMin) / (rMax - rMin)) * (h - 20) - 10;
        ctx.fillStyle = "#6ddf8a";
        ctx.beginPath();
        ctx.arc(x, y, 3, 0, Math.PI * 2);
        ctx.fill();
      }
    });

    if (peakMHz) {
      const x = ((peakMHz - fMin) / (fMax - fMin)) * (w - 20) + 10;
      ctx.strokeStyle = "#ffd166";
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, h);
      ctx.stroke();
    }
  }

  function updateDashboard(st) {
    if (!st) return;
    $("#d-wifi").textContent = st.wifiMode || "—";
    $("#d-ip").textContent = st.ip || "—";
    $("#d-wifi-rssi").textContent = `${st.wifiRssi} dBm`;
    $("#d-uptime").textContent = fmtUptime(st.uptime);
    $("#d-heap").textContent = `${st.heap} B`;
    $("#top-ip").textContent = st.ip || "—";
    $("#top-rssi").textContent = st.wifiRssi != null ? `${st.wifiRssi} dBm` : "—";
    $("#brand-sub").textContent = st.rf ? `${st.rf.frequency.toFixed(3)} MHz` : "433 MHz";

    const cc = st.cc1101 || {};
    $("#d-cc").textContent = cc.detected ? "detected" : "NOT DETECTED";
    $("#d-cc").style.color = cc.detected ? "var(--ok)" : "var(--danger)";
    $("#d-part").textContent = `0x${Number(cc.partnum || 0).toString(16).padStart(2, "0")}`;
    $("#d-ver").textContent = `0x${Number(cc.version || 0).toString(16).padStart(2, "0")}`;
    if (st.rf) {
      $("#d-freq").textContent = `${st.rf.frequency.toFixed(3)} MHz`;
      $("#d-mod").textContent = st.rf.modulationName || st.rf.modulation;
      $("#d-rssi").textContent = `${st.rf.rssi} dBm`;
    }
    $("#d-state").textContent = st.radioState || "—";
    const trouble = $("#d-trouble");
    if (!cc.detected && cc.troubleshooting) {
      trouble.hidden = false;
      trouble.textContent = cc.troubleshooting;
    } else {
      trouble.hidden = true;
    }

    // settings form hydrate once-ish
    if (st.rf) {
      $("#set-freq").value = st.rf.frequency;
      $("#set-mod").value = st.rf.modulation;
      $("#set-bw").value = st.rf.rxBandwidthKHz;
      $("#set-dr").value = st.rf.dataRateBaud;
      $("#set-dev").value = st.rf.deviationKHz;
      $("#set-thr").value = st.rf.rssiThreshold;
      $("#tx-freq").value = st.rf.frequency;
      $("#tx-mod").value = st.rf.modulation;
    }
  }

  function onWsStatus(msg) {
    $("#state-pill").textContent = msg.radioState || "—";
    if (msg.freq != null) $("#rx-freq").textContent = `${Number(msg.freq).toFixed(3)} MHz`;
    if (msg.rssi != null) $("#rx-rssi").textContent = `${msg.rssi} dBm`;
    $("#rx-edges").textContent = msg.edgeCount ?? 0;
    $("#rx-pulses").textContent = msg.pulseCount ?? 0;
    $("#rx-len").textContent = `${msg.captureLengthUs ?? 0} µs`;
    const used = msg.bufferUsed || 0;
    const cap = msg.bufferCapacity || 1;
    $("#rx-buf").textContent = `${Math.round((used / cap) * 100)}%`;
    $("#rx-ovf").textContent = msg.overflow ? "YES" : "no";
    $("#rx-ovf").style.color = msg.overflow ? "var(--danger)" : "var(--ok)";

    if (Array.isArray(msg.pulses)) {
      state.pulses = msg.pulses;
      const zoom = Number($("#wave-zoom").value) || 1;
      const off = Number($("#wave-offset").value) || 0;
      drawWaveform($("#wave-canvas"), state.pulses, zoom, off);
      $("#rx-raw").textContent = state.pulses.join(", ");
    }

    if (msg.signalDetected) {
      $("#scan-banner").hidden = false;
      $("#det-freq").textContent = `${Number(msg.detectedMHz).toFixed(3)} MHz`;
      $("#det-rssi").textContent = `${msg.detectedRssi} dBm`;
    }

    if (msg.scanCurrent != null) {
      $("#scan-cur").textContent = `${Number(msg.scanCurrent).toFixed(3)} MHz`;
      $("#scan-peak").textContent = `${Number(msg.scanPeakMHz).toFixed(3)} MHz @ ${msg.scanPeakRssi} dBm`;
    }

    if (msg.recordPhase != null || msg.recordPhaseName) {
      updatePairUi({
        phaseName: msg.recordPhaseName,
        message: msg.recordMessage,
        pressesGot: msg.pressesGot,
        pressesNeeded: msg.pressesNeeded,
        liveRssiDbm: msg.liveRssiDbm,
        triggerThresholdDbm: msg.triggerThresholdDbm,
        noiseFloorDbm: msg.noiseFloorDbm,
        complete: msg.recordPhaseName === "complete",
      });
      if (msg.recordPhaseName === "complete") pollRecord();
    }
  }

  function connectWs() {
    const proto = location.protocol === "https:" ? "wss" : "ws";
    // WebSocketsServer listens on :81 (not under HTTP /ws)
    const host = location.hostname;
    const ws = new WebSocket(`${proto}://${host}:81/`);
    state.ws = ws;
    const pill = $("#conn-pill");
    pill.textContent = "connecting";
    pill.className = "pill warn";
    ws.onopen = () => {
      pill.textContent = "online";
      pill.className = "pill";
    };
    ws.onclose = () => {
      pill.textContent = "offline";
      pill.className = "pill err";
      setTimeout(connectWs, 2000);
    };
    ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data);
        if (msg.type === "status") {
          onWsStatus(msg);
          if (typeof msg.pressListening === "boolean") updateListenUi(msg.pressListening);
        } else if (msg.type === "press") {
          flashPress(msg);
        } else if (msg.type === "ota") {
          renderOta(msg);
        }
      } catch (_) {}
    };
  }

  async function refreshStatus() {
    try {
      const st = await api("/api/status");
      state.status = st;
      updateDashboard(st);
      if (st.fwVersion) {
        const pill = $("#ota-current-pill");
        if (pill) pill.textContent = `v${st.fwVersion}`;
      }
      if (state.page === "settings") refreshOta(false);
    } catch (e) {
      console.warn(e);
    }
  }

  const OTA_PHASE_LABEL = {
    idle: "Idle",
    checking: "Checking",
    ready: "Update available",
    downloading: "Downloading",
    writing: "Installing",
    rebooting: "Rebooting",
    failed: "Failed",
    up_to_date: "Up to date",
  };

  function renderOta(data) {
    if (!data || !$("#ota-status-card")) return;
    state.ota = data;
    const phase = data.phase || "idle";
    const busy = !!data.busy;
    $("#ota-phase-label").textContent = OTA_PHASE_LABEL[phase] || phase;
    $("#ota-message").textContent = data.message || "";
    if (data.version) $("#ota-current-pill").textContent = `v${data.version}`;

    $$(".ota-ch-btn").forEach((btn) => {
      btn.classList.toggle("active", btn.dataset.channel === data.channel);
      btn.disabled = busy;
    });

    const dot = $("#ota-dot");
    dot.className = "ota-dot";
    if (phase === "failed") dot.classList.add("err");
    else if (phase === "up_to_date") dot.classList.add("ok");
    else if (phase === "ready") dot.classList.add("warn");
    else if (busy || phase === "checking" || phase === "downloading" || phase === "writing" || phase === "rebooting") {
      dot.classList.add("busy");
    }

    const showProg = phase === "downloading" || phase === "writing" || phase === "rebooting";
    $("#ota-progress").hidden = !showProg;
    if (showProg) {
      const pct = Math.max(0, Math.min(100, Number(data.progress) || 0));
      $("#ota-progress-fill").style.width = `${pct}%`;
      $("#ota-progress-pct").textContent = `${pct}%`;
      $("#ota-progress-hint").textContent =
        phase === "rebooting" ? "Device is restarting…" : "Keep this page open";
    }

    const avail = data.available || {};
    const showAvail = !!avail.valid && (phase === "ready" || (avail.newer && phase !== "up_to_date"));
    $("#ota-avail").hidden = !showAvail;
    if (showAvail) {
      $("#ota-avail-ver").textContent = avail.version || "—";
      const bits = [];
      if (avail.publishedAt) {
        try { bits.push(new Date(avail.publishedAt).toLocaleString()); } catch (_) { bits.push(avail.publishedAt); }
      }
      if (avail.sizeBytes) bits.push(`${Math.round(avail.sizeBytes / 1024)} KB`);
      if (avail.prerelease) bits.push("pre-release");
      $("#ota-avail-meta").textContent = bits.join(" · ") || "—";
      const notes = (avail.notes || "").trim();
      $("#ota-notes").hidden = !notes;
      $("#ota-notes").textContent = notes;
    }

    $("#ota-check").disabled = busy;
    $("#ota-install").disabled = phase !== "ready" || busy;

    if (phase === "rebooting") {
      $("#conn-pill").textContent = "updating…";
      $("#conn-pill").className = "pill warn";
    }
  }

  async function refreshOta(forceCheck) {
    try {
      const data = forceCheck
        ? await api("/api/ota/check", { method: "POST", body: "{}" })
        : await api("/api/ota");
      renderOta(data);
    } catch (e) {
      console.warn(e);
      if (forceCheck) alert(e.message || "Update check failed");
    }
  }

  async function pollScan() {
    if (state.page !== "spectrum") return;
    try {
      const data = await api("/api/scan/data");
      state.scanPoints = data.points || [];
      drawScan($("#scan-canvas"), state.scanPoints, data.peakMHz);
      $("#scan-cur").textContent = `${Number(data.currentMHz).toFixed(3)} MHz`;
      $("#scan-peak").textContent = `${Number(data.peakMHz).toFixed(3)} MHz @ ${data.peakRssi} dBm`;
      if (data.signalDetected) {
        $("#scan-banner").hidden = false;
        $("#det-freq").textContent = `${Number(data.detectedMHz).toFixed(3)} MHz`;
        $("#det-rssi").textContent = `${data.detectedRssi} dBm`;
      }
    } catch (_) {}
  }

  function setPairSteps(phaseName, pressesGot, complete, pressesNeeded) {
    const steps = $$("#rec-steps .pair-step");
    steps.forEach((s) => s.classList.remove("active", "done"));
    const mark = (n, mode) => {
      const el = steps.find((s) => s.dataset.step === String(n));
      if (el) el.classList.add(mode);
    };
    const need = pressesNeeded || 2;
    if (complete || phaseName === "complete") {
      mark(1, "done");
      mark(2, "done");
      mark(3, "active");
      return;
    }
    if (phaseName === "calibrating") {
      mark(1, "active");
    } else if (phaseName === "waiting" || phaseName === "capturing") {
      mark(1, "done");
      mark(2, "active");
      if (pressesGot >= need) {
        // enough repeats seen while holding — still wait for release
      }
    } else if (phaseName === "discarded" || phaseName === "idle") {
      mark(1, "active");
    }
  }

  function rssiToPct(dbm) {
    // Map -110..-30 to 0..100
    const pct = ((Number(dbm) + 110) / 80) * 100;
    return Math.max(0, Math.min(100, pct));
  }

  function updatePairUi(st) {
    // After a successful pair, ignore later idle/waiting WS updates that
    // would yank the wizard back to the start.
    if (pairingLocked && !(st.complete || st.phaseName === "complete")) {
      return;
    }

    const phase = st.phaseName || "";
    const msg = st.message || "";
    $("#rec-msg").textContent = msg;
    $("#rec-live-rssi").textContent = (st.liveRssiDbm != null && st.liveRssiDbm > -128)
      ? `${st.liveRssiDbm} dBm` : "— dBm";
    $("#rec-noise").textContent = (st.noiseFloorDbm != null) ? `${st.noiseFloorDbm} dBm` : "—";
    $("#rec-thr").textContent = (st.triggerThresholdDbm != null) ? `${st.triggerThresholdDbm} dBm` : "—";
    const got = st.pressesGot || 0;
    const need = st.pressesNeeded || 2;
    $("#rec-presses").textContent = String(got);
    const fill = $("#rec-rssi-fill");
    const pct = rssiToPct(st.liveRssiDbm);
    fill.style.width = `${pct}%`;
    const hot = st.liveRssiDbm != null && st.triggerThresholdDbm != null &&
                st.liveRssiDbm >= st.triggerThresholdDbm;
    fill.classList.toggle("hot", !!hot);

    if (st.complete || phase === "complete") {
      pairingLocked = true;
    }

    setPairSteps(phase, got, st.complete || pairingLocked, need);
    const busy = ["calibrating", "waiting", "capturing"].includes(phase);
    $("#rec-btn").hidden = busy || st.complete || pairingLocked;
    $("#rec-cancel").hidden = !busy;
    if (!busy && !st.complete && !pairingLocked) {
      $("#rec-btn").textContent = "Start pairing";
    }
  }

  function renderRecord(sig) {
    pairingLocked = true;
    state.lastRecord = sig;
    $("#rec-result").hidden = false;
    $("#rec-actions").hidden = false;
    $("#rec-btn").hidden = true;
    $("#rec-cancel").hidden = true;
    $("#rec-another").hidden = false;
    setPairSteps("complete", sig.estimatedRepeats || 2, true, 2);
    $("#rec-msg").textContent = "Paired! Name this button and save it.";
    $("#rec-freq").textContent = `${Number(sig.frequency || 0).toFixed(3)} MHz`;
    $("#rec-rssi").textContent = `${sig.rssi} dBm`;
    $("#rec-pulses").textContent = sig.pulseCount;
    $("#rec-dur").textContent = `${sig.durationUs} µs`;
    $("#rec-reps").textContent = sig.estimatedRepeats;
    $("#rec-presses").textContent = String(sig.estimatedRepeats || 0);
    $("#rec-raw").textContent = (sig.pulses || []).join(", ");
    const a = sig.analysis || {};
    const ul = $("#rec-clusters");
    ul.innerHTML = "";
    (a.clusters || []).forEach((c) => {
      const li = document.createElement("li");
      li.textContent = `${c.centerUs} µs (n=${c.count}, ${c.minUs}-${c.maxUs})`;
      ul.appendChild(li);
    });
    $("#rec-bits").textContent = a.heuristicBits || "(no heuristic decode)";
    $("#rec-note").textContent = a.heuristicNote || "";
    drawWaveform($("#rec-wave"), sig.pulses || [], 1, 0);
    $("#rec-name").focus();
  }

  async function pollRecord() {
    try {
      const st = await api("/api/record/status");
      updatePairUi(st);
      if (st.complete && st.signal) renderRecord(st.signal);
      return st;
    } catch (_) {
      return null;
    }
  }

  async function loadSignals() {
    const data = await api("/api/signals");
    savedSignalsCache = data.signals || [];
    const list = $("#signals-list");
    list.innerHTML = "";
    const sel = $("#tx-load-sel");
    sel.innerHTML = `<option value="">Load saved signal…</option>`;
    savedSignalsCache.forEach((s) => {
      const item = document.createElement("div");
      item.className = "signal-item";
      item.setAttribute("data-signal-id", s.id);
      const last = lastBySignal[s.id];
      const ageTxt = last ? formatAge(last.ageMs) : "";
      item.innerHTML = `
        <div>
          <strong>${escapeHtml(s.name)}</strong>
          <span class="press-age">${escapeHtml(ageTxt)}</span>
          <div class="muted">${s.frequency.toFixed(3)} MHz · ${s.pulseCount} pulses · ${s.rssi} dBm</div>
        </div>
        <div class="signal-actions">
          <button class="btn" data-act="replay">Replay</button>
          <button class="btn" data-act="view">View</button>
          <button class="btn" data-act="rename">Rename</button>
          <button class="btn danger" data-act="delete">Delete</button>
        </div>`;
      item.querySelectorAll("button").forEach((btn) => {
        btn.addEventListener("click", async (ev) => {
          ev.stopPropagation();
          const act = btn.dataset.act;
          if (act === "replay") {
            await api(`/api/signals/${s.id}/replay`, { method: "POST" });
          } else if (act === "delete") {
            if (confirm(`Delete ${s.name}?`)) {
              await api(`/api/signals/${s.id}`, { method: "DELETE" });
              loadSignals();
            }
          } else if (act === "rename") {
            const name = prompt("New name", s.name);
            if (name) {
              await api("/api/signal/rename", {
                method: "POST",
                body: JSON.stringify({ id: s.id, name }),
              });
              loadSignals();
            }
          } else if (act === "view") {
            const full = await api(`/api/signals/${s.id}`);
            const sig = full.signal;
            $("#signal-detail").hidden = false;
            $("#sig-detail-name").textContent = sig.name;
            $("#sig-detail-raw").textContent = "RAW:\n" + (sig.pulses || []).join(", ");
            $("#sig-detail-analysis").textContent =
              "ANALYSIS (heuristic):\n" + JSON.stringify(sig.analysis || {}, null, 2);
            drawWaveform($("#sig-wave"), sig.pulses || [], 1, 0);
          }
        });
      });
      list.appendChild(item);

      const opt = document.createElement("option");
      opt.value = s.id;
      opt.textContent = s.name;
      sel.appendChild(opt);
    });
  }

  function signalOptionsHtml(selected) {
    return [`<option value="">Select saved button…</option>`]
      .concat(savedSignalsCache.map((s) =>
        `<option value="${escapeHtml(s.id)}" ${s.id === selected ? "selected" : ""}>${escapeHtml(s.name)}</option>`))
      .join("");
  }

  // Device button binding: signal or action. Values use sig:/act: prefixes.
  function buttonBindOptionsHtml(selectedSignalId, selectedActionId) {
    const selected = selectedActionId
      ? `act:${selectedActionId}`
      : (selectedSignalId ? `sig:${selectedSignalId}` : "");
    const parts = [`<option value="">None</option>`];
    if (savedSignalsCache.length) {
      parts.push(`<optgroup label="Saved signals">`);
      savedSignalsCache.forEach((s) => {
        const v = `sig:${s.id}`;
        parts.push(`<option value="${escapeHtml(v)}" ${v === selected ? "selected" : ""}>${escapeHtml(s.name)}</option>`);
      });
      parts.push(`</optgroup>`);
    }
    if (actionsCache.length) {
      parts.push(`<optgroup label="Actions">`);
      actionsCache.forEach((a) => {
        const v = `act:${a.id}`;
        parts.push(`<option value="${escapeHtml(v)}" ${v === selected ? "selected" : ""}>${escapeHtml(a.name)}</option>`);
      });
      parts.push(`</optgroup>`);
    }
    return parts.join("");
  }

  function parseButtonBind(value) {
    if (!value) return { signalId: "", actionId: "" };
    if (value.startsWith("act:")) return { signalId: "", actionId: value.slice(4) };
    if (value.startsWith("sig:")) return { signalId: value.slice(4), actionId: "" };
    // Legacy plain signal id
    return { signalId: value, actionId: "" };
  }

  function actionName(id) {
    const a = actionsCache.find((x) => x.id === id);
    return a ? a.name : id;
  }

  function bindLabel(signalId, actionId) {
    if (actionId) return actionName(actionId);
    if (signalId) {
      const s = savedSignalsCache.find((x) => x.id === signalId);
      return s ? s.name : signalId;
    }
    return "—";
  }

  function syncRemoteButtonRowMode(row) {
    const mode = row.querySelector(".rb-mode")?.value || "stateless";
    const secondary = row.querySelector(".rb-secondary");
    if (!secondary) return;
    const disabled = mode === "stateless";
    secondary.disabled = disabled;
    if (disabled) secondary.value = "";
  }

  function addRemoteButtonRow(btn = {}) {
    const row = document.createElement("div");
    row.className = "remote-btn-row";
    const name = btn.name || "";
    // Migrate legacy on/off fields into primary/secondary
    let primarySignal = btn.signalId || "";
    let primaryAction = btn.actionId || "";
    let secondarySignal = btn.secondarySignalId || "";
    let secondaryAction = btn.secondaryActionId || "";
    if (!primaryAction && btn.actionIdOn) primaryAction = btn.actionIdOn;
    if (!secondaryAction && btn.actionIdOff) secondaryAction = btn.actionIdOff;
    if (primaryAction) primarySignal = "";
    if (secondaryAction) secondarySignal = "";
    const mode = ["stateless", "stateful", "toggle"].includes(btn.homekitMode)
      ? btn.homekitMode
      : "stateless";
    const secondaryDisabled = mode === "stateless";
    row.innerHTML = `
      <label>Label<input class="rb-name" value="${escapeHtml(name)}" placeholder="Power" /></label>
      <label>Primary<select class="rb-primary">${buttonBindOptionsHtml(primarySignal, primaryAction)}</select></label>
      <label>Secondary<select class="rb-secondary" ${secondaryDisabled ? "disabled" : ""}>${buttonBindOptionsHtml(secondarySignal, secondaryAction)}</select></label>
      <label>HomeKit
        <select class="rb-mode">
          <option value="stateless" ${mode === "stateless" ? "selected" : ""}>Stateless (press)</option>
          <option value="stateful" ${mode === "stateful" ? "selected" : ""}>Stateful (On/Off)</option>
          <option value="toggle" ${mode === "toggle" ? "selected" : ""}>Toggle (reverse)</option>
        </select>
      </label>
      <button class="btn danger rb-del" type="button">Remove</button>`;
    row.querySelector(".rb-del").addEventListener("click", () => row.remove());
    row.querySelector(".rb-mode").addEventListener("change", () => syncRemoteButtonRowMode(row));
    $("#remote-buttons").appendChild(row);
  }

  function deviceTypeLabel(t) {
    return ({
      remote: "Remote",
      sensor: "Sensor",
      temperature: "Temperature",
      humidity: "Humidity",
    })[t] || "Remote";
  }

  function syncDeviceEditorType() {
    const type = $("#remote-type").value || "remote";
    const weather = type === "temperature" || type === "humidity";
    $("#remote-buttons-section").hidden = weather;
    $("#remote-weather-section").hidden = !weather;
    $("#remote-buttons-heading").textContent = type === "sensor" ? "Trigger signals" : "Buttons";
    $("#remote-name").placeholder = weather
      ? (type === "temperature" ? "Patio Temperature" : "Patio Humidity")
      : (type === "sensor" ? "Driveway Sensor" : "Living Room Remote");
    if (weather) refreshWeatherList().catch(console.warn);
  }

  async function refreshWeatherList() {
    const data = await api("/api/weather");
    const list = $("#weather-list");
    if (!list) return;
    const readings = data.readings || [];
    $("#weather-status").textContent = readings.length
      ? `${readings.length} recent reading(s)`
      : "Listening for weather stations…";
    list.innerHTML = "";
    // Dedupe by protocol+id+channel, keep newest
    const seen = new Map();
    readings.forEach((r) => {
      const key = `${r.protocol}:${r.sensorId}:${r.channel}`;
      if (!seen.has(key)) seen.set(key, r);
    });
    [...seen.values()].forEach((r) => {
      const item = document.createElement("div");
      item.className = "signal-item";
      const temp = r.tempC != null ? `${Number(r.tempC).toFixed(1)}°C` : "—";
      const hum = r.humidity != null ? `${Math.round(r.humidity)}%` : "—";
      item.innerHTML = `
        <div>
          <strong>${escapeHtml(r.protocol)}</strong> · id ${r.sensorId} · ch ${r.channel}
          <div class="muted">${temp} · ${hum} · ${r.rssi} dBm · ${formatAge(r.ageMs)}</div>
        </div>
        <div class="signal-actions">
          <button class="btn primary" type="button">Use</button>
        </div>`;
      item.querySelector("button").addEventListener("click", () => {
        pickedWeather = r;
        $("#weather-protocol").value = r.protocol || "auto";
        $("#weather-id").value = r.sensorId;
        $("#weather-channel").value = String(r.channel || 0);
        $("#weather-picked").textContent =
          `Selected ${r.protocol} id=${r.sensorId} ch=${r.channel} (${temp}, ${hum})`;
      });
      list.appendChild(item);
    });
  }

  async function loadRemotes() {
    await loadSignals();
    try {
      const actData = await api("/api/actions");
      actionsCache = actData.actions || [];
    } catch (_) {
      actionsCache = [];
    }
    await refreshPresses();
    const data = await api("/api/remotes");
    remotesCache = data.remotes || [];
    const list = $("#remotes-list");
    list.innerHTML = "";
    remotesCache.forEach((r) => {
      const item = document.createElement("div");
      item.className = "signal-item";
      const type = r.type || "remote";
      const warns = r.warnings || [];
      let detail = "";
      if (type === "temperature" || type === "humidity") {
        const temp = r.lastTempC != null ? `${Number(r.lastTempC).toFixed(1)}°C` : "—";
        const hum = r.lastHumidity != null ? `${Math.round(r.lastHumidity)}%` : "—";
        detail = `<div class="muted">${escapeHtml(r.weatherProtocol || "auto")} · id ${r.weatherId} · ch ${r.weatherChannel || "any"}</div>
          <div style="margin-top:0.35rem">${type === "temperature" ? temp : hum}</div>`;
      } else {
        const chips = (r.buttons || []).map((b) => {
          const mode = b.homekitMode === "toggle" ? "toggle"
            : b.homekitMode === "stateful" ? "on/off"
            : "press";
          const primary = bindLabel(b.signalId, b.actionId || b.actionIdOn);
          const secondary = bindLabel(b.secondarySignalId, b.secondaryActionId || b.actionIdOff);
          const bind = (b.homekitMode === "stateful" || b.homekitMode === "toggle")
            ? `${escapeHtml(primary)} / ${escapeHtml(secondary)}`
            : escapeHtml(primary);
          const sid = b.signalId || b.secondarySignalId || "";
          const attr = sid ? `data-btn-signal="${escapeHtml(sid)}"` : "";
          return `<span class="remote-btn-chip" ${attr}>${escapeHtml(b.name)} <span class="muted">(${mode}: ${bind})</span></span>`;
        }).join("");
        detail = `<div class="muted">${(r.buttons || []).length} ${(type === "sensor") ? "triggers" : "buttons"}</div>
          <div style="margin-top:0.35rem">${chips || "<span class='muted'>None</span>"}</div>`;
      }
      const warnHtml = warns.length
        ? `<div style="color:var(--danger);margin-top:0.4rem;font-size:0.85rem">${warns.map(escapeHtml).join("<br>")}</div>`
        : "";
      item.innerHTML = `
        <div>
          <strong>${escapeHtml(r.name)}</strong>
          <span class="muted"> · ${deviceTypeLabel(type)}</span>
          ${detail}
          ${warnHtml}
        </div>
        <div class="signal-actions">
          <button class="btn" data-act="edit">Edit</button>
          <button class="btn danger" data-act="delete">Delete</button>
        </div>`;
      item.querySelectorAll("button").forEach((btn) => {
        btn.addEventListener("click", async () => {
          if (btn.dataset.act === "edit") openRemoteEditor(r);
          if (btn.dataset.act === "delete") {
            if (!confirm(`Delete device "${r.name}"? Hub will reboot.`)) return;
            await api(`/api/remote?id=${encodeURIComponent(r.id)}`, { method: "DELETE" });
          }
        });
      });
      list.appendChild(item);
    });
  }

  function openRemoteEditor(remote) {
    editingRemoteId = remote ? remote.id : null;
    pickedWeather = null;
    $("#remote-editor").hidden = false;
    $("#remote-editor-title").textContent = remote ? "Edit device" : "New device";
    $("#remote-name").value = remote ? remote.name : "";
    $("#remote-type").value = (remote && remote.type) || "remote";
    $("#remote-buttons").innerHTML = "";
    const buttons = remote && remote.buttons && remote.buttons.length
      ? remote.buttons
      : [{ name: "", signalId: "", actionId: "" }];
    buttons.forEach((b) => addRemoteButtonRow(b));
    $("#weather-protocol").value = (remote && remote.weatherProtocol) || "auto";
    $("#weather-id").value = remote && remote.weatherId != null && remote.weatherId >= 0 ? remote.weatherId : "";
    $("#weather-channel").value = String((remote && remote.weatherChannel) || 0);
    $("#weather-picked").textContent = remote && remote.weatherId >= 0
      ? `Selected id=${remote.weatherId} ch=${remote.weatherChannel || "any"}`
      : "No station selected yet.";
    syncDeviceEditorType();
    if (weatherPollTimer) clearInterval(weatherPollTimer);
    weatherPollTimer = setInterval(() => {
      if (!$("#remote-weather-section").hidden) refreshWeatherList().catch(() => {});
    }, 3000);
    $("#remote-name").focus();
  }

  async function loadHomeKit() {
    const st = await api("/api/homekit/status");
    $("#hk-status").textContent = st.enabled ? "Enabled (Wi-Fi STA)" : "Disabled — connect to Wi-Fi first";
    $("#hk-port").textContent = st.port || "—";
    $("#hk-code").textContent = st.setupCode || "—";
    $("#hk-code-big").textContent = st.setupCode || "—";
    $("#hk-remotes").textContent = st.remoteCount != null ? String(st.remoteCount) : "—";
  }

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
    }[c]));
  }

  function signalName(id) {
    const s = savedSignalsCache.find((x) => x.id === id);
    return s ? s.name : (id || "—");
  }

  function updateActionRunnerUi(runner) {
    const el = $("#action-runner-status");
    if (!el) return;
    if (!runner || !runner.active) {
      el.textContent = "Idle";
      return;
    }
    el.textContent = `Running: ${runner.actionName || "action"} — step ${runner.step}/${runner.steps} — ${runner.message || ""}`;
  }

  async function pollActionStatus() {
    try {
      const d = await api("/api/action/status");
      updateActionRunnerUi(d.runner);
      if (d.runner && d.runner.active) {
        if (!actionStatusTimer) {
          actionStatusTimer = setInterval(() => pollActionStatus().catch(() => {}), 400);
        }
      } else if (actionStatusTimer) {
        clearInterval(actionStatusTimer);
        actionStatusTimer = null;
      }
    } catch (_) {}
  }

  function addActionStepRow(step = { type: "tx", signalId: "", repeats: 0, holdSec: 2, delaySec: 0.5 }) {
    const row = document.createElement("div");
    row.className = "remote-btn-row action-step-row";
    const type = step.type || "tx";
    row.dataset.type = type;
    const sigOpts = signalOptionsHtml(step.signalId || "");
    if (type === "delay") {
      row.innerHTML = `
        <label>Delay (s)<input class="as-delay" type="number" min="0" max="120" step="0.1" value="${Number(step.delaySec) || 0.5}" /></label>
        <span class="muted" style="align-self:center">Wait before next step</span>
        <button class="btn danger as-del" type="button">Remove</button>`;
    } else if (type === "hold") {
      row.innerHTML = `
        <label>Hold signal<select class="as-signal">${sigOpts}</select></label>
        <label>Seconds<input class="as-hold" type="number" min="0.1" max="10" step="0.1" value="${Number(step.holdSec) || 2}" /></label>
        <button class="btn danger as-del" type="button">Remove</button>`;
    } else {
      row.innerHTML = `
        <label>Transmit<select class="as-signal">${sigOpts}</select></label>
        <label>Repeats<input class="as-reps" type="number" min="0" max="50" step="1" value="${Number(step.repeats) || 0}" placeholder="auto" /></label>
        <button class="btn danger as-del" type="button">Remove</button>`;
    }
    row.querySelector(".as-del").addEventListener("click", () => row.remove());
    $("#action-steps").appendChild(row);
  }

  function openActionEditor(action) {
    editingActionId = action ? action.id : "";
    $("#action-editor-title").textContent = action ? "Edit action" : "New action";
    $("#action-name").value = (action && action.name) || "";
    $("#action-steps").innerHTML = "";
    const steps = (action && action.steps && action.steps.length)
      ? action.steps
      : [{ type: "hold", signalId: "", holdSec: 2 }, { type: "tx", signalId: "", repeats: 0 }];
    steps.forEach((s) => addActionStepRow(s));
    $("#actions-list-panel").hidden = true;
    $("#action-editor").hidden = false;
  }

  function closeActionEditor() {
    editingActionId = null;
    $("#action-editor").hidden = true;
    $("#actions-list-panel").hidden = false;
  }

  async function loadActions() {
    await loadSignals();
    const data = await api("/api/actions");
    actionsCache = data.actions || [];
    updateActionRunnerUi(data.runner);
    const list = $("#actions-list");
    list.innerHTML = "";
    if (!actionsCache.length) {
      list.innerHTML = `<div class="muted">No actions yet. Create one to wake a device (hold) then send a command.</div>`;
      return;
    }
    actionsCache.forEach((a) => {
      const item = document.createElement("div");
      item.className = "signal-item";
      const stepSummary = (a.steps || []).map((s) => {
        if (s.type === "delay") return `wait ${s.delaySec}s`;
        if (s.type === "hold") return `hold ${escapeHtml(signalName(s.signalId))} ${s.holdSec}s`;
        return `tx ${escapeHtml(signalName(s.signalId))}${s.repeats ? " ×" + s.repeats : ""}`;
      }).join(" → ");
      item.innerHTML = `
        <div>
          <strong>${escapeHtml(a.name)}</strong>
          <div class="muted" style="margin-top:0.35rem">${stepSummary || "No steps"}</div>
        </div>
        <div class="signal-actions">
          <button class="btn primary" data-act="run">Run</button>
          <button class="btn" data-act="edit">Edit</button>
          <button class="btn danger" data-act="delete">Delete</button>
        </div>`;
      item.querySelectorAll("button").forEach((btn) => {
        btn.addEventListener("click", async () => {
          const act = btn.dataset.act;
          if (act === "edit") openActionEditor(a);
          if (act === "run") {
            await api("/api/action/run", { method: "POST", body: JSON.stringify({ id: a.id }) });
            pollActionStatus();
          }
          if (act === "delete") {
            if (!confirm(`Delete action "${a.name}"?`)) return;
            await api(`/api/action?id=${encodeURIComponent(a.id)}`, { method: "DELETE" });
            loadActions().catch(console.warn);
          }
        });
      });
      list.appendChild(item);
    });
  }

  function bind() {
    $$(".nav-btn").forEach((b) => b.addEventListener("click", () => {
      showPage(b.dataset.page);
      if (b.dataset.page === "signals") loadSignals().catch(console.warn);
      if (b.dataset.page === "transmitter") loadSignals().catch(console.warn);
      if (b.dataset.page === "remotes") loadRemotes().catch(console.warn);
      if (b.dataset.page === "actions") loadActions().catch(console.warn);
      if (b.dataset.page === "homekit") loadHomeKit().catch(console.warn);
    }));
    $("#menu-btn").addEventListener("click", () => $("#sidebar").classList.toggle("open"));

    $("#btn-rx-start").addEventListener("click", () => api("/api/rx/start", { method: "POST" }));
    $("#btn-rx-stop").addEventListener("click", () => api("/api/rx/stop", { method: "POST" }));
    $("#live-rx-start").addEventListener("click", () => api("/api/rx/start", { method: "POST" }));
    $("#live-rx-stop").addEventListener("click", () => api("/api/rx/stop", { method: "POST" }));

    $("#wave-zoom").addEventListener("input", () => {
      drawWaveform($("#wave-canvas"), state.pulses, Number($("#wave-zoom").value), Number($("#wave-offset").value));
    });
    $("#wave-offset").addEventListener("input", () => {
      drawWaveform($("#wave-canvas"), state.pulses, Number($("#wave-zoom").value), Number($("#wave-offset").value));
    });

    $("#scan-start-btn").addEventListener("click", async () => {
      $("#scan-banner").hidden = true;
      await api("/api/scan/start", {
        method: "POST",
        body: JSON.stringify({
          start: Number($("#scan-start").value),
          end: Number($("#scan-end").value),
          stepKHz: Number($("#scan-step").value),
          dwellMs: Number($("#scan-dwell").value),
          rssiThreshold: Number($("#scan-thr").value),
          findSignal: $("#scan-find").checked,
        }),
      });
    });
    $("#scan-stop-btn").addEventListener("click", () => api("/api/scan/stop", { method: "POST" }));

    const startPairing = async () => {
      resetRecorderUi(true);
      $("#rec-result").hidden = true;
      await api("/api/record/start", { method: "POST" });
      updatePairUi({
        phaseName: "calibrating",
        message: "Stay quiet… measuring background noise",
        pressesGot: 0,
        pressesNeeded: 2,
        liveRssiDbm: -128,
      });
      recPoll = setInterval(async () => {
        const st = await pollRecord();
        if (!st) return;
        if (st.complete && st.signal) {
          stopRecPoll();
          return;
        }
        // Ignore idle after we've locked onto the save step
        if (pairingLocked) {
          stopRecPoll();
          return;
        }
        if (st.phaseName === "idle" || st.phaseName === "discarded") {
          stopRecPoll();
          $("#rec-btn").hidden = false;
          $("#rec-another").hidden = true;
          $("#rec-cancel").hidden = true;
          $("#rec-btn").textContent = "Start pairing";
        }
      }, 250);
    };

    $("#rec-btn").addEventListener("click", startPairing);
    $("#rec-another").addEventListener("click", startPairing);

    $("#rec-cancel").addEventListener("click", async () => {
      stopRecPoll();
      await api("/api/record/discard", { method: "POST" });
      resetRecorderUi(true);
      $("#rec-msg").textContent = "Cancelled. Ready when you are.";
    });

    $("#rec-save").addEventListener("click", async () => {
      const name = $("#rec-name").value.trim() || "Unnamed button";
      await api("/api/record/save", { method: "POST", body: JSON.stringify({ name }) });
      resetRecorderUi(true);
      $("#rec-msg").textContent = `"${name}" saved. Pair another or add it to a HomeKit remote.`;
      $("#rec-another").hidden = false;
      $("#rec-btn").hidden = true;
    });
    $("#rec-replay").addEventListener("click", async () => {
      const sig = state.lastRecord;
      if (!sig) return;
      await api("/api/tx", {
        method: "POST",
        body: JSON.stringify({
          frequency: sig.frequency,
          modulation: sig.modulation,
          repeats: sig.estimatedRepeats || 1,
          gapUs: sig.gapBetweenRepeatsUs || 10000,
          pulses: sig.pulses,
        }),
      });
    });
    $("#rec-discard").addEventListener("click", async () => {
      await api("/api/record/discard", { method: "POST" });
      resetRecorderUi(true);
      $("#rec-msg").textContent = "Discarded. Start again when ready.";
    });

    $("#signals-refresh").addEventListener("click", () => loadSignals());

    $("#remote-new").addEventListener("click", async () => {
      await loadSignals();
      openRemoteEditor(null);
    });
    $("#action-new").addEventListener("click", async () => {
      await loadSignals();
      openActionEditor(null);
    });
    $("#action-cancel").addEventListener("click", () => closeActionEditor());
    $("#action-add-tx").addEventListener("click", () => addActionStepRow({ type: "tx" }));
    $("#action-add-hold").addEventListener("click", () => addActionStepRow({ type: "hold", holdSec: 2 }));
    $("#action-add-delay").addEventListener("click", () => addActionStepRow({ type: "delay", delaySec: 0.5 }));
    $("#action-run-edit").addEventListener("click", async () => {
      if (!editingActionId) {
        alert("Save the action first, then run it.");
        return;
      }
      await api("/api/action/run", { method: "POST", body: JSON.stringify({ id: editingActionId }) });
      pollActionStatus();
    });
    $("#action-save").addEventListener("click", async () => {
      const name = $("#action-name").value.trim() || "Action";
      const steps = [];
      $$("#action-steps .action-step-row").forEach((row) => {
        const type = row.dataset.type || "tx";
        if (type === "delay") {
          steps.push({ type: "delay", delaySec: Number(row.querySelector(".as-delay").value) || 0 });
        } else if (type === "hold") {
          const sid = row.querySelector(".as-signal").value;
          if (sid) {
            steps.push({
              type: "hold",
              signalId: sid,
              holdSec: Number(row.querySelector(".as-hold").value) || 2,
            });
          }
        } else {
          const sid = row.querySelector(".as-signal").value;
          if (sid) {
            steps.push({
              type: "tx",
              signalId: sid,
              repeats: Number(row.querySelector(".as-reps").value) || 0,
            });
          }
        }
      });
      if (!steps.length) { alert("Add at least one step"); return; }
      await api("/api/actions", {
        method: "POST",
        body: JSON.stringify({
          id: editingActionId || "",
          name,
          steps,
        }),
      });
      closeActionEditor();
      loadActions().catch(console.warn);
    });
    $("#remote-add-btn").addEventListener("click", () => addRemoteButtonRow());
    $("#remote-type").addEventListener("change", syncDeviceEditorType);
    $("#weather-refresh").addEventListener("click", () => refreshWeatherList());
    $("#remote-cancel").addEventListener("click", () => {
      $("#remote-editor").hidden = true;
      editingRemoteId = null;
      if (weatherPollTimer) { clearInterval(weatherPollTimer); weatherPollTimer = null; }
    });
    $("#remote-save").addEventListener("click", async () => {
      const name = $("#remote-name").value.trim();
      if (!name) { alert("Enter a device name"); return; }
      const type = $("#remote-type").value || "remote";
      const payload = { id: editingRemoteId || "", name, type, reboot: true };

      if (type === "temperature" || type === "humidity") {
        const wid = Number($("#weather-id").value);
        if (!Number.isFinite(wid) || wid < 0) {
          alert("Select a weather station from the list (or enter its sensor ID)");
          return;
        }
        payload.weatherProtocol = $("#weather-protocol").value || "auto";
        payload.weatherId = wid;
        payload.weatherChannel = Number($("#weather-channel").value) || 0;
        if (pickedWeather) {
          if (pickedWeather.tempC != null) payload.lastTempC = pickedWeather.tempC;
          if (pickedWeather.humidity != null) payload.lastHumidity = pickedWeather.humidity;
        }
      } else {
        const buttons = [];
        $$("#remote-buttons .remote-btn-row").forEach((row) => {
          const bn = row.querySelector(".rb-name").value.trim();
          const mode = row.querySelector(".rb-mode")?.value || "stateless";
          const primary = parseButtonBind(row.querySelector(".rb-primary").value);
          const secondary = mode === "stateless"
            ? { signalId: "", actionId: "" }
            : parseButtonBind(row.querySelector(".rb-secondary").value);
          if (primary.actionId || primary.signalId || secondary.actionId || secondary.signalId) {
            buttons.push({
              name: bn || "Button",
              signalId: primary.signalId,
              actionId: primary.actionId,
              secondarySignalId: secondary.signalId,
              secondaryActionId: secondary.actionId,
              homekitMode: mode,
            });
          }
        });
        if (!buttons.length) { alert("Add at least one primary or secondary binding"); return; }
        payload.buttons = buttons;
      }

      if (!confirm("Save this device? The hub will reboot so HomeKit updates.")) return;
      await api("/api/remotes", {
        method: "POST",
        body: JSON.stringify(payload),
      });
    });

    $("#hk-refresh").addEventListener("click", () => loadHomeKit());
    $("#hk-rebuild").addEventListener("click", async () => {
      if (!confirm("Rebuild HomeKit accessories and reboot?")) return;
      await api("/api/homekit/rebuild", { method: "POST" });
    });

    $("#tx-send").addEventListener("click", async () => {
      await api("/api/tx", {
        method: "POST",
        body: JSON.stringify({
          frequency: Number($("#tx-freq").value),
          modulation: Number($("#tx-mod").value),
          repeats: Number($("#tx-reps").value),
          pulsesCsv: $("#tx-pulses").value,
        }),
      });
      alert("Transmit started");
    });
    $("#tx-load-btn").addEventListener("click", async () => {
      const id = $("#tx-load-sel").value;
      if (!id) return;
      const full = await api(`/api/signals/${id}`);
      const sig = full.signal;
      $("#tx-freq").value = sig.frequency;
      $("#tx-mod").value = sig.modulation;
      $("#tx-reps").value = sig.estimatedRepeats || 1;
      $("#tx-pulses").value = (sig.pulses || []).join(",");
    });

    $("#set-save").addEventListener("click", async () => {
      await api("/api/settings", {
        method: "POST",
        body: JSON.stringify({
          frequency: Number($("#set-freq").value),
          modulation: Number($("#set-mod").value),
          rxBandwidthKHz: Number($("#set-bw").value),
          dataRateBaud: Number($("#set-dr").value),
          deviationKHz: Number($("#set-dev").value),
          rssiThreshold: Number($("#set-thr").value),
        }),
      });
      refreshStatus();
      alert("RF settings applied");
    });
    $("#set-reset").addEventListener("click", async () => {
      await api("/api/settings/rf/reset", { method: "POST" });
      refreshStatus();
    });
    $("#wifi-clear").addEventListener("click", async () => {
      if (!confirm("Clear Wi-Fi credentials and reboot?")) return;
      await api("/api/wifi/clear", { method: "POST" });
    });

    $$(".ota-ch-btn").forEach((btn) => {
      btn.addEventListener("click", async () => {
        const channel = btn.dataset.channel;
        try {
          const data = await api("/api/ota/channel", {
            method: "POST",
            body: JSON.stringify({ channel }),
          });
          renderOta(data);
        } catch (e) {
          alert(e.message || "Could not change channel");
        }
      });
    });
    $("#ota-check").addEventListener("click", async () => {
      $("#ota-check").disabled = true;
      $("#ota-message").textContent = "Contacting GitHub…";
      await refreshOta(true);
    });
    $("#ota-install").addEventListener("click", async () => {
      const ver = state.ota?.available?.version || "this build";
      if (!confirm(`Install ${ver} now?\n\nThe device will download firmware and reboot. Do not power off.`)) return;
      try {
        const data = await api("/api/ota/install", { method: "POST", body: "{}" });
        renderOta(data);
      } catch (e) {
        alert(e.message || "Install failed to start");
      }
    });

    $("#wifi-scan-btn").addEventListener("click", async () => {
      const data = await api("/api/wifi/scan");
      const box = $("#wifi-list");
      box.innerHTML = "";
      (data.networks || []).forEach((n) => {
        const el = document.createElement("div");
        el.className = "wifi-item";
        el.innerHTML = `<div><strong>${escapeHtml(n.ssid)}</strong><div class="muted">${n.rssi} dBm ${n.secure ? "· secured" : "· open"}</div></div>`;
        el.addEventListener("click", () => { $("#wifi-ssid").value = n.ssid; });
        box.appendChild(el);
      });
    });
    $("#wifi-form").addEventListener("submit", async (ev) => {
      ev.preventDefault();
      await api("/api/wifi/save", {
        method: "POST",
        body: JSON.stringify({
          ssid: $("#wifi-ssid").value,
          password: $("#wifi-pass").value,
        }),
      });
    });
  }

  async function boot() {
    bind();
    connectWs();
    await refreshStatus();
    // If in AP mode, show setup page first for convenience
    if (state.status && state.status.wifiMode === "AP") {
      showPage("setup");
      $("#page-setup").hidden = false;
      // keep nav usable
    } else {
      showPage("dashboard");
    }
    setInterval(refreshStatus, 5000);
    setInterval(pollScan, 700);
  }

  boot();
})();
