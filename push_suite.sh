#!/usr/bin/env bash
# push-suite.sh — update github.com/micomrkaic/noise-suite from a tarball.
# Clones fresh, overlays the tarball contents, commits, pushes, cleans up.
#
# Usage: ./push-suite.sh [tarball] [commit message]
#   defaults: ~/Downloads/noise-suite.tar.gz, dated message

set -euo pipefail

REPO_URL="${REPO_URL:-git@github.com:micomrkaic/noise-suite.git}"
TARBALL="${1:-$HOME/Downloads/noise-suite.tar.gz}"
MSG="${2:-Update from tarball $(date +%F)}"

[ -f "$TARBALL" ] || { echo "no tarball at $TARBALL" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "==> cloning $REPO_URL"
git clone --depth 1 "$REPO_URL" "$WORK/repo"

echo "==> overlaying $TARBALL"
# tarball contains a top-level noise-suite/ directory: strip it so the
# files land at the repo root
tar xzf "$TARBALL" -C "$WORK/repo" --strip-components=1

cd "$WORK/repo"
git add -A                       # stages modifications AND deletions
if git diff --cached --quiet; then
    echo "==> repo already up to date, nothing to push"
    exit 0
fi

git commit -m "$MSG"
echo "==> pushing"
git push
echo "==> done"
