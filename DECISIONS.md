# Decisions

Architecture decisions and known-issue/root-cause knowledge for the Wi-Fi
WOL feature fork — the durable stuff a future agent or PR reviewer needs to
not re-derive or re-discover. Not a running log of every bug fixed or every
test run; see `CHANGELOG.md` for that history. Newest first.

---

## Known issue: merging/rebasing onto a new upstream release can collide `COMMAND_ID` values — always check for gaps, don't just append

**What happened (2026-08-15, merging upstream v1.7.0):** upstream added
`SET_RADIAL_DEADZONES` and `SET_EDGE_PROFILE_SWITCHING_BLOCKED` at `0x37`/
`0x45` between when this fork branched and v1.7.0 shipped. This fork's own
`SET_WOL_ENABLED`/`SET_WOL_WIFI_SSID`/`SET_WOL_WIFI_PASSWORD`/
`SET_WOL_TARGET_MAC` had independently claimed `0x37`-`0x3A` — a silent
collision that `git merge` cannot detect on its own (both sides just add a
new enum value; the same numeric ID ends up meaning two different commands
depending on which side's build you're running). Caught during manual
conflict resolution, not by any automated check.

**Fix applied:** renumbered this fork's four WOL command IDs to
`0x46`-`0x49`, past upstream's highest currently-assigned ID (`0x45`) at
merge time, in both `companion/src/shared/protocol.ts`'s `COMMAND_ID` and
`src/companion.cpp`'s `CommandId` enum (must always be edited together —
see `AGENTS.md`'s protocol-change rebuild table). `PROTOCOL_MINOR`/
`kProtocolMinor` bumped to 23 (upstream's own bump to 22 plus this fork's
independent bump to 21, combined) in both files, plus the hardcoded parity
check in `tests/firmware/usb_descriptor_migration_test.cpp` that asserts
the exact string `constexpr uint8_t kProtocolMinor = N;` — that test will
fail loudly (not silently) if the two files disagree, which is useful, but
it still needs updating by hand to the new expected value after any bump.

**How to apply next time:** before assigning a new `COMMAND_ID` value —
whether adding a WOL feature or rebasing onto a newer upstream release —
grep both `protocol.ts`'s `COMMAND_ID` and `companion.cpp`'s `CommandId`
for every currently-assigned hex value first (`grep -n "0x" ...` on both
enums), take the max, and assign new IDs above it. Don't assume the next
"round" unused number (like `0x3B`) is actually free just because this
fork's own history doesn't use it — upstream may have claimed it since the
last sync. This will recur on every future upstream merge as long as both
sides keep adding commands independently between syncs.

---

## Known issue: `watchdog_enable_caused_reboot()` misses deliberate `watchdog_reboot()` calls

**Symptom that exposed this:** the host-alive gate (see the entry below)
appeared to misfire — `wol-trigger-fired` fired even though the user
confirmed the target PC was genuinely on. The trace showed `board-boot
detail=1` (raw `watchdog_hw->reason` bit0=TIMER set) just ~7 seconds
before the controller reconnected, but **no `board-watchdog-reboot`
trace event appeared**, despite the reason bits clearly showing the RP2's
hardware watchdog fired. Root cause of *that* absence, not the gate: the
board had rebooted moments earlier for a real reason (see below), but
`watchdog_enable_caused_reboot()` — the check gating whether
`BoardWatchdogReboot` gets logged — only returns true when **this
firmware's own periodic `watchdog_enable(1000, true)` call** (`main.cpp`)
caused the reboot; it checks a scratch-register magic value that call
sets, not just the raw hardware reason bits (confirmed via the SDK source,
`hardware_watchdog/watchdog.c`). A reboot via a **direct**
`watchdog_reboot()` call elsewhere in the firmware sets the same raw
reason bits but *not* that magic value, so it's a real watchdog-hardware
reset that nonetheless reads as "not a watchdog-attributed reboot" to that
specific check.

**Where this firmware calls `watchdog_reboot()` directly (four sites, all
in `bt.cpp`, all "bounded transport recovery" — deliberate, not a stall):**
a disconnect retry exhausting `DISCONNECT_RETRY_MAX_ATTEMPTS` attempts
(`service_disconnect_recovery()`), an incoming ACL connection staying
pending past its timeout, and an ACL cancel not completing in time. These
exist to bound how long the firmware will wait stuck in a bad BT transport
state before giving up and resetting for a clean slate — a legitimate
recovery mechanism, but one that was **completely invisible** in the board
trace before this was found: `BoardBoot`'s raw reason bits told you *a*
watchdog reset happened, `BoardWatchdogReboot` told you nothing (false
negative), and nothing else logged the actual cause.

**Fix (diagnostic only so far):** added `WolTraceStage::
BoardTransportRecoveryReboot` (26, detail distinguishes which of the three
sites) at each `watchdog_reboot()` call site, and
`WolTraceStage::ConnDisconnectRetrySent` (27) at the retry-send point so
the escalation leading up to a detail=0 reboot is visible too, not just
the reboot itself.

**Still open:** *why* the disconnect/ACL-pending recovery paths are firing
in the first place during otherwise-normal operation (this reboot happened
during a routine test, PC on, nothing unusual reported) is not yet
diagnosed — this entry only makes the reboot and its immediate cause
visible in the trace; the next real trace with this instrumentation should
show which of the three sites is firing and how often.

---

## Architecture: host-alive gate is a bounded observation window, not an instant check

**Why:** the board's own USB persona is plugged into the *same* PC that WOL
wakes (the deployment model — dongle plugged into the PC you want to
remotely wake), so `usb_host_active()` (`usb.cpp` — `usb_mounted &&
!usb_host_suspended_active()`) is a meaningful signal for whether that PC
needs waking. Skipping WOL (and the visible lightbar pulse) when it doesn't
avoids both a pointless magic packet and unnecessary CYW43 radio activity
(see the radio-contention entry below).

**First version was wrong — checked the signal at the wrong time.** An
earlier iteration called `usb_host_active()` synchronously, once, at the
exact instant `wolwifi_on_controller_connect()` runs. A real test (PC
confirmed on and USB solidly connected the whole time) proved this never
works: this firmware's own USB persona is **session-scoped** —
`usb_mounted` only becomes true after `usb_handle_controller_transport_ready()`
(`usb.cpp`), itself gated on a controller-type-identification L2CAP
handshake that only *starts* after `wolwifi_on_controller_connect()` has
already run (same `Ready`-transition block, `bt.cpp`). So the instant check
was structurally checking a signal that could not yet be true, independent
of the target PC's actual power state — not a timing edge case, a
fundamental design error.

**Corrected design: an `ObserveHost` mini-state-machine
(`begin_observe_host()`/`drive_observe_host()` in `wolwifi.cpp`), matching
`awalol/DS5Dongle#207` and `DevFreezing/DS5Dongle-WoL`'s validated
`Observe` state pattern** (re-read both in depth specifically on this
question — `gh pr diff 207 --repo awalol/DS5Dongle`; local clone of
`DevFreezing/DS5Dongle-WoL`'s `wake-on-lan` branch, removed after reading).
Per the user's explicit correction — *"we need proper signals... events
not timers"* — this is event-driven, not a lookback/recently-seen
heuristic:
- `wolwifi_on_controller_connect()` starts the window
  (`begin_observe_host()`) instead of proceeding immediately.
- `wolwifi_task()` drives it every tick (`drive_observe_host()`), sampling
  `usb_host_active()` live each tick.
- **Default action is to fire WOL.** The window only *aborts* an
  already-armed trigger if the host is positively, continuously observed
  active — it never requires proving the host is off first. If
  `WOL_OBSERVE_HOST_WINDOW_MS` elapses with no sustained-active read, the
  deferred trigger logic (`proceed_with_wol_trigger()`) runs exactly as the
  old immediate path did.
- A single-tick active read isn't enough to abort — it must hold for
  `WOL_OBSERVE_HOST_SUSTAIN_MS` continuously (debounces a transient blip,
  same role as the references' `HOST_ACTIVE_SUSTAIN_US`).

**Why the reference values (3s window / 300ms sustain) don't transfer
directly, and what we used instead:** confirmed via source inspection that
neither reference PR's diff touches `tud_connect()`/`tud_disconnect()` —
their USB persona is *always* enumerated, so a live `tud_mounted()` sample
is meaningful the instant a controller connects. Ours needs the window to
also cover our own session-scoped mount delay, which their design was
never sized for. Measured the real delay on this hardware instead of
guessing or copying: a clean `ConnControllerTypeIdentified` trace event
(added specifically for this) showed the handshake completing in **23ms**
with no radio contention. `WOL_OBSERVE_HOST_WINDOW_MS = 2000` gives ~85x
margin over that (this codebase's own BT-phase timeouts, e.g.
`HID_OPENING_PHASE_TIMEOUT_US`, use 8s as their outer "something is wrong"
bound — 2s is a small fraction of that). `WOL_OBSERVE_HOST_SUSTAIN_MS =
100`.

**Semantics by host power state (unchanged from the original design,
just correctly timed now):**
- PC on, USB active → sustained-active observed within the window → skip.
- PC asleep (S3) → USB stays enumerated but suspended → never reads
  active → window elapses → WOL fires. Harmless (a magic packet to a
  sleeping NIC still wakes it) and doesn't conflict with the separate,
  faster BT-based "Wake PC on Controller" S3 path.
- PC fully off (S5), Pico still powered → USB bus fully torn down → never
  reads active → window elapses → WOL fires. The actual target scenario.

**Trade-off accepted:** this reintroduces a small, bounded delay
(`WOL_OBSERVE_HOST_WINDOW_MS`, worst case) before Wi-Fi starts, but *only*
in the case where the host turns out to be off — in tension with the
explicit "fire the instant the controller connects" requirement from
earlier in this session (the twelfth-bug fix, which removed a *different*,
unconditional 2s pre-delay for BT/Wi-Fi radio-contention reasons). User
explicitly confirmed this specific, narrower delay is acceptable, given
both reference implementations independently converged on the same
trade-off for the same reason. `g_observe_host_active` is included in
`wolwifi_wake_in_progress()` so the USB-suspend controller-power-off
suppression covers this window too, same reasoning as every other WOL
in-progress window.

**Known limitation, with an escape hatch (matches `awalol/DS5Dongle#207`'s
`WOL_ALWAYS` exactly — same name, same mechanism):** some motherboards/BIOS
settings ("power on by USB keyboard/mouse", always-on charging ports) or
Modern Standby (S0ix) keep the USB bus enumerated and active even with the
PC nominally off — on those boards the observation window would always see
"host active" and block every wake. `WOL_ALWAYS` is a **compile-time**
`option()` in root `CMakeLists.txt` (default OFF, only meaningful when
`ENABLE_WOLWIFI` is on) that skips straight to
`proceed_with_wol_trigger()`, bypassing the observation entirely.
Compile-time rather than a runtime companion-app setting deliberately —
this is a board/BIOS hardware quirk workaround, decided once at flash
time, not an everyday user preference; matches `ENABLE_WOLWIFI` itself as
precedent, and avoids a new `COMMAND_ID`/settings-store/UI-row/protocol
surface for something that isn't a normal setting.

**New trace stages:** `WolTraceStage::ConnControllerTypeIdentified` (28,
`bt.h`/`bt.cpp` — the measurement event, elapsed ms since `Ready`) and the
existing `WolTriggerSkippedHostActive` (25) now fires from
`drive_observe_host()`'s sustained-active branch instead of the old
instant check. Diagnostic-only additions for verifying this design:
`usb_host_active_debug_bits()` (`usb.h`/`usb.cpp`, a bitmask of every flag
`usb_host_active()` depends on) plus `ObserveHostBegin`/
`ObserveHostSampleEdge`/`ObserveHostWindowElapsed` (29-31) tracing the
window's live samples.

**Confirmed working on real hardware** (PC on, companion app open): window
armed at `Ready`, `usb_host_active()` flipped true ~217ms later (session
mount + a brief settle), sustained-active correctly detected 100ms after
that (matching `WOL_OBSERVE_HOST_SUSTAIN_MS`) — `wol-trigger-skipped-host-active`
fired, no Wi-Fi connect, no lightbar WOL pulse. Full detect-and-skip cycle
~340ms, well inside the 2s window.

---

## Known issue: CYW43 Wi-Fi/Bluetooth radio contention is a recurring failure mode

The CYW43439 is a combo Wi-Fi/BT chip that **time-shares one radio**.
Almost every hard-to-diagnose bug in this feature's history (see
`CHANGELOG.md`'s fourth, eighth, ninth, tenth, and thirteenth entries) traced
back to some form of this: any Wi-Fi radio activity (association, DHCP,
beacon listening, or disassociation/leave) that overlaps with an active or
settling BT connection can produce a genuine BT link-layer timeout
(**HCI disconnect reason `0x22`**,
`ERROR_CODE_LMP_RESPONSE_TIMEOUT_LL_RESPONSE_TIMEOUT`) that drops the
controller.

**Sources of contention identified so far, and how each is currently
handled:**
- Wi-Fi connect **start** (fresh BT session + fresh WPA2 association +
  DHCP, all at once) — a 2s pre-delay was tried and later explicitly
  removed per user requirement (WOL must fire the instant the controller
  connects); the controller is instead protected via
  `wolwifi_wake_in_progress()` suppressing the USB-suspend power-off for
  the whole attempt.
- Staying associated **after** WOL is done (DHCP renewal, beacon listening,
  ongoing for the rest of a PC boot) — fixed by
  `disconnect_wifi_after_wol()` calling `cyw43_wifi_leave()` once a resend
  cycle ends.
- DHCP **stalling** under contention (open-ended, bounded only by a
  timeout) — fixed by a separate, shorter `DHCP_WAIT_TIMEOUT_MS` for the
  `WaitingForIp` phase, distinct from the longer association timeout.
- The **leave itself** (`cyw43_wifi_leave()` → `cyw43_ioctl(...,
  CYW43_IOCTL_SET_DISASSOC, ...)`) is a synchronous ioctl that kicks off an
  *asynchronous* over-the-air deauth/cleanup exchange continuing to use the
  radio after the call returns (confirmed via the Pico SDK's
  `cyw43_ctrl.c`) — this stacked with the lightbar WOL indicator's own BT
  sends right at confirm time; fixed by deferring the leave
  (`WOL_DISCONNECT_DELAY_MS`) past both.

**If a future disconnect/timing bug shows the same signature** (HCI
`0x22`, correlates in time with Wi-Fi connect/DHCP/leave activity in the
board trace) — check for a *new* source of overlapping radio activity
before assuming it's a repeat of an already-fixed one. The pattern keeps
recurring because fixes for one source of contention (e.g. the deferred
leave) can still stack with a different one that wasn't touched.

**Diagnostic tool:** the board-level WOL trace (see the entry below) is
what makes this diagnosable at all — HCI `0x22` disconnects during a
real PC-off test are otherwise invisible, since both the live WOL debug log
and the firmware UART log require an active companion HID connection to
capture anything, and the target PC is off for exactly the scenario being
tested.

---

## Known issue: two CYW43 driver gotchas that will bite again on any Wi-Fi work

1. **`cyw43_wifi_link_status()` vs `cyw43_tcpip_link_status()`** —
   `cyw43_wifi_link_status()` (`cyw43_ctrl.c`) only reflects the low-level
   radio join state; it can **never** return `CYW43_LINK_NOIP`/`CYW43_LINK_UP`
   regardless of DHCP/IP status. Any state machine gating on
   `status == CYW43_LINK_UP` using this function will never see a real
   connection complete. Use `cyw43_tcpip_link_status()` (`cyw43_lwip.c`)
   instead — the documented "superset" that actually checks `netif->ip_addr`.
2. **`cyw43_arch_wifi_connect_async()` doesn't clean up a prior
   association.** A retry after a genuine link drop joins on top of stale
   driver join state and fails immediately. Always call
   `cyw43_wifi_leave()` before every reconnect attempt after the first.

Both confirmed via direct SDK source reading, not just symptom-matching —
see `wolwifi.cpp`'s `start_wifi_connect()` for the current handling of
both.

---

## Architecture: board-level WOL trace (ring buffer, survives a host-off gap)

**Problem it solves:** the live WOL debug log and firmware UART log are
both transported entirely over the companion HID channel and only written
when the companion app is actively polling — useless for diagnosing
anything that happens while the target PC (which, in this dev setup, is
also the PC running the companion app) is off. That's exactly the scenario
WOL exists to handle.

**Design:** a small RAM-only ring buffer (`wol_trace_ring`, 48 records,
`bt.cpp`/`bt.h`) records both BT connection-phase transitions (connecting,
securing, HID-opening, ready, disconnecting, disconnected-with-HCI-reason,
plus timeout events) and `wolwifi.cpp`'s own trigger/connect/resend events,
sequence-numbered with a dropped-count. Survives a BT disconnect (doesn't
need to survive a power cycle — only needs to bridge the gap until the
companion app next polls, possibly minutes later). Exposed via
`COMPANION_REPORT_WOL_TRACE`, drained by the companion app into the same
on-disk `ds5bridge-wol-debug.log` the live debug log already writes to.

**Why both connection-phase and WOL events in one ring, not a WOL-only
trace:** a WOL-only trace would be blind to the BT connection-establishment
sequence itself timing out or disconnecting *before*
`wolwifi_on_controller_connect()` is ever reached — which turned out to be
exactly what several early bugs looked like from the outside.

**Convention:** `WolTraceStage` enum values are **append-only, never
renumbered** — old trace records from prior firmware must keep decoding to
the same stage names. A removed stage (e.g. `WolConnectDelayStart`, from
the twelfth-bug fix) is left as an unused number, not reclaimed.

**`board-boot` marker:** appended unconditionally on *every* boot (not just
watchdog-caused ones), with the raw `watchdog_hw->reason` register as
detail — the only way to unambiguously tell whether an unexplained
trace-sequence reset (jumping back to `seq=1`) means a real reboot
happened, independent of whether it was watchdog-caused, a brownout, a
manual power cycle, or a BOOTSEL/picotool reset.

---

## Architecture: WOL config persists to on-board flash, not just runtime/companion state

**Why:** the companion app only re-sends WOL config as one of the last of
~40 sequential commands in its post-connect settings-reapply sequence. A
*paired* controller's own BT reconnect is fast (pairing already done) and
reliably wins that race, firing the controller-connect trigger while WOL
config is still all-false in RAM. Persisting to flash (BTstack's TLV store,
same mechanism as pairing-key/blacklist persistence, tag `'WOLC'`) and
loading in `wolwifi_init()` — which runs immediately after `bt_init()`
starts, strictly before BT can possibly complete a reconnect — closes the
race entirely, independent of whether a companion app ever talks to the
board that session.

**Caveat:** only closes the race for config set *after* flashing — a flash
that has never had WOL configured starts empty, so WOL must be
(re)configured once via the companion app post-flash before a true
"PC fully off, no prior companion-app session this boot" test is valid.

**Known issue this fix didn't fully close (found and fixed later):**
persisting the config *values* to flash wasn't enough — `wolwifi_init()`
unconditionally called `enter_state(WifiState::Unconfigured)` after loading
them, and `WifiState::Unconfigured`'s handler in `wolwifi_task()` never
re-checks `g_have_ssid`; it's a dead end. The *only* code that calls
`enter_state(Idle)` is the SSID/password setters — so even with a valid
SSID loaded from flash, the connect state machine stayed stuck in
`Unconfigured` until the companion app's slow post-connect settings-reapply
sequence happened to re-send the SSID again, sometimes tens of seconds
after the controller had already connected and fired the trigger. Found via
the new `WolConnectStarted` trace stage (added to diagnose a "WOL fires ~61s
late" symptom) showing a ~27s gap between `wol-trigger-fired` and the first
actual `start_wifi_connect()` call, with zero explanatory events in between.
Fixed: `wolwifi_init()` now enters `Idle` directly (not `Unconfigured`) when
`wolwifi_load_persisted_config()` loaded a valid SSID. **Lesson for
anything else that loads persisted state on boot:** loading the values is
necessary but not sufficient — check whether a dependent state machine also
needs an explicit kick to notice the values are now valid, rather than
assuming it'll get there on its own.

---

## Architecture: wire-format and protocol decisions

- **SSID/password**: variable-length payload (byte length in the command's
  `value` field, bytes in the trailing payload area), reusing
  `SET_CHORD_BINDINGS`'s existing framing — the companion protocol's HID
  report is fixed-size (63 bytes), not truly variable-length. Password wire
  budget (53 bytes after a 10-byte header) is below WPA2's 63-char max, so
  `WOL_WIFI_PASSWORD_MAX_LENGTH`/`MAX_PASSWORD_LEN` are both capped at 53.
- **Target MAC**: fixed 6-byte format (reusing the existing Bluetooth-address
  wire format), not length-prefixed — a MAC is always exactly 6 bytes.
- **IP address status field**: sent as 4 raw octets, not a packed
  `uint32_t` — lwIP's `ip4_addr.addr` is network-byte-order, this
  protocol's `write_u32` is little-endian; packing through a `uint32_t`
  would silently byte-reverse depending on which side got the convention
  backwards. Same reasoning applied to the MAC wire format.
- **UI capability gate**: `firmwareFlags.wolControl`, a protocol-minor
  version check (`report[6] >= 20`), not a new status-report capability
  bit — the existing `firmwareFlags`/`statusFlags` bytes are already fully
  packed. Accepted tradeoff: a non-Waveshare board on firmware ≥ 1.20 shows
  the WOL UI as "supported" even though `ENABLE_WOLWIFI`-off firmware no-ops
  every WOL command — harmless since an inert toggle is the worst case.
- **Controller-connect hook point**: `wolwifi_on_controller_connect()` is
  called from `finish_hid_session_if_ready()` in `bt.cpp`, at the point
  `connection_phase` transitions to `BtConnectionPhase::Ready` — fires
  exactly once per successful BT connection, natural edge-triggering.
  Reused from upstream PR #93's analogous `wake_on_bt_connect()` call site
  (PR #93 itself is reference-only, unmerged, based on `main` not
  `port-dev` — do not attempt to cherry-pick it).
- **Debug ARP-liveness mechanism**: lwIP's ARP API only resolves IP→MAC,
  never the reverse, so there's no direct "ping this MAC" call. Instead:
  broadcast an ARP request, then snoop `netif_default->input` (normally
  `ethernet_input`) for any inbound frame whose EtherType is ARP and source
  MAC matches the target, always forwarding to the real input function
  afterward so normal lwIP operation is unaffected. **Must check EtherType
  first** — matching on any Ethernet frame's source address (not just ARP)
  was a real bug (see `CHANGELOG.md`'s eleventh-bug entry) that produced
  false-positive "target confirmed awake" results.

---

## Architecture: lightbar WOL indicator needs its own color snapshot

`bt_set_lightbar_color()` overwrites its own restore target
(`saved_lightbar_*`) on every call — there's no way to snapshot "what was
showing before" through the normal restore mechanism, since the pulse
animation itself calls `bt_set_lightbar_color()` every tick (the only way
to actually change what's displayed), which would otherwise immediately
clobber any snapshot taken the normal way. Solved with a dedicated,
separate snapshot (`wol_indicator_pre_red/green/blue/brightness`) captured
once in `bt_wol_indicator_begin()`, independent of
`saved_lightbar_*`/`lightbar_restore_pending`. Any future "flash to a color
and then truly restore" feature will hit the same problem and needs the
same pattern (a dedicated snapshot, not the general restore mechanism).

---

## Decision: keep this fork's WOL feature additive, rebase-friendly

Per `AGENTS.md`: new files (`wolwifi.h/.cpp`) over modifying existing
logic; where existing files must change, keep the diff minimal and
localized (a single call site, a new case in a switch). Branch is
`feature/wol-wifi` off `upstream/port-dev` directly (not a separate
long-lived integration branch) — matches the plan's actual merge target.
Build outputs stay in upstream's existing locations (`build/<name>/`,
`companion/artifacts/`) — no new `dist/` convention, since that's pure
rebase friction for zero benefit (tried once, reverted).

The WOL debug Ping/Test tooling is a candidate to keep in the eventual
upstream PR, not just as local dev tooling — the bugs found via this
tooling are a strong argument that upstream reviewers/users would want the
same diagnostic capability. Decide before opening the PR.

**Superseded 2026-08-10:** once the feature was confirmed working
end-to-end on real hardware, explicit user instruction was to strip all of
it out before opening the PR — see "Decision: strip all debug/diagnostic
scaffolding before the PR" below. The bug-hunting value was real but is a
one-time cost already paid; it doesn't justify permanent surface area in
an upstream PR for a feature that now works.

---

## Decision: strip all debug/diagnostic scaffolding before the PR

Once WOL was confirmed correct end-to-end on real hardware (skips when the
host is on, fires promptly when off, survives the resend/confirm/leave
cycle), all of the bug-hunting infrastructure built up over ~15 rounds of
hardware debugging was removed in one pass: the `WolTraceStage` ring buffer
(`bt.h`/`bt.cpp`, all `bt_append_wol_trace_event()` call sites across
`bt.cpp`/`main.cpp`/`wolwifi.cpp`), `usb_host_active_debug_bits()`
(`usb.h`/`usb.cpp` — `usb_host_active()` itself, the real production
signal `ObserveHost` gates on, was kept), the `WOL_DEBUG_STATUS` report and
`wolwifi_debug_ping()`/`wolwifi_debug_send_wol()`/`wolwifi_debug_status()`
(`wolwifi.h`/`wolwifi.cpp`), the `TRIGGER_WOL_DEBUG_PING`/`_SEND` commands
and their `companion.cpp` handlers, and the companion app's entire "Debug
Target" UI row (Ping/WOL Test buttons, Debug Log path) plus the
`wolDebugStatus`/WOL-trace read-and-log machinery in `bridge-service.ts`,
its two IPC channels (`main.ts`/`preload.ts`), and the corresponding
constants/types/report IDs in `protocol.ts`/`types.ts`.

**Why removed rather than kept as permanent tooling:** per explicit user
instruction — the fork's stated purpose (`AGENTS.md`) is to ship WOL as a
minimal, clean addition suitable for an upstream PR or a patch onto a
future release; debug-only surface area works against that "touch the
least number of things" goal once the feature it was built to debug is
confirmed working. The reference implementations this feature was modeled
on (`awalol/DS5Dongle#207`, `DevFreezing/DS5Dongle-WoL`) also ship without
equivalent tooling.

**What two genuinely shared code paths needed instead of deletion:**
`arp_snoop_input()` (`wolwifi.cpp`) had its `debug_ping_watching` branch
stripped but kept the `g_resend_active` branch — the real
resend-confirmation ARP-liveness check is core to production behavior, not
debug-only. `send_magic_packet_now()` lost its `is_debug_action` parameter
and `finish_debug_action(...)` branches, simplified to a no-arg function
with the same two production call sites (`begin_resend_cycle()`,
`drive_resend_cycle()`).

**Protocol IDs:** `COMMAND_ID.TRIGGER_WOL_DEBUG_PING`/`_SEND` (`0x3B`/
`0x3C`) were the trailing two entries in both `CommandId` (`companion.cpp`)
and `COMMAND_ID` (`protocol.ts`) — deleted cleanly, no gap. `REPORT_ID.
WOL_TRACE`/`WOL_DEBUG_STATUS` (`0x0B`/`0x0C`) were **not** trailing
(`DEVICE_IDENTITY`/`FIRMWARE_LOG` follow at `0x0D`/`0x0E`) — per explicit
user instruction ("touch the least number of things, keeping vanilla
baseline as much as possible"), those two constants were deleted outright
rather than shifting the still-used entries down to close the gap; `0x0B`/
`0x0C` are now simply unused, not reassigned. `PROTOCOL_MINOR` bumped
20→21 in both `protocol.ts` and `companion.cpp`'s `kProtocolMinor`, since
removing wire-format constants is exactly what that gate exists for.

**Verification:** both firmware targets (Waveshare/`ENABLE_WOLWIFI` and the
default non-WOL board) rebuilt clean with zero leftover
`wol_trace`/`wol_debug`/`usb_host_active_debug_bits` symbols (confirmed via
`arm-none-eabi-nm`); companion `npm run typecheck` and the full `vitest`
suite (279/279) pass. The shippable feature surface left behind is exactly
the "Wake-on-LAN" UI section (enable toggle, Wi-Fi SSID/password, target
MAC) plus the underlying automatic-trigger → `ObserveHost` host-alive gate
→ Wi-Fi connect/DHCP → magic-packet send → resend-until-confirmed logic —
no debug-only code paths remain.
