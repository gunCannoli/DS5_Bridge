# Task

Current + next task. Cleared and summarized into changelog.md when the plan
below is fully completed.

## Status

Setup, research (Phase 1-2), feature definition (Phase 3), firmware Wi-Fi/WOL
implementation (Phase 4), companion app configuration (Phase 5), and build
integration (Phase 6a/6b) complete and committed on `feature/wol-wifi`.

First real hardware smoke test (2026-08-09): PC off, controller off, board
on (USB hub powered). Controller paired with board but WOL did not wake the
PC. Root cause undiagnosed -- release firmware has no logging, and since
the target PC and the PC running the companion app were the same machine,
there was no way to read logs after the PC was off anyway.

In response, added a WOL debug feature (Ping + WOL Test buttons in the
companion app's Wake-on-LAN section, ARP-snoop-based ping, dedicated
always-on log file) so the pipeline can be verified while the target PC is
on -- see decisions.md for the full design writeup. This needed a firmware
change (new debug entry points in wolwifi.h/.cpp, new WOL_DEBUG_STATUS
report, two new trigger commands) and a companion app change (new IPC
channels, status polling, debug log writer, UI row). Both committed and
built: debug-logging Waveshare firmware at
`build/waveshare-debug2/ds5-bridge.uf2`, companion installer at
`companion/artifacts/installer/DS5-Bridge-Companion-Setup-1.7.0.exe`.

Debugging session (2026-08-09, continued) -- three real firmware bugs found
and fixed via the debug tooling, in order:
1. SSID/password commands sent `value=0` instead of the actual payload
   length, so firmware always saw a zero-length string (`have_ssid` stuck
   false no matter what was typed in the UI).
2. `wolwifi_task()` polled `cyw43_wifi_link_status()`, which can never
   return `CYW43_LINK_UP`/`NOIP` at all (driver-level join state only, no
   DHCP/IP awareness) -- our state machine's guards all required
   `status == CYW43_LINK_UP`, so it could never see a real connection
   complete, no matter how long DHCP had a bound lease. Fixed by switching
   to `cyw43_tcpip_link_status()`, the IP-aware superset function.
3. `cyw43_arch_wifi_connect_async()` doesn't clean up a prior association
   itself; retrying after a genuine link drop was joining on top of stale
   driver state and failing immediately. Fixed by calling
   `cyw43_wifi_leave()` before every reconnect attempt after the first.

Also added comprehensive diagnostics (raw `wifi_join_state` bitmask,
lifetime attempt/timeout/link-loss counters, DHCP client state) and
automatic link-state-change logging (not just on debug-button clicks) so
the full connect timeline is visible without guessing.

**2026-08-10: first successful end-to-end WOL Test** --
`result=success`, `link=connected`, `raw_link_status=up`, magic packet
actually sent. There was a brief ~2.5s reconnect flap right at send time
(self-healed via the leave-before-rejoin fix, `connect_attempts` jumped
2->4) but the send ultimately succeeded. Full debugging narrative and
fixes recorded in decisions.md.

**Fourth bug found (2026-08-10, same session): controller drops BT
during a real automatic WOL trigger.** The manual WOL Test (Wi-Fi already
connected) worked, but a real controller-connect-triggered WOL fires a
fresh Wi-Fi association + DHCP handshake on the CYW43439 (a combo Wi-Fi/BT
chip sharing one radio) at the exact moment the BT session is still
stabilizing, and that RF contention was dropping the controller. Researched
PR #93 and the existing "Wake PC on Controller" feature (neither actually
keeps BT connected -- both let it drop and reconnect) and
`awalol/DS5Dongle#207/#186/#136` (independent prior art on the same combo
chip; confirmed `cyw43_tcpip_link_status()` as correct, found two unused
lwIP DHCP options). Fix (committed): delay the *start* of the Wi-Fi connect
sequence by 2s after a controller-connect edge
(`WIFI_CONNECT_START_DELAY_MS` in `wolwifi.cpp`), skipped entirely if
Wi-Fi is already connected from a prior session; added
`DHCP_DOES_ARP_CHECK=0`/`LWIP_DHCP_DOES_ACD_CHECK=0` to `lwipopts.h` to
further shorten the DHCP handshake. Debug firmware rebuilt at
`build/waveshare/ds5-bridge.uf2`. Full writeup in decisions.md.

**Real PC-off retest with the delay fix (2026-08-10): PC still didn't
wake**, despite the log showing a clean connect (`connect_attempts=2`,
`link=connected`, `raw_link_status=up`) -- but that log entry was from the
*manual* WOL Test debug button (`event=debug-action action=send-wol`), run
after the PC was already confirmed on, not from the automatic trigger
during the actual off-PC test; the automatic path's own log line from the
real attempt wasn't captured. Regardless, the underlying reliability gap
was real: `wolwifi_on_controller_connect()` only ever sent one UDP magic
packet with no delivery guarantee and no confirmation. Fixed (committed):
`begin_resend_cycle()`/`drive_resend_cycle()` now resend the magic packet
every 3s for up to 15s on the automatic trigger, stopping early once an
ARP reply confirms the target woke up (reusing/generalizing the debug
Ping's `arp_snoop_input` mechanism). Debug WOL Test button unaffected --
still a single send. Debug firmware rebuilt at
`build/waveshare/ds5-bridge.uf2`. Full writeup in decisions.md.

**Lightbar WOL indicator added (2026-08-10, same session, committed):**
requested after the resend fix -- pulsing dark<->light green while a
resend cycle is active, solid light green for 2s on ARP-confirmed wake,
then true restore to the prior color. See Phase 11b below for the full
writeup; implemented in `bt.cpp`/`bt.h` (`bt_wol_indicator_*`), wired into
`wolwifi.cpp`'s resend cycle. Debug firmware rebuilt at
`build/waveshare/ds5-bridge.uf2`.

**2026-08-10: color/resend confirmed working on real hardware, but the
real bug found** -- user confirmed the resend + lightbar behavior worked
in a follow-up test, but reported the controller still doesn't survive
until the PC turns on. Root cause found: **not** BT/Wi-Fi radio contention
(already fixed) -- `usb_pm_poll()` in `usb.cpp` has a pre-existing,
separate battery-saving feature that powers the DualSense off via
`bt_power_off_controller()` ~3s after USB suspend is detected
(`USB_SUSPEND_POWEROFF_DEBOUNCE_US`), controlled by the existing
"USB suspend disconnect" companion setting (default on, unrelated to WOL).
The instant the target PC's USB goes away, the board deliberately powers
the controller off a few seconds later, well before a 15s WOL resend cycle
(or even the 2s connect-start delay) can finish. Fixed (committed): new
`wolwifi_wake_in_progress()` (true while `g_resend_active` or
`g_send_pending`) checked in `usb_pm_poll()` before the power-off call --
suppressed only while a wake is actually in progress, normal behavior
resumes immediately once the wake ends (confirmed or timed out). Debug
firmware rebuilt at `build/waveshare/ds5-bridge.uf2`. Full writeup in
decisions.md.

**2026-08-10: real PC-off retest of the suspend-power-off fix failed
differently** -- controller connected, lightbar **never turned green** at
all (meaning `wolwifi_on_controller_connect()` likely never even ran), and
disconnected after ~15s. The WOL debug log had no new entries for this
attempt -- confirmed both it and the firmware UART log are live-only
(streamed over the companion HID channel), so neither can capture anything
that happens while the target PC/companion app is off, which is exactly
the scenario being tested. User's diagnosis: the last connection-persist
change likely introduced a bug that stops WOL from ever starting, and
asked for a board-level log that can be read back once the app reopens.
Added (committed): a small RAM-only ring buffer
(`wol_trace_ring`/`bt_append_wol_trace_event()` in `bt.cpp`) recording BT
connection-phase transitions/timeouts/disconnect-reasons AND wolwifi.cpp's
own trigger/resend events, survives across a disconnect, exposed via a new
`COMPANION_REPORT_WOL_TRACE` HID report and drained into the same
always-on WOL debug log file (`event=board-trace` lines) whenever the
companion app next polls -- even minutes after the actual attempt. Debug
firmware rebuilt at `build/waveshare/ds5-bridge.uf2`, companion app
repackaged at
`companion/artifacts/DS5 Bridge-win32-x64-2026-08-10T01-55-26-806Z`. Full
writeup in decisions.md.

**2026-08-10: trace worked -- root cause found and fixed: WOL config
boot-time race.** After fixing a stale-shortcut issue (desktop shortcut
was launching an old `win-unpacked` build predating the trace feature;
rebuilt via `npm run installer:win`), a real trace came through:
`wol-trigger-skipped detail=0` (enabled/have-ssid/have-target-mac all
false) fired the instant `conn-ready` was reached, on two consecutive
fresh boots. Root cause: WOL config only ever lived in RAM, re-sent by the
companion app as one of the last of ~40 sequential commands in its
startup-reapply sequence -- a paired controller's BT reconnect (fast,
since pairing is already done) was completing and firing the trigger
before the companion app got anywhere near the WOL commands. Fixed
(committed): WOL config (enabled/SSID/password/target-MAC) now persists
to on-board flash via BTstack's TLV store (same mechanism already used for
pairing-key/blacklist persistence in `bt.cpp`), saved on every setter call
and loaded in `wolwifi_init()` -- which runs at boot before BT can
possibly finish a reconnect, closing the race entirely. Debug firmware
rebuilt at `build/waveshare/ds5-bridge.uf2`. Full writeup in decisions.md.

**Important for the next test:** this fix only takes effect for WOL
config set *after* flashing this firmware -- the very first boot post-flash
still starts with nothing in flash, so WOL needs to be (re)configured once
via the companion app after flashing (which will now persist it) before a
true "PC fully off, no prior companion-app session this boot" test is
valid.

**2026-08-10: persistence confirmed working; real trace of a full attempt
exposed an eighth bug.** After the companion app's normal reapply had
already run once (confirmed by a successful manual Ping/WOL Test), a board
power-cycle showed `wol-trigger-fired` immediately at `conn-ready` on every
reconnect -- persistence fix confirmed working as designed. That let a full
real trace through for the first time: `wol-trigger-fired` ->
`wol-connect-delay-start` -> `wol-resend-begin` -> `wol-resend-confirmed`
(WOL actually worked), but then the controller disconnected mid-PC-boot
with **HCI reason 0x22 (LMP/LL response timeout)** -- a genuine BT
radio-level timeout, several to ~17s after the resend cycle ended, on
every cycle in the trace. Root cause: the 2s connect-start delay only
covers contention at Wi-Fi connect *start*; staying associated to the AP
afterward (DHCP renewal, beacon listening) is ongoing radio activity that
kept contending with BT for the rest of the PC's boot. Fixed (committed):
`disconnect_wifi_after_wol()` calls `cyw43_wifi_leave()` once a resend
cycle ends (confirmed or given up), wired into both end branches of
`drive_resend_cycle()` -- WOL doesn't need Wi-Fi connected once its job is
done, and the next controller-connect trigger reconnects fresh via the
existing delayed-start + leave-before-rejoin path. Debug firmware rebuilt
at `build/waveshare/ds5-bridge.uf2`. Full writeup in decisions.md.

**2026-08-10: eighth-bug fix retested -- WOL/lightbar confirmed working,
but controller still dropped before boot finished (ninth bug: the leave
call was undoing itself).** Real trace showed `wol-resend-confirmed`
followed almost immediately by fresh `dhcp_state=rebooting`/incrementing
`connect_attempts` in the WOL debug log -- Wi-Fi was reassociating again
right after the intentional leave, not staying disconnected. Root cause:
`disconnect_wifi_after_wol()`'s `enter_state(WifiState::Idle)` fed straight
into `wolwifi_task()`'s `Idle` case, which unconditionally starts a new
connect on its very next tick -- turning the intentional leave into a
self-inflicted immediate leave/rejoin loop, so the ongoing contention never
actually stopped. Fixed (committed): new `g_wifi_intentionally_idle` flag
set by `disconnect_wifi_after_wol()`, checked in the `Idle` case to
actually stay idle; cleared only by the three legitimate reconnect
triggers (`wolwifi_on_controller_connect()`, SSID/password setters). User
confirmed WOL send + lightbar pulse/confirm both work correctly now --
this fix is purely about making the post-WOL Wi-Fi teardown actually stick.
Debug firmware rebuilt at `build/waveshare/ds5-bridge.uf2`. Full writeup in
decisions.md.

**2026-08-10: ninth-bug fix confirmed -- reconnect loop is gone, but a real
boot exposed a tenth issue: DHCP itself stalling for up to 15s, feeding
back into repeated BT drops.** Real full-session trace: the controller
disconnected (reason 0x22) multiple times *before* Wi-Fi ever reached
Connected, each time with Wi-Fi association fast but DHCP stuck in
`selecting`/`checking` for the full 15s `WIFI_CONNECT_TIMEOUT_MS` before
retrying. Automatic magic packets did get sent on some cycles, but only
reached the PC after the user manually powered it on, since the ~30-40s of
Wi-Fi struggle pushed the actual send later than the manual button press.
Fixed (committed): split out a separate `DHCP_WAIT_TIMEOUT_MS = 3000`
(user chose 3s) for the `WaitingForIp` phase specifically, down from
sharing the 15s association timeout -- caps how long a single stalled DHCP
attempt can contend with BT before retrying. Also added
`WolWifiAssocTimeout`/`WolDhcpWaitTimeout` trace stages (user requested
more log detail) so both timeout paths are now visible in the survives-a-
host-off-gap board trace, not just the live-only WOL debug log. Debug
firmware rebuilt at `build/waveshare/ds5-bridge.uf2`; companion app
installer rebuilt at
`companion/artifacts/installer/DS5-Bridge-Companion-Setup-1.7.0.exe` (also
updates the win-unpacked build the desktop shortcut targets -- the app was
closed and reinstalled cleanly this time, no stale-build risk). Full
writeup in decisions.md.

**2026-08-10: 3s DHCP timeout retested -- same outcome, and a new lead:
possible silent watchdog reboot mid-resend-cycle.** Real trace: a resend
cycle started (`wol-connect-delay-start`) and then simply stopped --
no `wol-resend-begin`, no confirmed/gave-up, nothing -- followed minutes
later by `connect_attempts` resetting to 0 and `link=unconfigured`, which
normally only happens right after boot. User confirmed the board itself
never lost power. `main.cpp` already has a 1000ms watchdog with per-phase
telemetry (`WatchdogMainLoopPhase`, including a `Wolwifi` phase) that
would explain exactly this if something stalled for over a second during
the resend cycle -- but that telemetry only ever reached `DS5_LOG`, live-
only, invisible for a real host-off gap, and a watchdog reboot would also
wipe the RAM-only WOL trace ring along with everything else, so there was
no way to confirm this from the log the user has. Added (committed): new
`BoardWatchdogReboot` trace stage, appended once at boot with the prior
`WatchdogMainLoopPhase` as detail, so the *next* boot's trace (the reboot
wipes the current one, but the new boot's first trace event now records
what happened) can confirm or rule this out directly. Also surfaced the
trace ring buffer's `droppedCount` as an explicit
`event=board-trace-dropped` log line (previously read but never
logged/acted on) so a ring-buffer overflow gap is distinguishable from
"nothing happened." Debug firmware rebuilt at
`build/waveshare/ds5-bridge.uf2`; companion app installer rebuilt at
`companion/artifacts/installer/DS5-Bridge-Companion-Setup-1.7.0.exe`. Full
writeup in decisions.md.

## Current task

- [ ] **Flash the rebuilt debug firmware, launch the companion app (fresh
      install), and re-attempt the real PC-off wake test.** WOL config is
      already persisted in flash, no re-save needed. This round is
      specifically about confirming or ruling out the watchdog-reboot
      theory: check `ds5bridge-wol-debug.log` for
      `event=board-trace stage=board-watchdog-reboot` appearing anywhere
      -- if it does, `detail` is the `WatchdogMainLoopPhase` index the
      board was stuck in (16 = `Wolwifi` would directly confirm the
      resend-cycle-stall theory; see `watchdog_telemetry.h` for the full
      list). Also still check: does `wol-wifi-assoc-timeout`/
      `wol-dhcp-wait-timeout` still appear (DHCP still stalling even at
      3s?), does the controller survive the entire boot without
      disconnecting, and does the automatic magic packet arrive without
      needing a manual power-on.

## Next task

- [ ] If the real wake test succeeds: proceed to Phase 9 (failure-behavior
      verification) and Phase 10 (PR prep).
- [ ] If it fails: the same debug tooling (Ping/WOL Test buttons, now with
      working Wi-Fi) should narrow it down quickly -- likely candidates
      would be the magic packet not reaching the target's NIC specifically
      (vs. the debug ARP ping, which only proves general network
      reachability, not that the NIC's WOL listener is armed), or an
      OS/BIOS-level WOL setting on the target PC itself (a common real-world
      gap: WOL must be allowed in both the NIC driver's power-management
      settings and Windows' fast-startup/hybrid-shutdown settings, which
      can silently disable WOL after a "shutdown" that isn't a true
      power-off).
- [ ] Phase 11a/11b (WOL trace in Diagnostics, lightbar pulse) -- still
      planned, deprioritized behind confirming the core feature actually
      works end to end first. Revisit once Phase 7 passes.
- [ ] Phase 9: verify failure-behavior claims at test time (non-blocking
      design is already in place per 4d/4e, needs runtime confirmation)
- [ ] Phase 10: PR prep — write PR description, fill in testing checklist
      once Phase 7 runs, review for unrelated changes before opening PR.
      Note: the WOL debug feature (Ping/WOL Test) is a reasonable candidate
      to keep in the upstream PR too, not just as our local dev tooling --
      decide before opening the PR whether to present it as part of the
      feature or strip it as internal-only. The three bugs found and fixed
      via this tooling are also a strong argument for keeping it: without
      it, upstream reviewers/users hitting the same issues would have no
      way to diagnose them either.

### Phase 11 — WOL trace log + lightbar pulse (planned 2026-08-09, deferred until Phase 7 passes)

Two UX/diagnostic additions requested after the debug Ping/WOL Test row
proved useful but (a) its log file requires digging through a text file by
hand, and (b) there's no in-the-room visual signal that WOL fired at all.
Researched existing patterns first (see decisions.md for the write-up);
both build on infrastructure that already exists.

**11a. WOL trace in the Diagnostics tab** (chosen: full ring-buffer
pattern, matching Trigger Trace / Feedback Trace exactly, not a lighter
tail-the-log-file option, since it needs to also capture automatic
WOL-on-controller-connect events, not just companion-triggered
Ping/WOL Test debug actions):
- [ ] Firmware: small ring buffer struct in `wolwifi.cpp` (or
      `companion.cpp`, TBD which owns it -- trigger/feedback trace ring
      buffers currently live in `companion.cpp`) recording WOL events:
      controller-connect trigger fired, ping started/result, WOL-send
      started/result, link-state transitions. Sequence-numbered with a
      dropped-count like `TriggerTraceEvent`/`FeedbackTraceEvent` (see
      `src/companion.cpp` `kTriggerTraceRecordSize`,
      `trigger_trace_ring`, `build_trigger_trace()`).
- [ ] New `COMPANION_REPORT_WOL_TRACE` report id (next free companion
      report slot) + `build_wol_trace()` packing multiple ring records
      per HID feature-report read, same as `build_trigger_trace()`.
- [ ] Protocol: `parseWolTraceReport` + `WolTraceEventPayload` type +
      stage-label/format helper in `protocol.ts`, mirroring
      `parseTriggerTraceReport`/`triggerTraceStageLabel`.
- [ ] bridge-service.ts: `readWolTraceThrottled()`, `appendWolTraceLines()`
      with a line-limit constant (existing traces use 300 lines), wired
      into the diagnostics poll loop next to
      `readTriggerTraceThrottled()`/`readFeedbackTraceThrottled()`; new
      `wolTraceLines`/`wolTraceDroppedCount` fields on `BridgeDiagnostics`.
- [ ] App.tsx: one more `useMemo` text builder + one more `.debug-entry`
      `<textarea readOnly>` row in the Diagnostics `<dl>`, next to Trigger
      Trace / Feedback Trace / Audio Events -- same pattern, no new CSS.
- [ ] Decide: gate behind a new `DS5_WOL_TRACE_ENABLED` diagnostics-preset
      flag (matching how trigger/feedback trace are opt-in) or leave
      always-on like the current single-status `WOL_DEBUG_STATUS` report.

**11b. Lightbar indicator while a WOL send is in flight (DONE, committed
2026-08-10)** -- superseded the original single-flash plan below with a
richer 3-stage sequence once the resend-until-confirmed fix (above) made
"in flight" a real multi-second window worth showing, not just an instant:
- [x] Pulsing dark<->light green (~1.5s breathing cycle) for the duration
      of an active resend cycle (`bt_wol_indicator_begin()`, called from
      `begin_resend_cycle()` in `wolwifi.cpp`).
- [x] Solid light green for a 2s hold once the target confirms it's awake
      via the same ARP liveness check the resend cycle uses to know when
      to stop (`bt_wol_indicator_confirm()`).
- [x] True restore-to-previous-color once the hold ends, or immediately if
      the resend budget is exhausted with no confirmation
      (`bt_wol_indicator_cancel()`, no distinct failure color -- kept
      purely positive, matching the earlier decision not to add
      complexity for the unconfirmed case). Implemented via a dedicated
      `wol_indicator_pre_*` snapshot in `bt.cpp`, separate from
      `saved_lightbar_*`/`lightbar_restore_pending`, since the pulse's own
      repeated `bt_set_lightbar_color()` calls would otherwise clobber the
      color it needs to restore to.
- [x] New `bt_wol_indicator_begin/confirm/cancel/loop()` API in
      `bt.cpp`/`bt.h`; `bt_wol_indicator_loop()` polled from `main.cpp`'s
      existing `Lightbar` phase alongside `bt_lightbar_loop()`.
- [x] Debug WOL Test button unaffected -- doesn't go through the resend
      cycle, so no indicator fires for it.

Debug firmware rebuilt at `build/waveshare/ds5-bridge.uf2`. Not yet
visually verified on real hardware (pending next test).

---

## Full execution plan (from research phase)

### Phase 3 — Feature definition (DONE, decided)

Config surface:
- `Wake-on-LAN Enabled` (bool)
- `Wi-Fi SSID` (string)
- `Wi-Fi Password` (string)
- `WOL Target MAC` (string, `AA:BB:CC:DD:EE:FF`)

Single target PC. Standard UDP magic packet (`FF FF FF FF FF FF` + target MAC
x16), broadcast address, standard WOL UDP port.

### Phase 4 — Firmware design (DONE, committed)

- [x] 4a. CMake: `ENABLE_WOLWIFI` option (defaults on for
      `WAVESHARE_RP2350B_PLUS_W_BUILD`, forced off elsewhere), swaps
      `pico_cyw43_arch_poll` for `pico_cyw43_arch_lwip_poll`, adds
      `boards/headers/lwipopts.h` (NO_SYS=1, UDP+DHCP only, no
      TCP/sockets/PPP). Confirmed scoped correctly — other boards unaffected.
- [x] 4b. `src/wolwifi.h` / `src/wolwifi.cpp` — isolated module, no-op stubs
      when `ENABLE_WOLWIFI` is off. Owns Wi-Fi connect/retry (async, polled
      state machine), magic-packet construction, UDP send via lwIP raw API.
- [x] 4c. Hooked `wolwifi_on_controller_connect()` into
      `finish_hid_session_if_ready()` in `src/bt.cpp`, right after
      `connection_phase = BtConnectionPhase::Ready`. Confirmed edge-triggered
      by the existing early-return guard in that function.
- [x] 4d. `wolwifi_task()` polled from main loop (`src/main.cpp`), alongside
      other `RUN_MAIN_PHASE` calls; new `WatchdogMainLoopPhase::Wolwifi` phase.
- [x] 4e. De-risked: `cyw43_arch_init()`/`cyw43_arch_poll()` already drive
      lwIP's async context together with BTstack's when `CYW43_LWIP=1`
      (confirmed by reading `cyw43_arch_poll.c` in the SDK) — no separate
      polling loop or bus-contention handling needed beyond what's already
      in `main.cpp`. Full firmware build + link + existing SRAM/heap
      verification passes for the Waveshare target with WOL on.

### Phase 5 — Companion app changes (DONE, committed)

- [x] 5a. New `COMMAND_ID`s (`0x37`-`0x3A`) in `companion/src/shared/protocol.ts`
      + mirrored `enum CommandId` in `src/companion.cpp`.
- [x] 5b. Variable-length payload commands for SSID/password, reusing the
      `SET_CHORD_BINDINGS` payload framing (length in `value`, bytes after
      the 10-byte header). Target MAC reuses the existing 6-byte
      `bluetoothAddressPayload`-style wire format instead (fixed size, no
      length prefix needed) — added `wolTargetMacPayload` as a MAC-specific
      variant with WOL-relevant validation (rejects null/broadcast MAC).
- [x] 5c. `wolEnabled` bool command (`SET_WOL_ENABLED`), following the
      existing bool-setting pattern (e.g. `SET_WAKE_ON_CONNECT`, which
      turned out to already exist in `port-dev` as a real merged USB-wake
      feature, separate from PR #93's proposal — used as the direct
      template instead).
- [x] 5d. Settings plumbing: `settings-store.ts` (defaults/normalize, with
      length clamping), `bridge-service.ts` (setters + reconnect-reapply,
      conditional on non-empty for SSID/password/MAC), `main.ts` (IPC
      handlers), `preload.ts` (renderer bridge). `ipc-contract.test.ts`
      updated with a matching contract test.
- [x] 5e. `App.tsx` UI: enable toggle (same format as the existing "Wake PC
      on Controller" row) + 3 stacked full-width text inputs in a new
      "Wake-on-LAN" section, right after Connection Behavior in the left
      column. Modal `max-height` increased 560px -> 700px to fit without
      scrolling.
- [x] 5f. MAC address validation: companion-app side via `wolTargetMacPayload`
      (throws `ProtocolError`, shown inline in the UI) AND firmware side
      re-validates independently in `wolwifi_set_target_mac`/`_set_wifi_ssid`/
      `_set_wifi_password` — never trusts the app's client-side checks alone.
- [x] Bonus: gated the whole WOL UI section on a new `firmwareFlags.wolControl`
      capability flag (protocol-minor gate, bumped `PROTOCOL_MINOR` 19 -> 20
      in both `protocol.ts` and `companion.cpp`, kept in sync), matching the
      pattern `wakeOnConnectControl`/`audioReactiveHapticsControl` already use.

### Phase 6 — Build integration (6a/6b DONE, done early while validating Phase 4)

- [x] 6a. Waveshare target (`-DWAVESHARE_RP2350B_PLUS_W_BUILD=ON`) builds,
      links, and passes SRAM/heap verification with `ENABLE_WOLWIFI` on.
      (Note: `ds5-bridge.uf2`/`.hex`/`.bin`/`.dis` generation via the
      chained CMake POST_BUILD command intermittently crashes
      `arm-none-eabi-objdump` on this machine when invoked through
      Ninja's nested `cmd.exe` chain — confirmed NOT related to our code:
      `objdump`/`objcopy`/`picotool` all succeed when run manually against
      the same `.elf`. Environment quirk local to this machine; revisit if
      it recurs, e.g. by disabling `PICO_NO_COPRO_DIS` interaction or
      running the build outside a nested shell.)
- [x] 6b. Default `pico2_w` build (`-DENABLE_COMPANION=ON`, no Waveshare
      flag) builds and links cleanly; verified zero `wolwifi` symbols in
      the resulting ELF via `nm | grep -ic wolwifi` = 0.

### Phase 7 — Smoke test (needs real SSID/password/target MAC from user before running)

- [ ] Test 1: Wi-Fi connects, get IP, check reconnection behavior
- [ ] Test 2: WOL wakes PC from hibernate; from full shutdown if NIC supports
- [ ] Test 3: controller connect fires WOL once
- [ ] Test 4: disconnect/reconnect fires WOL again, no dupes while connected
- [ ] Test 5: existing functionality unaffected (USB, BT stability, input,
      reconnect, no added controller latency)

### Phase 8 — Logging (substantially DONE)

- [x] `[WOL]` prefixed `DS5_LOG` calls already present in `wolwifi.cpp` at:
      init, Wi-Fi connecting/connected (+ IP), connect failure/timeout, link
      lost, SSID/password/MAC updated, controller-connect trigger, magic
      packet sent/failed. Matches the existing `DS5_LOG` macro used
      throughout the firmware (compiles out entirely unless
      `DS5_DEBUG_LOGS_ENABLED`, so no runtime cost in release builds).
- [ ] Revisit verbosity once real hardware testing (Phase 7) shows what's
      actually useful vs. noisy.

### Phase 9 — Failure behavior (design constraint, verify at test time)

- [ ] Confirm Wi-Fi/WOL failure never blocks or delays BT/controller
      init or normal operation (non-blocking by construction from 4d)

### Phase 10 — PR prep

- [ ] Commits separated: firmware Wi-Fi/WOL support, companion app config,
      controller-trigger wiring, Waveshare build integration
- [ ] PR description: why WOL, how it works, how PR #93's event mechanism
      was reused, supported board, config requirements, test results
- [ ] Testing checklist filled in (see Phase 7)
- [ ] Move completed summary to changelog.md, clear this file
