# AGENTS.md

Working agreement for AI-assisted work on this fork (`gunCannoli/DS5_Bridge`).

## Purpose of this fork

This fork exists to develop **Wake-on-LAN over Wi-Fi** as an optional, isolated
feature on top of upstream `SundayMoments/DS5_Bridge`, targeting the
`port-dev` branch, with the end goal of submitting it upstream as a PR.

Treat this fork's changes as **our own patch**, not a divergent product:

- Keep the WOL feature additive and isolated (new files + minimal, well-marked
  hook points in existing files) so it stays easy to rebase/cherry-pick onto
  future upstream `port-dev` commits.
- Prefer new files (`wolwifi.h`, `wolwifi.cpp`) over modifying existing logic.
- Where existing files must change, keep the diff minimal and localized (a
  single call site, a new case in a switch, a new field in a settings object)
  rather than restructuring.
- Do not refactor or "clean up" unrelated upstream code. Any such change makes
  future rebases harder and is out of scope.
- Follow existing project conventions (naming, error handling, logging style,
  companion app patterns) rather than introducing new ones.

## Tracking files (repo root)

- **task.md** — current + next task, organized by phase of the active plan.
  Updated as work progresses within a plan. Cleared and reset when a plan
  completes (its summary moves to changelog.md).
- **changelog.md** — durable record of completed plans/phases, newest first.
- **decisions.md** — architecture/technical decisions made along the way,
  with rationale, so later rebases or PR reviewers understand *why*.
- **AGENTS.md** — this file.

## SDK / library install location

All SDKs and libraries needed for this work install to **`C:\auto\arduino\build`**,
not project-local or user-profile locations.

- Pico SDK is already present at `C:\auto\arduino\build\pico-sdk` — use this
  checkout (`PICO_SDK_PATH=C:\auto\arduino\build\pico-sdk`) rather than
  cloning a new one.
- Any additional toolchain/SDK/library needed for the WOL feature (ARM
  toolchain, Ninja, lwIP if it ships separately from the Pico SDK checkout,
  etc.) installs under `C:\auto\arduino\build\<name>` following the same
  convention, so everything stays in one predictable place across sessions.
- Before installing anything new, check `C:\auto\arduino\build` first — it
  may already be there.

## Push / GitHub sync guidelines

Keep `origin` (the fork) reasonably up to date as work progresses, but don't
push on every single commit.

- Push when a phase (or a meaningful chunk of one) from task.md is complete
  and builds/tests cleanly — e.g. after Wi-Fi bring-up compiles, after the
  companion app changes are wired end-to-end, after a smoke-test round passes.
- Push before ending a work session, so nothing is stranded only in the local
  worktree.
- Prefer pushing a branch (`feature/wol-wifi`) over pushing to a shared
  branch; never force-push without explicit confirmation.
- Commit locally frequently (small, logically separated commits per
  decisions.md/task.md phase breakdown) — pushing is about syncing to GitHub,
  not about commit granularity.

## Rebase/backport workflow

When upstream `port-dev` moves forward:

1. Fetch `upstream/port-dev`.
2. Rebase (or merge) our feature branch onto the new `port-dev` tip.
3. Because WOL lives mostly in `wolwifi.h/.cpp` (firmware) and isolated
   companion-app additions, conflicts should be limited to the few hook
   points documented in `decisions.md` (e.g. the `bt.cpp` connect-event call
   site, `CMakeLists.txt` option block, `protocol.ts` COMMAND_ID list).
4. Re-run the smoke tests in task.md/changelog.md before re-opening or
   updating the PR.

## Working style

- Research before implementing; do not assume a referenced PR (e.g. upstream
  PR #93) applies cleanly without verifying against the current branch.
- Confirm non-trivial architecture decisions with the user before committing
  to them (see decisions.md for the log of decisions already made).
- Keep commits logically separated (firmware Wi-Fi/WOL, companion app config,
  build integration) rather than one large commit.
