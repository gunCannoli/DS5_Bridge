# Task

Current + next task. Cleared and summarized into changelog.md when the plan
below is fully completed.

## Status

Setup, research (Phase 1-2), feature definition (Phase 3), and firmware Wi-Fi/WOL
implementation (Phase 4) complete and committed on `feature/wol-wifi`. Build
integration verified for both the Waveshare target (WOL on) and the default
`pico2_w` target (WOL compiled out, zero wolwifi symbols) — Phase 6a/6b done
ahead of schedule since it was the natural way to validate Phase 4.

## Current task

- [ ] Phase 5a: new `COMMAND_ID`s in `companion/src/shared/protocol.ts` +
      mirrored `enum CommandId` in `src/companion.cpp`

## Next task

- [ ] Phase 5b: variable-length payload command(s) for SSID/password/MAC,
      reusing the `SET_CHORD_BINDINGS` payload pattern (see decisions.md)
- [ ] Phase 5c: `wolEnabled` bool command, following existing bool-setting
      pattern (e.g. `SET_WAKE_ENABLED` from PR #93 as reference)

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
