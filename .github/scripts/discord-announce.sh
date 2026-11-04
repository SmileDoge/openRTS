#!/usr/bin/env bash
# Announces a version release on Discord: the title and the opening
# paragraph of the version's CHANGELOG section, its subsections, and the
# link to the release.
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
    for line in lines[start + 1:]:
        if line.startswith('## '):
            break
        if line.startswith('### '):
            sections.append(line[4:].strip())
description = ' '.join(intro)
if sections:
    description += '\n\n**Что внутри:** ' + ', '.join(sections) + '.'
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
