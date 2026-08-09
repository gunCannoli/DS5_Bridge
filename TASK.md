# Task

Current + next task. Cleared and summarized into changelog.md when the plan
below is fully completed.

## Status

Setup and research (Phase 1-2) complete. Feature definition and Wi-Fi bring-up
scope agreed with user. About to start Phase 3-4 implementation.

## Current task

- [ ] Create branch `feature/wol-wifi` off `upstream/port-dev`, push to `origin`

## Next task

- [ ] Phase 4a: `CMakeLists.txt` — conditionally enable `CYW43_LWIP` and link
      required lwIP libs, scoped to `WAVESHARE_RP2350B_PLUS_W_BUILD` only

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

### Phase 4 — Firmware design

- [ ] 4a. CMake: enable `CYW43_LWIP` conditionally under
      `WAVESHARE_RP2350B_PLUS_W_BUILD`, add `pico_cyw43_arch_lwip_*` (poll or
      threadsafe-background — TBD based on coexistence with BTstack poll
      loop) + lwIP libs. Must not affect other board builds (`pico2_w` etc).
- [ ] 4b. New `src/wolwifi.h` / `src/wolwifi.cpp`, following the `wake.h/.cpp`
      isolation pattern from PR #93: header-only no-op stubs when the feature
      compile flag is off.
      Owns: Wi-Fi connect, WOL target MAC, UDP magic packet construction,
      UDP send, connection/retry handling.
- [ ] 4c. Hook `wolwifi_on_controller_connect()` into `finish_hid_session_if_ready()`
      in `src/bt.cpp` (see decisions.md for exact call site rationale).
      Must be edge-triggered: fire once per DISCONNECTED->CONNECTED
      transition, not per input report, not repeatedly while connected.
- [ ] 4d. `wolwifi_task()` polled from main loop (`src/main.cpp`), non-blocking.
- [ ] 4e. Verify BT latency/stability unaffected by enabling lwIP on CYW43
      (shared SPI bus with BTstack) — this is the main open technical risk,
      see decisions.md.

### Phase 5 — Companion app changes

- [ ] 5a. New `COMMAND_ID`s in `companion/src/shared/protocol.ts` +
      mirrored `enum CommandId` in `src/companion.cpp`
- [ ] 5b. Variable-length payload command(s) for SSID/password/MAC, reusing
      the `SET_CHORD_BINDINGS` payload pattern (see decisions.md)
- [ ] 5c. `wolEnabled` bool command, following existing bool-setting pattern
      (e.g. `SET_WAKE_ENABLED` from PR #93 as reference)
- [ ] 5d. Settings plumbing: `settings-store.ts` (defaults/normalize),
      `bridge-service.ts` (setters + reapply-on-connect list), `main.ts`
      (IPC handlers), `preload.ts` (renderer bridge)
- [ ] 5e. `App.tsx` UI: enable toggle + 3 text fields under existing
      settings section pattern
- [ ] 5f. MAC address validation: companion-app side (JS, before send) AND
      firmware side (C++, on receive — never trust the app blindly)

### Phase 6 — Build integration

- [ ] 6a. Verify `boards/build_waveshare_rp2350b_plus_w.sh` builds clean with
      WOL enabled
- [ ] 6b. Verify default/other board build (`pico2_w`) still builds clean
      with WOL code compiled out (conditional compilation confirmed working)

### Phase 7 — Smoke test (needs real SSID/password/target MAC from user before running)

- [ ] Test 1: Wi-Fi connects, get IP, check reconnection behavior
- [ ] Test 2: WOL wakes PC from hibernate; from full shutdown if NIC supports
- [ ] Test 3: controller connect fires WOL once
- [ ] Test 4: disconnect/reconnect fires WOL again, no dupes while connected
- [ ] Test 5: existing functionality unaffected (USB, BT stability, input,
      reconnect, no added controller latency)

### Phase 8 — Logging

- [ ] `[WOL]` prefixed logs at: Wi-Fi init, Wi-Fi connected (+ IP), config
      received, WOL enabled/disabled, controller connected, WOL triggered,
      packet sent, failure. Match existing DS5 Bridge logging verbosity/style
      (check `src/firmware_log.cpp` conventions before adding).

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
