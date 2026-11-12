#!/usr/bin/env bash
# Runs a word list through the language packs installed for this user and prints
#   input -> output (milliseconds, pack)
# so you can see which words translate badly, come back unchanged (UNCHANGED) or stall (SLOW, ERROR).
# Needs the real engine, so it cannot run in CI. See docs/translation.md.
#
#   scripts/translate-check.sh [-p he-en] [-d /path/to/clipitto-daemon] [-o report.txt] [wordlist]
#
# wordlist: default scripts/translate-wordlist.txt (one word or sentence per line; "#" lines are comments).
# -p: use only that pack (default: for each line the first installed pack that translates from its language).
# The daemon is looked for in $CLIPITTO_DAEMON, the PATH, /usr/bin, /usr/lib/clipitto and build/*/src/daemon.
# Nothing is sent anywhere, and the clipboard and history are not touched; no running ClipItTo is needed.
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
pack=""
daemon="${CLIPITTO_DAEMON:-}"
report=""
while getopts "p:d:o:h" opt; do
    case "$opt" in
        p) pack="$OPTARG" ;;
        d) daemon="$OPTARG" ;;
        o) report="$OPTARG" ;;
        *) sed -n '2,12p' "$0"; exit 0 ;;
    esac
done
shift $((OPTIND - 1))
list="${1:-$here/translate-wordlist.txt}"

if [ -z "$daemon" ]; then
    for candidate in "$(command -v clipitto-daemon 2>/dev/null || true)" /usr/bin/clipitto-daemon /usr/lib/clipitto/clipitto-daemon \
        "$here"/../build/*/src/daemon/clipitto-daemon; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            daemon="$candidate"
            break
        fi
    done
fi
if [ -z "$daemon" ] || [ ! -x "$daemon" ]; then
    echo "clipitto-daemon not found: give it with -d or CLIPITTO_DAEMON." >&2
    exit 2
fi
if [ ! -f "$list" ]; then
    echo "word list not found: $list" >&2
    exit 2
fi

args=(--translate-check "$list")
[ -n "$pack" ] && args+=(--translate-pack "$pack")
[ -n "$report" ] && args+=(--translate-out "$report")
# No display is needed; the engine runs on the CPU in this process.
QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" exec "$daemon" "${args[@]}"
