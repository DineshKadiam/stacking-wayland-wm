#!/bin/sh
set -eu
tool=$(realpath "$1")
buttons=$(realpath "$2")
cow=$(realpath "$3")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM
export XDG_DATA_HOME="$tmp/data" XDG_DATA_DIRS="$tmp/empty"
export XDG_RUNTIME_DIR="$tmp/runtime" WAYLAND_DISPLAY=missing
export XDG_CURRENT_DESKTOP=CoW LC_ALL=C
mkdir -p "$XDG_DATA_HOME/applications" "$XDG_DATA_DIRS" \
    "$XDG_RUNTIME_DIR" "$tmp/bin"
export PATH="$tmp/bin:$PATH"
printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/cowiconman"
chmod +x "$tmp/bin/cowiconman"
# Keep default-handler discovery independent of the host desktop settings.
cat > "$tmp/bin/xdg-mime" <<'EOF'
#!/bin/sh
test "$1" = query && test "$2" = default || exit 1
test "$3" = text/plain && echo TextEditor.desktop
exit 0
EOF
chmod +x "$tmp/bin/xdg-mime"
cp "$tmp/bin/cowiconman" "$tmp/bin/foot"
cp "$tmp/bin/cowiconman" "$tmp/bin/cowpager"
mkdir -p "$XDG_DATA_HOME/icons/hicolor/16x16/apps"
cat > "$XDG_DATA_HOME/icons/hicolor/16x16/apps/start-here.xpm" <<'EOF'
/* XPM */
static char *icon[] = {
"1 1 1 1",
"x c #ffffff",
"x"
};
EOF
cat > "$XDG_DATA_HOME/applications/a-missing.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Missing editor
Exec=cow-test-no-such-program-827364
Categories=TextEditor;
EOF
cat > "$XDG_DATA_HOME/applications/editor's.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=An "Editor"
Exec=true
Categories=TextEditor;WebBrowser;
EOF
cp "$4" "$tmp/panel"
mkdir "$tmp/icons"
cp "$(dirname "$4")/../../share/cow/icons/default.png" "$tmp/icons/default.png"
ln -s "$tool" "$tmp/bin/cow-appmenu"
if "$tool" --format cowbuttons >/dev/null 2>&1; then exit 1; fi
"$5" "$tmp/panel" 1

for role in TerminalEmulator WebBrowser FileManager TextEditor Email AudioVideo Graphics Development; do
    printf '[Desktop Entry]\nType=Application\nName=%s\nExec=true\nCategories=%s;\n' \
        "$role" "$role" > "$XDG_DATA_HOME/applications/$role.desktop"
done
"$5" "$tmp/panel" 8

# A missing role is backfilled rather than leaving a hole in the row.
sed 's/Categories=AudioVideo;/Categories=Utility;/' \
    "$XDG_DATA_HOME/applications/AudioVideo.desktop" > "$tmp/backfill.desktop"
cp "$tmp/backfill.desktop" "$XDG_DATA_HOME/applications/AudioVideo.desktop"
"$5" "$tmp/panel" 8

# An explicit desktop ID takes precedence, even outside the named category.
sed 's/TerminalEmulator,WebBrowser/TerminalEmulator=TextEditor.desktop,WebBrowser/' \
    "$tmp/panel" > "$tmp/preferred"
"$5" "$tmp/preferred" 8 TextEditor.desktop

# Optional discovery and embedded programs may all be absent. The handle,
# Applications button and window-list fallback must still form a panel.
mkdir "$tmp/no-programs"
PATH="$tmp/no-programs" "$5" "$tmp/panel" 0

# Task lists have a caller-supplied identity; missing values must not fall
# back to an ambiguous wildcard title.
if COW_STARTER_TASK_TITLE= "$6" -c "$tmp/panel" > "$tmp/iconman.log" 2>&1; then exit 1; fi
grep -q 'title-env requires' "$tmp/iconman.log"
if COW_STARTER_TASK_TITLE=cowbuttons:Starter:DP-1-tasks \
    "$6" -c "$tmp/panel" > "$tmp/iconman.log" 2>&1; then exit 1; fi
grep -q 'no Wayland display' "$tmp/iconman.log"

# The embedded pager reads the same file and tracks the panel's output.
if "$8" -f "$tmp/panel" -m DP-1 > "$tmp/pager.log" 2>&1; then exit 1; fi
grep -q 'no Wayland display' "$tmp/pager.log" || {
    cat "$tmp/pager.log" >&2
    exit 1
}

# Read the complete scoped file, including its multiline quoted alias.
# Without IPC, loading should fail only when submitting the cow block.
if "$buttons" -c "$tmp/panel" > "$tmp/scoped.log" 2>&1; then exit 1; fi
grep -q 'cow configuration failed' "$tmp/scoped.log" || {
    cat "$tmp/scoped.log" >&2
    exit 1
}

# Validate both grammars without contacting a running CoW or Wayland session.
# cowbuttons submits the embedded configuration inside an outer script.
awk 'BEGIN {print "script {\nlet panel_title = \"cowbuttons:Starter\"\nlet panel_output = \"@current\""}
    /^cow \{/ {cow=1; next} cow && /^}/ {cow=0; next} cow
    END {print "}"}' \
    "$tmp/panel" > "$tmp/cow.conf"
"$7" --starter "$tmp/cow.conf"
awk '/^cow \{/ {cow=1; next} cow && /^}/ {cow=0; next} !cow' \
    "$tmp/panel" > "$tmp/buttons.conf"
# Restarting a panel must safely replace its existing rules.
cat "$tmp/cow.conf" "$tmp/cow.conf" > "$tmp/reload.conf"
if "$cow" -n -c "$tmp/reload.conf" > "$tmp/cow.log" 2>&1; then exit 1; fi
grep -q 'failed to connect to Wayland display' "$tmp/cow.log"
! grep 'ERROR' "$tmp/cow.log" | grep -qv 'failed to connect to Wayland display'
if "$buttons" -c "$tmp/buttons.conf" > "$tmp/buttons.log" 2>&1; then exit 1; fi
grep -q 'no Wayland display' "$tmp/buttons.log" || {
    cat "$tmp/buttons.log" >&2
    exit 1
}
