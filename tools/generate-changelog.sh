#!/bin/sh

set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
	echo "usage: $0 VERSION RELEASE_NOTES_ASSISTANT RELEASE_NOTES [CHANGELOG]" >&2
	exit 1
fi

version=$1
assistant=$2
release_notes=$3
changelog=${4:-CHANGELOG.md}
forgejo_url=${FORGEJO_SERVER_URL:-https://codeberg.org}
repository=${FORGEJO_REPOSITORY:-cow-wm/cow}

case "$version" in
	*.*.*) tag_format='%[1]d.%[2]d.%[3]d' ;;
	*.*) tag_format='%[1]d.%[2]d' ;;
	*)
		echo "invalid release version: $version" >&2
		exit 1
		;;
esac

raw=$(mktemp)
formatted=$(mktemp)
updated=$(mktemp)
trap 'rm -f "$raw" "$formatted" "$updated"' EXIT HUP INT TERM

header=$(printf '## [%s](%s/%s/releases/tag/%s) - %s' \
	"$version" "$forgejo_url" "$repository" "$version" \
	"$(date +%Y-%m-%d)")
header="$header
"

"$assistant" \
	--config .release-notes-assistant.yaml \
	--forgejo-url "$forgejo_url" \
	--repository "$repository" \
	--token "${GITEA_TOKEN:-}" \
	--tag-from-version "$tag_format" \
	--render-header "$header" \
	--storage file \
	--storage-location "$raw" \
	release "$version"

awk '
	/^<!--start release-notes-assistant-->$/ { next }
	/^<!--URL:/ { next }
	/^<!--end release-notes-assistant-->$/ { next }
	/^$/ { next }
	$0 == "- Excluded" { excluded = 1; next }
	excluded && /^- / { excluded = 0 }
	excluded { next }
	/^## / {
		print
		print ""
		next
	}
	/^  - / {
		sub(/^  - /, "- ")
		gsub(/<!--number [^>]*-->/, "")
		gsub(/<!--line [^>]*-->/, "")
		gsub(/<!--description [^>]*-->/, "")
		gsub(/<!--description-->/, "")
		print
		next
	}
	/^- / {
		if (have_category)
			print ""
		have_category = 1
		sub(/^- /, "### ")
		print
		print ""
		next
	}
	{ print }
' "$raw" > "$formatted"

mkdir -p "$(dirname "$release_notes")"
cp "$formatted" "$release_notes"

if grep -Fq "## [$version]" "$changelog"; then
	echo "CHANGELOG already contains release $version" >&2
	exit 1
fi

{
	printf '# Changelog\n\n'
	cat "$formatted"
	printf '\n'
	tail -n +2 "$changelog" | sed '1{/^$/d;}'
} > "$updated"
mv "$updated" "$changelog"
