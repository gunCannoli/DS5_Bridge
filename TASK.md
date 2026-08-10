# Task

Current + next task only. Completed work is purged from here into
`CHANGELOG.md` as it finishes — this file should stay short. Architecture
knowledge and known issues belong in `DECISIONS.md`, not here.

## Current task

**Core WOL feature confirmed working end-to-end on real hardware**
(2026-08-10): controller connects while the target PC is off, WOL fires and
gets ARP-confirmed within ~10s, the PC wakes automatically, and the
controller stays connected through the whole boot with no disconnect. This
closes out the fifteenth-bug fix and, with it, Phase 7's smoke test.

- [ ] Phase 9 — failure-behavior verification: confirm Wi-Fi/WOL failure
      never blocks or delays BT/controller init or normal operation
      (non-blocking by construction; needs a runtime check — e.g. WOL
      disabled/misconfigured, or Wi-Fi genuinely unreachable, shouldn't
      affect controller connect/input latency at all).
- [ ] Phase 10 — PR prep: commits already reasonably separated by feature
      area (firmware Wi-Fi/WOL, companion app config, build integration) —
      review the branch's full commit log for anything to squash/reorder
      before opening the PR. Write the PR description (why WOL, how it
      works, supported board, config requirements, test results — the real
      end-to-end success above is the headline result). Decide whether the
      debug Ping/WOL Test tooling ships as part of the feature or stays
      internal-only (see `DECISIONS.md`'s closing note — leaning toward
      shipping it, given how many real bugs it caught). Review the whole
      diff for anything unrelated to WOL before opening the PR.

## Next task

- Once Phase 9/10 are done, open the PR against `upstream/port-dev`.
- Longer-running/soak testing (WOL across many PC on/off cycles, different
  network conditions) is optional polish, not a blocker — the core
  end-to-end path is proven.
