#!/bin/sh
# Runs the extension's tests with gjs: extension.js is copied next to fake GNOME Shell
# modules (stubs/) and its gi:// and resource:// imports are pointed at them. GLib stays real.
#   usage: run-tests.sh OUTPUT_DIR
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: run-tests.sh OUTPUT_DIR}

mkdir -p "$out/stubs"
cp "$here"/stubs/*.js "$out/stubs/"
cp "$here/test_extension.js" "$out/"
sed -e "s|'gi://Clutter'|'./stubs/Clutter.js'|" \
    -e "s|'gi://Gio'|'./stubs/Gio.js'|" \
    -e "s|'gi://Meta'|'./stubs/Meta.js'|" \
    -e "s|'gi://Shell'|'./stubs/Shell.js'|" \
    -e "s|'gi://St'|'./stubs/St.js'|" \
    -e "s|'resource:///org/gnome/shell/extensions/extension.js'|'./stubs/Extension.js'|" \
    -e "s|'resource:///org/gnome/shell/ui/main.js'|'./stubs/Main.js'|" \
    "$here/../clipitto@nimiz.github.io/extension.js" > "$out/extension.js"

if grep -nE "from '(gi://(Clutter|Gio|Meta|Shell|St)|resource://)" "$out/extension.js"; then
    echo "run-tests.sh: an import above has no stub" >&2
    exit 1
fi

exec gjs -m "$out/test_extension.js"
