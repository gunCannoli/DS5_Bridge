#!/usr/bin/env bash
# Rewrite this fork's commit messages to remove:
#   1. Cross-repo auto-linking refs (owner/repo#N, bare #N, issue/pull URLs)
#      that post backlink comments on other projects' issue trackers.
#   2. AI co-author trailers and assistant mentions.
#
# See CONTRIBUTING-FORK.md sections 1 and 2 for the policy this enforces.
#
# THIS REWRITES HISTORY. Every commit hash above the base changes. After
# running it you must force-push, and anyone else with the branch must reset.
#
# Usage:
#   ./tools/scrub-history.sh --dry-run          # show what would change
#   ./tools/scrub-history.sh                    # rewrite (creates a backup ref)
#   ./tools/scrub-history.sh --base upstream/main
#
# The message filter is Python, not sed: commit messages contain UTF-8 (e.g.
# the robot emoji in generated footers) and sed's multibyte handling fails on
# them under Git Bash ("is_mb_char: mbrtowc returned 3").

set -euo pipefail

BASE="upstream/main"
DRY_RUN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --base) BASE="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if ! git rev-parse --verify "${BASE}" >/dev/null 2>&1; then
    echo "base '${BASE}' not found (fetch upstream first?)" >&2
    exit 1
fi

FILTER_PY="$(mktemp)"
trap 'rm -f "${FILTER_PY}"' EXIT

cat >"${FILTER_PY}" <<'PYEOF'
import re, sys

def scrub(m):
    # issue/pull URLs -> "owner/repo PR N"
    m = re.sub(r'https://github\.com/([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+)/(?:issues|pull)/(\d+)',
               r'\1/\2 PR \3', m)
    # owner/repo#N -> "owner/repo PR N"
    m = re.sub(r'\b([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)#(\d+)', r'\1 PR \2', m)
    # "PR #93" / "issue #12" -> drop just the hash, so the next rule can't
    # turn it into "PR PR 93".
    m = re.sub(r'(?i)\b(PR|issue|pull)\s+#(\d+)', r'\1 \2', m)
    # bare #N -> "PR N" (not preceded by a word char or slash)
    m = re.sub(r'(^|[^A-Za-z0-9_/])#(\d+)', r'\1PR \2', m)
    # drop AI co-author trailers and generated-with footers
    m = re.sub(r'(?im)^Co-authored-by:.*(?:claude|anthropic).*$\n?', '', m)
    m = re.sub(r'(?im)^\s*\U0001F916\s*Generated with.*$\n?', '', m)
    m = re.sub(r'(?im)^\s*Generated with \[Claude.*$\n?', '', m)
    return m.rstrip('\n') + '\n'

data = sys.stdin.buffer.read().decode('utf-8', 'replace')
sys.stdout.buffer.write(scrub(data).encode('utf-8'))
PYEOF

PY=python
command -v python >/dev/null 2>&1 || PY=python3

COUNT="$(git rev-list --count "${BASE}..HEAD")"
echo "branch : ${BRANCH}"
echo "base   : ${BASE}"
echo "commits: ${COUNT}"
echo

if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "=== DRY RUN: before -> after (only commits that change) ==="
    changed=0
    while read -r sha; do
        before="$(git log -1 --format='%s%n%b' "${sha}")"
        after="$(printf '%s\n' "${before}" | "${PY}" "${FILTER_PY}")"
        if [[ "${before}" != "${after}" ]]; then
            changed=$((changed+1))
            echo "--- ${sha:0:9} ---"
            diff <(printf '%s\n' "${before}") <(printf '%s\n' "${after}") | sed 's/^/    /' || true
        fi
    done < <(git rev-list "${BASE}..HEAD")
    echo
    echo "${changed} commit(s) would be rewritten"
    exit 0
fi

BACKUP="refs/backup/pre-scrub-$(date +%Y%m%d-%H%M%S)"
git update-ref "${BACKUP}" HEAD
echo "backup ref: ${BACKUP}"
echo "  (restore with: git reset --hard ${BACKUP})"
echo

echo "using filter-branch"
FILTER_BRANCH_SQUELCH_WARNING=1 git filter-branch -f \
    --msg-filter "${PY} '${FILTER_PY}'" \
    "${BASE}..HEAD"

echo
echo "done. verify with:"
echo "  git log --format='%s%n%b' ${BASE}..HEAD | grep -nE '[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+#[0-9]+|Co-Authored-By|[Cc]laude'"
echo "then:"
echo "  git push --force-with-lease origin ${BRANCH}"
