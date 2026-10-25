#!/bin/sh
# Standalone, offline regression check for release tooling (requires jq).
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export FIXTURES="$tmp"
export PATH="$tmp:$PATH"

cat > "$tmp/assistant" <<'EOF'
#!/bin/sh
while [ "$#" -gt 0 ]; do
    case "$1" in
        --storage-location) shift; output=$1 ;;
    esac
    shift
done
cp "$FIXTURES/raw" "$output"
EOF
cat > "$tmp/curl" <<'EOF'
#!/bin/sh
set -eu
poster=
connect=false
bounded=false
while [ "$#" -gt 0 ]; do
    case "$1" in
        --output) shift; output=$1 ;;
        --connect-timeout) connect=true; shift ;;
        --max-time) bounded=true; shift ;;
        --data-urlencode)
            shift
            case "$1" in
                poster=*) poster=${1#poster=} ;;
                page=*) test "$1" = page=1 ;;
            esac ;;
        https://*) url=$1 ;;
    esac
    shift
done
test "$connect" = true
test "$bounded" = true
printf '%s %s\n' "$url" "$poster" >> "$FIXTURES/requests"
test ! -f "$FIXTURES/fail"
case "$url" in
    */pulls/2) printf '{"number":2,"user":{"login":"new"}}' ;;
    */pulls)
        case "$poster" in
            new) printf '[{"number":2,"merged":true}]' ;;
            old) printf '[{"number":99,"merged":true}]' ;;
            *) printf '[{"number":1,"user":{"login":"old"}}]' ;;
        esac ;;
    *) exit 1 ;;
esac > "$output"
EOF
chmod +x "$tmp/assistant" "$tmp/curl"

printf '# Changelog\n\n## [0.3]\n\nOld release.\n' > "$tmp/original"
cat > "$tmp/raw" <<'EOF'
## [0.4]
- Features
  - [PR](https://codeberg.org/cow-wm/cow/pulls/1) Existing author
  - [PR](https://codeberg.org/cow-wm/cow/pulls/2) New author
EOF
cp "$tmp/original" "$tmp/changelog"
sh tools/generate-changelog.sh 0.4 "$tmp/assistant" "$tmp/notes" "$tmp/changelog"
grep -q '### New contributors' "$tmp/notes"
grep -q '\[@new\]' "$tmp/notes"
grep -q '\[@old\]' "$tmp/notes"
test "$(wc -l < "$tmp/requests")" -eq 4
grep -q 'Old release.' "$tmp/changelog"

# A failed API request must not publish incomplete attribution.
touch "$tmp/fail"
cp "$tmp/original" "$tmp/changelog"
if sh tools/generate-changelog.sh 0.4 "$tmp/assistant" "$tmp/notes" "$tmp/changelog"; then
    exit 1
fi
cmp "$tmp/original" "$tmp/changelog"
rm "$tmp/fail" "$tmp/requests"

# Empty author input must not swallow the release text in awk.
printf '## [0.4]\n- Features\n  - No pull requests\n' > "$tmp/raw"
cp "$tmp/original" "$tmp/changelog"
sh tools/generate-changelog.sh 0.4 "$tmp/assistant" "$tmp/notes" "$tmp/changelog"
grep -q 'No pull requests' "$tmp/notes"
test ! -e "$tmp/requests"
