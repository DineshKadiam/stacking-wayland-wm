#!/bin/sh
set -eu
tool=$(realpath "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export CAPTURE="$tmp/result"
export XDG_CURRENT_DESKTOP=CoW LC_ALL=C

cat > "$tmp/record" <<'EOF'
#!/bin/sh
pwd > "$CAPTURE"
printf '<%s>\n' "$@" >> "$CAPTURE"
EOF
chmod +x "$tmp/record"
cat > "$tmp/test.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Label %f \$(not-a-command)
Icon=example
Path=$tmp
Exec=$tmp/record "two words" "" %c %k %i %U %%
EOF
"$tool" --launch "$tmp/test.desktop"
cat > "$tmp/expected" <<EOF
$tmp
<two words>
<>
<Label %f \$(not-a-command)>
<$tmp/test.desktop>
<--icon>
<example>
<%>
EOF
cmp "$tmp/expected" "$CAPTURE"

printf '\nTerminal=true\n' >> "$tmp/test.desktop"
"$tool" --launch "$tmp/test.desktop" --terminal "$tmp/record"
test "$(sed -n '2p' "$CAPTURE")" = '<-e>'
test "$(sed -n '3p' "$CAPTURE")" = "<$tmp/record>"

# Desktop-entry string escapes are decoded before Exec quoting.
cat > "$tmp/test.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Escapes
Exec=record "\\$HOME" "\\`id\\`" "a\\\\b" "a\\"b"
EOF
PATH="$tmp:$PATH" "$tool" --launch "$tmp/test.desktop"
tail -n 4 "$CAPTURE" > "$tmp/actual"
cat > "$tmp/expected" <<'EOF'
<$HOME>
<`id`>
<a\b>
<a"b>
EOF
cmp "$tmp/expected" "$tmp/actual"

for exec in 'record %Z' 'record "unterminated' 'record "%c"' \
    'record prefix%i' 'record %f %U' 'record ; echo unsafe' \
    'record "a"b' '=bad'; do
    printf '[Desktop Entry]\nType=Application\nName=Bad\nExec=%s\n' "$exec" > "$tmp/test.desktop"
    if "$tool" --launch "$tmp/test.desktop" > /dev/null 2>&1; then
        exit 1
    fi
done
printf '[Desktop Entry]\nType=Application\nName=DBus\nDBusActivatable=true\n' > "$tmp/test.desktop"
if "$tool" --launch "$tmp/test.desktop" > /dev/null 2>&1; then
    exit 1
fi
