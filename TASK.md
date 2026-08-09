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

## Current task

- [ ] Re-run Phase 7 smoke test using the new debug tooling: with the
      target PC ON, flash the debug-logging firmware + install the updated
      companion app, use Ping first (confirms Wi-Fi connects and the
      target NIC answers ARP) then WOL Test (confirms the send path
      works) before trusting an actual PC-off wake attempt again. Check
      `ds5bridge-wol-debug.log` in the companion app's userData folder
      after each click regardless of pass/fail.

## Next task

- [ ] Once Ping/WOL Test both succeed with the PC on, re-attempt the
      original PC-off wake test and use whatever the debug tooling showed
      (or didn't show) to narrow down where the real failure was.
- [ ] Phase 9: verify failure-behavior claims at test time (non-blocking
      design is already in place per 4d/4e, needs runtime confirmation)
- [ ] Phase 10: PR prep — write PR description, fill in testing checklist
      once Phase 7 runs, review for unrelated changes before opening PR.
      Note: the WOL debug feature (Ping/WOL Test) is a reasonable candidate
      to keep in the upstream PR too, not just as our local dev tooling --
      decide before opening the PR whether to present it as part of the
      feature or strip it as internal-only.

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
