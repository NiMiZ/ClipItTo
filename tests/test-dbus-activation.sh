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

dbus-run-session --config-file="$work/bus.conf" -- sh -c '
    cli=$1 daemon=$2
    "$cli" --status > status.txt 2>&1 || { cat status.txt; echo "FAIL: no reply from an activated daemon"; exit 1; }
    grep -q "^backend: " status.txt || { cat status.txt; echo "FAIL: unexpected status"; exit 1; }
    "$cli" --ingest activated
    "$cli" --status | grep -q "^items: 1$" || { echo "FAIL: ingest not stored"; exit 1; }
    pkill -x -f "$daemon" # the private bus goes away next; do not leave the daemon behind
' sh "$cli" "$daemon"
echo "PASS: the daemon was started by D-Bus activation"
