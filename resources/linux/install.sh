#!/bin/sh
# Installs LYNX SHARE for the current user (no root): the program in ~/.local/opt/LYNX, a launcher in ~/.local/bin,
# the menu entry, the icon and the .lynx file type. Run it again to update; ./install.sh --uninstall removes it.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
data="${XDG_DATA_HOME:-$HOME/.local/share}"
opt="$HOME/.local/opt/LYNX"
bin="$HOME/.local/bin"

if [ "$1" = "--uninstall" ]; then
    rm -rf "$opt" "$bin/LYNX" "$data/applications/lynx.desktop" "$data/icons/hicolor/256x256/apps/lynx.png" "$data/mime/packages/lynx-mime.xml"
    command -v update-mime-database > /dev/null && update-mime-database "$data/mime" || true
    command -v update-desktop-database > /dev/null && update-desktop-database "$data/applications" || true
    echo "LYNX SHARE removed (your settings stay in ${XDG_CONFIG_HOME:-$HOME/.config}/LYNX)."
    exit 0
fi

mkdir -p "$opt" "$bin" "$data/applications" "$data/icons/hicolor/256x256/apps" "$data/mime/packages"
cp -r "$here"/. "$opt"/
ln -sf "$opt/LYNX" "$bin/LYNX"
sed "s|^Exec=.*|Exec=\"$opt/LYNX\" %F|" "$here/lynx.desktop" > "$data/applications/lynx.desktop"
cp "$here/lynx.png" "$data/icons/hicolor/256x256/apps/lynx.png"
cp "$here/lynx-mime.xml" "$data/mime/packages/lynx-mime.xml"
command -v update-mime-database > /dev/null && update-mime-database "$data/mime" || true
command -v update-desktop-database > /dev/null && update-desktop-database "$data/applications" || true
command -v gtk-update-icon-cache > /dev/null && gtk-update-icon-cache -q "$data/icons/hicolor" 2> /dev/null || true
echo "LYNX SHARE installed: start it from the applications menu or run $bin/LYNX"
