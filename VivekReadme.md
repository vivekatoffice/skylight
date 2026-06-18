# Skylight — Developer Setup Guide (Windows)

This guide outlines the steps to set up and run the Skylight project on Windows.

---

## 🛠️ Prerequisites

- **Node.js** >= 20 installed
- **pnpm** installed globally (if not, it will be installed in step 1)

---

## 🚀 Step-by-Step Setup

### 1. Install pnpm (if not already installed)
```bash
npm install -g pnpm
```
> [!NOTE]
> If `pnpm` is not recognized after install, use `npx pnpm` as a prefix for all pnpm commands below.

### 2. Install Dependencies
```bash
npx pnpm install
```

### 3. Approve Build Scripts (First Time Only)
After the first install, pnpm may block build scripts for `onnxruntime-node` and `sharp`. Add them to `pnpm-workspace.yaml` under `onlyBuiltDependencies`:
```yaml
onlyBuiltDependencies:
  - esbuild
  - onnxruntime-node
  - sharp
```
Then re-run install:
```bash
npx pnpm install
```

### 4. Run the Development Servers

#### Option A: With Public API (No Radio Hardware Needed — Recommended)
Uses the free [airplanes.live](https://airplanes.live/) API for live aircraft data:

**PowerShell:**
```powershell
$env:DATA_SOURCE="api"; npx pnpm dev
```

**CMD / Git Bash:**
```bash
set DATA_SOURCE=api && npx pnpm dev
# or (Git Bash)
DATA_SOURCE=api npx pnpm dev
```

#### Option B: With Local RTL-SDR Radio
Requires a dump1090/readsb feed running at `http://localhost:8080/data/aircraft.json`:
```bash
npx pnpm dev
```

---

## 🌐 URLs (after `pnpm dev`)

| Page | URL | Description |
|------|-----|-------------|
| **Display** | http://localhost:5173/ | Main ceiling projection view |
| **Control Panel** | http://localhost:5173/control.html | Phone-friendly settings UI |
| **Tracker Debug** | http://localhost:5173/tracker.html | Camera tracker debug UI (uses simulator) |
| **TV Dashboard** | http://localhost:5173/tv.html | Live feed + radar inset |

---

## 📍 Set Your Location (e.g., Bangalore)

Edit `server/data/config.json` (create it if it doesn't exist):
```json
{
  "centerLat": 12.9716,
  "centerLon": 77.5946,
  "locationName": "Bangalore",
  "radiusMiles": 25,
  "showAirport": false
}
```
Then restart the dev server. You can also change the location live from the **Control Panel** → **Location** section.

> [!TIP]
> Increase `radiusMiles` (e.g., 25) if the airport is far from your city center.

---

## 🔍 Troubleshooting

### `pnpm` not recognized
Use `npx pnpm` instead, or refresh your terminal after `npm install -g pnpm`.

### HTTP 429 (Too Many Requests)
The free airplanes.live API has rate limits. The server backs off and recovers automatically — aircraft will appear within a few minutes.

### WebSocket errors in browser console
These are harmless during startup. They resolve once the server starts streaming data.

### Docker build fails with DNS errors
If Docker can't reach `registry-1.docker.io`, it's a network/proxy issue. Use the local dev setup (Option A above) instead.

---

## 📂 Project Structure

| Directory | Purpose |
|-----------|---------|
| `server/` | Backend — polls radio/API, enriches aircraft, proxies TLEs, WebSocket |
| `web/` | Frontend — Vite + React, display/control/tracker/TV pages |
| `tracker/` | Camera brain — PTZ target selection, vision, zoom |
| `shared/` | TypeScript types, config schema, geo/projection math |

---

## ⚙️ Key Environment Variables

| Env | Default | Meaning |
|-----|---------|---------|
| `DATA_SOURCE` | `radio` | `radio` (dump1090) or `api` (airplanes.live) |
| `AIRCRAFT_JSON_URL` | `http://localhost:8080/aircraft.json` | dump1090 feed URL |
| `PORT` | `3000` | Server HTTP + WebSocket port |

