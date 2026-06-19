// Canvas-based waveform viewer component.
// Renders digital signal transitions with zoom, pan, and cursor measurement.

import React, { useRef, useEffect, useCallback, useState } from "react";

export interface WaveEntry {
  signal: string;
  time_fs: number;
  value: string;
}

interface GroupedSignal {
  name: string;
  transitions: WaveEntry[];
  width: number; // bit width (1 = scalar, >1 = bus)
}

const NAME_COL = 120;       // px for signal name labels
const TRACK_H = 28;         // px per signal track
const RULER_H = 24;         // px for time ruler
const MIN_ZOOM = 0.1;       // fs per pixel minimum
const MAX_ZOOM = 100000;    // fs per pixel maximum

interface WaveformViewerProps {
  entries: WaveEntry[];
}

export function WaveformViewer({ entries }: WaveformViewerProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const containerRef = useRef<HTMLDivElement>(null);
  const [zoom, setZoom] = useState(50); // fs per pixel
  const [offset, setOffset] = useState(0); // scroll offset in fs
  const [cursorTime, setCursorTime] = useState<number | null>(null);
  const [canvasWidth, setCanvasWidth] = useState(800);

  // Group entries by signal name, sort by time
  const groups = React.useMemo(() => {
    const map = new Map<string, WaveEntry[]>();
    for (const e of entries) {
      const list = map.get(e.signal) || [];
      list.push(e);
      map.set(e.signal, list);
    }
    const result: GroupedSignal[] = [];
    for (const [name, trans] of map) {
      trans.sort((a, b) => a.time_fs - b.time_fs);
      const width = trans.length > 0 ? Math.max(1, trans[0].value.length) : 1;
      result.push({ name, transitions: trans, width });
    }
    return result;
  }, [entries]);

  const totalHeight = RULER_H + groups.length * TRACK_H + 8;

  // Resize observer
  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    const ro = new ResizeObserver(() => {
      setCanvasWidth(el.clientWidth - NAME_COL);
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // Draw
  const draw = useCallback(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const dpr = window.devicePixelRatio || 1;
    const w = canvasWidth;
    const h = totalHeight;
    canvas.width = w * dpr;
    canvas.height = h * dpr;
    ctx.scale(dpr, dpr);
    ctx.clearRect(0, 0, w, h);

    // Background — dark theme
    ctx.fillStyle = "#1a1a2e";
    ctx.fillRect(0, 0, w, h);

    // Time ruler
    drawRuler(ctx, w, offset, zoom);

    // Signals
    for (let i = 0; i < groups.length; i++) {
      const g = groups[i];
      const y = RULER_H + i * TRACK_H;
      drawSignalTrack(ctx, w, offset, zoom, g, y, TRACK_H);
    }

    // Cursor line
    if (cursorTime !== null) {
      const cx = timeToPixel(cursorTime, offset, zoom);
      if (cx >= 0 && cx <= w) {
        ctx.strokeStyle = "#f00";
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(cx, RULER_H);
        ctx.lineTo(cx, RULER_H + groups.length * TRACK_H);
        ctx.stroke();
        // Time label at top
        ctx.fillStyle = "#f00";
        ctx.font = "11px monospace";
        const label = formatTime(cursorTime);
        ctx.fillText(label, cx + 4, 14);
      }
    }
  }, [groups, canvasWidth, offset, zoom, cursorTime, totalHeight]);

  useEffect(() => {
    draw();
  }, [draw]);

  // Mouse wheel = zoom
  const handleWheel = useCallback((e: React.WheelEvent) => {
    e.preventDefault();
    const delta = e.deltaY > 0 ? 1.2 : 1 / 1.2;
    setZoom(z => Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, z * delta)));
  }, []);

  // Mouse move = cursor
  const handleMouseMove = useCallback((e: React.MouseEvent) => {
    const rect = canvasRef.current?.getBoundingClientRect();
    if (!rect) return;
    const x = e.clientX - rect.left;
    const time = pixelToTime(x, offset, zoom);
    setCursorTime(Math.max(0, time));
  }, [offset, zoom]);

  const handleMouseLeave = useCallback(() => {
    setCursorTime(null);
  }, []);

  // Pan via click-drag
  const [panning, setPanning] = useState(false);
  const panStart = useRef({ x: 0, offset: 0 });

  const handleMouseDown = useCallback((e: React.MouseEvent) => {
    setPanning(true);
    panStart.current = { x: e.clientX, offset };
  }, [offset]);

  const handleMouseUp = useCallback(() => {
    setPanning(false);
  }, []);

  useEffect(() => {
    if (!panning) return;
    const handleMove = (e: MouseEvent) => {
      const dx = panStart.current.x - e.clientX;
      setOffset(() => Math.max(0, panStart.current.offset + dx * zoom));
    };
    const handleUp = () => setPanning(false);
    window.addEventListener("mousemove", handleMove);
    window.addEventListener("mouseup", handleUp);
    return () => {
      window.removeEventListener("mousemove", handleMove);
      window.removeEventListener("mouseup", handleUp);
    };
  }, [panning, zoom]);

  return (
    <div style={{ border: "1px solid #0f3460", borderRadius: "4px", overflow: "hidden" }}>
      {/* Controls bar */}
      <div style={{
        display: "flex", alignItems: "center", gap: "12px", padding: "6px 12px",
        background: "#16213e", borderBottom: "1px solid #0f3460", fontSize: "12px",
      }}>
        <button onClick={() => setZoom(50)} style={btnStyle}>Reset Zoom</button>
        <button onClick={() => setOffset(0)} style={btnStyle}>Reset Pan</button>
        <span style={{ color: "#e0e0e0" }}>
          Zoom: {zoom.toFixed(0)} fs/px
          {cursorTime !== null && ` | Cursor: ${formatTime(cursorTime)}`}
        </span>
      </div>

      {/* Canvas area */}
      <div ref={containerRef} style={{ display: "flex", position: "relative" }}>
        {/* Signal names column */}
        <div style={{
          width: NAME_COL, flexShrink: 0, overflow: "hidden",
          borderRight: "1px solid #0f3460", userSelect: "none",
        }}>
          <div style={{ height: RULER_H, background: "#16213e", borderBottom: "1px solid #0f3460" }} />
          {groups.map((g, i) => (
            <div key={g.name} style={{
              height: TRACK_H, lineHeight: `${TRACK_H}px`, padding: "0 8px",
              fontSize: "12px", fontFamily: "monospace", overflow: "hidden",
              textOverflow: "ellipsis", whiteSpace: "nowrap",
              color: "#e0e0e0",
              background: i % 2 === 0 ? "#1a1a2e" : "#162447",
              borderBottom: "1px solid #0f3460",
            }} title={g.name}>
              {g.name}
            </div>
          ))}
        </div>

        {/* Waveform canvas */}
        <canvas
          ref={canvasRef}
          style={{
            width: canvasWidth, height: totalHeight, cursor: panning ? "grabbing" : "crosshair",
          }}
          onWheel={handleWheel}
          onMouseMove={handleMouseMove}
          onMouseLeave={handleMouseLeave}
          onMouseDown={handleMouseDown}
          onMouseUp={handleMouseUp}
        />
      </div>
    </div>
  );
}

// ── Drawing helpers ──

function drawRuler(ctx: CanvasRenderingContext2D, w: number, offset: number, zoom: number) {
  ctx.fillStyle = "#162447";
  ctx.fillRect(0, 0, w, RULER_H);
  ctx.strokeStyle = "#0f3460";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, RULER_H - 0.5);
  ctx.lineTo(w, RULER_H - 0.5);
  ctx.stroke();

  const tickTargetPx = 80;
  const tickStep = tickTargetPx * zoom;
  const niceStep = niceTick(tickStep);
  const firstTick = Math.floor(offset / niceStep) * niceStep;
  const lastTick = offset + w * zoom;

  ctx.fillStyle = "#e0e0e0";
  ctx.font = "10px monospace";
  ctx.textAlign = "center";

  for (let t = firstTick; t <= lastTick; t += niceStep) {
    const x = timeToPixel(t, offset, zoom);
    if (x < 0 || x > w) continue;
    // Tick mark
    ctx.beginPath();
    ctx.moveTo(x, RULER_H - 6);
    ctx.lineTo(x, RULER_H);
    ctx.stroke();
    // Label
    ctx.fillText(formatTime(t), x, 10);
  }
}

function drawSignalTrack(
  ctx: CanvasRenderingContext2D,
  w: number, offset: number, zoom: number,
  sig: GroupedSignal, y: number, h: number,
) {
  const isBus = sig.width > 1;
  const midY = y + h / 2;
  const labelY = y + h / 2 + 4;

  // Background
  ctx.fillStyle = "#162447";
  ctx.fillRect(0, y, w, h);

  if (sig.transitions.length === 0) {
    ctx.strokeStyle = "#e94560";
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(0, midY);
    ctx.lineTo(w, midY);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = "#e94560";
    ctx.font = "11px monospace";
    ctx.fillText("X", 4, labelY);
    return;
  }

  const last = sig.transitions[sig.transitions.length - 1];
  const extended: WaveEntry[] = [...sig.transitions, { ...last, time_fs: offset + w * zoom }];

  ctx.strokeStyle = isBus ? "#4ecca3" : "#e94560";
  ctx.lineWidth = isBus ? 2 : 1.5;

  if (isBus) {
    ctx.beginPath();
    for (let i = 0; i < extended.length; i++) {
      const x1 = timeToPixel(extended[i].time_fs, offset, zoom);
      const x2 = i < extended.length - 1
        ? timeToPixel(extended[i + 1].time_fs, offset, zoom)
        : w;
      if (x2 < 0 || x1 > w) continue;
      const cx = Math.max(0, x1);
      const cx2 = Math.min(w, x2);
      ctx.moveTo(cx, midY);
      ctx.lineTo(cx2, midY);
    }
    ctx.stroke();

    ctx.font = "11px monospace";
    ctx.fillStyle = "#4ecca3";
    for (let i = 0; i < extended.length; i++) {
      const x = timeToPixel(extended[i].time_fs, offset, zoom);
      const nextX = i < extended.length - 1
        ? timeToPixel(extended[i + 1].time_fs, offset, zoom)
        : w;
      if (nextX < 0 || x > w) continue;
      const cx = Math.max(0, x);
      const label = formatBusValue(extended[i].value);
      ctx.fillText(label, cx + 3, labelY);
    }
  } else {
    // Single-bit: draw square wave
    let prevVal = "";
    ctx.beginPath();
    for (let i = 0; i < extended.length; i++) {
      const val = extended[i].value;
      const x = timeToPixel(extended[i].time_fs, offset, zoom);
      const currentY = val === "1" || val === "Z" ? y + 4 : y + h - 4;
      const prevY = prevVal === "1" || prevVal === "Z" ? y + 4 : y + h - 4;

      if (i === 0) {
        ctx.moveTo(0, currentY);
      } else {
        // Draw horizontal from previous position, then vertical
        ctx.lineTo(x, prevY);
        ctx.lineTo(x, currentY);
      }
      prevVal = val;
    }
    ctx.stroke();
  }

  // Transition lines
  ctx.strokeStyle = "#0f3460";
  ctx.lineWidth = 1;
  for (let i = 1; i < sig.transitions.length; i++) {
    const x = timeToPixel(sig.transitions[i].time_fs, offset, zoom);
    if (x >= 0 && x <= w) {
      ctx.beginPath();
      ctx.moveTo(x, y);
      ctx.lineTo(x, y + h);
      ctx.stroke();
    }
  }
}

// ── Coordinate helpers ──

function timeToPixel(time: number, offset: number, zoom: number): number {
  return (time - offset) / zoom;
}

function pixelToTime(pixel: number, offset: number, zoom: number): number {
  return pixel * zoom + offset;
}

// ── Formatting helpers ──

function formatTime(fs: number): string {
  if (fs >= 1000000) return (fs / 1000000).toFixed(1) + "ns";
  if (fs >= 1000) return (fs / 1000).toFixed(1) + "ps";
  return fs.toFixed(0) + "fs";
}

function formatBusValue(val: string): string {
  if (!val) return "X";
  // Show as hex if width is multiple of 4
  if (val.length >= 4 && val.length % 4 === 0) {
    const hex = parseInt(val, 2);
    if (!isNaN(hex)) return "0x" + hex.toString(16).toUpperCase();
  }
  // Show as binary
  return "b" + val;
}

function niceTick(step: number): number {
  const order = Math.pow(10, Math.floor(Math.log10(step)));
  const rem = step / order;
  if (rem < 1.5) return 1 * order;
  if (rem < 3.5) return 2 * order;
  if (rem < 7.5) return 5 * order;
  return 10 * order;
}

const btnStyle: React.CSSProperties = {
  padding: "3px 10px", fontSize: "11px", cursor: "pointer",
  border: "1px solid #999", borderRadius: "3px", background: "#fff",
};
