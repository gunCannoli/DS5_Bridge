# Decisions

Architecture and technical decisions for the Wi-Fi WOL feature, newest first.
Each entry: decision, rationale, alternatives considered, date.

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
