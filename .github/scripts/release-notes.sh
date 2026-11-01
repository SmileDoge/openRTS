#!/usr/bin/env bash
# Text of a GitHub release: what the build needs to run (Russian and English),
# then the commits since the previous version tag.
#   release-notes.sh <tag>      (vX.Y.Z, or nightly)
# Run from the repository with full history (checkout fetch-depth: 0).
set -euo pipefail
tag="$1"
if [[ "$tag" == v* ]]; then
    prev=$(git describe --tags --abbrev=0 --match 'v*' "${tag}^" 2>/dev/null || true)
    limit=()
else
    prev=$(git describe --tags --abbrev=0 --match 'v*' HEAD 2>/dev/null || true)
    limit=(-n 15)
fi
range=HEAD
[ -n "$prev" ] && range="$prev..HEAD"

cat <<'EOF'
Сборка openRTS для Windows x64. / openRTS build for Windows x64.

### Перед запуском / Before you start

- **Microsoft Visual C++ Redistributable (x64)**: https://aka.ms/vs/17/release/vc_redist.x64.exe
  Без него Windows пишет, что не найден `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` или `MSVCP140.dll`.
  Without it Windows reports `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` or `MSVCP140.dll` missing.
- Своя установка RTS с маршрутами и подвижным составом.
  Your own RTS installation with routes and rolling stock.

### Запуск / Run

```
openrts_viewer.exe --window --route Kuban_RTS --root D:\Games\RTS --consist "ED4M-0050 12vag"
```

Подробности — `README.md` в архиве. / Details: `README_EN.md` in the archive.

EOF
# A version takes its section of CHANGELOG.md ("## vX.Y.Z ..." up to the next
# "## "); without one, and for nightly, the commits.
section=""
if [[ "$tag" == v* && -f CHANGELOG.md ]]; then
    section=$(awk -v tag="$tag" '
        /^## / { if (found) exit; split($0, word, " "); if (word[2] == tag) { found = 1; next } }
        found { print }' CHANGELOG.md | sed -e '/./,$!d' -e 's/^###/####/')
fi
if [ -n "$section" ]; then
    echo "### Что нового / What's new"
    echo
    printf '%s\n' "$section"
    exit 0
fi
if [ -n "$prev" ]; then
    echo "### Изменения с $prev / Changes since $prev"
else
    echo "### Изменения / Changes"
fi
echo
# Commit trailers are not for players: co-authors, session links, sign-offs.
git log --no-merges "${limit[@]}" --format='#### %s%n%n%b' "$range" |
    grep -viE '^(Co-Authored-By|Claude-Session|Signed-off-by):' | cat -s
