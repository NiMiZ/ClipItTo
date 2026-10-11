#!/bin/sh
# Fetches everything needed to build and drive ClipItTo on Ubuntu 24.04 WITHOUT root:
# Qt 6 / SQLite / toml++ / X11 dev packages and Xvfb / xdotool / xclip, unpacked with
# `apt-get download` + `dpkg -x` into DIR/sysroot. Writes DIR/env.sh to source afterwards.
#   usage: setup-noroot.sh DIR
set -eu
DIR=$(mkdir -p "$1" && cd "$1" && pwd)
mkdir -p "$DIR/debs" "$DIR/sysroot"
cd "$DIR/debs"
apt-get download \
  qt6-base-dev qt6-base-dev-tools qmake6 qmake6-bin qt6-qpa-plugins \
  libqt6core6t64 libqt6dbus6t64 libqt6gui6t64 libqt6widgets6t64 libqt6test6t64 libqt6network6t64 \
  libqt6opengl6t64 libqt6concurrent6t64 libqt6sql6t64 libqt6xml6t64 libqt6printsupport6t64 \
  libsqlite3-dev libtomlplusplus-dev libtomlplusplus3t64 libdouble-conversion3 libb2-1 libmd4c0 libpcre2-16-0 \
  libgl-dev libglx-dev libopengl-dev libegl-dev libgles-dev libgl1 libglx0 libopengl0 libegl1 libglvnd0 libgles2 \
  libglvnd-dev libglx-mesa0 libegl-mesa0 libxkbcommon0 libxkbcommon-x11-0 libts0t64 \
  libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 libxcb-util1 libxcb-xinput0 libxcb-xkb1 \
  libx11-dev libxtst-dev libxext-dev libxi-dev x11proto-dev libxfixes-dev libxau-dev libxdmcp-dev libxcb1-dev \
  qtkeychain-qt6-dev libqt6keychain1 libsecret-1-0 \
  xvfb xdotool libxdo3 xclip >/dev/null
for d in *.deb; do dpkg -x "$d" "$DIR/sysroot"; done
L="$DIR/sysroot/usr/lib/x86_64-linux-gnu"
# Development symlinks point at runtime libraries that were not unpacked: use the system's.
for f in "$L"/*.so; do [ -e "$f" ] || ln -sf "/usr/lib/x86_64-linux-gnu/$(readlink "$f")" "$f"; done
cat > "$DIR/env.sh" <<EOF
export CMAKE_PREFIX_PATH="$DIR/sysroot/usr"
export LD_LIBRARY_PATH="$L\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$L/qt6/plugins"
export PATH="$DIR/sysroot/usr/bin:\$PATH"
EOF
echo "ready: . $DIR/env.sh"
