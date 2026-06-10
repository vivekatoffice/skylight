// Offline clip stabilizer. The PTZ motor can't pan smoothly below ~3.6°/s, so
// recordings of slow passes lurch. We can't fix that mechanically — but offline
// we have every frame AND the motion-compensated detector, so we find the plane
// in each frame, smooth the track, and crop a window LOCKED onto it. The plane
// is pinned to centre and the mechanical stop-go is cancelled — a clean tracking
// shot. Perfectly in sync (the detection and the frame are the same frame), and
// the compute is offline so the Pi isn't pressured in real time.
//
// Pipeline:
//   1. detect — pipe downscaled gray frames from ffmpeg, motion-comp per frame,
//      nearest-neighbour track the dominant mover (the plane), centre-seeded.
//   2. smooth — median (outlier reject) + EMA; interpolate detection gaps.
//   3. crop+encode — ONE ffmpeg pass: sendcmd drives the crop x/y along the
//      track, scale to the output size, libx264. No frame extraction to disk.

import { spawn } from "node:child_process";
import { writeFile, unlink } from "node:fs/promises";
import { existsSync, readFileSync } from "node:fs";
import { estimateShift, compensatedResidual, findMovingBlobs } from "../vision/motion.js";

const DW = 480;
const DH = 270;

export interface StabilizeOpts {
  /** Crop window height as a fraction of source height (smaller = more zoom). */
  cropFrac?: number;
  /** Output height (px); width follows 16:9. */
  outH?: number;
  /** Nudge the sidecar track vs the video for stream latency, ms. */
  alignMs?: number;
  onProgress?: (frac: number) => void;
}

interface Probe {
  width: number;
  height: number;
  fps: number;
  frames: number;
  duration: number;
}

function ffprobe(path: string): Promise<Probe> {
  return new Promise((resolve, reject) => {
    const p = spawn("ffprobe", [
      "-v", "error", "-select_streams", "v:0",
      "-show_entries", "stream=width,height,r_frame_rate,nb_read_frames:format=duration",
      "-count_frames", "-of", "json", path,
    ]);
    let out = "";
    p.stdout.on("data", (c) => (out += c));
    p.on("error", reject);
    p.on("close", () => {
      try {
        const j = JSON.parse(out);
        const s = j.streams[0];
        const [n, d] = String(s.r_frame_rate).split("/").map(Number);
        resolve({
          width: s.width,
          height: s.height,
          fps: d ? n / d : 30,
          frames: Number(s.nb_read_frames) || 0,
          duration: Number(j.format?.duration) || 0,
        });
      } catch (e) {
        reject(e);
      }
    });
  });
}

/** Track the plane (frame fractions) across the clip via motion compensation. */
function detectTrack(path: string): Promise<({ cx: number; cy: number } | null)[]> {
  return new Promise((resolve, reject) => {
    const ff = spawn("ffmpeg", [
      "-hide_banner", "-loglevel", "error", "-i", path,
      "-vf", `scale=${DW}:${DH},format=gray`, "-f", "rawvideo", "pipe:1",
    ]);
    const frameBytes = DW * DH;
    let buf = Buffer.alloc(0);
    let prev: Uint8Array | null = null;
    let plane: { cx: number; cy: number } | null = null;
    const track: ({ cx: number; cy: number } | null)[] = [];

    const onFrame = (lum: Uint8Array) => {
      if (!prev) {
        prev = lum;
        track.push(null);
        return;
      }
      const shift = estimateShift(prev, lum, DW, DH, 0, 0, 14);
      const resid = compensatedResidual(prev, lum, DW, DH, shift);
      // Mask the lower frame: the camera keeps the tracked plane high/centred,
      // while rooftops/poles/trees are world-static clutter that leaves strong
      // residual edges along the bottom. (No per-frame pose offline, so a fixed
      // fraction; safe while actively tracking.)
      for (let y = (0.7 * DH) | 0; y < DH; y++) {
        const row = y * DW;
        for (let x = 0; x < DW; x++) resid[row + x] = 0;
      }
      const blobs = findMovingBlobs(resid, DW, DH, 6, { minPeakSigma: 3.5 });
      // The plane is the mover that's strong AND near frame centre (the tracker
      // holds it there) AND continuous with the running estimate.
      let best = -1;
      let pick: { cx: number; cy: number } | null = null;
      for (const b of blobs) {
        const central = Math.exp(
          -((b.cx - 0.5) ** 2 + (b.cy - 0.45) ** 2) / (0.28 * 0.28),
        );
        const cont = plane
          ? Math.exp(-((b.cx - plane.cx) ** 2 + (b.cy - plane.cy) ** 2) / (0.13 * 0.13))
          : 1;
        const score = b.peakSigma * (0.35 + 0.65 * central) * cont;
        if (score > best) {
          best = score;
          pick = { cx: b.cx, cy: b.cy };
        }
      }
      if (pick) {
        plane = plane
          ? { cx: plane.cx * 0.4 + pick.cx * 0.6, cy: plane.cy * 0.4 + pick.cy * 0.6 }
          : pick;
        track.push({ ...pick });
      } else {
        track.push(null);
      }
      prev = lum;
    };

    ff.stdout.on("data", (chunk: Buffer) => {
      buf = Buffer.concat([buf, chunk]);
      while (buf.length >= frameBytes) {
        onFrame(new Uint8Array(buf.subarray(0, frameBytes)));
        buf = buf.subarray(frameBytes);
      }
    });
    ff.on("error", reject);
    ff.on("close", () => resolve(track));
  });
}

/** Per-frame plane track from the live recorder sidecar (the tracker already
 *  knew where the plane was). Maps detection times to frame indices and
 *  interpolates to the video frame rate. `alignMs` nudges for stream latency. */
function trackFromSidecar(
  sc: { startedAt: number; track: { t: number; cx: number; cy: number }[] },
  nFrames: number,
  fps: number,
  alignMs = 0,
): ({ cx: number; cy: number } | null)[] {
  const out: ({ cx: number; cy: number } | null)[] = new Array(nFrames).fill(null);
  const pts = sc.track
    .map((d) => ({ fi: ((d.t - sc.startedAt + alignMs) / 1000) * fps, cx: d.cx, cy: d.cy }))
    .filter((p) => Number.isFinite(p.fi))
    .sort((a, b) => a.fi - b.fi);
  if (!pts.length) return out;
  let j = 1;
  for (let i = 0; i < nFrames; i++) {
    if (i <= pts[0].fi) {
      out[i] = { cx: pts[0].cx, cy: pts[0].cy };
      continue;
    }
    const last = pts[pts.length - 1];
    if (i >= last.fi) {
      out[i] = { cx: last.cx, cy: last.cy };
      continue;
    }
    while (j < pts.length && pts[j].fi < i) j++;
    const a = pts[j - 1];
    const b = pts[j];
    const f = (i - a.fi) / Math.max(1e-6, b.fi - a.fi);
    out[i] = { cx: a.cx + (b.cx - a.cx) * f, cy: a.cy + (b.cy - a.cy) * f };
  }
  return out;
}

/** Median(window 5, outlier reject) + EMA smoothing; linear-interp gaps + hold ends. */
function smoothTrack(
  track: ({ cx: number; cy: number } | null)[],
): { cx: number; cy: number }[] {
  const n = track.length;
  // Fill gaps by linear interpolation between the nearest detected frames.
  const filled: { cx: number; cy: number }[] = new Array(n);
  let lastIdx = -1;
  const idxs = track.map((t, i) => (t ? i : -1)).filter((i) => i >= 0);
  if (idxs.length === 0) {
    return new Array(n).fill({ cx: 0.5, cy: 0.5 });
  }
  for (let i = 0; i < n; i++) {
    if (track[i]) {
      filled[i] = { ...track[i]! };
      lastIdx = i;
    } else {
      const nextIdx = idxs.find((j) => j > i);
      if (lastIdx < 0 && nextIdx != null) filled[i] = { ...track[nextIdx]! };
      else if (nextIdx == null) filled[i] = { ...track[lastIdx]! };
      else {
        const a = track[lastIdx]!;
        const b = track[nextIdx]!;
        const f = (i - lastIdx) / (nextIdx - lastIdx);
        filled[i] = { cx: a.cx + (b.cx - a.cx) * f, cy: a.cy + (b.cy - a.cy) * f };
      }
    }
  }
  // Median-5 to kill detection outliers (a single bad frame jumping the crop).
  const med = (arr: number[]): number => arr.slice().sort((x, y) => x - y)[arr.length >> 1];
  const cleaned: { cx: number; cy: number }[] = new Array(n);
  for (let i = 0; i < n; i++) {
    const lo = Math.max(0, i - 2);
    const hi = Math.min(n - 1, i + 2);
    const xs: number[] = [];
    const ys: number[] = [];
    for (let j = lo; j <= hi; j++) {
      xs.push(filled[j].cx);
      ys.push(filled[j].cy);
    }
    cleaned[i] = { cx: med(xs), cy: med(ys) };
  }
  // Light forward+backward EMA (zero phase) so the crop glides.
  const a = 0.35;
  const out = cleaned.map((p) => ({ ...p }));
  for (let i = 1; i < n; i++) {
    out[i].cx = out[i - 1].cx + a * (out[i].cx - out[i - 1].cx);
    out[i].cy = out[i - 1].cy + a * (out[i].cy - out[i - 1].cy);
  }
  for (let i = n - 2; i >= 0; i--) {
    out[i].cx = out[i + 1].cx + a * (out[i].cx - out[i + 1].cx);
    out[i].cy = out[i + 1].cy + a * (out[i].cy - out[i + 1].cy);
  }
  return out;
}

/** Stabilize a clip: detect the plane, smooth, crop locked onto it, encode. */
export async function stabilizeClip(
  src: string,
  out: string,
  opts: StabilizeOpts = {},
): Promise<void> {
  const cropFrac = Math.min(0.95, Math.max(0.3, opts.cropFrac ?? 0.62));
  const probe = await ffprobe(src);
  const { width: W, height: H, fps } = probe;
  const nFrames = probe.frames || Math.ceil((probe.duration || 30) * fps) + 15;
  opts.onProgress?.(0.1);

  // Prefer the live track logged during recording (the tracker knew exactly
  // where the plane was); fall back to offline motion-comp detection.
  const sidecar = `${src}.track.json`;
  let raw: ({ cx: number; cy: number } | null)[];
  if (existsSync(sidecar)) {
    const sc = JSON.parse(readFileSync(sidecar, "utf8"));
    raw = trackFromSidecar(sc, nFrames, fps, opts.alignMs ?? 0);
  } else {
    raw = await detectTrack(src);
  }
  opts.onProgress?.(0.5);
  const track = smoothTrack(raw);

  // Crop window (even dims), 16:9, clamped inside the frame as it follows the plane.
  const cw = Math.round((Math.round(H * cropFrac) * 16) / 9 / 2) * 2;
  const ch = Math.round((Math.round(H * cropFrac)) / 2) * 2;
  const cmds: string[] = [];
  for (let i = 0; i < track.length; i++) {
    const t = (i / fps).toFixed(4);
    const x = Math.max(0, Math.min(W - cw, Math.round(track[i].cx * W - cw / 2)));
    const y = Math.max(0, Math.min(H - ch, Math.round(track[i].cy * H - ch / 2)));
    cmds.push(`${t} crop x ${x}, crop y ${y};`);
  }
  const cmdFile = `${out}.cmds.txt`;
  await writeFile(cmdFile, cmds.join("\n"));

  const outH = opts.outH ?? 720;
  const outW = Math.round((outH * 16) / 9 / 2) * 2;
  await new Promise<void>((resolve, reject) => {
    // niced + thread-capped so the offline encode yields to the live tracker on
    // the already-busy Pi (no hardware H.264 encoder, so this is software x264).
    const ff = spawn("nice", [
      "-n", "19", "ffmpeg",
      "-hide_banner", "-loglevel", "error", "-y", "-threads", "2", "-i", src,
      "-vf", `sendcmd=f=${cmdFile},crop=${cw}:${ch}:0:0,scale=${outW}:${outH}`,
      "-an", "-c:v", "libx264", "-preset", "veryfast", "-crf", "21",
      "-movflags", "+faststart", out,
    ]);
    let err = "";
    ff.stderr.on("data", (c) => (err = (err + c.toString()).slice(-500)));
    ff.on("error", reject);
    ff.on("close", (code) =>
      code === 0 ? resolve() : reject(new Error(`ffmpeg ${code}: ${err}`)),
    );
  });
  await unlink(cmdFile).catch(() => {});
  opts.onProgress?.(1);
}
