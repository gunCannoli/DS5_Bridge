# Task

Current + next task only. Completed work is purged from here into
`CHANGELOG.md` as it finishes — this file should stay short. Architecture
knowledge and known issues belong in `DECISIONS.md`, not here.

> Upstream sync: fork is current on **v1.7.1** as of 2026-09-06 (three
> "keep both" conflicts, no `COMMAND_ID` renumbering) — see `CHANGELOG.md`.
> DS5_Bridge PR 120 (Wake-on-LAN) still open upstream against `port-dev`.

## Current state

Four fork features, all documented in `CUSTOM.md`:

| # | Feature | State |
|---|---|---|
| 1 | Wake-on-LAN over Wi-Fi (firmware + companion) | shipped in DS5_Bridge PR 120, open upstream |
| 2 | Auto Switch Audio on Jack (companion only) | **DS5_Bridge PR 147 open** (branch `pr/auto-switch-audio-on-jack` off upstream/main). Multi-output dropdown still untested on 3+ devices -- noted in the PR body. |
| 3 | Audio-aware Idle Disconnect (firmware) | **DS5_Bridge PR 146 open** (branch `pr/idle-disconnect-audio-aware` off upstream/main). Hardware smoke test still pending -- noted in the PR body. |
| 4 | Speaker-audio stutter | investigated and **reverted** — see `PLAN-audio-stutter.md`; nothing shipped |

Repo hygiene work completed 2026-09-07:

- **Cross-repo pollution removed.** This fork had been writing
  `owner/repo#N`-style refs in commit messages and code comments, which
  GitHub turns into backlink comments on other projects' trackers. Sources,
  docs, and **all 86 commit messages** are now clean (`tools/scrub-history.sh`,
  already run; backup refs under `refs/backup/`). Verified: 0 polluting refs.
- **`agents/` folder** — all fork tracking docs moved here; the repo root
  keeps only `AGENTS.md` plus upstream's `README.md`/`CONTRIBUTING.md`.
- **`CONTRIBUTING-FORK.md`** — the durable rule for both the pollution ban
  and the PR-vs-local-patch policy.

## Next tasks

- [ ] **Push the scrubbed history.** `git push --force-with-lease origin
      feature/wol-wifi`. `origin/feature/wol-wifi` (the clean PR 120 branch)
      needs the same scrub before PR 120 is next updated.
- [ ] **PR 147 follow-up:** smoke test with **3+ Windows render outputs** to
      exercise the fallback dropdown (it only renders with ≥2 non-controller
      outputs): all outputs listed, menu wide enough for long names, selection
      persists, a saved-but-absent device shows "(not connected)", and
      auto-resolve is *not* used once a device is explicitly picked. Post the
      result on the PR.
- [ ] **PR 146 follow-up:** hardware smoke test. Flash a `final`-variant UF2.
      (1) headset in jack + long video, Idle Disconnect at 1 min, no input →
      stays connected, audio uninterrupted; (2) stop playback, no input →
      disconnects ~1 min later; (3) no audio + no input → disconnects as
      before; (4) mic-mute still independently suppresses idle disconnect.
      Post the result on the PR.
- [ ] If either PR needs revision: the branches are `pr/idle-disconnect-audio-aware`
      and `pr/auto-switch-audio-on-jack`, each one clean commit off
      `upstream/main`. Amend + force-push; do not merge fork-branch history in.
- Local-only items (full-window settings modal, the `:disabled` flicker fix)
  stay on `feature/wol-wifi` only and are NOT in either PR.

## Known, deliberately not fixed

- **`debug` build variant does not link** — fails `verify_core1_sram`'s
  runtime-heap minimum (85600 < 87312 bytes). Pre-existing, unrelated to any
  fork feature. Use `final` for smoke tests.
- **Waveshare top-of-flash BTstack bank.** DS5Dongle PR 241 reports that
  accesses near the end of the advertised 16 MiB stall on this board
  (config-load hang → no BT pairing, or `VID_0000&PID_0002`). DS5_Bridge uses
  the SDK default bank, which sits in exactly that region. We have **not**
  reproduced it across many real-hardware cycles. If it ever appears, the fix
  is to relocate low: `-DPICO_FLASH_BANK_STORAGE_OFFSET=0x003FD000`.
