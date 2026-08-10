# Decisions

Architecture and technical decisions for the Wi-Fi WOL feature, newest first.
Each entry: decision, rationale, alternatives considered, date.

---

## 2026-08-10 — Seventh bug (root cause of "WOL never starts"): boot-time settings race

**Context:** with the board-level trace working (after fixing a stale
desktop-shortcut build issue -- see below), a real PC-off retest produced
actual trace data:

```
seq=1 stage=conn-connecting
seq=2 stage=conn-securing
seq=3 stage=conn-hid-opening
seq=4 stage=conn-ready
seq=5 stage=wol-trigger-skipped detail=0
```

...twice, on two consecutive fresh boots. `detail=0` decodes via the
bitmask (`bit0=enabled, bit1=have_ssid, bit2=have_target_mac`) to **all
three false** at the exact moment the connection reached Ready -- despite
the companion app's own live `WOL_DEBUG_STATUS` polling showing
`wol_enabled=true have_ssid=true have_target_mac=true` a few seconds
later in the same log.

**Root cause:** WOL config (`g_enabled`/`g_ssid`/`g_password`/
`g_target_mac` in `wolwifi.cpp`) lived only in RAM. The companion app
re-sends it via `SET_WOL_ENABLED`/`SET_WOL_WIFI_SSID`/etc. as part of
`applyCurrentSettings()` in `bridge-service.ts` -- a long sequential list
of ~40 `await this.sendCommand(...)` calls, with the WOL commands placed
near the end (after LED, idle-disconnect, USB-suspend, wake-on-connect,
and others). This whole sequence only runs once per new companion HID
session (`reapplySettingsUntilSettled()`), i.e. after the companion app
itself reconnects post-boot. Meanwhile, a *paired* controller's own BT
reconnect is fast -- pairing/link-key exchange is already done, so it's
just page scan + security + HID channel open, confirmed by the trace to
complete in well under half a second (`board_time_ms` 73889 to 74279
across all four `conn-*` stages). The controller was reliably winning that
race and firing the trigger before the companion app got anywhere near
the WOL commands, so `wolwifi_on_controller_connect()` saw completely
unconfigured state and (correctly, given what it could see) skipped.

This explains every prior "WOL never starts" observation cleanly and
retroactively invalidates nothing about the fourth/fifth/sixth-bug fixes --
those were all real, independently-verified issues (radio contention,
single-send reliability, USB-suspend power-off) that would still need
fixing once WOL actually starts firing. This bug just meant it often never
got the chance to.

**Fix:** persist WOL config to on-board flash via BTstack's TLV store --
the exact mechanism `bt.cpp` already uses for pairing-key and blacklist
persistence (`bt_blacklist_persist()`/`_load()`, `store_tag`/`get_tag`
under a 4-ASCII-char tag). Added `wolwifi_persist_config()` (called from
every setter: `wolwifi_set_enabled`/`_wifi_ssid`/`_wifi_password`/
`_target_mac`) and `wolwifi_load_persisted_config()` (called from
`wolwifi_init()`). Tag `'WOLC'` (0x574f4c43), versioned struct
(`WolPersistedConfig`, `version` field for future migration room).

**Why `wolwifi_init()` and not `HCI_STATE_WORKING`:** initially considered
loading alongside `bt_blacklist_load()` in `bt.cpp`'s `HCI_STATE_WORKING`
handler (same event bt.cpp already uses for its own flash loads), but
`wolwifi_init()` is called in `main.cpp` immediately after `bt_init()`
*starts* -- `bt_init()` only kicks off the async HCI power-on sequence,
`HCI_STATE_WORKING` fires later as a callback. Loading in `wolwifi_init()`
is strictly earlier and simpler (no need to hook into bt.cpp's event
handler for an unrelated module), and still trivially before BT can
possibly complete any reconnect.

**Caveat for testing:** this only closes the race for config set *after*
flashing this firmware -- flash starts empty, so WOL must be (re)entered
once via the companion app post-flash (which persists it going forward)
before a true "fresh boot, no companion app involved yet" test is valid.

**Status:** implemented and committed, debug firmware rebuilt at
`build/waveshare/ds5-bridge.uf2`. Not yet verified against a real PC-off
test with the persisted config in place (pending: configure once, power-
cycle to confirm persistence, then retest).

---

## 2026-08-10 — Board-level connection/WOL trace: diagnosing a host-off-only failure

**Context:** the sixth-bug fix (below) was retested for real (PC off) and
failed differently: the controller connected, the lightbar **never turned
green** (meaning the WOL indicator's `bt_wol_indicator_begin()`, called
only from `begin_resend_cycle()`, never ran -- so `wolwifi_on_controller_connect()`
likely never even fired), and the controller disconnected roughly 15
seconds later. Re-reading `ds5bridge-wol-debug.log`
(`C:\Users\limap\AppData\Roaming\DS5 Bridge\ds5bridge-wol-debug.log`)
showed **no new entries at all** for this attempt -- the last lines were
stale, from `01:36:41` in a prior session.

**Root cause of the missing data (not the WOL bug itself, a diagnostics
gap):** both the WOL debug log (`readWolDebugStatus()`/`appendWolDebugLog()`
in `bridge-service.ts`) and the firmware UART log (`build_firmware_log()`
in `companion.cpp`, documented in AGENTS.md) are transported entirely over
the companion HID channel and only ever written when the companion app is
actively polling. Since the target PC -- which is also the machine running
the companion app in this test setup -- was off for the whole attempt,
neither log could capture anything, regardless of what actually happened
on the board. This is a real, structural blind spot for exactly the
scenario WOL exists to handle (host off), not something a build flag or a
UI tweak could fix.

**User's diagnosis and request:** suspected the connection-persist changes
introduced a bug that stops the WOL sequence from starting at all (not a
BT/Wi-Fi radio contention issue, not the USB-suspend power-off issue --
both already fixed and both would still show `wolwifi_on_controller_connect()`
having fired, just failing partway through). Asked for a board-level log
that survives independent of a live companion connection, readable once
the app reopens.

**Design decision (asked and confirmed):** rather than a narrower
wolwifi-only trace, capture BT connection-phase events too, in the same
ring buffer -- directly because the lightbar-never-green symptom points at
the connection-establishment sequence in `bt.cpp` (existing code, predates
WOL, shared by every controller connection) possibly timing out and
disconnecting *before* `wolwifi_on_controller_connect()` is ever reached,
which a WOL-only trace would be blind to.

**Implementation:**
- `wol_trace_ring` (48 records) + `bt_append_wol_trace_event()`/
  `bt_read_wol_trace()` in `bt.cpp`/`bt.h`, modeled directly on the
  existing `trigger_trace_ring`/`append_trigger_trace_event()` pattern in
  `companion.cpp` (Phase 11a's originally-planned precedent, now actually
  used one step ahead of schedule). RAM-only, survives a BT disconnect
  (doesn't need to survive a power cycle -- only needs to bridge the gap
  until the companion app next polls, same principle the always-on WOL
  debug log already relies on).
- `WolTraceStage` enum spans both connection-phase events
  (`ConnPhaseConnecting/Securing/HidOpening/Ready/Disconnecting`,
  `ConnSecurityTimeout`, `ConnHidOpeningTimeout`,
  `ConnHidInterruptFollowupTimeout`, `ConnDisconnected` with the HCI reason
  code as `detail`) and wolwifi events (`WolTriggerFired`/
  `WolTriggerSkipped` -- the latter's `detail` is a bitmask of
  enabled/have-ssid/have-target-mac, directly answering "did WOL even try"
  -- `WolConnectDelayStart`, `WolResendBegin`, `WolResendConfirmed`,
  `WolResendGaveUp`).
- Append calls added at every `connection_phase` assignment site in
  `bt.cpp` (there were 7: `begin_connection_attempt`, `note_acl_connected`,
  `begin_hid_opening`, `begin_connection_disconnect`,
  `finish_hid_session_if_ready`'s `Ready` assignment, the HID-channel-
  closed-while-Ready recovery path, plus the three existing timeout
  disconnect sites and the disconnection-complete handler) and at the
  wolwifi.cpp trigger/resend-cycle call sites.
- New `COMPANION_REPORT_WOL_TRACE` (`0x0B`, the one free slot between
  `0x0A` FEEDBACK_TRACE and `0x0C` WOL_DEBUG_STATUS) + `build_wol_trace()`
  in `companion.cpp`, packing 8-byte records
  (u16 sequence + u32 timestamp_ms + stage + detail) per report, following
  `build_trigger_trace()`'s paging pattern exactly.
- Companion app: `parseWolTraceReport()`/`wolTraceStageLabel()` in
  `protocol.ts`; `readWolTraceThrottled()` polls every tick (like
  `readWolDebugStatus()`) and drains up to 32 reports per poll (matching
  `TRIGGER_TRACE_MAX_READS_PER_POLL`), appending `event=board-trace` lines
  to the *same* `ds5bridge-wol-debug.log` file the live WOL debug log
  already writes to -- one file, one place to look, rather than a second
  log or a Diagnostics-tab-only view (deferred Phase 11a's UI-facing trace
  view; this is UI-less by design, matching the "keep it in the log file,
  not the settings UI" pattern already established for the debug Ping/WOL
  Test row).

**Status:** implemented, tested (typecheck + full companion test suite
both pass), debug firmware rebuilt at `build/waveshare/ds5-bridge.uf2`,
companion app repackaged at
`companion/artifacts/DS5 Bridge-win32-x64-2026-08-10T01-55-26-806Z`. Not
yet used against a real failure -- this is diagnostic tooling, not a fix
for the underlying "WOL never starts" bug itself, which remains
unconfirmed pending the next test with this trace available to read.

---

## 2026-08-10 — Sixth bug (the real one): USB-suspend controller power-off fires before WOL can finish

**Context:** after confirming (via a real test) that the resend cycle,
ARP confirmation, and lightbar indicator all worked correctly, the user
reported the original symptom was still unresolved: the controller
doesn't stay connected to the board until the PC wakes. This meant the
BT/Wi-Fi radio-contention fix (fourth bug, above) was not actually the
cause of the original report -- it was a real, separate bug worth fixing
in its own right (RF contention during a fresh Wi-Fi connect racing a
fresh BT connection is real and was observed), but not *this* symptom.

**Root cause:** `usb.cpp` has a pre-existing, WOL-unrelated feature:
`usb_pm_poll()` calls `bt_power_off_controller()` (sends the DualSense's
own BT feature-report 0x08 power-off, not just an HCI disconnect) roughly
`USB_SUSPEND_POWEROFF_DEBOUNCE_US` (3s) after `tud_suspend_cb()` fires,
gated by the `usb_suspend_disconnect_enabled` setting (default `true`,
exposed in the companion app as "USB suspend disconnect" --
`companion.cpp` `CommandSetUsbSuspendDisconnectEnabled` /
`protocol.ts` `SET_USB_SUSPEND_DISCONNECT_ENABLED`). This exists to save
the controller's battery when the host goes to sleep and nothing needs it
connected -- a reasonable feature on its own. But it fires unconditionally
on any USB suspend, including the exact moment WOL needs the controller to
stay present: the target PC's USB disappearing *is* the trigger that
starts the WOL attempt in the first place, so the 3s power-off debounce
was racing (and always beating) the up-to-15s resend cycle, and even the
2s connect-start delay in the worst case.

**Fix:** asked the user whether to suppress this power-off only during an
active WOL attempt, or for the entire time WOL is enabled; chose the
narrower option. Added `wolwifi_wake_in_progress()` to `wolwifi.h`/`.cpp`
(true while `g_resend_active`, or `g_send_pending` for the gap between a
controller-connect trigger and the resend cycle actually starting once
Wi-Fi comes up) and check it in `usb_pm_poll()` before calling
`bt_power_off_controller()` -- skip the call while a wake is in progress,
but leave `usb_suspend_at_us` set so the check re-runs every tick and the
power-off still fires normally the moment the wake ends (confirmed awake
or resend budget exhausted), rather than being permanently skipped.

Scoped narrowly on purpose: the controller still gets its normal
battery-saving auto-off whenever WOL isn't actively in the middle of an
attempt (including whenever WOL is simply enabled but no controller-connect
trigger has fired), consistent with the earlier "don't add complexity
beyond what's needed" calls in this same session (the ARP-liveness-skip
and no-distinct-failure-color decisions above).

`wolwifi.h` is designed so callers never need an `#ifdef` around it
(no-op stubs on boards without `ENABLE_WOLWIFI`); extended that existing
pattern to `usb.cpp` (which didn't previously depend on `wolwifi.h`) the
same way `bt.cpp`/`main.cpp`/`companion.cpp` already do.

**Status:** implemented and committed, debug firmware rebuilt at
`build/waveshare/ds5-bridge.uf2`. This is believed to be the actual fix
for the original reported symptom (controller not staying connected until
wake) -- the fourth-bug radio-contention fix and fifth-bug resend fix were
both real improvements but didn't address this specific mechanism. Not yet
verified against a real PC-off test.

---

## 2026-08-10 — Pulsing lightbar indicator for an in-flight WOL send (Phase 11b, revised scope)

**Context:** Phase 11b (see the plan entry further below) had originally
scoped a single flash-and-restore, matching the existing controller-wake
blue flash. Once the resend-until-confirmed fix (above) turned "sending
WOL" into a real multi-second-to-15-second window rather than one instant,
the user asked for a richer sequence reflecting that: pulsing while
waiting, a distinct confirmed state, then restore.

**Design, as requested:**
- Pulsing dark green <-> light green while a resend cycle is actively
  sending/waiting for confirmation.
- Solid light green once the PC is confirmed awake, held briefly, then
  restore to whatever color was showing before.
- Asked the user to choose the trigger mapping and pulse speed rather than
  guessing: confirmed "pulse starts at first send, confirmed = solid light
  green then restore, no distinct failure color" over a variant that also
  flashed a distinct color on budget-exhausted/no-confirmation (kept
  it a purely positive signal, consistent with the earlier decision not to
  ARP-check-then-skip on the automatic trigger -- same philosophy: don't
  add negative/uncertain-outcome UI for a background feature). Chose
  ~1.5s per pulse cycle over a faster ~0.8s "urgent" pulse -- a gentle
  breathing indicator, not an alert.

**Implementation:** the original Phase 11b plan (see below) already
identified the core problem -- `bt_set_lightbar_color()` overwrites its
own restore target (`saved_lightbar_*`) on every call, so nothing can
snapshot "what was showing before" without a dedicated accessor. Solved
with a separate snapshot (`wol_indicator_pre_red/green/blue/brightness` in
`bt.cpp`) captured once in `bt_wol_indicator_begin()`, independent of
`saved_lightbar_*`/`lightbar_restore_pending` -- necessary because the
pulse animation itself calls `bt_set_lightbar_color()` on every tick
(that's the only way to actually change what's displayed), which would
otherwise immediately overwrite any snapshot taken via the normal
mechanism.

Four-function API (`bt_wol_indicator_begin/confirm/cancel/loop()`) rather
than the originally-planned single
`bt_flash_lightbar_and_restore(r,g,b,brightness,duration_ms)` helper --
that shape fit a one-shot flash, not a multi-stage animation that needs to
keep updating every tick and can end via two different paths (confirmed
vs. budget-exhausted). `bt_wol_indicator_loop()` polled from `main.cpp`'s
existing `Lightbar` phase, alongside `bt_lightbar_loop()`.

Wired into `wolwifi.cpp`'s resend cycle, not `send_magic_packet_now()`
itself (contrary to the original Phase 11b firing-point plan) -- the
resend cycle is now the thing with a clear begin/end/confirm lifecycle;
`send_magic_packet_now()` is called repeatedly within one cycle and isn't
the right granularity for "start pulsing" or "stop and restore" anymore.
Debug WOL Test button intentionally excluded (unchanged, single send, no
indicator) -- same reasoning as the resend fix: it's on-demand manual
tooling, not the reliability/UX-critical automatic path.

**Status:** implemented and committed, debug firmware rebuilt at
`build/waveshare/ds5-bridge.uf2`. Not yet visually verified on real
hardware.

---

## 2026-08-10 — Fifth bug/gap: single magic-packet send has no delivery guarantee; resend until confirmed

**Context:** after the connect-start-delay fix (below), the user re-ran
the real PC-off automatic-trigger wake test and the PC still didn't wake,
even though the debug log showed a clean Wi-Fi connect and a
`result=success` send. On inspection, that particular log entry was
tagged `event=debug-action action=send-wol` -- the *manual* WOL Test
button, run after the fact once the PC was confirmed back on to check the
log, not the automatic trigger from the actual off-PC attempt. The
automatic path's own log line from the real attempt wasn't available to
inspect this round.

That ambiguity aside, the underlying design was genuinely fragile
regardless of which specific attempt failed: `wolwifi_on_controller_connect()`
(directly, or via `g_send_pending` once Wi-Fi comes up) only ever called
`send_magic_packet_now()` once. A UDP magic packet has no delivery
guarantee, no ack, and no retry -- if it's lost (dropped by the AP, lost
during the brief post-connect reconnect flap already observed once in
testing, or simply missed by the target NIC's WOL listener), there is no
recovery. A "worked once in manual testing" result doesn't mean the
automatic path is reliable.

**Decision:** resend the magic packet periodically until either the
target confirms it woke up, or a time budget runs out, rather than firing
once and hoping. Asked the user for the retry shape; chose "retry every 3s
for 15s, stop early on ARP confirm" over a simpler fixed-retry-count
option with no liveness detection.

**Implementation** (`wolwifi.cpp`):
- `WOL_RESEND_INTERVAL_MS = 3000`, `WOL_RESEND_TOTAL_BUDGET_MS = 15000`.
- `begin_resend_cycle()`: sends immediately and arms the cycle
  (`g_resend_active`, `g_resend_started_ms`, `g_resend_last_sent_ms`,
  resets `g_target_confirmed_awake`). Called from both branches of
  `wolwifi_on_controller_connect()` (immediate case) and from the
  `g_send_pending` handling in `WifiState::WaitingForIp` (deferred case),
  replacing the old single `send_magic_packet_now(false)` calls in both
  places.
- `drive_resend_cycle()`: called every `wolwifi_task()` tick, independent
  of `g_wifi_state` -- a resend can span a Wi-Fi link drop/reconnect, and
  the cycle's own budget/confirmation logic (not the connect state
  machine) decides when to stop. Each interval also broadcasts a fresh
  `etharp_request()` before resending, to actively prompt a reply instead
  of only listening for ambient ARP chatter.
- Liveness confirmation reuses `arp_snoop_input()` -- the same
  ARP-snoop-on-inbound-Ethernet-frames mechanism the debug Ping button
  already uses to detect a target's MAC on the wire -- generalized with a
  separate `g_resend_active` gate alongside the existing debug-Ping gate,
  so a debug Ping and an automatic resend cycle running back to back can't
  clobber each other's result.
- Debug WOL Test button (`wolwifi_debug_send_wol()`) intentionally
  unchanged: still a single fire-and-forget `send_magic_packet_now()`
  call, since it's a manual on-demand smoke-test action, not the
  reliability-critical automatic path.

**Alternative considered and rejected:** skip sending on the automatic
trigger entirely if an ARP liveness check shows the target is already
awake (raised again in this context, having been rejected once already
for a different reason -- see the entry below). Still not adopted here:
this fix is about *guaranteeing delivery of a needed packet*, not avoiding
an unneeded one; the ARP watch here is used only to know *when to stop*
resending, not to decide *whether to send at all*.

**Status:** implemented and committed, debug firmware rebuilt at
`build/waveshare/ds5-bridge.uf2`. Not yet verified against a real PC-off
automatic-trigger test (pending user retest; this fix and the previous
connect-start-delay fix are both awaiting the same next real test).

---

## 2026-08-10 — Fourth bug: CYW43 Wi-Fi/Bluetooth radio contention drops the controller during a real WOL trigger

**Context:** after the first successful WOL Test (via the manual debug
button, Wi-Fi already connected), the user reported that a real
automatic-trigger WOL left the controller disconnected from the board
until the PC was already awake -- unlike the existing "Wake PC on
Controller" (USB) feature, which the user believed kept the controller
connected through a wake.

**Research:**
- `gh pr diff` on `SundayMoments/DS5_Bridge#93` (USB Wake Feature) and a
  reading of the existing "Wake PC on Controller" implementation in
  `bt.cpp`/`main.cpp`: neither actually keeps the *Bluetooth* link itself
  connected during a USB suspend/wake cycle. "Wake PC on Controller" holds
  the USB-enumerated persona on the bus during suspend and calls
  `tud_remote_wakeup()` from the BT ACL-connect HCI event; PR #93 uses a
  different, keyboard-HID-tap-based mechanism. In both cases the BT link
  itself always drops and reconnects fresh -- there was no existing
  "keep BT connected through wake" pattern to reuse, contrary to the
  initial assumption.
- `gh pr diff` on `awalol/DS5Dongle#207` (independent prior-art
  WOL-over-Wi-Fi implementation for the same CYW43439 combo chip),
  `#186`, `#136`: confirmed `cyw43_tcpip_link_status()` (not
  `cyw43_wifi_link_status()`) is the correct call, matching this repo's
  bug-2 fix. Its `Observe` state (checking `tud_mounted() &&
  !tud_suspended()` to see if the PC is already on before doing anything)
  incidentally imposes a ~3s delay before Wi-Fi starts, though for an
  unrelated reason (waiting to see if WOL is even needed), not explicit
  BT-settling. Also revealed two lwIP options this repo didn't have set:
  `DHCP_DOES_ARP_CHECK=0` and `LWIP_DHCP_DOES_ACD_CHECK=0`, which skip the
  DHCP client's post-lease ARP conflict-detection probe and shorten the
  handshake.

**Root cause:** `wolwifi_on_controller_connect()` fires at the exact
BT-connection-Ready edge (see the 4c call-site decision below). If Wi-Fi
isn't already connected, the very next `wolwifi_task()` tick immediately
starts a full WPA2 association + DHCP handshake -- comparatively heavy
RF/firmware-scheduling activity on the CYW43439, which time-shares one
radio between Wi-Fi and Bluetooth. That fresh Wi-Fi activity, starting the
instant the BT session is still stabilizing, contends for the shared radio
and was observed dropping the controller.

**Fix:** delay the *start* of the Wi-Fi connect sequence (not just the
eventual magic-packet send, which was already deferred via
`g_send_pending`) by `WIFI_CONNECT_START_DELAY_MS = 2000` after a
controller-connect edge. Implemented as `g_connect_start_not_before_ms`,
set in `wolwifi_on_controller_connect()` and checked in the `Idle` case of
`wolwifi_task()`'s state machine before calling `start_wifi_connect()`.
User chose 2 seconds over 5 seconds when asked -- WOL is not time-critical
enough to need faster, but 2s should be enough for a BT session to
stabilize without a needlessly long delay.

Scoped narrowly:
- Only applies when Wi-Fi isn't already connected. If a prior session's
  Wi-Fi link is still up (e.g. a second controller connect shortly after
  the first), there's no new radio activity about to start, so the packet
  still sends immediately -- no reason to delay when there's nothing to
  contend with.
- Does not apply to the debug Ping/WOL Test buttons, which are
  user-initiated on demand rather than tied to a fresh BT-connect event,
  so there's no adjacent BT session to protect.

Also applied the `DHCP_DOES_ARP_CHECK`/`LWIP_DHCP_DOES_ACD_CHECK` options
found via the #207 research to `boards/headers/lwipopts.h` as a
complementary shrink of the same contention window (shorter DHCP handshake
= less time Wi-Fi RF activity overlaps with a fresh BT session), since it
was a low-risk, directly-relevant finding from the same research pass.

**Alternative considered and rejected:** skip sending the magic packet
entirely if an ARP check shows the target host is already awake (reusing
the debug Ping's ARP-snoop mechanism). Asked the user; rejected in favor
of always sending unconditionally on the automatic trigger -- a stray
magic packet to an already-on host is harmless (the NIC just ignores it),
so the added latency (waiting out an ARP timeout) and complexity isn't
worth it for a trigger that isn't time-critical either way. The ARP-based
liveness check remains debug-tooling-only (the existing Ping button).

**Status:** implemented and committed, debug firmware rebuilt at
`build/waveshare/ds5-bridge.uf2`. Not yet verified against a real PC-off
automatic-trigger test (pending user retest).

---

## 2026-08-09/10 — Three real Wi-Fi bugs found and fixed via the debug tooling

**Context:** the first hardware smoke test failed silently (PC off,
controller connected, no wake, no way to diagnose since the target PC and
the companion-app PC were the same machine). Building the debug Ping/WOL
Test tooling (documented separately below) and iteratively expanding its
diagnostics surfaced three distinct, real firmware bugs in sequence,
rather than one root cause -- worth recording each since they're the kind
of thing a future rebase/PR reviewer would want to know were found and
why the fixes look the way they do.

**Bug 1 -- SSID/password length never sent.** `setWolWifiSsid`/
`setWolWifiPassword` in `bridge-service.ts` hardcoded the command's wire
`value` field (which firmware reads as the exact byte count to copy from
the payload) to `0`, instead of the actual payload length. Every SSID/
password update silently told firmware "here are 0 bytes" -- the command
still acked OK and `settings_revision` incremented, so nothing in the UI
indicated a problem, but `wolwifi_set_wifi_ssid(ssid, 0)` sets
`have_ssid = false` regardless of what was in the payload. Caught by
adding `have_ssid`/`have_target_mac`/`wol_enabled` flags to the debug
status: `have_target_mac` was true (fixed 6-byte field, no length
ambiguity) while `have_ssid` stayed false despite a correct SSID typed in
the UI. Fixed by sending `payload.length` as the command value at both
call sites (the direct setters and the reconnect-reapply path). No
existing test caught this; added a regression test asserting the wire
value byte matches the payload length, not just that a command with the
right ID gets sent.

**Bug 2 -- polling the wrong CYW43 link-status function.**
`wolwifi_task()` polled `cyw43_wifi_link_status()` (`cyw43_ctrl.c`) to
decide when to advance from Connecting to WaitingForIp to Connected. That
function only reflects the low-level radio join state
(`WIFI_JOIN_STATE_*`) -- per its own switch statement it can return
`CYW43_LINK_JOIN/FAIL/NONET/BADAUTH/DOWN`, but it can **never** return
`CYW43_LINK_NOIP` or `CYW43_LINK_UP`, regardless of DHCP/IP status. Every
one of our state-machine guards keyed off `status == CYW43_LINK_UP`,
which that function could never produce, so every connect attempt
eventually timed out or was judged "link lost" and restarted from
scratch -- even while genuinely associated with a bound DHCP lease the
whole time. Caught by adding lwIP's own DHCP client state
(`struct dhcp.state` via `netif_dhcp_data()`) to the debug status:
`dhcp_state=bound` (a real, successful lease) while `raw_link_status`
stayed stuck at `join` and `WifiState` cycled connecting -> failed ->
connecting forever was the tell. Fixed by switching all three call sites
to `cyw43_tcpip_link_status()` (`cyw43_lwip.c`), the documented "superset"
function that actually checks `netif->ip_addr`.

**Bug 3 -- no cleanup before reconnecting.** After fixing bug 2, a fresh
boot's first connect attempt succeeded correctly, but a *retry* after a
genuine link drop failed immediately (`raw_link_status=fail`,
`dhcp_state=init`) where the first attempt had worked. Root cause:
`cyw43_arch_wifi_connect_async()` -> `cyw43_wifi_join()` does not clean up
a prior association itself -- the SDK docs describe `cyw43_wifi_leave()`
as a separate, caller-managed disassociation step, and `start_wifi_connect()`
never called it. Retrying was joining on top of stale driver join state.
Fixed by calling `cyw43_wifi_leave()` before every reconnect attempt after
the first (skipped on the very first attempt, since there's nothing to
leave yet).

**Process note:** after finding bug 2, the user asked to stop adding one
diagnostic field per guess and add comprehensive diagnostics in one pass
instead -- see the "comprehensive diagnostics" work: raw
`cyw43_state.wifi_join_state` bitmask, four lifetime counters
(`connect_attempt_count`, `wifi_connect_timeout_count`,
`dhcp_timeout_count`, `link_lost_count`) that persist across the whole
session rather than only reflecting the latest event, and automatic
link-state-change logging (not just logging when a debug button's result
settles) so a connection attempt mid-flight at the moment of a button
click doesn't leave a gap in the log with no record of what happened next.

**Result (2026-08-10):** first successful end-to-end WOL Test --
`result=success`, `link=connected`, `raw_link_status=up`, magic packet
sent. A brief ~2.5s reconnect flap occurred right at send time
(`connect_attempts` jumped 2->4 in under a second) but self-healed via the
bug-3 fix and the send succeeded on the retry.

---

## 2026-08-09 — WOL trace log + lightbar pulse: research and scope (Phase 11, planned)

**Context:** the debug Ping/WOL Test feature (previous entries) proved
useful for the second real smoke test, but two gaps showed up: (1) its log
is a flat on-disk file you have to open and read by hand, no in-app trace
view; (2) there's no physical/visual confirmation in the room that a WOL
packet actually got sent when a controller connects -- you only find out
by opening the companion app.

**Research findings** (full detail in an Explore-agent pass, not
reproduced here): the app already has a ring-buffer trace pattern (Trigger
Trace, Feedback Trace) -- `TriggerTraceEvent ring[]` in `companion.cpp`,
sequence-numbered with a dropped-count, packed multiple-per-report via
`build_trigger_trace()`, polled via `readTriggerTraceThrottled()`, and
rendered as a scrollable read-only `<textarea>` in the Diagnostics tab
(`App.tsx`'s `.debug-entry` rows). The lightbar already has a working
"flash a color, then re-apply" primitive (`bt_set_lightbar_color()` +
`bt_schedule_lightbar_restore()`, used today for the controller-wake flash
at `bt.cpp:3900-3904`) -- but that primitive restores to "whatever was
last explicitly set," not a true snapshot of the prior color, since
`bt_set_lightbar_color()` overwrites `saved_lightbar_*` immediately when
called.

**Decisions:**
- **WOL trace**: build the full ring-buffer + new report type pattern
  (matching Trigger/Feedback Trace exactly), not the lighter "tail the
  existing on-disk debug log into a textarea" option. Chosen because the
  on-disk log only captures actions triggered through the companion app's
  Ping/WOL Test buttons -- it has no visibility into automatic
  WOL-on-controller-connect events, which is exactly the case that matters
  most (the debug buttons are for testing, the real feature is the
  automatic trigger). A ring-buffer report captures both.
- **Lightbar pulse trigger point**: inside `send_magic_packet_now()` in
  `wolwifi.cpp`, on an attempted send -- not in
  `wolwifi_on_controller_connect()`. That function can defer the actual
  send via `g_send_pending` until Wi-Fi comes up later in `wolwifi_task()`,
  so pulsing at the trigger point could fire before (or without) an actual
  packet going out.
- **Lightbar restore behavior**: true restore-to-previous-color, not the
  existing flash-then-refresh-same-color semantics the wake-flash uses.
  Requires new firmware work (a snapshot point before the flash, or a
  small `bt_flash_lightbar_and_restore(...)` helper in `bt.cpp`/`bt.h`
  that wolwifi.cpp calls without needing lightbar internals) since no
  "restore to true prior state" primitive exists today.
- **Lightbar pulse policy**: always fires when WOL is enabled and a send
  is attempted, regardless of `lightbarOverrideEnabled`/
  `lightbarRestoreEnabled`. Treated as a distinct "WOL fired" confirmation
  signal rather than general lightbar behavior those settings govern.
- **Pulse color**: green, distinct from the existing blue
  (`0x00,0x00,0xFF`) controller-wake flash so the two are visually
  distinguishable.

See task.md's Phase 11 for the concrete implementation checklist.

---

## 2026-08-09 — WOL debug Ping/Test feature: motivation and mechanism

**Context:** First real smoke test failed silently -- PC off, controller off,
board on, controller paired, but WOL never woke the PC. Root cause
diagnosis was blocked: the release firmware build has all `DS5_LOG` calls
compiled out (see the "how to diagnose a failed smoke test" doc pass), and
even with a debug-logging build, the companion app -- the only way to read
those logs -- runs on the same PC that WOL is supposed to wake. Once the
target PC is off, there's no PC left to run the companion app on.

**Decision:** Add an on-demand debug row (Ping / WOL Test buttons) so the
whole pipeline (Wi-Fi connect, target reachability, magic-packet send) can
be exercised and verified *while the target PC is on*, building confidence
before relying on it blind.

**Ping mechanism:** lwIP's ARP API (`etharp_query`/`etharp_request`) only
resolves IP->MAC, never the reverse, so there's no direct "ping this MAC"
call. Instead: broadcast an ARP request (to generate traffic) and install a
wrapper around `netif_default->input` (normally `ethernet_input`) that
inspects every inbound frame's source MAC while a ping is in-flight, then
always forwards to the real `ethernet_input` so nothing about normal lwIP
operation changes. A match on the configured target MAC = success.

**Alternatives considered:**
- ICMP ping to a separately-configured IP: rejected, needs a new config
  field and doesn't confirm the *specific target MAC* owns that IP.
- Ship WOL Test only, no ping: rejected, gives no independent signal that
  the target NIC is actually reachable before/after a send.

**Status delivery:** Both the main status report and `firmwareFlags`/
`statusFlags` bytes are fully packed (0 free bytes/bits) -- confirmed this
independently of the earlier `wolControl` capability-flag decision, same
constraint. Added a new `WOL_DEBUG_STATUS` report type instead (same
pattern as `AUDIO_STATUS` being separate from the main status report).

**IP address wire format:** Sent as 4 raw octets (`ip_octets[0..3]`), not a
packed `uint32_t`. lwIP's `ip4_addr.addr` is network-byte-order; this
protocol's `write_u32` is little-endian. Packing/unpacking through a
`uint32_t` would silently produce a byte-reversed IP on the wire depending
on which side got the convention backwards -- raw octets sidestep the
question entirely, same reasoning as the existing MAC-address wire format.

**Log delivery:** A new always-on `ds5bridge-wol-debug.log` file in the
companion app's `userData` directory, written on every settled Ping/WOL
Test result -- deliberately independent of the existing Firmware UART Log
feature (which requires the user to have already picked a folder via
Diagnostics > Choose Folder). The whole point of this feature is to work
without that prior setup, given the same-PC constraint above.

---

## 2026-08-09 — Build outputs stay in upstream's existing locations, no new dist/ folder

**Decision:** Firmware UF2 stays at `build/waveshare/ds5-bridge.uf2` (or
`build/<name>/ds5-bridge.uf2` for other CMake `-B` dirs); companion installer
stays at `companion/artifacts/installer/`; companion portable package stays
at `companion/artifacts/DS5 Bridge-win32-x64-<timestamp>/`. No new root-level
`dist/` folder was added, even though one was briefly implemented (build
script copy step, electron-builder output path change, `.gitignore` entry)
before being reverted.

**Why:** A new output convention that upstream doesn't already use is pure
rebase friction — every future backport would need to remember to re-apply
it, for no functional benefit over just knowing where the existing outputs
already land. See AGENTS.md's "Build output locations" section for the
authoritative reference of where each output currently goes.

**Alternatives considered:** Root `dist/firmware/` + `dist/companion/`
(initially implemented, then reverted per user feedback: "let's use what the
original repo uses for build folders... so in the future we won't need to
tweak too much when backporting from new versions").

---

## 2026-08-09 — WOL UI capability gate: protocol-minor version, not a new status byte

**Decision:** Gate the WOL settings UI on `firmwareFlags.wolControl`, computed
as `report[6] >= 20` (protocol minor version), the same pattern already used
for `wakeOnConnectControl`/`audioReactiveHapticsControl`. Bumped
`PROTOCOL_MINOR`/`kProtocolMinor` from 19 to 20 in both `protocol.ts` and
`companion.cpp`.

**Why:** The alternative -- a real board-capability bit -- would require a
new byte in the fixed-size status report (`firmwareFlags`/`statusFlags` are
both already fully packed at 8/8 bits each), a wire-format change touching
every feature that reads that report. User chose the simpler,
lower-risk version gate, accepting the known tradeoff: a non-Waveshare board
running firmware >= 1.20 will show the WOL section as "supported" in the UI
even though it has no effect there (firmware without `ENABLE_WOLWIFI`
compiled in no-ops on every WOL command per `wolwifi.h`'s stubs). Acceptable
since sending WOL commands to an unsupported board is harmless, just inert.

**Alternatives considered:** New status-report capability byte with a
dedicated `wolControl` bit -- rejected as disproportionate for what is, in
practice, a soft UI nicety (worst case if wrong: an inert toggle).

---

## 2026-08-09 — Wake-on-Connect (USB wake) already exists in port-dev

**Discovery, not really a decision:** While wiring the companion app,
found that `port-dev` already ships a real, merged USB wake-on-controller-
connect feature (`SET_WAKE_ON_CONNECT = 0x35`, `wakeOnConnectEnabled` in
`CompanionSettings`, `usb_set_wake_on_connect()` in firmware) -- separate
from and unrelated to PR #93's *proposed* (unmerged) `SET_WAKE_ENABLED`
USB-wake feature. Used the real, merged `wakeOnConnectEnabled` end-to-end
wiring (settings-store, bridge-service setter + reapply, main.ts, preload.ts,
App.tsx toggle row) as the direct template for WOL's settings plumbing and
UI row, since it's the actual current pattern in this codebase -- more
relevant than PR #93's version, which was already established as
reference-only (see the PR #93 entry below).

**Why this matters for rebases:** If upstream ever merges PR #93's
`SET_WAKE_ENABLED` (`0x34` in their branch), it will collide with nothing
here since we never used `0x34` -- our WOL commands start at `0x37`, after
`SET_LIGHTBAR_RESTORE_ENABLED = 0x36`, the last command ID actually present
in `port-dev` at the time of writing.

---

## 2026-08-09 — SSID/password use variable-length payload; target MAC uses fixed 6-byte format

**Decision:** `SET_WOL_WIFI_SSID`/`SET_WOL_WIFI_PASSWORD` carry their byte
length in the command's `value` field and the string bytes in the
variable-length payload area (10 bytes after report start, ~53 bytes
available) -- the same framing `SET_CHORD_BINDINGS` uses. `SET_WOL_TARGET_MAC`
instead reuses the fixed 6-byte format already used for controller Bluetooth
addresses (`bluetoothAddressPayload` in `protocol.ts`), since a MAC is
always exactly 6 bytes and doesn't need a length prefix.

**Why:** Firmware's companion HID report is a fixed 63-byte payload
(`COMPANION_PAYLOAD_SIZE`), not a truly variable-length transport -- "reuse
SET_CHORD_BINDINGS's payload pattern" (from the Phase 2 research) means
reusing this fixed-report-with-trailing-data framing, not literal variable
HID report sizes. Password's wire budget (53 bytes after the 10-byte header)
is below WPA2's 63-character passphrase max, so `WOL_WIFI_PASSWORD_MAX_LENGTH`
is capped at 53, not 63, and firmware's `MAX_PASSWORD_LEN` matches.

**Also:** the original `wolwifi_set_wifi_credentials(ssid, ssid_len,
password, password_len)` combined setter was replaced with independent
`wolwifi_set_wifi_ssid()`/`wolwifi_set_wifi_password()`, since the companion
app always sends SSID and password as separate commands -- a combined
setter would silently wipe whichever field wasn't included in a given call.

---

## 2026-08-09 — Full Wi-Fi/lwIP bring-up now, not a separate spike branch

**Decision:** Do the CYW43 Wi-Fi/lwIP bring-up directly inside the
`feature/wol-wifi` branch alongside the rest of the feature, rather than
building a throwaway standalone test firmware first.

**Why:** `port-dev`'s `CMakeLists.txt` currently sets `CYW43_LWIP=0` — the
CYW43439 chip is wired up for Bluetooth only via `pico_btstack_cyw43`; no
lwIP/Wi-Fi stack exists anywhere in the repo yet. This is the largest
technical risk in the plan (BT + Wi-Fi sharing one SPI bus, with the repo's
existing SRAM-relocation tuning aimed at BT/USB latency). User chose to do
this as full bring-up in the real branch rather than a separate isolated
spike, accepting the larger/riskier scope in exchange for not needing to fold
a spike back in later.

**Alternatives considered:** Isolated minimal spike firmware (Wi-Fi connect +
magic packet only, no BT/controller code) to de-risk coexistence before
touching `bt.cpp`/companion app. Rejected by user preference.

---

## 2026-08-09 — Branch base: `feature/wol-wifi` off `upstream/port-dev`

**Decision:** Create the feature branch directly from `upstream/port-dev`
(not a new long-lived integration branch), pushed to `origin`
(`gunCannoli/DS5_Bridge`).

**Why:** Matches the plan's stated baseline (`port-dev` is the real
implementation target, not `main`). Keeps history simple for the eventual PR
into `port-dev`.

---

## 2026-08-09 — PR #93 is reference only, not a mergeable base

**Decision:** Treat upstream PR #93 (`USB Wake Feature`, `feature/enable-wake`
branch, based on `main`) purely as an architectural reference. Do not attempt
to cherry-pick or merge it.

**Why:** Verified `wake.cpp`/`wake.h` do not exist in `port-dev` — PR #93 is
open/unmerged and based on `main`, which has diverged from `port-dev`. Its
value here is the *pattern*: an isolated `wake.h/.cpp` module, gated by a
CMake `option()`, hooked into `bt.cpp`'s `finish_hid_session_if_ready()` at
the point of BT connection-phase transition to `Ready`. We reuse that pattern
for `wolwifi.h/.cpp` and hook at the same call site.

---

## 2026-08-09 — Config transport: reuse variable-length command payload pattern

**Decision:** Transmit Wi-Fi SSID / password / target MAC from the companion
app to firmware using a variable-length payload appended after the fixed
command header, the same pattern `CommandSetChordBindings` already uses
(`src/companion.cpp`, `valid_chord_bindings_payload(buffer + 10, bufsize - 10,
value)`).

**Why:** All existing companion settings are single-byte bool/enum/numeric
values; there is no existing string-typed setting to copy. The chord-bindings
command is the only existing precedent for a variable-length payload over the
same command channel, so it's the least novel way to add this rather than
inventing a new wire mechanism.

---

## 2026-08-09 — Controller-connect hook point for WOL

**Decision:** Call `wolwifi_on_controller_connect()` from the same site PR #93
used for `wake_on_bt_connect()`: inside `finish_hid_session_if_ready()` in
`src/bt.cpp`, at the point `connection_phase` transitions to
`BtConnectionPhase::Ready`.

**Why:** This fires exactly once per successful BT connection (not per input
report), giving natural edge-triggering without needing PR #93's fuller
suspend-aware state machine (WOL doesn't need to know about USB host-suspend
state, just disconnect->connect transitions).
