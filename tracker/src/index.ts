// Tracker entry point: aircraft feed in (server WS or replay file), pointing
// pipeline + camera driver in the middle, debug hub + video out.

import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import type { CameraDriver } from "./camera/driver.js";
import { SimCamera } from "./camera/sim.js";
import { ViscaCamera } from "./camera/visca.js";
import { TrackerHub } from "./hub.js";
import { ControlLoop } from "./loop.js";
import { Recorder } from "./record.js";
import { ReplayUpstream, WsUpstream, type Upstream } from "./upstream.js";
import { Mp4Stream } from "./video/mse.js";
import { VideoStream } from "./video/stream.js";
import { VideoRecorder } from "./video/recorder.js";

const __dirname = dirname(fileURLToPath(import.meta.url));

const PORT = Number(process.env.TRACKER_PORT ?? 3001);
const HOST = process.env.TRACKER_HOST ?? "0.0.0.0";
const SERVER_WS = process.env.SERVER_WS ?? "ws://localhost:3000/ws";
const REPLAY = process.env.TRACKER_REPLAY;
const SESSIONS_DIR = resolve(__dirname, "../data/sessions");
const RECORDINGS_DIR = resolve(__dirname, "../data/recordings");

function rtspUrl(template: string, ip: string): string {
  return template.replace("{ip}", ip);
}

async function main(): Promise<void> {
  const recorder = new Recorder(SESSIONS_DIR);

  // Driver is swappable at runtime (config.tracker.driver).
  let driver: CameraDriver | null = null;
  const getDriver = (): CameraDriver => {
    if (!driver) {
      driver = makeDriver(upstream.getConfig().tracker.driver);
    }
    return driver;
  };
  const makeDriver = (kind: "sim" | "visca"): CameraDriver => {
    const t = upstream.getConfig().tracker;
    const d: CameraDriver =
      kind === "visca"
        ? new ViscaCamera({
            ip: t.cameraIp,
            port: t.viscaPort,
            units: t.units,
            limits: t.limits,
          })
        : new SimCamera(t.limits, t.units);
    d.start();
    console.log(`[tracker] camera driver: ${kind}`);
    return d;
  };
  const swapDriver = (kind: "sim" | "visca"): void => {
    driver?.stop();
    driver = makeDriver(kind);
    // The RTSP pull is only useful (and reachable) with the real camera.
    if (kind === "visca") {
      video.start();
      mse.start();
    } else {
      video.stop();
      mse.stop();
    }
  };

  const upstream: Upstream = REPLAY
    ? new ReplayUpstream(REPLAY, {
        onSnapshot: (now, aircraft) => loop.onSnapshot(now, aircraft),
      })
    : new WsUpstream(SERVER_WS, {
        onSnapshot: (now, aircraft) => loop.onSnapshot(now, aircraft),
        onConfig: (config) => {
          loop.onConfig(config);
          // Vision/MJPEG runs on the substream; the TV gets the main stream
          // remuxed untouched.
          video.setUrl(rtspUrl(config.tracker.rtspSubUrl, config.tracker.cameraIp));
          mse.setUrl(rtspUrl(config.tracker.rtspUrl, config.tracker.cameraIp));
          videoRec.setUrl(rtspUrl(config.tracker.rtspUrl, config.tracker.cameraIp));
          hub.broadcastConfig();
        },
      });

  const initial = upstream.getConfig().tracker;
  const video = new VideoStream(rtspUrl(initial.rtspSubUrl, initial.cameraIp));
  const mse = new Mp4Stream(rtspUrl(initial.rtspUrl, initial.cameraIp));
  const videoRec = new VideoRecorder(RECORDINGS_DIR, rtspUrl(initial.rtspUrl, initial.cameraIp));

  const loop = new ControlLoop(
    upstream,
    getDriver,
    recorder,
    video,
    swapDriver,
    () => mse.status(),
    () => videoRec.status(),
    (cx, cy, t) => videoRec.noteDetection(cx, cy, t),
  );

  const hub = new TrackerHub(loop, upstream, recorder, video, videoRec);

  upstream.start();
  getDriver(); // instantiate per current config
  loop.start();
  // Only pull RTSP when we're actually driving the real camera.
  if (initial.driver === "visca") {
    video.start();
    mse.start();
  }
  mse.attach();
  // Single upgrade router: two `ws` servers attached to the same HTTP server
  // 400-reject each other's paths, so dispatch manually.
  hub.server.on("upgrade", (req, socket, head) => {
    const path = (req.url ?? "").split("?")[0];
    if (path === "/tracker-ws") hub.handleUpgrade(req, socket, head);
    else if (path === "/video-ws") mse.handleUpgrade(req, socket, head);
    else socket.destroy();
  });
  hub.start(PORT, HOST);

  if (REPLAY) console.log(`[tracker] REPLAY mode: ${REPLAY}`);
  console.log(`[tracker] upstream: ${REPLAY ?? SERVER_WS}`);

  // Graceful shutdown: the camera keeps executing its last velocity drive
  // after this process dies — without an explicit stop it grinds the ±175°
  // hard stop until the next process happens to command it (observed across
  // the many dev restarts). stop() on the driver sends motor stops.
  let shuttingDown = false;
  const shutdown = (signal: string): void => {
    if (shuttingDown) return;
    shuttingDown = true;
    console.log(`[tracker] ${signal} — stopping motors and exiting`);
    loop.stop();
    videoRec.stop(); // finalize an in-progress clip
    try {
      driver?.stop();
    } catch {
      /* best effort — exiting anyway */
    }
    // Leave time for the stop datagrams to flush.
    setTimeout(() => process.exit(0), 250);
  };
  process.on("SIGTERM", () => shutdown("SIGTERM"));
  process.on("SIGINT", () => shutdown("SIGINT"));
}

main().catch((err) => {
  console.error("[tracker] fatal:", err);
  process.exit(1);
});
