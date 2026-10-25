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
set -eu
while [ "$#" -gt 0 ]; do
    case "$1" in
        --storage-location) shift; output=$1 ;;
    esac
    shift
done
if [ -f "$FIXTURES/rna-failure" ]; then
    test ! -s "$output"
    printf 'attempt\n' >> "$FIXTURES/attempts"
    mode=$(cat "$FIXTURES/rna-failure")
    if [ "$mode" != once ] || [ "$(wc -l < "$FIXTURES/attempts")" -eq 1 ]; then
        printf 'Partial release\n' > "$output"
        case "$mode" in
            permanent) echo 'panic: invalid configuration' >&2 ;;
            *) echo 'panic: Get /api/v1/repos/cow-wm/cow/pulls?sort=recentupdate&limit=100&page=1: status 504 Gateway Timeout' >&2 ;;
        esac
        exit 2
    fi
fi
cp "$FIXTURES/raw" "$output"
EOF
cat > "$tmp/sleep" <<'EOF'
#!/bin/sh
printf '%s\n' "$1" >> "$FIXTURES/delays"
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
chmod +x "$tmp/assistant" "$tmp/curl" "$tmp/sleep"

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

# Retry transient RNA failures, discard partial output, and stop after three
# attempts. Unrelated failures must fail immediately without publishing.
for mode in once always permanent; do
    printf '%s\n' "$mode" > "$tmp/rna-failure"
    : > "$tmp/attempts"
    : > "$tmp/delays"
    cp "$tmp/original" "$tmp/changelog"
    printf 'Existing notes\n' > "$tmp/notes"
    status=0
    sh tools/generate-changelog.sh 0.4 "$tmp/assistant" "$tmp/notes" "$tmp/changelog" || status=$?
    case "$mode" in
        once)
            test "$status" -eq 0
            test "$(wc -l < "$tmp/attempts")" -eq 2
            test "$(cat "$tmp/delays")" = 5
            grep -q 'No pull requests' "$tmp/notes"
            ! grep -q Partial "$tmp/notes" ;;
        always)
            test "$status" -eq 2
            test "$(wc -l < "$tmp/attempts")" -eq 3
            test "$(tr '\n' ' ' < "$tmp/delays")" = '5 10 ' ;;
        permanent)
            test "$status" -eq 2
            test "$(wc -l < "$tmp/attempts")" -eq 1
            test ! -s "$tmp/delays" ;;
    esac
    if [ "$mode" != once ]; then
        cmp "$tmp/original" "$tmp/changelog"
        test "$(cat "$tmp/notes")" = 'Existing notes'
    fi
done
