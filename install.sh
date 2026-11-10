#!/bin/sh
# Installs ClipItTo on Ubuntu 24.04 or 26.04 (amd64) from the GitHub release: downloads the .deb,
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

base=https://github.com/$repo/releases
echo "Looking up the release..."
if [ -n "$version" ]; then
    tag=v$version
else
    # The latest release's page redirects to .../releases/tag/vX.Y.Z: no API, no rate limit.
    latest=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "$base/latest") || die "cannot reach $base"
    tag=${latest##*/}
    case "$tag" in
    v[0-9]*) ;;
    *) die "no published release found at $base" ;;
    esac
fi
# One .deb per Ubuntu release (library names and sonames differ): 26.04 has its own, 24.04 the default.
variant=
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    variant=$(. /etc/os-release; [ "${ID:-}" = ubuntu ] && [ "${VERSION_ID%%.*}" = 26 ] && echo _ubuntu26.04) || variant=
fi
name=clipitto_${tag#v}${variant}_amd64.deb
url=$base/download/$tag/$name

# The checksum: from the release's SHA256SUMS, else (older releases) from the API.
sha=$(curl -fsSL "$base/download/$tag/SHA256SUMS" 2>/dev/null | awk -v n="$name" '$2 == n || $2 == "*" n {print $1; exit}')
if [ -z "$sha" ]; then
    # The asset's "name" and "digest" fields are on separate lines of the API's JSON.
    sha=$(curl -fsSL -H 'Accept: application/vnd.github+json' "https://api.github.com/repos/$repo/releases/tags/$tag" 2>/dev/null |
        awk -v n="\"$name\"" '$0 ~ "\"name\": *" n {found=1} found && /"digest":/ {print; exit}' |
        sed -n 's/.*"sha256:\([0-9a-f]\{64\}\)".*/\1/p')
fi

if [ -n "$download_dir" ]; then
    mkdir -p "$download_dir"
    dir=$download_dir
else
    dir=$(mktemp -d)
    trap 'rm -f "$dir/$name"; rmdir "$dir"' EXIT
    chmod 755 "$dir" # apt reads the file as its unprivileged _apt user
fi

echo "Downloading $name..."
curl -fL --progress-bar -o "$dir/$name" "$url" || die "cannot download $url (is $tag a release with an amd64 .deb?)"
if [ -n "$sha" ]; then
    echo "$sha  $dir/$name" | sha256sum -c --quiet - || die "checksum mismatch: the download is damaged"
    echo "Checksum OK."
else
    echo "Note: no checksum available for $name (GitHub's API may be rate-limited); not checked."
fi

if [ -n "$download_dir" ]; then
    echo "Saved $dir/$name"
    exit 0
fi

chmod 644 "$dir/$name"
sudo=
[ "$(id -u)" -eq 0 ] || sudo=sudo
if dpkg-query -W -f='${Status}' clipitto 2>/dev/null | grep -q '^install ok installed$'; then
    was_installed=1
else
    was_installed=0
fi
echo "Installing (apt may ask for your password)..."
# Fresh package lists, so apt finds the Qt libraries the package depends on.
$sudo apt-get update -qq
$sudo apt-get install -y "$dir/$name"

if command -v systemctl >/dev/null && systemctl --user is-active --quiet clipitto.service; then
    echo "Restarting the running ClipItTo daemon..."
    systemctl --user restart clipitto.service
fi

if [ "$was_installed" -eq 1 ]; then
    cat <<'EOF'

ClipItTo is updated. Your existing settings, including your configured shortcut, are preserved.
Use your configured shortcut to open your clipboard history; you can change it in ClipItTo Settings.
If GNOME Shell needs to reload an updated extension, log out and back in.
EOF
else
    cat <<'EOF'

ClipItTo is installed. To finish, as your normal user:
  1. Log out and back in (so GNOME Shell loads the extension).
  2. Run: clipitto-setup
Then press Ctrl+Alt+V to open your clipboard history.
EOF
fi

cat <<'EOF'
To remove it later: sudo apt remove clipitto
EOF
