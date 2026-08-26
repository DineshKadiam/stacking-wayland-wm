#!/bin/sh

set -eu

rna_version=1.7.3
rna_sha256=3e7442dec237d092683b025521747c1a2ddb13613f79cafaeb558823ece34c7d

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_dir=$(dirname "$script_dir")
version=$(sed -n 's/^VERSION=//p' \
	"$repository_dir/tools/cow-version-str.sh")
preview_dir=$(mktemp -d "${TMPDIR:-/tmp}/cow-changelog.XXXXXX")
assistant="$preview_dir/release-notes-assistant"
release_notes="$preview_dir/release-notes.md"
changelog="$preview_dir/CHANGELOG.md"

url="https://code.forgejo.org/forgejo/release-notes-assistant"
url="${url}/releases/download/v${rna_version}/release-notes-assistant"
curl -fsSL -o "$assistant" "$url"
printf '%s  %s\n' "$rna_sha256" "$assistant" | sha256sum -c -
chmod +x "$assistant"

cp "$repository_dir/CHANGELOG.md" "$changelog"

(
	cd "$repository_dir"
	tools/generate-changelog.sh "$version" "$assistant" \
		"$release_notes" "$changelog"
)

printf 'Changelog preview: %s\n' "$changelog"
printf 'Release notes preview: %s\n' "$release_notes"

if [ -t 1 ]; then
	if command -v glow >/dev/null 2>&1; then
		glow "$changelog"
	elif command -v mdcat >/dev/null 2>&1; then
		mdcat "$changelog"
	elif command -v nix >/dev/null 2>&1; then
		nix shell nixpkgs#glow --command glow -t "$changelog"
	else
		less "$changelog"
	fi
fi
