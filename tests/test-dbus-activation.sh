#!/bin/sh
# `clipitto` must start the daemon on demand through a D-Bus .service file.
# Uses a private bus whose only service directory is ours, so an installed ClipItTo
# can never be picked up instead.
#   usage: test-dbus-activation.sh CLIPITTO_CLI CLIPITTO_DAEMON WORKDIR
set -eu
cli=$1
daemon=$2
work=$3
mkdir -p "$work/services"
printf '[D-BUS Service]\nName=io.github.nimiz.ClipItTo\nExec=%s\n' "$daemon" \
    > "$work/services/io.github.nimiz.ClipItTo.service"
cat > "$work/bus.conf" <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <servicedir>$work/services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF

# Inherited by the activated daemon: keep it away from the real config and history.
export XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data" QT_QPA_PLATFORM=offscreen

# The bus (and the daemon it activates) log to a file, not to ctest's pipe, which a
# leftover daemon would otherwise keep open.
dbus-run-session --config-file="$work/bus.conf" -- sh -c '
    cli=$1 daemon=$2
    trap "pkill -x -f \"\$daemon\" 2>/dev/null" EXIT # never leave the daemon behind
    "$cli" --status > status.txt 2>&1 || { cat status.txt; echo "FAIL: no reply from an activated daemon"; exit 1; }
    grep -q "^backend: " status.txt || { cat status.txt; echo "FAIL: unexpected status"; exit 1; }
    "$cli" --ingest activated
    "$cli" --status | grep -q "^items: 1$" || { echo "FAIL: ingest not stored"; exit 1; }
' sh "$cli" "$daemon" </dev/null >"$work/bus.log" 2>&1 || { cat "$work/bus.log"; exit 1; }
cat "$work/bus.log"
echo "PASS: the daemon was started by D-Bus activation"
