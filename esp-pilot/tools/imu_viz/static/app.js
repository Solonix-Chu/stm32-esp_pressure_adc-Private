const $ = (id) => document.getElementById(id);

const connPill = $("connPill");
const lastUpdate = $("lastUpdate");
const tempC = $("tempC");

const rollDeg = $("rollDeg");
const pitchDeg = $("pitchDeg");
const yawDeg = $("yawDeg");

const rollBar = $("rollBar");
const pitchBar = $("pitchBar");
const yawBar = $("yawBar");

const cube = $("cube");
const plot = $("plot");
const lastJson = $("lastJson");
const ctx = plot.getContext("2d");

const linNavG = $("linNavG");
const gyroNavDps = $("gyroNavDps");
const shakeMetrics = $("shakeMetrics");
const rotMetrics = $("rotMetrics");

const series = {
  roll: [],
  pitch: [],
  yaw: [],
};

const MAX_POINTS = 600; // enough for ~20s @ 30Hz

function wrap180(deg) {
  let a = deg;
  while (a > 180) a -= 360;
  while (a < -180) a += 360;
  return a;
}

function makeUnwrapper() {
  return { init: false, last: 0, unwrapped: 0 };
}

// Works for both wrapped (-180..180) and already-continuous inputs.
function unwrapStep(st, deg) {
  const v = deg;
  if (!st.init) {
    st.init = true;
    st.last = v;
    st.unwrapped = v;
    return st.unwrapped;
  }
  let delta = v - st.last;
  if (delta > 180) delta -= 360;
  if (delta < -180) delta += 360;
  st.last = v;
  st.unwrapped += delta;
  return st.unwrapped;
}

const unwrapState = {
  roll: makeUnwrapper(),
  pitch: makeUnwrapper(),
  yaw: makeUnwrapper(),
};

function resetUnwrap() {
  unwrapState.roll = makeUnwrapper();
  unwrapState.pitch = makeUnwrapper();
  unwrapState.yaw = makeUnwrapper();
}

function setConn(ok) {
  connPill.textContent = ok ? "CONNECTED" : "DISCONNECTED";
  connPill.classList.toggle("good", ok);
  connPill.classList.toggle("bad", !ok);
}

function fmt(n) {
  if (n === null || n === undefined || Number.isNaN(n)) return "-";
  return n.toFixed(2);
}

function fmtN(n, d) {
  if (n === null || n === undefined || Number.isNaN(n)) return "-";
  return n.toFixed(d);
}

function fmtVec3(v, d = 3) {
  if (!Array.isArray(v) || v.length < 3) return "-";
  return `[${fmtN(v[0], d)} ${fmtN(v[1], d)} ${fmtN(v[2], d)}]`;
}

function fmtTriple(v, d0 = 3, d1 = 3, d2 = 2) {
  if (!Array.isArray(v) || v.length < 3) return "-";
  return `[${fmtN(v[0], d0)} ${fmtN(v[1], d1)} ${fmtN(v[2], d2)}]`;
}

function barFill(el, deg) {
  const v = Math.max(-180, Math.min(180, deg));
  const pct = (v + 180) / 360; // 0..1
  el.style.transform = `scaleX(${pct})`;
}

function addPoint(name, t, v) {
  series[name].push({ t, v });
  if (series[name].length > MAX_POINTS) series[name].shift();
}

function drawAxes() {
  const w = plot.width;
  const h = plot.height;
  ctx.clearRect(0, 0, w, h);

  // grid
  ctx.save();
  ctx.globalAlpha = 0.9;
  ctx.strokeStyle = "rgba(255,255,255,0.10)";
  ctx.lineWidth = 1;

  const ny = 6;
  for (let i = 0; i <= ny; i++) {
    const y = (h * i) / ny;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  const nx = 10;
  for (let i = 0; i <= nx; i++) {
    const x = (w * i) / nx;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, h);
    ctx.stroke();
  }
  ctx.restore();

  // labels
  ctx.save();
  ctx.fillStyle = "rgba(255,255,255,0.55)";
  ctx.font = "12px ui-monospace, Menlo, Consolas, monospace";
  ctx.fillText(`${currentRange.max.toFixed(0)}°`, 8, 16);
  ctx.fillText(`${((currentRange.min + currentRange.max) / 2).toFixed(0)}°`, 8, Math.round(h / 2) + 4);
  ctx.fillText(`${currentRange.min.toFixed(0)}°`, 8, h - 10);
  ctx.restore();
}

function mapY(v, yMin, yMax) {
  const h = plot.height;
  const clamped = Math.max(yMin, Math.min(yMax, v));
  const p = (clamped - yMin) / (yMax - yMin); // 0..1
  return h - p * h;
}

function drawLine(points, color, yMin, yMax) {
  if (points.length < 2) return;
  const w = plot.width;
  const t0 = points[0].t;
  const t1 = points[points.length - 1].t;
  const dt = Math.max(0.001, t1 - t0);

  ctx.save();
  ctx.strokeStyle = color;
  ctx.lineWidth = 2;
  ctx.beginPath();
  for (let i = 0; i < points.length; i++) {
    const p = points[i];
    const x = ((p.t - t0) / dt) * w;
    const y = mapY(p.v, yMin, yMax);
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();
  ctx.restore();
}

let currentRange = { min: -180, max: 180 };

function computeRange() {
  let min = Infinity;
  let max = -Infinity;
  for (const k of ["roll", "pitch", "yaw"]) {
    for (const p of series[k]) {
      if (typeof p.v !== "number" || Number.isNaN(p.v)) continue;
      if (p.v < min) min = p.v;
      if (p.v > max) max = p.v;
    }
  }
  if (!Number.isFinite(min) || !Number.isFinite(max)) return { min: -180, max: 180 };
  if (min === max) return { min: min - 1, max: max + 1 };

  const pad = Math.max(5, (max - min) * 0.08);
  min -= pad;
  max += pad;

  // snap to nice ticks (10 degrees)
  const step = 10;
  min = Math.floor(min / step) * step;
  max = Math.ceil(max / step) * step;

  // avoid zero-span
  if (max - min < 2) {
    max = min + 2;
  }
  return { min, max };
}

function redraw() {
  currentRange = computeRange();
  drawAxes();
  drawLine(series.roll, "rgba(96,165,250,0.95)", currentRange.min, currentRange.max);
  drawLine(series.pitch, "rgba(167,139,250,0.95)", currentRange.min, currentRange.max);
  drawLine(series.yaw, "rgba(52,211,153,0.90)", currentRange.min, currentRange.max);
}

function applySample(s) {
  const t = s.t ?? (Date.now() / 1000);

  const eRoll = typeof s.roll === "number" ? s.roll : null;
  const ePitch = typeof s.pitch === "number" ? s.pitch : null;
  const eYaw = typeof s.yaw === "number" ? s.yaw : null;

  const rollPlotIn = typeof s.turn_roll === "number" ? s.turn_roll : eRoll ?? 0;
  const pitchPlotIn = typeof s.turn_pitch === "number" ? s.turn_pitch : ePitch ?? 0;
  const yawPlotIn = typeof s.turn_yaw === "number" ? s.turn_yaw : eYaw ?? 0;

  const rollU = unwrapStep(unwrapState.roll, rollPlotIn);
  const pitchU = unwrapStep(unwrapState.pitch, pitchPlotIn);
  const yawU = unwrapStep(unwrapState.yaw, yawPlotIn);

  // numeric display: show Euler angles for intuitive control
  rollDeg.textContent = fmt(eRoll ?? rollPlotIn);
  pitchDeg.textContent = fmt(ePitch ?? pitchPlotIn);
  yawDeg.textContent = fmt(eYaw ?? yawPlotIn);

  // bars: still show wrapped (-180..180)
  barFill(rollBar, wrap180(eRoll ?? rollU));
  barFill(pitchBar, wrap180(ePitch ?? pitchU));
  barFill(yawBar, wrap180(eYaw ?? yawU));

  // simple 3D (Euler): rotateX(pitch) rotateZ(roll) rotateY(yaw)
  const cr = eRoll ?? rollU;
  const cp = ePitch ?? pitchU;
  const cy = eYaw ?? yawU;
  cube.style.transform = `rotateX(${cp}deg) rotateZ(${cr}deg) rotateY(${cy}deg)`;

  if (typeof s.temp_c === "number") tempC.textContent = s.temp_c.toFixed(1);
  lastUpdate.textContent = new Date().toLocaleTimeString();

  if (linNavG) linNavG.textContent = fmtVec3(s.lin_a_nav_g, 3);
  if (gyroNavDps) gyroNavDps.textContent = fmtVec3(s.gyro_nav_dps, 2);
  if (shakeMetrics) shakeMetrics.textContent = fmtTriple(s.shake, 3, 3, 2);
  if (rotMetrics) rotMetrics.textContent = fmtTriple(s.rot, 2, 2, 2);

  addPoint("roll", t, rollU);
  addPoint("pitch", t, pitchU);
  addPoint("yaw", t, yawU);
  redraw();

  lastJson.textContent = JSON.stringify(
    {
      raw: s,
      derived: {
        plot_unwrapped: {
          roll: rollU,
          pitch: pitchU,
          yaw: yawU,
        },
      },
    },
    null,
    2,
  );
}

async function loadHistory() {
  try {
    const res = await fetch("/history", { cache: "no-store" });
    const arr = await res.json();
    resetUnwrap();
    for (const s of arr) applySample(s);
  } catch (e) {
    // ignore
  }
}

function connect() {
  setConn(false);
  const es = new EventSource("/events");

  es.onopen = () => setConn(true);
  es.onerror = () => setConn(false);

  es.addEventListener("snapshot", (ev) => {
    try {
      const arr = JSON.parse(ev.data);
      // reset series
      series.roll = [];
      series.pitch = [];
      series.yaw = [];
      resetUnwrap();
      for (const s of arr) applySample(s);
    } catch (e) {
      // ignore
    }
  });

  es.onmessage = (ev) => {
    try {
      const s = JSON.parse(ev.data);
      applySample(s);
    } catch (e) {
      // ignore
    }
  };
}

drawAxes();
loadHistory().finally(connect);
