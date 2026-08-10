# Decisions

Architecture and technical decisions for the Wi-Fi WOL feature, newest first.
Each entry: decision, rationale, alternatives considered, date.

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
