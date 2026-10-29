# CONTRIBUTING-FORK.md — rules for this fork's own changes

Rules that apply to **every** change made in `gunCannoli/DS5_Bridge`, on top
of upstream's own contribution guide. Two goals:

1. **Nothing we do pollutes other projects' issue trackers.**
2. **Every change is either an upstream PR or a clean local patch, and stays
   easy to re-apply to a future DS5_Bridge release.**

---

## 1. Never create cross-repo backlinks

GitHub turns certain text into a **cross-reference comment on the target
issue** — visible to that project's maintainers forever. This fork has
already polluted an unrelated upstream PR this way; do not repeat it.

**Forms that auto-link (never use these in commit messages, PR bodies, or
code comments):**

| Never write | Why |
|---|---|
| `owner/repo#123` | posts a backlink comment on that repo's issue 123 |
| `#123` (in a PR body or commit) | backlinks within the target repo |
| `https://github.com/owner/repo/issues/123` | same as above |
| `https://github.com/owner/repo/pull/123` | same as above |
| `GH-123` | same as above |

**Write instead** — plain prose that a human can search for, which GitHub does
not linkify:

- `DS5Dongle PR 207 (upstream-of-upstream)`
- `DS5Dongle issue 252`
- `DS5_Bridge PR 120`

Linking to a **repository root** (`https://github.com/awalol/DS5Dongle`) is
fine — it creates no backlink. Upstream's own `README.md` credits are
untouched for this reason.

**Before pushing anything, check:**

```bash
git log --format='%s%n%b' <base>..HEAD | grep -nE '[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+#[0-9]+|(^|[^a-zA-Z])#[0-9]+|github\.com/[^/]+/[^/]+/(issues|pull)/'
grep -rnE '[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+#[0-9]+|github\.com/[^/]+/[^/]+/(issues|pull)/' src/ companion/src/ boards/ *.md
```

Both should return nothing (except upstream's own repo-root credit links).

## 2. Commit and PR authorship

- Commits are authored by the repo owner. **No `Co-Authored-By:` trailers for
  AI tooling**, and no assistant names anywhere in commit messages, PR titles,
  or PR bodies.
- PR text is **objective and technical**: what changed, why, how it was
  tested. No narrative about how the change was produced.
- Keep PR bodies free of the auto-linking forms in §1.

## 3. Every change is a PR or a documented local patch

Each fork change falls into exactly one bucket, recorded in `CUSTOM.md`:

- **Upstream PR** — a self-contained feature/fix we intend upstream to take.
  Must be minimal, additive, and independently revertible.
- **Local patch** — something we keep but upstream would not want (build
  scripts, dev conveniences, UI preferences). Must be clearly marked in the
  code with a `LOCAL-ONLY` comment stating why it isn't in a PR.

Never let the two mix inside one commit. A PR branch must contain *only* its
feature; local patches stay on the working branch.

## 4. Keep changes portable to future DS5_Bridge releases

This is the fork's core constraint — see `AGENTS.md` for the full working
agreement. The short form:

- **Prefer new files** over editing existing ones.
- Where an existing file must change, make it a **single call site, a new enum
  value, or a new case** — never a restructure.
- **Never reformat, rename, or "clean up"** upstream code. Every such change
  is a future merge conflict for zero benefit.
- Document every hook point in `CUSTOM.md` with its exact location and a
  "if upstream moved this, the rule is…" fallback, so a port is mechanical.
- Re-check `COMMAND_ID` / `REPORT_ID` numbering on every upstream merge —
  independently-added enum values collide silently and `git` cannot detect it
  (this has happened; see `DECISIONS.md`).

## 5. Before opening any PR

1. Rebase/merge onto the current upstream target branch.
2. Run: `./boards/run_firmware_tests.sh`, and in `companion/`
   `npm run typecheck` + `npx vitest run src`.
3. Hardware smoke-test the actual feature.
4. Run the §1 grep checks.
5. Confirm the diff contains **no** `LOCAL-ONLY` blocks and nothing unrelated
   to the feature.
6. Confirm no `Co-Authored-By` trailers and no assistant mentions.
