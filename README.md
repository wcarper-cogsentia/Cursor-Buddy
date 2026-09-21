# Cursor Buddy v1

Desktop **agent pager** for Cursor: a small Mac service watches agent lifecycle via **user-level Cursor hooks**, and a **Waveshare ESP32-S3-Touch-LCD-2.1** shows status over LAN Wi‑Fi (WebSocket), with buzzer + touch mute/dismiss.

```
Cursor → ~/.cursor hooks → Buddy Service (Mac) → WebSocket → Waveshare ESP32
```

V1 is **observational only** (no remote approve/deny).

## States

| State | Meaning |
|-------|---------|
| WORKING | Agent actively executing |
| ATTENTION | Come back (e.g. permission denied) |
| COMPLETE | Turn finished successfully (v1 also chimes — refine later) |
| ERROR | Agent stopped with error |
| IDLE / OFFLINE | Nothing active / device not connected |

See [docs/protocol.md](docs/protocol.md) for the JSON protocol.

## Quick start (Mac)

### 1. Run the Buddy service

```bash
./scripts/run-service.sh
```

Listens on `0.0.0.0:8787` (WebSocket `/ws`). Ingest `POST /ingest` is **localhost-only**.

Open the **local receiver** in a browser to watch every session live:

[http://127.0.0.1:8787/](http://127.0.0.1:8787/)

It connects to the same `/ws` snapshot stream the ESP32 will use, and can dismiss sessions, mute alerts, or **Clear** all old session data. On localhost it also exposes a small simulator so you can drive states without Cursor. To reset from the shell:

```bash
curl -s -X POST http://127.0.0.1:8787/clear
```

Health check:

```bash
curl -s http://127.0.0.1:8787/health
```

### 2. Install user-level Cursor hooks

```bash
./scripts/install-hooks.sh
```

This merges Buddy forwarders into `~/.cursor/hooks.json` and symlinks the fire-and-forget shell wrapper. Cursor reloads hooks automatically.

Hooks are **fail-open** and return immediately: ingest happens in the background so agents are not stalled. Events are always appended to:

`~/Library/Logs/cursor-buddy/hooks.jsonl`

### 3. Phase 0 — capture real payloads

Use Cursor normally (start agent, tools, permission prompts, success, error). Inspect:

```bash
tail -f ~/Library/Logs/cursor-buddy/hooks.jsonl
```

Capture-only (no ingest) for a single invocation:

```bash
echo '{"hook_event_name":"stop","status":"completed","conversation_id":"test"}' | ./scripts/capture-hooks.sh
```

### 4. Dev without the ESP32

Keep the service running and open [http://127.0.0.1:8787/](http://127.0.0.1:8787/). Use **Load 3 demos** or the state buttons, or ingest from the shell:

```bash
# Simulate working
curl -s -X POST http://127.0.0.1:8787/ingest \
  -H 'content-type: application/json' \
  -d '{"hook_event_name":"beforeSubmitPrompt","conversation_id":"demo","_buddy_project":"SCOT"}'

curl -s http://127.0.0.1:8787/snapshot | python3 -m json.tool
```

## ESP32 firmware (Waveshare ESP32-S3-Touch-LCD-2.1)

Requirements: [PlatformIO](https://platformio.org/) CLI or IDE.

```bash
cd firmware
cp include/config.h.example include/config.h
# Edit WIFI SSID/password and BUDDY_HOST (your Mac LAN IP)
pio run -t upload
pio device monitor
```

**Display note:** Panel bring-up varies by Waveshare demo revision. If the screen stays blank, replace the bus/panel init in `src/ui.cpp` with the exact init from Waveshare’s official Arduino example for **ESP32-S3-Touch-LCD-2.1**, keeping `drawHero()` / touch button regions. Buzzer default pin is `42` (`BUDDY_BUZZER_PIN`).

Touch: MUTE / UNMUTE and DISMISS send WebSocket `ack` messages back to the service.

## Tests

```bash
cd service
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt pytest
cd ..
PYTHONPATH=service .venv/bin/pytest service/tests -q
# or from service/:
cd service && .venv/bin/pytest tests -q
```

## Layout

```
cursor-buddy/
  service/buddy/     Mac service (FastAPI + WebSocket)
  service/buddy/static/  Local browser receiver
  hooks/             Cursor hook forwarder
  firmware/          ESP32 PlatformIO project
  scripts/           install-hooks, run-service, capture-hooks
  docs/protocol.md   Device protocol
```

## Out of scope (v1)

- BLE pairing
- MQTT / off-LAN relay
- Approving Cursor tools from the device
- Relying on subagent hooks for correctness

## Refine ATTENTION vs COMPLETE

After collecting `hooks.jsonl` from real sessions, adjust `BuddyState.apply_hook` in `service/buddy/state.py` if Cursor’s payloads distinguish “permission dialog” from “turn complete” more cleanly than docs suggest.
