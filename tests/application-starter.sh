#!/bin/sh
set -eu
launcher=$(realpath "$1")
terminal_launcher=$(realpath "$2")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM
mkdir "$tmp/bin" "$tmp/runtime"
export XDG_RUNTIME_DIR="$tmp/runtime" STARTER_LOG="$tmp/log"
export PATH="$tmp/bin:$PATH"
cat > "$tmp/bin/cow-appmenu" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >> "$STARTER_LOG"
echo 'menu-add CoWApplications Example title'
exit "${GENERATOR_STATUS:-0}"
EOF
cat > "$tmp/bin/moocow" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >> "$STARTER_LOG"
if [ "$1" = include ]; then
	test -s "$2"
	cat "$2" >> "$STARTER_LOG"
fi
exit "${IPC_STATUS:-0}"
EOF
chmod +x "$tmp/bin/cow-appmenu" "$tmp/bin/moocow"
sh "$launcher" 'terminal with spaces'
grep -qx 'terminal with spaces' "$STARTER_LOG"
grep -qx menu-show "$STARTER_LOG"
test -z "$(ls -A "$tmp/runtime")"

: > "$STARTER_LOG"
sh "$launcher" --load 'terminal with spaces'
grep -qx include "$STARTER_LOG"
grep -qx 'terminal with spaces' "$STARTER_LOG"
! grep -qx menu-show "$STARTER_LOG"
test -z "$(ls -A "$tmp/runtime")"

# With no override, defer terminal discovery to the shared launcher.
: > "$STARTER_LOG"
sh "$launcher" --load
grep -qx cow-terminal "$STARTER_LOG"

# Partial generation must never reach CoW, and failed imports must not open
# the previous menu. Both failures must clean up their private temporary file.
for failure in generation import; do
	: > "$STARTER_LOG"
	if [ "$failure" = generation ]; then
		if GENERATOR_STATUS=1 sh "$launcher"; then exit 1; fi
		! grep -qx include "$STARTER_LOG"
		grep -q 'previous menu was kept' "$STARTER_LOG"
	else
		if IPC_STATUS=1 sh "$launcher"; then exit 1; fi
	fi
	! grep -qx menu-show "$STARTER_LOG"
	grep -qx CoWApplicationsError "$STARTER_LOG"
	test -z "$(ls -A "$tmp/runtime")"
done

# A notification failure must not mask the original generator exit status.
status=0
GENERATOR_STATUS=7 IPC_STATUS=3 sh "$launcher" || status=$?
test "$status" -eq 7

# A later successful refresh dismisses the error, without opening the menu.
: > "$STARTER_LOG"
sh "$launcher" --load
grep -qx CoWApplicationsError "$STARTER_LOG"
grep -qx -- -d "$STARTER_LOG"
! grep -qx menu-show "$STARTER_LOG"

# A missing runtime directory fails before generation and reports why.
: > "$STARTER_LOG"
if XDG_RUNTIME_DIR="$tmp/missing" sh "$launcher" --load; then exit 1; fi
grep -q 'Could not create a temporary application menu' "$STARTER_LOG"
! grep -qx include "$STARTER_LOG"

# Terminal selection is shared by the default alias and desktop-entry
# launches. Preserve argument boundaries and prefer foot when both exist.
mkdir "$tmp/terminals"
cat > "$tmp/terminals/xterm" <<'EOF'
#!/bin/sh
printf '%s\n' "$0" "$@" > "$STARTER_LOG"
EOF
chmod +x "$tmp/terminals/xterm"
PATH="$tmp/terminals" /bin/sh "$terminal_launcher" -e example 'two words'
grep -qx "$tmp/terminals/xterm" "$STARTER_LOG"
grep -qx 'two words' "$STARTER_LOG"
cp "$tmp/terminals/xterm" "$tmp/terminals/foot"
PATH="$tmp/terminals" /bin/sh "$terminal_launcher"
grep -qx "$tmp/terminals/foot" "$STARTER_LOG"
mkdir "$tmp/no-terminals"
status=0
PATH="$tmp/no-terminals" /bin/sh "$terminal_launcher" 2>/dev/null || status=$?
test "$status" -eq 127
