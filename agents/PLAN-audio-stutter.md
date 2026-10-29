# Plan — Feature #4: reduce speaker-audio stutter

Status: **INVESTIGATED AND REVERTED — no code change kept** (2026-09-07).
Kept as a record of what was measured and why nothing shipped, so this isn't
re-investigated from scratch.

## Outcome

The instrumentation and queue changes were built, measured on real hardware,
and then **reverted in full**. Nothing from this investigation remains in
`src/` or `companion/`.

**What the measurements showed.** With counters on every stage of the send
path (Opus queue drops, raw USB FIFO drops, send underruns, queue high-water
mark, and delivery cadence to the controller), across ~2000 packets of real
playback at controller buffer lengths 48/32/24/16:

- Opus drops: **0**. Raw FIFO drops: **0**. Underruns: **0**.
- Batches sent without a speaker payload: **0**.
- Speaker-send intervals >30 ms during playback: **0** at every setting.
- Queue high-water mark: 3 of 6 slots.

Yet audio was audibly broken at 16 and 24, and clean at 32+.

**Conclusion.** Nothing on the Pico drops or delays audio. The limit is the
**batch granularity** introduced by the two-frames-per-packet design (the
same design DS5Dongle PR 227 added and DS5Dongle issue 252 reports against):
audio is delivered in 20 ms lumps, so the controller's own playout buffer
must be at least one batch period to avoid draining between them. 16 frames
(5.3 ms) and 24 (8 ms) are below that; 32 (10.7 ms) and above are not.

That is a property of the packing scheme, not a bug in this fork. Lowering
the floor further would mean reverting to one frame per packet, doubling the
Bluetooth packet rate — the exact pressure the batching exists to relieve,
and a trade against rumble/adaptive-trigger reliability. Not worth it.

**Practical guidance:** controller buffer length 32 is the floor, 48 is a
comfortable setting. Nothing to change in the firmware.

**Reverted:** the four `audio_debug_stats` loss counters, the cadence
counters, the `COMPANION_REPORT_AUDIO_STATS2` (0x0B) report and its
companion-side parser/formatter, the `SPEAKER_OPUS_FIFO_CAPACITY` /
`AUDIO_RAW_FIFO_CAPACITY` / priming changes, and the `audio` build variant.

**Incidental finding worth keeping in mind:** the `debug` build variant
(`DS5_DIAGNOSTICS_PRESET=all`) **does not link** — it fails
`verify_core1_sram`'s runtime-heap minimum (85600 < 87312 bytes) and did so
*before* any of this work. Unrelated pre-existing issue; see §7.

---

## Original investigation (kept for reference)

Prompted by the upstream-of-upstream DS5Dongle issue 252 ("Speaker audio
severely stutters at low audio_buffer_length"), a regression from DS5Dongle
PR 227 which batched two audio frames per packet. DS5_Bridge already
implements that batching design, so it inherits the same failure mode: on
this fork we get audible stutter unless the controller-side buffer hint is
raised, and raising it delays audio haptics.

(Cross-repo references in this fork's docs are written as plain prose, never
as `owner/repo#N` or issue URLs, so they don't create backlink comments on
other projects' issues. See CONTRIBUTING-FORK.md.)

---

## 1. What DS5_Bridge does today

`src/audio.cpp`:

- `try_send_pending_audio_batch()` (line ~1068) composes one 547-byte report
  carrying **`AUDIO_BATCH_FRAMES` = 2** haptics samples (64 B each) plus, when
  a speaker payload is included, **2 Opus frames** (200 B each). ~20 ms of
  audio per BT packet. This is #227's design, already here.
- USB ingest (`audio_loop` → line ~1824) reads 384 B at a time from TinyUSB,
  accumulates 512 stereo frames, pushes an `audio_raw_element` into
  `audio_fifo`.
- Core 1 pops `audio_fifo`, resamples 512→480 frames, Opus-encodes, pushes a
  `speaker_opus_element` into `speaker_opus_fifo` (line ~2559).
- `speaker_opus_batch_ready()` gates the send on
  `queue_get_level(&speaker_opus_fifo) >= AUDIO_BATCH_FRAMES`.

## 2. Root causes of the stutter

### 2.1 Zero-slack queues (the main one)

```cpp
static ExactAudioQueue<audio_raw_element, 2> audio_fifo;                       // line 218
static ExactAudioQueue<speaker_opus_element, AUDIO_BATCH_FRAMES> speaker_opus_fifo;  // line 219 -> capacity 2
```

`speaker_opus_fifo`'s **capacity equals the batch size**. The send needs
exactly 2 frames and the queue can hold exactly 2. There is no headroom: the
pipeline is either exactly ready or stalled. Any jitter — BT scheduling, a
slow Opus encode, CYW43 Wi-Fi/BT radio contention (a known recurring issue in
this fork, see `DECISIONS.md`), a long main-loop phase — immediately causes a
drop or an underrun.

### 2.2 Silent oldest-frame drops

Both producers evict the oldest element when full, with no counter on the
speaker path:

```cpp
if (queue_is_full(&speaker_opus_fifo)) { speaker_opus_fifo.try_remove(nullptr); }  // line ~2562
```

Each eviction discards **10 ms of audio** — that *is* the audible stutter.
`AudioDebugOpusFifoDrop` (enum 9) exists but is **never emitted**; only the
much rarer `AudioDebugOpusFifoAddFail` is logged. So today the dominant loss
path is invisible.

The USB ingest path (line ~1881) does log `AudioDebugAudioFifoDrop`, but has
no persistent counter in `audio_stats` either.

### 2.3 `haptics_buffer_length` is a controller-side hint, not host buffering

```cpp
#define DEFAULT_HAPTICS_BUFFER_LENGTH 64   // line 43, range 16..128
pkt[5] = pkt[6] = pkt[7] = pkt[8] = haptics_buffer_length;   // line ~1099
```

It only writes bytes 5-8 of the outgoing report — a hint telling the
**DualSense** how much to buffer before playing. Raising it masks stutter by
making the controller more tolerant, at the cost of **added haptics latency**.
It adds no slack on the Pico side. This is exactly the tradeoff the user
reports, and it's why #252's reporter had to raise the value rather than fix
the underlying jitter.

### 2.4 Fixed resample ratio, no clock tracking

```cpp
resampler_audio.SetRates(51200, 48000);   // line ~2592 (512 -> 480 frames)
```

Assumes the USB host delivers exactly 512 input frames per 480 output frames
indefinitely. The host's audio clock and the Pico's clock are independent and
drift; with no feedback the queue slowly fills (→ drops) or empties (→
underruns), producing **periodic** stutter even in an otherwise quiet system.

## 3. Planned changes

### Phase C — instrumentation first (measure before changing behavior)

Cannot tell whether A/B help without numbers. Add to `audio_stats` (the
existing `DS5_AUDIO_TRANSPORT_STATS_ENABLED` block) and the companion
Diagnostics tab:

- `speakerOpusDropCount` — increment at the `speaker_opus_fifo` eviction
  (line ~2562) and emit the unused `AudioDebugOpusFifoDrop` there.
- `audioFifoDropCount` — persistent counter at the `audio_fifo` eviction
  (line ~1881), alongside the existing debug log.
- `speakerOpusUnderrunCount` — increment when a speaker payload was wanted
  but `speaker_opus_batch_ready()` was false in
  `try_send_pending_audio_batch()`.
- `speakerOpusLevelMin` / `LevelMax` — high-water marks of
  `queue_get_level(&speaker_opus_fifo)` to see how much headroom is actually
  used.

Surfaced through the existing audio-stats report → `AudioStatusPayload` →
Diagnostics tab, same path as `micPacketsDropped` etc.

### Phase A — give the queues real headroom

Decouple queue capacity from batch size:

```cpp
#define AUDIO_BATCH_FRAMES 2                 // unchanged: 2 frames per BT packet
#define SPEAKER_OPUS_FIFO_CAPACITY 6         // was AUDIO_BATCH_FRAMES (2)
#define AUDIO_RAW_FIFO_CAPACITY 4            // was 2
```

- Send gate stays `>= AUDIO_BATCH_FRAMES` — cadence unchanged.
- Absorbs ~40 ms of jitter on the Opus side instead of 0.
- RAM cost: `speaker_opus_element` is ~204 B → +4 slots ≈ **+816 B**;
  `audio_raw_element` is 512·2·4 = 4096 B → +2 slots ≈ **+8 KB**. Check
  against the build's reported post-startup headroom (currently ~31 KB) — if
  8 KB is too much, raise `audio_fifo` to 3 (+4 KB) instead of 4.
- Expected: allows `haptics_buffer_length` to be **lowered** (less haptics
  delay) while stutter goes down, breaking the tradeoff in 2.3.

### Phase B — prime the stream before the first send

At stream start, wait until the Opus FIFO reaches a small target (e.g. 3)
before the first batch, then run normally at `>= 2`. Prevents the
"start-of-stream immediately underruns" glitch and keeps a steady-state
cushion. Implemented as a `speaker_opus_primed` flag cleared in
`clear_opus_buffer()` / `drain_audio_queues()`.

### Phase D — adaptive resample ratio (only if C shows residual drift)

Nudge `resampler_audio.SetRates()` by a small factor (±0.1 %) based on the
Opus FIFO level to track the host clock. Deferred: medium risk (pitch
artifacts if over-corrected), and A+B may make the drift harmless.

## 4. Files to touch

| File | Change |
|---|---|
| `src/audio.cpp` | Queue capacity defines + declarations; drop/underrun/level counters; prime flag; emit `AudioDebugOpusFifoDrop`. |
| `src/audio.h` | New fields on the audio status/stats struct. |
| `src/companion.cpp` | Pack the new counters into the audio-stats report. |
| `companion/src/shared/protocol.ts` | Parse them into `AudioStatusPayload` (may need a `PROTOCOL_MINOR` bump if the report layout grows — check for spare bytes first). |
| `companion/src/renderer/App.tsx` | Show them in the Diagnostics tab. |
| `tests/firmware/*` | Source-text assertion that the eviction paths increment counters. |
| `companion/src/**/*.test.ts` | Parser test for the new fields. |

## 5. Testing

- `./boards/run_firmware_tests.sh`, `npm run typecheck`, `npx vitest run src`.
- **Debug-variant smoke test** (`.\tools\build-firmware.ps1 debug`) — the
  audio debug log is the point of Phase C:
  1. Headset in the controller jack, play continuous audio (music/movie) for
     ~2 minutes at the **current** `haptics_buffer_length` (64). Record
     `speakerOpusDropCount`, `audioFifoDropCount`, underruns, level min/max.
  2. Lower `haptics_buffer_length` toward 16-32 and repeat — confirm drops
     rise as stutter becomes audible (validates the counters measure the
     right thing).
  3. Apply Phase A+B, repeat both runs. Success = drop counts near zero and
     no audible stutter at a **lower** buffer length than before.
  4. Confirm audio haptics latency subjectively improves at the lower
     setting.
  5. Regression: WOL still fires, controller input latency unaffected, mic
     path unaffected.

## 6. Resolved questions

1. ~~RAM headroom~~ **RESOLVED — `audio_fifo` must stay at 2.** Each
   `audio_raw_element` is 4 KiB; depth 3 dropped the `final` build under
   `verify_core1_sram`'s minimum (79812 < 87312 required). Reverted to 2 and
   documented in the code. The Opus queue (204 B/element) is the one that
   matters anyway — it's the stage that gates the send. `final` at Opus
   capacity 6: **30424 bytes post-startup headroom** (needs 8192). Comfortable.
2. ~~Protocol bump~~ **RESOLVED — no bump.** The 0x06 audio-stats report is
   completely full (its last `u32` ends at the final payload byte). Added a
   **second page as report `0x0B`** (`COMPANION_REPORT_AUDIO_STATS2`), one of
   the two IDs freed by the WOL debug strip-down. Parsed best-effort, so older
   firmware just yields no page-2 line.
3. **Upstream framing** — this is arguably a fix for `awalol/DS5Dongle`#252
   too, but our PR goes to `SundayMoments/DS5_Bridge`. Cross-reference both
   #252 and #227 in the PR body.

## 7. Implementation log (2026-09-06)

Phases C + A + B implemented together so one smoke test can measure both.

**Phase C — instrumentation**
- `src/audio.h`: four new `audio_debug_stats` fields —
  `speaker_opus_drop_count`, `audio_fifo_drop_count`,
  `speaker_opus_underrun_count`, `speaker_opus_level_max`.
- `src/audio.cpp`: `audio_stats_note_*()` helpers next to the existing ones;
  counter at the Opus eviction (which also now emits the previously-dead
  `AudioDebugOpusFifoDrop` stage), at the raw-FIFO eviction, at the
  speaker-underrun return, and a high-water mark of the Opus level on each
  successful batch.
- `src/companion.h` / `companion.cpp`: `COMPANION_REPORT_AUDIO_STATS2 0x0B` +
  `build_audio_stats2()` + dispatch case.
- `companion/src/shared/protocol.ts`: `REPORT_ID.AUDIO_STATS2`,
  `AudioStats2Payload`, `parseAudioStats2Report()`.
- `companion/src/main/bridge-service.ts`: reads 0x0B alongside 0x06,
  `formatAudioStats2()` emits an `[AudioStats2] ...` line into the audio debug
  log (deduped by signature, same as page 1).

**Phase A — Opus queue headroom**
- `SPEAKER_OPUS_FIFO_CAPACITY 6` (was `AUDIO_BATCH_FRAMES` = 2),
  `AUDIO_RAW_FIFO_CAPACITY 2` (unchanged, see resolved Q1). Send threshold
  still `>= AUDIO_BATCH_FRAMES`, so cadence is unchanged.

**Phase B — stream priming**
- `SPEAKER_OPUS_PRIME_FRAMES 3` + a `speaker_opus_primed` flag, cleared in
  `clear_opus_buffer()`. `speaker_opus_batch_ready()` holds the first batch
  until the cushion exists, then runs normally. Priming deliberately does not
  count as an underrun.

**New `audio` build variant.** The `debug` variant (`DS5_DIAGNOSTICS_PRESET=all`)
**already failed the runtime-heap guard before any of this work** (85600 <
87312 required — verified by stashing the changes and rebuilding). Since the
new counters are gated on `DS5_AUDIO_DEBUG_ENABLED`, the smoke test needs
audio diagnostics *without* the UART log and trace rings. Added
`.\tools\build-firmware.ps1 audio` → `DS5_DIAGNOSTICS_PRESET=audio`, which
builds clean with **24368 bytes post-startup headroom**. Staged as
`firmware/ds5-bridge-1.71-wol-audio.uf2`.

**Pre-existing issue found, not fixed here:** the `debug` variant cannot link.
Out of scope for this feature; worth its own look later.

**Verification:** `./boards/run_firmware_tests.sh` all suites pass (the
`usb_descriptor_migration_test` assertion that pinned the literal
`ExactAudioQueue<audio_raw_element, 2>` was updated to assert the named
constants plus `#define AUDIO_RAW_FIFO_CAPACITY 2`, preserving its RAM-guard
intent). `npm run typecheck` + `npx vitest run src` 333/333. Companion
rebuilt and relaunched.
