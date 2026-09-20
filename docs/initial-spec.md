Yes — and that specific board is much better suited to this project than I realized. I checked Waveshare's current documentation.

The Waveshare ESP32-S3-Touch-LCD-2.1 documentation confirms that your board already gives us:

2.1" 480×480 color display
Capacitive touchscreen
Bluetooth 5 LE
2.4 GHz Wi-Fi
Built-in buzzer
16 MB Flash + 8 MB PSRAM
Battery connector and charging circuitry
RTC
6-axis IMU
microSD slot
USB-C/programming interfaces

The built-in buzzer is especially nice—I had assumed we'd need to add one. Waveshare even provides a buzzer-control example in its ESP-IDF demo.

So you already have everything required for Cursor Buddy v1. No additional electronics are necessary.

I would make the first version look something like this
       ╭────────────────────────╮
      ╱                          ╲
     │       CURSOR BUDDY         │
     │                            │
     │            ◉               │
     │                            │
     │         WORKING            │
     │                            │
     │      SCOT • 04:32          │
     │                            │
     │    1 Agent Running         │
      ╲                          ╱
       ╰────────────────────────╯

Then when Cursor stops because it needs you:

       ╭────────────────────────╮
      ╱                          ╲
     │       NEEDS ATTENTION      │
     │                            │
     │            !               │
     │                            │
     │          SCOT              │
     │                            │
     │     Agent needs input      │
     │                            │
     │        [ MUTE ]            │
      ╲                          ╱
       ╰────────────────────────╯

The Mute/Acknowledge button can be on-screen, since you have capacitive touch. That means no physical controls are necessary either.

I'd use the buzzer differently for each event: a short double-beep for attention, pleasant ascending tone for completion, and a distinctive lower tone for error. The screen can also change dramatically enough that you can recognize the state from across the room.

One architectural change I'd make

Because this board has both Wi-Fi and BLE, I wouldn't commit ourselves to Bluetooth quite yet.

For a prototype, Wi-Fi may actually be considerably easier:

Cursor
   ↓
Buddy Mac Service
   ↓
Local Wi-Fi
   ↓
ESP32 Buddy

The Mac service could expose a tiny WebSocket connection to the ESP32. That gives us bidirectional, persistent communication and makes sending richer data trivial:

{
  "source": "cursor",
  "project": "SCOT",
  "session": "Agent 1",
  "state": "attention",
  "elapsed": 272,
  "message": "Permission required"
}

BLE remains attractive for a finished appliance because pairing it directly with the computer makes the product independent of network configuration. But Wi-Fi/WebSocket could get the proof of concept running faster, and the ESP32 could eventually support both.

There is also a nice benefit from the board's battery support: eventually this can become a completely wireless little desktop puck. Charge over USB-C, put it wherever you want, and have it wake/display/chime when an agent wants you.

I think we now know enough about the hardware. The next thing I'd investigate is Cursor itself: exactly what current Cursor hooks/events we can capture on your Mac for agent started → working → needs attention → completed → failed. That's the one remaining technical uncertainty. Once we've established that, we can build the first working version around the Waveshare board.

that is a good point, I had not thought of that but you are correct the wifi might be more practical and would not limit me to close proximity to the PC

Exactly. For this device, I'd now make Wi-Fi the primary transport and treat Bluetooth as an optional future provisioning/fallback mechanism.

Wi-Fi changes the product from a "wireless indicator sitting beside the computer" into a genuine agent pager. You could leave the Waveshare unit in your office, kitchen, workshop, or elsewhere on the same network while Cursor continues working on your Mac.

More importantly, the ESP32 doesn't need a direct relationship with Cursor. I'd structure it as:

Cursor Agent(s)
      │
      ▼
┌──────────────────────┐
│ Buddy Service        │  ← small process on Mac
│                      │
│ Detects agent state  │
│ Tracks sessions      │
│ Normalizes events    │
└──────────┬───────────┘
           │
        WebSocket
           │ Wi-Fi
           ▼
┌──────────────────────┐
│ Waveshare Buddy      │
│                      │
│  ● SCOT    Working   │
│  ! Aiden   Attention │
│  ✓ Regis   Complete  │
│                      │
│       🔊             │
└──────────────────────┘

I'd use WebSockets rather than polling. The ESP32 establishes one persistent connection to the Buddy Service. When anything changes, the Mac pushes an event immediately. Bandwidth and CPU requirements are essentially trivial.

There are several nice consequences to this architecture. The Mac service can track multiple Cursor windows/projects simultaneously, the ESP32 can reconnect automatically if Wi-Fi drops, and we can eventually have multiple Buddy displays listening to the same service. It also gives us room to support Claude Code, Codex, Gemini CLI, or other agents without changing the hardware firmware much.

For the protocol, I'd start slightly richer than the bare minimum:

{
  "version": 1,
  "source": "cursor",
  "session_id": "a82f91",
  "project": "SCOT",
  "state": "attention",
  "message": "Agent waiting for user",
  "elapsed_seconds": 384,
  "timestamp": "2026-09-20T15:42:18-06:00"
}

The ESP32 shouldn't interpret Cursor-specific behavior. It only understands standardized states such as:

IDLE → WORKING → ATTENTION → COMPLETE → ERROR

That separation is important. We could later add Claude Code support entirely on the Mac without reflashing the Buddy.

And your touchscreen suddenly becomes genuinely useful. For example, tapping a completed session could dismiss it; tapping the speaker could mute alerts; swiping could move between agents; and the main screen could show something like:

        AGENT BUDDY
    ─────────────────

    SCOT
    ● Working       12:43

    Aiden
    ⚠ NEEDS ATTENTION

    Tomaz
    ✓ Completed      3m

    ─────────────────
       🔊 ON    3 agents

I would not initially let the ESP32 approve Cursor actions. For v1, I'd make it strictly observational: monitor, notify, acknowledge/dismiss. Once we've proven that reliable, we could consider carefully constrained remote actions.

One other benefit: if we eventually want Buddy to work outside your home/office network, the architecture can evolve to MQTT or a small authenticated relay. Then you could literally take the device elsewhere and have it notify you that an agent running back at your office needs attention. I wouldn't build that complexity into v1, though.

So I think our prototype plan is becoming quite clean: Waveshare ESP32-S3-Touch-LCD-2.1 + Wi-Fi + WebSocket + lightweight macOS Buddy Service + Cursor event integration.

The next piece I'd tackle is the critical one: research the current Cursor hooks/API and determine exactly how reliably we can detect those five states. That's what will tell us whether this is a straightforward build or whether we need a clever workaround for "needs attention."

OK, proceed with that research

I dug through Cursor's current documentation and recent issue reports. The project is viable, and Cursor now exposes enough hooks that we do not need to screen-scrape Cursor or monitor its UI. The one wrinkle is detecting every kind of “needs attention” event.

What Cursor gives us

Cursor's current Hooks system is designed specifically to let external scripts observe the agent loop. Hooks are ordinary processes that receive JSON over stdin, so our hook can simply forward events to the Buddy service running on your Mac.

For Buddy, these are the important events:

What we want	Cursor event	Confidence
New agent/session	sessionStart	Excellent
User starts agent work	beforeSubmitPrompt	Excellent
Agent actively doing things	preToolUse, afterAgentThought, etc.	Excellent
Tool failed	postToolUseFailure	Excellent
Agent finishes a turn	stop	Excellent
Agent returns final response	afterAgentResponse	Excellent
Session closes/errors	sessionEnd	Excellent
Permission/input required	indirect	Needs work

Cursor explicitly says stop fires when the agent loop ends and supplies a status of completed, aborted, or error. sessionEnd provides even richer information, including completion/error reason and duration.

So DONE and ERROR are easy.

Starting and Working are also easy

When you create a new Composer conversation, sessionStart gives us a unique session_id, whether it is a background agent, and the Composer mode.

When you actually submit work, beforeSubmitPrompt fires immediately after you hit Send and before Cursor sends the request to the backend.

That gives us a very clean state transition:

Cursor opened
     │
     ▼
sessionStart
     │
     ▼
    IDLE
     │
     │ beforeSubmitPrompt
     ▼
   WORKING
     │
     ├── tool calls
     ├── thinking
     ├── file edits
     └── subagents

We don't have to infer whether the agent is running from CPU usage, processes, files changing, or anything hacky.

The interesting problem: NEEDS ATTENTION

This is the only place where Cursor doesn't currently give us exactly the hook I'd like.

Cursor's Claude Code compatibility documentation is revealing: Claude has Notification and PermissionRequest events, but Cursor explicitly lists those two events as not supported in its Claude-hook mapping.

Cursor's own preToolUse hook does support allow and deny, but although "ask" exists in the schema, Cursor says it isn't currently enforced there.

There is one useful signal, however. postToolUseFailure fires if a tool fails, times out or is denied, and Cursor tells us which occurred:

failure_type:
    error
    timeout
    permission_denied

So we can catch some permission-related events, but that isn't equivalent to being notified the instant Cursor puts a permission dialog on the screen.

I don't think that's a showstopper

For the first prototype, I'd define attention more broadly:

WORKING
   │
   ├── stop(completed)
   │       ↓
   │     DONE 🔔
   │
   ├── stop(error)
   │       ↓
   │     ERROR 🔔
   │
   ├── postToolUseFailure(permission_denied)
   │       ↓
   │   ATTENTION 🔔
   │
   └── Agent response / stops waiting
           ↓
       ATTENTION 🔔

In practice, what you care about isn't necessarily distinguishing:

"Cursor needs permission."

from:

"Cursor finished and needs Walt to look at it."

Both mean the same thing to the physical device:

Come back to the computer.

That's actually helpful because it simplifies Buddy.

Multiple Cursor agents look feasible too

Cursor supplies unique session IDs, and subagents have their own IDs plus a parent_conversation_id. Cursor also exposes subagentStart and subagentStop, including whether they're parallel workers.

So the Mac service can maintain something like:

Buddy Service

SESSION             PROJECT       STATE

a82f91              SCOT          WORKING
c93072              Aiden         ATTENTION
f18231              Regis         DONE

There is one current caveat worth knowing: a recent Cursor community bug report found cases where subagentStart/subagentStop stopped firing until Cursor was restarted, and background subagentStop had an acknowledged issue. That doesn't undermine our primary-agent monitoring, but I would not make v1 dependent upon subagent lifecycle hooks.

I would change one thing from our earlier architecture

We don't actually need a sophisticated Cursor-specific application.

I'd make the Mac side extremely small:

                 CURSOR

 sessionStart ──────────────┐
 beforeSubmitPrompt ────────┤
 preToolUse ────────────────┤
 postToolUseFailure ────────┤
 stop ──────────────────────┤
 sessionEnd ────────────────┤
                            ▼
                   ┌─────────────────┐
                   │ Buddy Service   │
                   │                 │
                   │ State manager   │
                   │ WebSocket server│
                   └────────┬────────┘
                            │
                         Wi-Fi
                            │
                            ▼
                  ┌────────────────────┐
                  │ Waveshare ESP32-S3 │
                  │ Touch LCD 2.1      │
                  │                    │
                  │    ⚠ ATTENTION     │
                  │                    │
                  │       SCOT         │
                  │                    │
                  │      18:43         │
                  │                    │
                  │     [ DISMISS ]    │
                  └────────────────────┘

Cursor hooks can be installed at the user level in ~/.cursor/hooks.json, rather than individually in every project's repository. That's ideal for your use case because Buddy should follow you, not a particular codebase. Cursor automatically watches the hooks file and reloads it when changed.

That means SCOT, Aiden, Regis, Tomaz or any new project automatically participates.

Even better: Cursor tells us the project

Every hook process receives the CURSOR_PROJECT_DIR environment variable containing the workspace root. Cursor can also provide CURSOR_TRANSCRIPT_PATH when transcripts are enabled.

So if you're working in:

/Users/walt/projects/scot

we can automatically turn that into:

SCOT
● WORKING
12:43

No configuration per project.

My recommended v1

I think we have enough information now to build it.

I'd keep the first implementation to four states rather than five:

● WORKING
Agent is actively executing

! ATTENTION
Agent has stopped and Walt needs to return

✓ COMPLETE
Agent completed normally

✕ ERROR
Agent stopped because of an error

IDLE simply becomes the default screen when nothing is happening.

For the first version, I would make any completed agent turn generate an attention chime. The distinction between COMPLETE and ATTENTION can be refined later once we see exactly what Cursor emits in real-world sessions.

And I'd build it in this order:

Cursor hook logger — install user-level hooks and record the actual JSON Cursor generates while you use Cursor normally.
Buddy Mac service — consume those events and build our session state machine.
WebSocket endpoint — publish state over your LAN.
ESP32 firmware — connect the Waveshare to Wi-Fi/WebSocket and display states.
Buzzer + touchscreen — alerts, mute and dismiss.
Run real Cursor sessions and refine how we classify ATTENTION.

The first step is particularly important. Rather than guessing from the documentation, we should instrument your Cursor installation and capture exactly what it emits when you start an agent, let it run, encounter a permission prompt, finish successfully, and encounter an error.

That's a very small piece of software, and it will give us the empirical information needed to design the rest correctly.
