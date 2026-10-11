#!/bin/sh
# Installs ClipItTo on Ubuntu 24.04+ (amd64) from the GitHub release: downloads the .deb,
# checks its SHA-256 against the release, and installs it with apt (which pulls in the
# Qt libraries it needs). Needs sudo.
#
#   curl -fsSL https://github.com/NiMiZ/ClipItTo/releases/latest/download/install.sh | sh
#   sh install.sh [--version 0.1.0] [--download-only DIR]
set -eu

repo=NiMiZ/ClipItTo
version=
download_dir=

die() { echo "install.sh: $*" >&2; exit 1; }
usage() {
    cat <<'EOF'
usage: install.sh [--version X.Y.Z] [--download-only DIR]
  --version X.Y.Z       install that release instead of the latest
  --download-only DIR   only download and verify the .deb into DIR
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
    --version) [ $# -ge 2 ] || die "--version needs a value"; version=${2#v}; shift 2 ;;
    --download-only) [ $# -ge 2 ] || die "--download-only needs a directory"; download_dir=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
    esac
done

[ "$(uname -s)" = Linux ] || die "this script is for Linux; on Windows use install.ps1"
command -v curl >/dev/null || die "curl is missing: sudo apt install curl"
if [ -z "$download_dir" ]; then
    command -v apt-get >/dev/null || die "apt is missing: this package is for Ubuntu/Debian"
    arch=$(dpkg --print-architecture)
    [ "$arch" = amd64 ] || die "only amd64 builds are published (this machine: $arch)"
fi

if [ -n "$version" ]; then
    api=https://api.github.com/repos/$repo/releases/tags/v$version
else
    api=https://api.github.com/repos/$repo/releases/latest
fi
echo "Looking up the release..."
release=$(curl -fsSL -H 'Accept: application/vnd.github+json' "$api") ||
    die "cannot read $api (no such release, or GitHub is unreachable)"

# The asset's URL and its "digest" field are on separate lines of the API's JSON.
url=$(printf '%s\n' "$release" | sed -n 's/.*"browser_download_url": *"\([^"]*_amd64\.deb\)".*/\1/p' | head -n 1)
[ -n "$url" ] || die "the release has no amd64 .deb"
name=${url##*/}
sha=$(printf '%s\n' "$release" |
    awk -v n="\"$name\"" '$0 ~ "\"name\": *" n {found=1} found && /"digest":/ {print; exit}' |
    sed -n 's/.*"sha256:\([0-9a-f]\{64\}\)".*/\1/p')

if [ -n "$download_dir" ]; then
    mkdir -p "$download_dir"
    dir=$download_dir
else
    dir=$(mktemp -d)
    trap 'rm -f "$dir/$name"; rmdir "$dir"' EXIT
    chmod 755 "$dir" # apt reads the file as its unprivileged _apt user
fi

echo "Downloading $name..."
curl -fL --progress-bar -o "$dir/$name" "$url"
if [ -n "$sha" ]; then
    echo "$sha  $dir/$name" | sha256sum -c --quiet - || die "checksum mismatch: the download is damaged"
    echo "Checksum OK."
else
    echo "Note: the release lists no checksum for $name; skipping the check."
fi

if [ -n "$download_dir" ]; then
    echo "Saved $dir/$name"
    exit 0
fi

chmod 644 "$dir/$name"
sudo=
[ "$(id -u)" -eq 0 ] || sudo=sudo
echo "Installing (apt may ask for your password)..."
$sudo apt-get install -y "$dir/$name"

cat <<'EOF'

ClipItTo is installed. To finish, as your normal user:
  1. Log out and back in (so GNOME Shell loads the ClipItTo extension).
  2. Run: clipitto-setup
Then press Ctrl+Alt+V to open your clipboard history.
To remove it later: sudo apt remove clipitto
EOF
