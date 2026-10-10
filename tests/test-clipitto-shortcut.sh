#!/bin/sh
# Tests scripts/clipitto-shortcut against a fake gsettings.   usage: test-clipitto-shortcut.sh WORKDIR
set -eu
here=$(cd "$(dirname "$0")" && pwd)
script="$here/../scripts/clipitto-shortcut"
work=${1:?usage: test-clipitto-shortcut.sh WORKDIR}
mkdir -p "$work"
export FAKE_GSETTINGS_DIR="$work"
export PATH="$here/fakebin:$PATH"
rm -f "$work"/org.*

schema=org.gnome.settings-daemon.plugins.media-keys
ours=/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/clipitto/
other=/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/custom0/
entry="$schema.custom-keybinding:$ours"
failures=0

check() { # description expected actual
    if [ "$2" = "$3" ]; then
        echo "PASS $1"
    else
        echo "FAIL $1: expected [$2], got [$3]"
        failures=$((failures + 1))
    fi
}

gsettings set $schema custom-keybindings "['$other']"

sh "$script" >/dev/null
check "adds our entry after existing ones" "['$other', '$ours']" "$(gsettings get $schema custom-keybindings)"
check "sets the name" "ClipItTo" "$(gsettings get "$entry" name)"
check "default binding" "<Control><Alt>v" "$(gsettings get "$entry" binding)"
check "command toggles" "clipitto --toggle" "$(gsettings get "$entry" command | sed 's|^.*/||')"

sh "$script" '<Super>v' >/dev/null 2>"$work/stderr"
check "no duplicate on re-run" "['$other', '$ours']" "$(gsettings get $schema custom-keybindings)"
check "binding updated" "<Super>v" "$(gsettings get "$entry" binding)"
check "warns about Super+V" "1" "$(grep -c 'toggle-message-tray' "$work/stderr")"

sh "$script" --remove >/dev/null
check "remove keeps other shortcuts" "['$other']" "$(gsettings get $schema custom-keybindings)"
check "remove clears our keys" "@as []" "$(gsettings get "$entry" binding)"

gsettings set $schema custom-keybindings "@as []"
sh "$script" >/dev/null
check "works from an empty list" "['$ours']" "$(gsettings get $schema custom-keybindings)"
sh "$script" --remove >/dev/null
check "remove to an empty list" "[]" "$(gsettings get $schema custom-keybindings)"

[ "$failures" -eq 0 ]
