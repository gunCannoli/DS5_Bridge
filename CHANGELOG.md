# Changelog

Durable record of completed work on the Wi-Fi WOL feature fork, newest
first. Entries move here from `TASK.md` once done — this is where "what
happened and when" lives; `TASK.md` only tracks what's still open.
Architecture/known-issue knowledge that should inform future work lives in
`DECISIONS.md` instead, not here.

---

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
