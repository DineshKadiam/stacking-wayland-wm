#!/bin/sh
set -eu
tool=$(realpath "$1")
cow=$(realpath "$2")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export XDG_DATA_HOME="$tmp/data" XDG_DATA_DIRS="$tmp/empty"
export XDG_CURRENT_DESKTOP=CoW LC_ALL=C
mkdir -p "$XDG_DATA_HOME/applications" "$XDG_DATA_DIRS"
mkdir -p "$XDG_DATA_HOME/icons/hicolor/16x16/apps" \
    "$XDG_DATA_HOME/icons/hicolor/32x32/apps" \
    "$XDG_DATA_HOME/desktop-directories" "$XDG_DATA_DIRS/pixmaps"
cat > "$XDG_DATA_HOME/icons/hicolor/16x16/apps/example.xpm" <<'EOF'
/* XPM */
static char *icon[] = {
"1 1 1 1",
"x c #ffffff",
"x"
};
EOF
cp "$XDG_DATA_HOME/icons/hicolor/16x16/apps/example.xpm" \
    "$XDG_DATA_HOME/icons/hicolor/32x32/apps/example.xpm"
cp "$XDG_DATA_HOME/icons/hicolor/16x16/apps/example.xpm" \
    "$XDG_DATA_DIRS/pixmaps/folder.xpm"
cat > "$XDG_DATA_HOME/desktop-directories/category.directory" <<'EOF'
[Desktop Entry]
Type=Directory
Name=Category
Icon=folder
EOF
cat > "$XDG_DATA_HOME/applications/odd'file.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Odd "quotes"; #text\nsecond line
Exec=true
Icon=example
EOF
cat > "$XDG_DATA_HOME/applications/dbus.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=DBus only
DBusActivatable=true
EOF
cat > "$XDG_DATA_HOME/applications/missing.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Missing executable
Exec=/definitely/not/an/installed/program
EOF
cat > "$tmp/apps.menu" <<'EOF'
<Menu><Name>Apps</Name><DefaultAppDirs/><DefaultDirectoryDirs/>
  <Menu><Name>Category</Name><Directory>category.directory</Directory>
    <Include><All/></Include></Menu>
</Menu>
EOF
"$tool" --menu "$tmp/apps.menu" --format cow --name 'My Apps' > "$tmp/generated" 2> "$tmp/warnings"
grep -q 'menu-add -T -- "My Apps/Apps/Category"' "$tmp/generated"
grep -q '"popup My Apps/Apps/Category"' "$tmp/generated"
grep -q '"popup My Apps/all"' "$tmp/generated"
grep -q 'menu-add -d -- .*"DBus only" nop' "$tmp/generated"
grep -q 'disabled unsupported launcher' "$tmp/warnings"
grep -q 'menu-add -d -- .*"Missing executable" nop' "$tmp/generated"
grep -q 'disabled unavailable launcher' "$tmp/warnings"
grep -Fq -- "-i $XDG_DATA_HOME/icons/hicolor/16x16/apps/example.xpm" "$tmp/generated"
grep -Fq -- "-i $XDG_DATA_DIRS/pixmaps/folder.xpm" "$tmp/generated"
"$tool" --menu "$tmp/apps.menu" --format cow --icon-size 32 > "$tmp/large" 2>/dev/null
grep -Fq -- "-i $XDG_DATA_HOME/icons/hicolor/32x32/apps/example.xpm" "$tmp/large"
for size in 0 -1 1025 invalid; do
    if "$tool" --menu "$tmp/apps.menu" --format cow --icon-size "$size" > /dev/null 2>&1; then
        exit 1
    fi
done
# Loading twice must be valid even with escaped labels and filenames.
cat "$tmp/generated" "$tmp/generated" > "$tmp/config"

# An empty menu is valid, but should explain why it has no launchers.
cat > "$tmp/empty.menu" <<'EOF'
<Menu><Name>Empty</Name><DefaultAppDirs/>
  <DefaultLayout show_empty="true"/>
  <Menu><Name>Child</Name></Menu>
</Menu>
EOF
"$tool" --menu "$tmp/empty.menu" --format cow > "$tmp/empty.conf"
grep -q 'menu-add -d -- Applications/Empty/Child "No applications match this menu" nop' "$tmp/empty.conf"
! grep -q 'popup Applications/all' "$tmp/empty.conf"
cat "$tmp/empty.conf" >> "$tmp/config"
cat > "$tmp/empty.menu" <<'EOF'
<Menu><Name>Empty</Name></Menu>
EOF
"$tool" --menu "$tmp/empty.menu" --format cow > "$tmp/empty.conf"
grep -q 'menu-add -d -- Applications "No applications match this menu" nop' "$tmp/empty.conf"
cat "$tmp/empty.conf" >> "$tmp/config"
status=0
XDG_RUNTIME_DIR="$tmp" WAYLAND_DISPLAY=missing COW_LOG_LEVEL=info \
    "$cow" -n -c "$tmp/config" > "$tmp/log" 2>&1 || status=$?
if test "$status" -ne 1 ||
    ! grep -q 'config loaded from' "$tmp/log" ||
    ! grep -q 'failed to connect to Wayland display' "$tmp/log" ||
    grep 'ERROR' "$tmp/log" | grep -qv 'failed to connect to Wayland display'; then
    cat "$tmp/log" >&2
    exit 1
fi
