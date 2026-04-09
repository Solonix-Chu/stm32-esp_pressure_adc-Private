const state = {
  status: null,
  files: [],
};

const el = (id) => document.getElementById(id);

function formatBytes(value) {
  if (value == null || Number.isNaN(value)) return "--";
  const units = ["B", "KB", "MB", "GB"];
  let size = Number(value);
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return `${size.toFixed(size >= 10 || unit === 0 ? 0 : 1)} ${units[unit]}`;
}

function formatTime(epoch) {
  if (!epoch) return "--";
  return new Date(epoch * 1000).toLocaleString();
}

function setMessage(text, tone = "neutral") {
  const node = el("message");
  node.textContent = text;
  node.dataset.tone = tone;
}

async function request(url, options = {}) {
  const response = await fetch(url, options);
  let payload = null;
  const type = response.headers.get("content-type") || "";
  if (type.includes("application/json")) {
    payload = await response.json();
  } else {
    payload = await response.text();
  }
  if (!response.ok) {
    const message = payload?.message || response.statusText;
    throw new Error(message);
  }
  return payload;
}

function renderStatus() {
  const status = state.status;
  if (!status) return;

  el("linkBadge").textContent = status.spi_ready ? "Link: ready" : "Link: waiting";
  el("recordBadge").textContent = status.recording_active ? "Recorder: active" : "Recorder: idle";
  el("wifiInfo").textContent = `Wi-Fi: ${status.ap_ssid} @ http://${status.ap_ip}`;
  el("activeFile").textContent = `Current file: ${status.active_file || "--"}`;
  el("packetsReceived").textContent = status.packets_received;
  el("packetsValid").textContent = status.packets_valid;
  el("packetsWritten").textContent = status.packets_written;
  el("queueDepth").textContent = `${status.queued_packets} queued / ${status.free_packet_slots} free`;
  el("stmDrop").textContent = status.last_stm32_dropped_packets;
  el("errorSummary").textContent = `spi ${status.spi_errors}, hdr ${status.header_errors}, sum ${status.checksum_errors}, q ${status.queue_overflows}`;
  el("lastSeq").textContent = `${status.last_sequence} (gaps ${status.sequence_gap_events})`;
  el("lastPacketAge").textContent = status.last_packet_age_ms >= 0 ? `${status.last_packet_age_ms} ms ago` : "--";
  el("storageUsage").textContent = `${formatBytes(status.storage_used_bytes)} / ${formatBytes(status.storage_total_bytes)}`;
  el("lastError").textContent = status.last_error || "--";
  el("startBtn").disabled = status.recording_active;
  el("stopBtn").disabled = !status.recording_active;
  el("fileHint").textContent = status.recording_active
    ? "Recording is active. Stop recording before downloading or deleting files."
    : "Recorder is idle. Files can be downloaded or deleted.";
}

function renderFiles() {
  const body = el("fileTable");
  const canManage = state.status && !state.status.recording_active;

  if (!state.files.length) {
    body.innerHTML = '<tr><td colspan="4" class="empty">No files stored on the device.</td></tr>';
    return;
  }

  body.innerHTML = state.files
    .map((file) => {
      const action = canManage
        ? `<div class="row-actions">
            <a class="mini-btn" href="/api/files/download?name=${encodeURIComponent(file.name)}">Download</a>
            <button class="mini-btn mini-btn-danger" data-delete="${file.name}">Delete</button>
          </div>`
        : '<span class="muted">Stop recording to manage</span>';

      return `<tr>
        <td>${file.name}</td>
        <td>${formatBytes(file.size_bytes)}</td>
        <td>${formatTime(file.modified_time)}</td>
        <td>${action}</td>
      </tr>`;
    })
    .join("");

  body.querySelectorAll("[data-delete]").forEach((button) => {
    button.addEventListener("click", async () => {
      const name = button.dataset.delete;
      if (!confirm(`Delete ${name}?`)) return;
      try {
        await request(`/api/files?name=${encodeURIComponent(name)}`, { method: "DELETE" });
        setMessage(`Deleted ${name}`, "success");
        await refreshAll();
      } catch (error) {
        setMessage(error.message, "error");
      }
    });
  });
}

async function loadStatus() {
  state.status = await request("/api/status");
  renderStatus();
}

async function loadFiles() {
  const payload = await request("/api/files");
  state.files = payload.files || [];
  renderFiles();
}

async function refreshAll() {
  await Promise.all([loadStatus(), loadFiles()]);
}

async function startRecording() {
  try {
    const payload = await request("/api/recording/start", { method: "POST" });
    setMessage(payload.message || "Recording started", "success");
    await refreshAll();
  } catch (error) {
    setMessage(error.message, "error");
  }
}

async function stopRecording() {
  try {
    const payload = await request("/api/recording/stop", { method: "POST" });
    setMessage(payload.message || "Recording stopped", "success");
    await refreshAll();
  } catch (error) {
    setMessage(error.message, "error");
  }
}

function bindActions() {
  el("startBtn").addEventListener("click", startRecording);
  el("stopBtn").addEventListener("click", stopRecording);
  el("refreshBtn").addEventListener("click", async () => {
    setMessage("Refreshing state...", "neutral");
    try {
      await refreshAll();
      setMessage("State updated", "success");
    } catch (error) {
      setMessage(error.message, "error");
    }
  });
}

async function boot() {
  bindActions();
  try {
    await refreshAll();
    setMessage("Device state loaded", "success");
  } catch (error) {
    setMessage(error.message, "error");
  }

  setInterval(async () => {
    try {
      await loadStatus();
      await loadFiles();
    } catch (error) {
      setMessage(error.message, "error");
    }
  }, 1500);
}

boot();
