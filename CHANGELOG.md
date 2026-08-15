# Changelog

Durable record of completed work on the Wi-Fi WOL feature fork, newest
first. Entries move here from `TASK.md` once done — this is where "what
happened and when" lives; `TASK.md` only tracks what's still open.
Architecture/known-issue knowledge that should inform future work lives in
`DECISIONS.md` instead, not here.

---

## 2026-08-15 — Merged upstream v1.7.0, resolved the WOL/radial-deadzone `COMMAND_ID` collision, rebuilt PR #120 as a clean 2-commit history

Merged `upstream/main` (v1.7.0: Edge persona, radial deadzones, Kitsune
Input promotion, persona mic fixes) into `feature/wol-wifi`. Two real
conflicts, both from independent `COMMAND_ID` additions at `0x37` — see
`DECISIONS.md`'s new entry for the fix (WOL commands renumbered to
`0x46`-`0x49`, `PROTOCOL_MINOR` bumped to 23) and the general lesson for
future upstream syncs. Verified: companion build + 320/320 vitest tests,
all 3 firmware host-side test suites, and a real Waveshare board build all
pass (`ds5-bridge.uf2` produced).

Separately rebuilt PR #120 (open against `SundayMoments/DS5_Bridge`,
`port-dev`) as a clean 2-commit history (firmware core, then protocol/
companion wiring) on top of the same v1.7.0 base, dropping this fork's
internal tracking docs (`AGENTS.md`/`CHANGELOG.md`/`DECISIONS.md`/
`TASK.md`) and the personal local-dev packaging tweak
(`package:win:local`) — neither belongs in an upstream PR. Force-pushed to
`origin/feature/wol-wifi`; PR #120 now shows `MERGEABLE`. Also stripped
`Co-Authored-By: Claude Sonnet 5` trailers that had accumulated on ~50
commits during iterative development — the squash incidentally cleared
these along with the history cleanup. PR description updated to match
(dropped stale `CHANGELOG.md`/`DECISIONS.md`-inclusion claim, corrected
test count, resolved the old "want a squash?" note).

**Post-merge trap hit and fixed:** after reflashing firmware with the new
protocol version, the companion app kept showing "Update required" because
the installed/packaged companion `.exe` builds
(`companion/artifacts/installer/win-unpacked/`, `C:\game\DS5 Bridge App`)
were several days stale — rebuilding firmware and rebuilding the packaged
companion app are two separate steps, and only the firmware side had been
redone. Also hit the same known Windows `objdump`/`picotool` Access
violation crash (see "Local build environment notes" in `AGENTS.md`) in a
second location — `tools/build-pico-universal-flash-nuke.ps1`, which
`npm run installer:win`/`package:win`/`package:win:local` all depend on —
worked around the same way. See `AGENTS.md`'s updated notes for the full
procedure now that it's been hit twice in two different build targets.

## 2026-08-10 — Release prep: stripped all debug/diagnostic scaffolding, kept only the shippable WOL feature

With WOL confirmed working end-to-end on real hardware (previous entry),
removed everything that existed only to debug it: the `WolTraceStage`
board-level trace ring buffer (`bt.h`/`bt.cpp`, all
`bt_append_wol_trace_event()` call sites in `bt.cpp`/`main.cpp`/
`wolwifi.cpp`), `usb_host_active_debug_bits()` (`usb.h`/`usb.cpp`, keeping
`usb_host_active()` itself since `ObserveHost` depends on it in
production), the `WOL_DEBUG_STATUS` report and
`wolwifi_debug_ping()`/`wolwifi_debug_send_wol()`/`wolwifi_debug_status()`
(`wolwifi.h`/`wolwifi.cpp`), and the `TRIGGER_WOL_DEBUG_PING`/`_SEND`
commands and handlers (`companion.cpp`). Two functions shared between
debug and production paths were edited rather than deleted:
`arp_snoop_input()` lost its debug-ping branch but kept the real
resend-confirmation ARP watch; `send_magic_packet_now()` dropped its
`is_debug_action` parameter down to a no-arg production-only send. On the
companion side, removed the "Debug Target" UI row (Ping/WOL Test buttons,
Debug Log path) and the already-dead `wolDebugStatusText()` helper from
`App.tsx`, the WOL debug status/trace read-and-log machinery from
`bridge-service.ts`, the two debug IPC channels from `main.ts`/
`preload.ts`, and all matching constants/types/report IDs from
`protocol.ts`/`types.ts`. `REPORT_ID.WOL_TRACE`/`WOL_DEBUG_STATUS`
(`0x0B`/`0x0C`) were deleted outright rather than renumbering the
still-used `DEVICE_IDENTITY`/`FIRMWARE_LOG` entries that follow them, per
explicit instruction to touch the fewest possible unrelated things;
`TRIGGER_WOL_DEBUG_PING`/`_SEND` were the trailing `COMMAND_ID` entries on
both sides and removed cleanly. `PROTOCOL_MINOR` bumped 20→21
(`protocol.ts` and `companion.cpp`'s `kProtocolMinor`). See `DECISIONS.md`
for the full writeup including why removal (not keeping it as permanent
tooling) was chosen.

Verified: both firmware targets (Waveshare/`ENABLE_WOLWIFI` and the
default non-WOL board) rebuilt clean with zero leftover debug/trace
symbols in either `.elf` (confirmed via `arm-none-eabi-nm`); companion
`npm run typecheck` and the full `vitest` suite (279/279) pass. What's left
is exactly the shippable "Wake-on-LAN" feature: the UI section (enable
toggle, Wi-Fi SSID/password, target MAC) and the underlying automatic-
trigger → `ObserveHost` host-alive gate → Wi-Fi connect/DHCP →
magic-packet send → resend-until-confirmed logic.

## 2026-08-10 — Host-alive gate (`ObserveHost`) confirmed working on real hardware

The new diagnostics (previous entry) paid off immediately: a real test (PC
on, companion app open) produced a clean trace confirming the gate works
exactly as designed. `observe-host-begin` (detail=2, USB not yet mounted
for the fresh session) → `conn-controller-type-identified` 22ms later
(matches the earlier 23ms measurement) → `observe-host-sample-edge`
(detail=51: mounted, not suspended, transport ready+attached) ~217ms after
`Ready` → `wol-trigger-skipped-host-active` exactly 100ms after that,
matching `WOL_OBSERVE_HOST_SUSTAIN_MS`. No Wi-Fi connect, no lightbar WOL
pulse — the user observed the ordinary controller-wake blue-flash restore,
not a WOL pulse, confirming WOL correctly never started. Whole detect-and-
skip cycle: ~340ms, well inside the 2s window. See `DECISIONS.md` for the
now-confirmed design writeup.

## 2026-08-10 — ObserveHost still not skipping WOL with the host confirmed active; deep diagnostics added

A real test with the companion app open (so USB was genuinely mounted and
active) and the target PC confirmed on still showed `wol-trigger-fired`
instead of the `ObserveHost` window aborting it — the window elapsed
without ever registering a sustained-active host, contradicting the known
test conditions. Rather than keep guessing at the state-machine logic, per
explicit instruction added deep diagnostics to see exactly what the window
observed: new `usb_host_active_debug_bits()` (`usb.cpp`/`usb.h`) exposing
the individual flags `usb_host_active()` depends on (`usb_mounted`,
`tud_inited()`, `tud_suspended()`, the suspend-debounce flag, and the
controller-transport session-scoping flags) as one bitmask, plus three new
trace stages: `ObserveHostBegin` (state at window start),
`ObserveHostSampleEdge` (every rising/falling edge of `usb_host_active()`
during the window — not every tick, to avoid flooding), and
`ObserveHostWindowElapsed` (state at the moment the window gave up). The
next real test's trace should show precisely which of `usb_host_active()`'s
component flags was false, closing the question instead of re-guessing.
Firmware/companion build clean, debug firmware rebuilt at
`build/waveshare-debug/ds5-bridge.uf2`, companion app rebuilt.

## 2026-08-10 — Host-alive gate rebuilt as an event-driven observation window

Got a real measurement from the prior change's `ConnControllerTypeIdentified`
trace event: the controller-type handshake (the thing that makes
`usb_mounted` become true for a session) completed in **23ms** with no
radio contention. Used that to design and implement the actual fix, per
the user's explicit design correction ("proper signals... events not
timers... proper investigation before coding a solution") and deep re-read
of both `awalol/DS5Dongle#207` and `DevFreezing/DS5Dongle-WoL`'s `Observe`
state pattern. Replaced the buggy instant `usb_host_active()` check with a
new `ObserveHost` mini-state-machine (`begin_observe_host()`/
`drive_observe_host()` in `wolwifi.cpp`, driven every `wolwifi_task()`
tick): defaults to firing WOL, only aborts if the host is observed active
continuously for `WOL_OBSERVE_HOST_SUSTAIN_MS` (100ms) within
`WOL_OBSERVE_HOST_WINDOW_MS` (2000ms, ~85x margin over the measured 23ms).
`proceed_with_wol_trigger()` extracted from the old
`wolwifi_on_controller_connect()` so both the `WOL_ALWAYS` immediate path
and the window-elapsed path share it. `wolwifi_wake_in_progress()` extended
to cover the new window. See `DECISIONS.md` for the full corrected design
writeup (supersedes the entry from the previous, buggy version). Firmware
compiles/links cleanly for both `WOL_ALWAYS` on and off; companion
typecheck + full test suite (280/280) pass (no companion changes needed,
firmware-only). Debug firmware rebuilt at
`build/waveshare-debug/ds5-bridge.uf2`. Not yet verified on real hardware.

## 2026-08-10 — Host-alive gate found to be checking the wrong (session-scoped) signal; measurement trace added, real fix pending

A real test (fresh boot, no reboot involved this time, USB confirmed
solidly connected to the target PC the whole time) showed the host-alive
gate still not skipping WOL despite the PC being on. Investigation (full
read of `usb.cpp`'s controller-transport lifecycle) found the real bug:
`usb_mounted` only becomes true after this firmware's own USB persona
re-enumerates for *this* controller session, which only happens after a
controller-type-identification handshake that itself only starts *after*
`wolwifi_on_controller_connect()` (the gate check) has already run. So the
gate was structurally checking a signal that cannot be true yet, regardless
of PC power state — not an environment issue, a real logic bug in last
session's design. User's correction: *"we need proper signals... events
not timers... proper investigation before coding a solution."* Re-read
`awalol/DS5Dongle#207` and `DevFreezing/DS5Dongle-WoL` in depth
specifically on this question — both use a bounded, event-driven `Observe`
state (watch live `tud_mounted() && !tud_suspended()` every tick for up to
3s, confirm only after 300ms sustained-active) rather than a point-in-time
check, and default to firing WOL unless the host is positively observed —
but their design relies on a USB persona that's *always* enumerated
(confirmed via diff inspection — neither PR touches `tud_connect`/
`tud_disconnect` logic), which doesn't hold for our session-scoped
architecture. User confirmed a bounded observation delay (only when the
host turns out to be off) is acceptable, and asked to measure real timing
before picking window/sustain values rather than guessing or copying
theirs blind. Added (this change): `WolTraceStage::ConnControllerTypeIdentified`,
appended at both controller-type-identification success sites in `bt.cpp`,
detail = elapsed ms since the `Ready` transition — measurement-only, no
behavior change. The actual `Observe`-equivalent state (new `WifiState`,
event-driven, replacing the buggy instant check) is a follow-up once real
elapsed-ms data comes back. Firmware/companion build clean, debug firmware
at `build/waveshare-debug/ds5-bridge.uf2`, companion app rebuilt.

## 2026-08-10 — Board-transport-recovery reboot: added trace coverage for a real, previously-invisible self-reboot

Retesting the host-alive gate showed it appearing to misfire (WOL fired
with the PC on) — investigation found the real cause was upstream of the
gate: the board had genuinely rebooted itself moments earlier via a
deliberate `watchdog_reboot()` call in `bt.cpp`'s disconnect/ACL-pending
recovery paths, and the board's own USB hadn't finished re-enumerating
with the PC yet by the time the (fast) BT reconnect fired the gate check —
a real race, not a gate bug. The reboot itself was invisible in the trace:
`watchdog_enable_caused_reboot()` (gating the existing `BoardWatchdogReboot`
trace stage) only recognizes reboots caused by this firmware's own
periodic `watchdog_enable()` call, not the four separate deliberate
`watchdog_reboot()` calls elsewhere — a real gap, found by reading the SDK
source rather than guessing. Added `BoardTransportRecoveryReboot` (at all
four call sites) and `ConnDisconnectRetrySent` (to show the escalation
leading up to one) trace stages. See `DECISIONS.md` for the full writeup.
Diagnostic only — *why* the recovery paths are firing during normal
operation is still open. Firmware/companion build clean. Debug firmware at
`build/waveshare-debug/ds5-bridge.uf2`.

## 2026-08-10 — Host-alive gate: skip WOL when the target PC is already on

User noticed the lightbar's WOL-in-progress pulse firing even when it
wasn't actually needed. Added a gate at the top of
`wolwifi_on_controller_connect()`: if `usb_host_active()` (new accessor in
`usb.cpp`, `usb_mounted && !usb_host_suspended_active()`) reports the
target PC already on, skip the whole WOL pipeline — no Wi-Fi connect, no
resend cycle, no lightbar pulse. Researched `awalol/DS5Dongle#207` per user
request and adopted its `WOL_ALWAYS` escape hatch design exactly (same
name, same compile-time-option mechanism) for boards/BIOS settings where
USB stays active even with the PC nominally off. New
`WolTraceStage::WolTriggerSkippedHostActive` records the skip in the board
trace. See `DECISIONS.md` for the full design writeup. Firmware compiles
cleanly for both `WOL_ALWAYS` on and off, and for the default (non-WOL)
board target. No companion-app changes needed (firmware-only, no
protocol/wire-format change). Not yet bench-tested on real hardware.

## 2026-08-10 — Core WOL feature confirmed working end-to-end on real hardware

With the fifteenth-bug fix in place: controller connects to the board while
the target PC is fully off, WOL fires immediately (Wi-Fi already connecting
from boot thanks to the fifteenth-bug fix), gets ARP-confirmed ~10s after
the trigger, the PC wakes automatically, and the controller stays connected
through the entire boot with no HCI `0x22` disconnect. This is the first
real confirmation of the complete path working end to end, across fifteen
rounds of hardware-tested bug fixes (see the entries below). Closes out
Phase 7's smoke test.

## 2026-08-10 — Fifteenth bug: connect state machine stuck in Unconfigured despite valid persisted config

The fourteenth bug's new trace instrumentation paid off immediately: the
next real test showed `wol-connect-started` (detail=1, first attempt) not
firing until ~27 real seconds after `wol-trigger-fired`, with zero events
in between. Root cause: `wolwifi_init()` always called
`enter_state(WifiState::Unconfigured)` after loading persisted config from
flash, even when a valid SSID was loaded (`g_have_ssid` true) —
`Unconfigured`'s handler in `wolwifi_task()` never re-checks `g_have_ssid`,
it's a dead end. Only the SSID/password setters ever call
`enter_state(Idle)`, so the state machine only escaped once the companion
app's slow post-connect settings-reapply sequence happened to re-send the
SSID again — the same class of race the seventh-bug flash-persistence fix
closed for the config *values*, just never closed for the state machine
itself. Fixed: `wolwifi_init()` now enters `Idle` directly when a valid
SSID was loaded. See `DECISIONS.md`'s flash-persistence entry for the full
writeup. Debug firmware and companion app (`win-unpacked`) rebuilt.
**Confirmed working on a real PC-off test** — see the entry above.

## 2026-08-10 — Fourteenth bug: trace instrumentation added for a 61s WOL-send delay (fix pending)

Real test confirmed the thirteenth-bug fix (below) works — no controller
disconnect — but WOL fired ~61 real seconds after the trigger, well after
the PC had already booted. Traced via `board_time_ms` (not wall-clock log
timestamps) to a genuine gap with zero trace events, despite
`connect_attempts` jumping by 2. Ruled out ring-buffer drops, retry-timeout
storms, and a stale `g_send_pending` directly against source. Root cause
not yet found — added trace coverage (`WolConnectStarted`,
`WolWifiLinkLostAfterConnect`, `WolWifiConnected`, `WolWifiBackoffElapsed`
in `WolTraceStage`) at four previously-silent `WifiState` transitions so
the next real test's trace explains it directly. No behavior change.
Firmware/companion build clean. Debug firmware at
`build/waveshare-debug/ds5-bridge.uf2`.

## 2026-08-10 — Thirteenth bug: deferred the post-WOL Wi-Fi leave

Root cause and fix moved to `DECISIONS.md` (the CYW43 radio-contention
pattern entry) since it's a recurrence of the same underlying class of
issue, not a one-off. Summary: `disconnect_wifi_after_wol()`'s
`cyw43_wifi_leave()` and the lightbar's confirm-sequence BT sends fired
back to back, stacking two contention bursts right when the controller
needed to survive the PC boot. Fixed by deferring the leave
(`WOL_DISCONNECT_DELAY_MS = 3000`, `g_wifi_leave_pending`) past both.
Confirmed working on real hardware (no more HCI 0x22 disconnect).

## 2026-08-10 — Trigger debounce + bounded connect retries

Researched `DevFreezing/DS5Dongle-WoL`'s `wake-on-lan` branch (independent
WOL implementation, same CYW43439 chip) per user request. Adopted: 90s
debounce on `wolwifi_on_controller_connect()` (`WOL_TRIGGER_DEBOUNCE_MS`,
armed only on an actual sent packet) and a capped Wi-Fi connect retry count
with distinct `CYW43_LINK_BADAUTH` handling (`MAX_WIFI_CONNECT_RETRIES = 2`,
one retry for a possibly-transient BADAUTH before giving up). Rejected: an
N-packets-with-gap send strategy (would have broken the lightbar's real
ARP-based "confirmed awake" signal) and their USB-mount-based "PC already
on" gate (out of scope, no concrete need). Firmware/companion build and
test clean (280/280).

## 2026-08-10 — Unconditional boot marker

Added `WolTraceStage::BoardBoot`, appended unconditionally on every boot
(raw `watchdog_hw->reason` as detail) — closes the last ambiguity around
whether an unexplained trace-sequence reset means a real reboot happened,
independent of the watchdog-specific detector. Also documented in
`AGENTS.md` that the desktop shortcut launches `win-unpacked/DS5 Bridge.exe`
directly, so `package:win`'s separate output doesn't update it (only
`installer:win` does).

## 2026-08-10 — Twelfth bug: removed the 2s connect-start delay entirely

Per explicit user requirement ("fire the instant the controller connects,
like `awalol/DS5Dongle#207`"), removed `WIFI_CONNECT_START_DELAY_MS`
(added for the fourth bug). Verified via an isolated git-worktree build of
a pre-fix commit that the underlying "WOL never wakes the PC" symptom
predated this session's whole disconnect-fix arc — not a regression.
Matches PR #207's own accepted tradeoff: no pre-delay, protect the
controller via power-off suppression instead (already in place, sixth bug).

## 2026-08-10 — Eleventh bug: false-positive ARP liveness confirmation

`arp_snoop_input()` matched the target MAC against the source of **any**
inbound Ethernet frame, not just ARP — `ETHTYPE_ARP` was defined but never
checked. Non-ARP broadcast/multicast traffic carrying the target's MAC as
source triggered a false "confirmed awake," cutting the real 15s resend
window down to one send before the ninth-bug fix tore Wi-Fi down. Fixed:
check the EtherType field before matching.

## 2026-08-10 — Watchdog-reboot visibility (diagnostic)

Added `WolTraceStage::BoardWatchdogReboot` (prior boot's
`WatchdogMainLoopPhase` as detail) to make a silent watchdog reboot
mid-resend-cycle visible in the trace instead of just erasing the evidence.
Also surfaced the trace ring's `droppedCount` as an explicit log line.
(Theory later ruled out — see the eleventh-bug entry.)

## 2026-08-10 — Tenth bug: DHCP itself stalling under radio contention

Split `WaitingForIp`'s timeout into its own `DHCP_WAIT_TIMEOUT_MS = 3000`,
separate from the 15s association timeout — caps how long a single stalled
DHCP attempt (itself a symptom of the same CYW43 radio-contention pattern,
see `DECISIONS.md`) can keep contending with BT before retrying. Added
`WolWifiAssocTimeout`/`WolDhcpWaitTimeout` trace stages.

## 2026-08-10 — Ninth bug: the post-WOL Wi-Fi leave was undoing itself

`disconnect_wifi_after_wol()`'s `enter_state(WifiState::Idle)` fed straight
into `Idle`'s unconditional "connect whenever possible" behavior, so the
intentional leave immediately reconnected on the very next tick — a
self-inflicted loop that defeated the eighth-bug fix. Fixed: new
`g_wifi_intentionally_idle` flag, checked in `Idle`, cleared only by
legitimate reconnect triggers.

## 2026-08-10 — Eighth bug: staying Wi-Fi-associated after WOL contends with BT for the rest of the boot

Real full-boot trace showed the controller disconnecting (HCI 0x22) several
to ~17s after a successful `wol-resend-confirmed`, on every cycle. Staying
associated post-WOL (DHCP renewal, beacon listening) is ongoing radio
activity that starves BT for as long as the board stays connected — far
longer than the original connect-start contention window. Fixed: added
`disconnect_wifi_after_wol()`, calling `cyw43_wifi_leave()` once a resend
cycle ends.

## 2026-08-10 — Seventh bug (root cause of "WOL never starts"): boot-time settings race

WOL config lived only in RAM, re-sent by the companion app near the end of
a ~40-command startup-reapply sequence — a paired controller's fast BT
reconnect reliably won the race and fired the trigger before config
arrived. Fixed: persist WOL config to on-board flash via BTstack's TLV
store (same mechanism as pairing-key/blacklist persistence), loaded in
`wolwifi_init()` before BT can possibly reconnect.

## 2026-08-10 — Board-level connection/WOL trace added

Built to diagnose a host-off-only failure the live-only WOL debug log
couldn't capture (target PC == companion-app PC, so nothing survives once
it's off). RAM-only ring buffer (`wol_trace_ring`, 48 records) in `bt.cpp`
capturing both BT connection-phase events and wolwifi.cpp's trigger/resend
events; new `COMPANION_REPORT_WOL_TRACE` report, drained into the same
on-disk log file whenever the companion app next polls, even minutes later.

## 2026-08-10 — Sixth bug (the real one): USB-suspend controller power-off fires before WOL can finish

`usb_pm_poll()`'s pre-existing battery-saving feature
(`bt_power_off_controller()`, ~3s after USB suspend) fired unconditionally,
racing and beating the WOL resend cycle every time. Fixed: new
`wolwifi_wake_in_progress()`, checked before the power-off call, suppresses
it only while a wake is actually in flight.

## 2026-08-10 — Pulsing lightbar WOL indicator (Phase 11b)

Pulsing dark↔light green while a resend cycle is active, solid light green
for 2s on ARP-confirmed wake, then true restore to the prior color.
`bt_wol_indicator_begin/confirm/cancel/loop()` in `bt.cpp`/`bt.h`, wired
into `wolwifi.cpp`'s resend cycle. Needed a dedicated pre-indicator color
snapshot since the existing lightbar-restore primitive doesn't support
true "restore to what was showing before," only "restore to whatever was
last explicitly set."

## 2026-08-10 — Fifth bug/gap: single magic-packet send has no delivery guarantee

`wolwifi_on_controller_connect()` only ever sent one UDP magic packet, no
ack, no retry. Fixed: resend every `WOL_RESEND_INTERVAL_MS` (3s) for up to
`WOL_RESEND_TOTAL_BUDGET_MS` (15s), stopping early on ARP-confirmed wake
(reusing the debug Ping's `arp_snoop_input` mechanism). Debug WOL Test
button unaffected (still single-send, on-demand tooling).

## 2026-08-10 — Fourth bug: CYW43 Wi-Fi/Bluetooth radio contention drops the controller

First entry in what became a recurring pattern across this whole session —
see `DECISIONS.md`'s CYW43 radio-contention entry for the durable
knowledge. This fix (later removed, twelfth bug): delayed the Wi-Fi connect
start by 2s after a controller-connect edge. Also added
`DHCP_DOES_ARP_CHECK=0`/`LWIP_DHCP_DOES_ACD_CHECK=0` to `lwipopts.h`
(found via `awalol/DS5Dongle#207` research) to shorten the DHCP handshake.

## 2026-08-09/10 — Three real Wi-Fi bugs found via the debug tooling

1. **SSID/password length never sent** — `bridge-service.ts` hardcoded the
   wire `value` field to `0` instead of the real payload length, so
   firmware always saw a zero-length string. Fixed at both call sites;
   added a regression test.
2. **Wrong CYW43 link-status function polled** — `cyw43_wifi_link_status()`
   can never return `CYW43_LINK_UP`/`NOIP` (driver join-state only, no
   DHCP/IP awareness); switched to `cyw43_tcpip_link_status()`. See
   `DECISIONS.md` for why this is a durable gotcha worth remembering.
3. **No cleanup before reconnecting** — `cyw43_arch_wifi_connect_async()`
   doesn't clean up a prior association; a retry after a genuine link drop
   joined on stale state and failed immediately. Fixed: `cyw43_wifi_leave()`
   before every reconnect attempt after the first. See `DECISIONS.md`.

Also added comprehensive diagnostics (raw `wifi_join_state` bitmask,
lifetime attempt/timeout/link-loss counters, automatic link-state-change
logging) after being asked to stop adding one field per guess.

**2026-08-10: first successful end-to-end WOL Test** — `result=success`,
magic packet actually sent.

## 2026-08-09 — WOL debug Ping/Test feature + always-on log file added

First real hardware smoke test failed silently (no logging in release
firmware, and the target PC was also the companion-app PC, so nothing
could be read once it powered off). Added on-demand Ping (ARP-snoop
liveness check)/WOL Test buttons plus a dedicated always-on
`ds5bridge-wol-debug.log`, so the pipeline could be verified while the
target PC was still on. New `WOL_DEBUG_STATUS` report, two new trigger
commands, companion IPC/UI wiring.

## 2026-08-09 — Phases 1–6: WOL feature designed, implemented, and building

Setup, research, feature definition, firmware Wi-Fi/WOL implementation
(`wolwifi.h/.cpp`, isolated module with no-op stubs off `ENABLE_WOLWIFI`),
companion app configuration (new `COMMAND_ID`s, settings plumbing, UI
section gated on a protocol-minor capability flag), and build integration
for both the Waveshare and default board targets — all designed and
committed. See `DECISIONS.md` for the architecture/wire-format decisions
made along the way (config transport, controller-connect hook point,
capability gate, build output locations, branch base).
