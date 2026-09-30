<img width="1344" height="768" alt="featured_image" src="https://github.com/user-attachments/assets/8731b75a-c52e-4fee-878f-f93b4d73a4cf" />
# Cursor Buddy v1

Desktop **agent pager** for Cursor: a small service on macOS, Windows, or Linux watches agent lifecycle via **user-level Cursor hooks**, and a **Waveshare ESP32-S3-Touch-LCD-2.1** shows status over LAN Wi‑Fi (WebSocket), with buzzer + touch mute/dismiss.

```
Cursor → ~/.cursor hooks → Buddy Service → WebSocket → Waveshare ESP32
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

## Setup

Install **Python 3.12+**, clone this repo, then follow the section for your OS. Leave the service running. It listens on `0.0.0.0:8787`. Cursor posts events to localhost; the ESP32 connects to your LAN IP on the same port. `POST /ingest` is localhost-only.

Hooks are fail-open and return immediately, so a stopped Buddy service does not stall the agent. Cursor reloads `~/.cursor/hooks.json` on its own.

### macOS

```bash
python3 --version   # 3.12 or newer
./scripts/run-service.sh
```

In a second terminal:

```bash
./scripts/install-hooks.sh
curl -s http://127.0.0.1:8787/health
tail -f ~/Library/Logs/cursor-buddy/hooks.jsonl
ipconfig getifaddr en0   # LAN IP, if the puck setup page needs one
```

AskQuestion is read from `~/Library/Application Support/Cursor/logs`.

### Windows

Install Python from [python.org](https://www.python.org/downloads/windows/) and enable **Add python.exe to PATH**. In PowerShell, from the repo root:

```powershell
python --version   # 3.12 or newer
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\scripts\run-service.ps1
```

The execution-policy line lasts for that window only. In a second PowerShell window:

```powershell
python .\scripts\install_hooks.py
curl.exe -s http://127.0.0.1:8787/health
Get-Content "$env:LOCALAPPDATA\cursor-buddy\hooks.jsonl" -Wait
ipconfig   # IPv4 address, if the puck setup page needs one
```

If `python` opens the Microsoft Store instead of running, use the `py -3.12` launcher in both commands. AskQuestion is read from `%APPDATA%\Cursor\logs`.

### Linux

Debian and Ubuntu need the venv package once: `sudo apt install python3 python3-venv`. Python must be 3.12 or newer.

```bash
python3 --version
./scripts/run-service.sh
```

In a second terminal:

```bash
./scripts/install-hooks.sh
curl -s http://127.0.0.1:8787/health
tail -f "${XDG_STATE_HOME:-$HOME/.local/state}/cursor-buddy/hooks.jsonl"
hostname -I   # LAN IP, if the puck setup page needs one
```

AskQuestion is read from `${XDG_CONFIG_HOME:-~/.config}/Cursor/logs`.

### Paths

`GET /health` reports the resolved `hook_log` and `cursor_log_root`. Override them when Cursor's data lives somewhere else:

| Variable | Effect |
|----------|--------|
| `BUDDY_LOG_DIR` | Directory that receives `hooks.jsonl` |
| `BUDDY_CURSOR_LOG_ROOT` | Cursor `logs` directory |
| `BUDDY_CURSOR_USER_DIR` | Cursor user-data directory; logs default to `<dir>/logs` |

### Check it without the ESP32

Open [http://127.0.0.1:8787/](http://127.0.0.1:8787/). The page uses the same `/ws` snapshot stream as the device. From there you can dismiss sessions, mute alerts, **Clear** old session data, or drive states with **Load 3 demos**.

From the shell:

```bash
curl -s -X POST http://127.0.0.1:8787/ingest \
  -H 'content-type: application/json' \
  -d '{"hook_event_name":"beforeSubmitPrompt","conversation_id":"demo","_buddy_project":"SCOT"}'

curl -s -X POST http://127.0.0.1:8787/clear
```

On Windows, call `curl.exe` the same way. Pretty-print a snapshot with `python -m json.tool` (`python3` on macOS and Linux):

```bash
curl -s http://127.0.0.1:8787/snapshot | python3 -m json.tool
```

Capture one hook payload without posting it. macOS and Linux:

```bash
echo '{"hook_event_name":"stop","status":"completed","conversation_id":"test"}' | ./scripts/capture-hooks.sh
```

Windows:

```powershell
$env:BUDDY_CAPTURE_ONLY = "1"
'{"hook_event_name":"stop","status":"completed","conversation_id":"test"}' | python .\hooks\forward.py
```

## ESP32 firmware (Waveshare ESP32-S3-Touch-LCD-2.1)

Case wiring for power, the LiPo, and the four header switches is in [docs/wiring.md](docs/wiring.md).

Requirements: [PlatformIO](https://platformio.org/) CLI or IDE.

```bash
cd firmware
cp include/config.h.example include/config.h
pio run -t upload
pio device monitor
```

Wi-Fi, the Buddy service host, the name on the puck, and the clock and weather options are stored on the device. `config.h` is only a first-boot copy: fill it in and that first boot saves it onto the puck, or leave the placeholders and set everything from a phone.

With nothing saved, or when you ask, the puck opens an unsecured Wi-Fi network named `Buddy-` plus four characters from its MAC address. The screen shows that name. Join it, and open [http://192.168.4.1](http://192.168.4.1) if the setup page does not appear on its own. Enter the Wi-Fi network and the computer running Buddy. The service host can be an IP address or a hostname such as `macbook.local`, so a new DHCP lease does not require a reflash. Then reconnect the phone to the normal network.

Open that page again from the clock's **SETUP** button, or by holding Mute and Dismiss together for two seconds. Holding those two while the puck turns on does the same thing, which still works if the touch screen does not.

Once the puck has saved its settings, putting the placeholders back into `config.h` keeps later flashes from writing a password into the firmware. The copy already on the puck stays.

**Display note:** Panel bring-up varies by Waveshare demo revision. If the screen stays blank, replace the bus/panel init in `src/ui.cpp` with the exact init from Waveshare’s official Arduino example for **ESP32-S3-Touch-LCD-2.1**, keeping `drawHero()` / touch button regions. Buzzer default pin is `42` (`BUDDY_BUZZER_PIN`).

Touch: MUTE / UNMUTE and DISMISS send WebSocket `ack` messages back to the service.

With no active sessions, and while the screen is awake, the puck shows the local time and the current weather. That lookup needs outbound internet. Location follows the public IP address unless a timezone and coordinates were saved on the setup page.

On battery, the display sleeps after 30 minutes with no touch or button press. Change that on the setup page. Touch the screen or any of the four header buttons to wake it; Wi-Fi reconnects and the last view is drawn again. While USB is powering the board it stays awake. This board has no USB-present pin, so that decision follows the battery voltage on GPIO4: sleep runs only after the pack has been discharging. Alerts that arrive during sleep show up on the next wake.

## Tests

macOS and Linux, from the repo root:

```bash
cd service
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt pytest
.venv/bin/pytest tests -q
```

Windows:

```powershell
cd service
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt pytest
.\.venv\Scripts\python.exe -m pytest tests -q
```

## Layout

```
cursor-buddy/
  service/buddy/     Desktop service (FastAPI + WebSocket)
  service/buddy/static/  Local browser receiver
  hooks/             Cursor hook forwarder
  firmware/          ESP32 PlatformIO project
  scripts/           install-hooks, run-service, capture-hooks
  docs/protocol.md   Device protocol
  docs/wiring.md     Case wiring (power, battery, switches)
```

## Out of scope (v1)

- BLE pairing
- MQTT / off-LAN relay
- Approving Cursor tools from the device
- Relying on subagent hooks for correctness

## Refine ATTENTION vs COMPLETE

After collecting `hooks.jsonl` from real sessions, adjust `BuddyState.apply_hook` in `service/buddy/state.py` if Cursor’s payloads distinguish “permission dialog” from “turn complete” more cleanly than docs suggest.
