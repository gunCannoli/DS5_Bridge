# Decisions

Architecture decisions and known-issue/root-cause knowledge for the Wi-Fi
WOL feature fork — the durable stuff a future agent or PR reviewer needs to
not re-derive or re-discover. Not a running log of every bug fixed or every
test run; see `CHANGELOG.md` for that history. Newest first.

---

## Architecture: host-alive gate skips WOL when the target PC is already on

**Why:** the board's own USB persona is plugged into the *same* PC that WOL
wakes (the deployment model — dongle plugged into the PC you want to
remotely wake). Before this, a controller-connect edge always ran the full
WOL pipeline (Wi-Fi connect, resend cycle, lightbar pulse) even when the PC
was already running and didn't need waking — visibly pointless (the
lightbar pulsing for no reason was the symptom that prompted this) and
unnecessary CYW43 radio activity (see the radio-contention entry below —
any avoidable Wi-Fi activity is worth skipping).

**Signal used:** `usb_host_active()` (new, `usb.cpp`) = `usb_mounted &&
!usb_host_suspended_active()` — true only while the host is both
enumerated and awake, not merely suspended (S3) or torn down (S5). Checked
at the very top of `wolwifi_on_controller_connect()` (`wolwifi.cpp`),
before anything else WOL-related runs, so a positive check skips the Wi-Fi
connect, the resend cycle, and the lightbar pulse entirely — not just the
final magic-packet send. New `WolTraceStage::WolTriggerSkippedHostActive`
(25) records the skip in the board trace, distinct from the existing
`WolTriggerSkipped` (disabled/unconfigured).

**Semantics by host power state:**
- PC on, USB active → skip (this is the case being fixed).
- PC asleep (S3) → USB stays enumerated but suspended → check reads
  "inactive" → WOL still fires. Harmless (a magic packet to a sleeping NIC
  still wakes it) and doesn't conflict with the separate, faster BT-based
  "Wake PC on Controller" S3 path.
- PC fully off (S5), Pico still powered → USB bus fully torn down → check
  reads "inactive" → WOL fires. This is the actual target scenario for the
  whole feature.

**Known limitation, with an escape hatch (matches
`awalol/DS5Dongle#207`'s `WOL_ALWAYS` exactly — same name, same
mechanism, confirmed via `gh pr diff 207 --repo awalol/DS5Dongle`):** some
motherboards/BIOS settings ("power on by USB keyboard/mouse", always-on
charging ports) or Modern Standby (S0ix) keep the USB bus enumerated and
active even with the PC nominally off — on those boards this gate would
see "host active" permanently and block every wake. `WOL_ALWAYS` is a
**compile-time** `option()` in root `CMakeLists.txt` (default OFF, only
meaningful when `ENABLE_WOLWIFI` is on) that skips the gate entirely when
set. Compile-time rather than a runtime companion-app setting deliberately
— this is a board/BIOS hardware quirk workaround, decided once at flash
time, not a everyday user preference; matches `ENABLE_WOLWIFI` itself as
precedent, and avoids a new `COMMAND_ID`/settings-store/UI-row/protocol
surface for something that isn't a normal setting.

**Not debounced:** unlike the Wi-Fi-connect trigger debounce
(`WOL_TRIGGER_DEBOUNCE_MS`, which exists specifically to avoid radio
contention from repeated Wi-Fi activity), `usb_host_active()` is a free
read of already-tracked in-memory flags — no radio/CPU cost to re-checking
on every controller-connect edge, so no debounce was added for this check.

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
