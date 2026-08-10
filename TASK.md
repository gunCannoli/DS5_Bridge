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

## Current task

- [ ] **Re-attempt the real PC-off wake test** (original Phase 7 Test 2):
      with the target PC OFF (hibernate first, then full shutdown if the
      NIC supports it) and the board on a powered USB hub as before, connect
      the DualSense and confirm the PC actually wakes via the automatic
      WOL-on-controller-connect trigger (not the manual debug buttons this
      time -- this is the real feature path). Note: the target PC being off
      means no companion app / log access during the test itself; check the
      log/companion app only after, once the PC is back up.

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

**11b. Lightbar pulse when a WOL magic packet is actually sent:**
- [ ] Firing point: inside `send_magic_packet_now()` in `wolwifi.cpp`, on
      an attempted send (chosen over firing in
      `wolwifi_on_controller_connect()`, since that can defer the actual
      send until Wi-Fi comes up later via `g_send_pending` -- pulsing at
      the trigger point could fire without a packet ever going out, or
      fire well before the real send if Wi-Fi is slow to connect).
- [ ] Restore behavior: true restore-to-previous-color was chosen over the
      existing flash-then-refresh-same-color pattern PR #93 uses for
      controller-wake (`bt.cpp:3900-3904`) -- `bt_set_lightbar_color()`
      currently overwrites `saved_lightbar_*` immediately, so there's no
      existing way to snapshot "what it was before". Needs either: (a) a
      small new accessor exposing current `saved_lightbar_*` before
      overwriting, captured by wolwifi.cpp/companion.cpp right before the
      flash call, then explicitly restored via `bt_set_lightbar_color()`
      again after the delay instead of relying on
      `bt_schedule_lightbar_restore()`'s current "restore to last-set"
      semantics; or (b) a small change to `bt_schedule_lightbar_restore`
      itself to snapshot before the caller's `bt_set_lightbar_color` call
      (needs call-order changes at the one existing call site too, so
      wake-flash doesn't regress).
- [ ] New function `bt_flash_lightbar_and_restore(r, g, b, brightness,
      duration_ms)` (name TBD) in `bt.cpp`/`bt.h` combining
      snapshot+flash+scheduled-true-restore in one call, so `wolwifi.cpp`
      doesn't need to know lightbar internals -- just calls this once, as
      a thin dependency on `bt.h` (already an established pattern;
      `companion.cpp` already depends on both modules).
- [x] Policy decided: always fires when WOL is enabled and a send is
      attempted, regardless of `lightbarOverrideEnabled`/
      `lightbarRestoreEnabled` -- it's a distinct "WOL fired" confirmation
      signal, not general lightbar behavior.
- [x] Color decided: green (`0x00, 0xFF, 0x00` or similar), distinct from
      the existing blue (`0x00, 0x00, 0xFF`) controller-wake flash.

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
