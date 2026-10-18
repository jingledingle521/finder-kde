#!/usr/bin/env bash
# Build Finder from this checkout and install it for the current user, into
# ~/.local/opt/finder with a launcher entry. Stock Dolphin is untouched:
# Finder is a separate app with its own settings (finderrc).
#
#   finder-extras/install.sh           build and install
#   finder-extras/install.sh --deps    first install Dolphin's build
#                                      dependencies (Fedora; asks for your password)
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
prefix=$HOME/.local/opt/finder
apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications

if [ "${1:-}" = --deps ]; then
    pkexec dnf -y builddep dolphin
fi

for tool in cmake ninja g++; do
    command -v "$tool" >/dev/null || { echo "missing $tool (try: $0 --deps)" >&2; exit 1; }
done

cmake -S "$root" -B "$root/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_RPATH="$prefix/lib64" \
    -DBUILD_TESTING=OFF
# Half the cores: a full-width build can run a 16 GB machine out of memory.
ninja -C "$root/build" -j"$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 ))"
ninja -C "$root/build" install

mkdir -p "$apps"
sed "s|@PREFIX@|$prefix|" "$root/finder-extras/org.brhett.finder.desktop" > "$apps/org.brhett.finder.desktop"
update-desktop-database "$apps" 2>/dev/null || true

# Finder's Open/Save windows for other apps, through xdg-desktop-portal.
config=${XDG_CONFIG_HOME:-$HOME/.config}
services=${XDG_DATA_HOME:-$HOME/.local/share}/dbus-1/services
mkdir -p "$services" "$config/xdg-desktop-portal"
sed "s|@PREFIX@|$prefix|" "$root/finder-extras/org.freedesktop.impl.portal.desktop.finder.service" \
    > "$services/org.freedesktop.impl.portal.desktop.finder.service"
if [ -f "$config/xdg-desktop-portal/kde-portals.conf" ]; then
    grep -q 'FileChooser=finder' "$config/xdg-desktop-portal/kde-portals.conf" \
        || echo "Add org.freedesktop.impl.portal.FileChooser=finder;kde under [preferred] in $config/xdg-desktop-portal/kde-portals.conf"
else
    cp "$root/finder-extras/kde-portals.conf" "$config/xdg-desktop-portal/kde-portals.conf"
fi
# xdg-desktop-portal only reads backend descriptions from its system folder.
portal=/usr/share/xdg-desktop-portal/portals/finder.portal
if ! cmp -s "$root/finder-extras/finder.portal" "$portal"; then
    pkexec install -m 644 "$root/finder-extras/finder.portal" "$portal" \
        || echo "Skipped the Finder file chooser: $portal needs installing as root."
fi
systemctl --user restart xdg-desktop-portal.service 2>/dev/null || true

echo "Finder installed. Launch it from the app menu, or: $prefix/bin/finder"
echo "For AirDrop, run finder-extras/install-airdrop."
