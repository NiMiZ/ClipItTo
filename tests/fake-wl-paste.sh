#!/bin/sh
# Stand-in for wl-paste in the WlPasteBackend tests. State lives in $FAKE_WLPASTE_DIR:
#   unsupported  if present, --watch fails as on a compositor without data-control
#   types, text  what --list-types and --no-newline --type text print
#   event        created by a test to signal a clipboard change (consumed here)
#   die          created by a test to make the watcher exit
#   reads        one line appended per text fetch: the requested --type
d=${FAKE_WLPASTE_DIR:?}
case "$1" in
--watch)
    if [ -e "$d/unsupported" ]; then
        echo "Watch mode requires a compositor that supports the wlr data control protocol" >&2
        exit 1
    fi
    shift
    "$@" # like wl-paste: run once for the current content
    while :; do
        if [ -e "$d/die" ]; then rm -f "$d/die"; exit 1; fi
        if [ -e "$d/event" ]; then rm -f "$d/event"; "$@"; fi
        sleep 0.02
    done
    ;;
--list-types) cat "$d/types" ;;
--no-newline) echo "$3" >> "$d/reads"; cat "$d/text" ;;
*) exit 2 ;;
esac
