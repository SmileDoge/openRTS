#!/usr/bin/env bash
# Announces a version release on Discord: the title, the opening paragraph
# of the version's CHANGELOG section, each subsection with its items' heads
# (up to the first colon or sentence end), and the link to the release.
#   discord-announce.sh <tag> [--dry-run]
# The webhook is DISCORD_WEBHOOK_URL (a repository secret: the URL itself is
# the secret, it is never printed). --dry-run prints the message instead.
set -euo pipefail
tag="$1"
mode="${2:-}"
repo="${GITHUB_REPOSITORY:-SmileDoge/openRTS}"
url="https://github.com/$repo/releases/tag/$tag"
py=python3
"$py" -c '' 2>/dev/null || py=python
payload=$("$py" - "$tag" "$url" <<'PYEOF'
import json
import re
import sys

sys.stdout.reconfigure(encoding='utf-8')
tag, url = sys.argv[1], sys.argv[2]
lines = open('CHANGELOG.md', encoding='utf-8').read().split('\n')
title, intro, sections = 'openRTS ' + tag, [], []
start = next((i for i, l in enumerate(lines) if l.startswith('## ' + tag + ' ') or l.strip() == '## ' + tag), None)
if start is not None:
    title = 'openRTS ' + lines[start][3:].strip()
    i = start + 1
    while i < len(lines) and not lines[i].strip():
        i += 1
    while i < len(lines) and lines[i].strip() and not lines[i].startswith('#'):
        intro.append(lines[i].strip())
        i += 1
    # Each subsection with its items' heads: an item's text up to its
    # first colon or sentence end, cut at a word.
    item = None
    for line in lines[start + 1:]:
        if line.startswith('## '):
            break
        if line.startswith('### '):
            sections.append((line[4:].strip(), []))
            item = None
        elif line.startswith('- ') and sections:
            item = [line[2:].strip()]
            sections[-1][1].append(item)
        elif line.startswith('  ') and item is not None and line.strip():
            item.append(line.strip())
        elif not line.strip():
            item = None


def head(text):
    text = ' '.join(text.split())
    # Asides in brackets left out, they do not end the head.
    while True:
        shorter = re.sub(r' ?\([^()]*\)', '', text)
        if shorter == text:
            break
        text = shorter
    # A short label before a colon ("Экспериментально: ...") keeps what
    # follows it, up to the next mark.
    start = 0
    k = text.find(': ')
    if 0 < k <= 20 and ' ' not in text[:k]:
        start = k + 2
    cut = len(text)
    for mark in (': ', '. ', '; '):
        k = text.find(mark, start)
        if start <= k < cut and k > 0:
            cut = k
    text = text[:cut].rstrip('.:,;')
    if len(text) > 110:
        text = text[:110].rsplit(' ', 1)[0] + '…'
    return text


description = ' '.join(intro)
for name, items in sections:
    part = '\n\n**' + name + '**'
    for it in items:
        part += '\n• ' + head(' '.join(it))
    if len(description) + len(part) > 3700:
        description += '\n\n…'
        break
    description += part
description += '\n\nСписок изменений и архив для Windows — в релизе.'
print(json.dumps({'username': 'openRTS',
                  'embeds': [{'title': title[:256], 'url': url, 'description': description[:4000].strip(),
                              'color': 0x2F6FB0}]}, ensure_ascii=False))
PYEOF
)
if [ "$mode" = --dry-run ]; then
    printf '%s\n' "$payload"
    exit 0
fi
: "${DISCORD_WEBHOOK_URL:?DISCORD_WEBHOOK_URL is not set}"
curl -fsS -H 'Content-Type: application/json' --data-binary "$payload" "$DISCORD_WEBHOOK_URL" > /dev/null
echo "Announced $tag on Discord."
