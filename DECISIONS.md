# Decisions

Architecture and technical decisions for the Wi-Fi WOL feature, newest first.
Each entry: decision, rationale, alternatives considered, date.

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
