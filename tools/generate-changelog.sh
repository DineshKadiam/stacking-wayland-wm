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
attributed=$(mktemp)
release=$(mktemp)
updated=$(mktemp)
contributors_dir=$(mktemp -d "${TMPDIR:-/tmp}/cow-contributors.XXXXXX")
cleanup()
{
	rm -f "$raw" "$formatted" "$attributed" "$release" "$updated"
	rm -rf "$contributors_dir"
}
trap cleanup EXIT HUP INT TERM

header=$(printf '## [%s](%s/%s/releases/tag/%s) - %s' \
	"$version" "$forgejo_url" "$repository" "$version" \
	"$(date +%Y-%m-%d)")
header="$header
"

set --
if [ -n "${RNA_WORKDIR:-}" ]; then
	set -- --workdir "$RNA_WORKDIR"
fi
"$assistant" "$@" \
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

token=${GITEA_TOKEN:-}
api_get_pull_page()
{
	if [ -n "$token" ]; then
		curl -fsSL -G -H "Authorization: token $token" \
			--data-urlencode state=closed \
			--data-urlencode sort=recentupdate \
			--data-urlencode limit=50 \
			--data-urlencode page="$1" \
			"$forgejo_url/api/v1/repos/$repository/pulls"
	else
		curl -fsSL -G \
			--data-urlencode state=closed \
			--data-urlencode sort=recentupdate \
			--data-urlencode limit=50 \
			--data-urlencode page="$1" \
			"$forgejo_url/api/v1/repos/$repository/pulls"
	fi
}

api_get_oldest_pulls()
{
	if [ -n "$token" ]; then
		curl -fsSL -G -H "Authorization: token $token" \
			--data-urlencode state=closed \
			--data-urlencode poster="$1" \
			--data-urlencode sort=oldest \
			--data-urlencode limit=50 \
			--data-urlencode page="$2" \
			"$forgejo_url/api/v1/repos/$repository/pulls"
	else
		curl -fsSL -G \
			--data-urlencode state=closed \
			--data-urlencode poster="$1" \
			--data-urlencode sort=oldest \
			--data-urlencode limit=50 \
			--data-urlencode page="$2" \
			"$forgejo_url/api/v1/repos/$repository/pulls"
	fi
}

# The raw RNA output contains all PRs selected for the previous-tag..HEAD
# release range, including entries which formatting later excludes.
sed -n 's#^  - \[PR\]([^)]*/pulls/\([0-9][0-9]*\)).*#\1#p' \
	"$raw" | sort -nu |
jq -Rsc 'split("\n") | map(select(length > 0) | tonumber)' \
	> "$contributors_dir/release-numbers.json"

wanted=$(cat "$contributors_dir/release-numbers.json")
page=1
while [ "$wanted" != "[]" ]; do
	page_file="$contributors_dir/page-$page.json"
	api_get_pull_page "$page" > "$page_file"
	missing=$(jq -s --argjson wanted "$wanted" \
		'$wanted - (add | map(.number)) | length' \
		"$contributors_dir"/page-*.json)
	[ "$missing" -eq 0 ] && break
	[ "$(jq 'length' "$page_file")" -lt 50 ] && break
	page=$((page + 1))
done

if [ "$wanted" = "[]" ]; then
	printf '[]\n' > "$contributors_dir/release-prs.json"
else
	jq -s --argjson wanted "$wanted" \
		'add | map(select(.number as $n | $wanted | index($n))) |
		    unique_by(.number)' \
		"$contributors_dir"/page-*.json \
		> "$contributors_dir/release-prs.json"
fi
jq -r '.[] | select(.user.login != null) |
    [.number, .user.login] | @tsv' "$contributors_dir/release-prs.json" \
	> "$contributors_dir/authors.tsv"

# Attribute every rendered pull-request entry to its opener.  Using the
# Forgejo account avoids trying to infer identities from commit email
# addresses, which is unreliable for rebased and maintainer-edited PRs.
awk -F '\t' -v forgejo_url="$forgejo_url" '
	NR == FNR {
		author[$1] = $2
		next
	}
	{
		line = $0
		if (line ~ /\/pulls\/[0-9]+/) {
			number = line
			sub(/^.*\/pulls\//, "", number)
			sub(/[^0-9].*$/, "", number)
			if (number in author)
				line = line " — [@" author[number] "](" \
				    forgejo_url "/" author[number] ")"
		}
		print line
	}
' "$contributors_dir/authors.tsv" "$formatted" > "$attributed"

# Query only the authors in this release.  Oldest-first ordering normally
# finds the first merged PR in one request; pagination only handles a run of
# old, unmerged PRs defensively.
: > "$contributors_dir/new-contributors.tsv"
jq -r '.[].user.login // empty' "$contributors_dir/release-prs.json" |
sort -fu > "$contributors_dir/release-authors"

pids=
while read -r login; do
	(
		page=1
		first=
		while :; do
			page_file="$contributors_dir/oldest-$login-$page.json"
			api_get_oldest_pulls "$login" "$page" > "$page_file"
			first=$(jq -r \
				'[.[] | select(.merged == true)][0].number // empty' \
				"$page_file")
			[ -n "$first" ] && break
			[ "$(jq 'length' "$page_file")" -lt 50 ] && break
			page=$((page + 1))
		done
		[ -z "$first" ] || printf '%s\t%s\n' "$login" "$first"
	) > "$contributors_dir/first-$login.tsv" &
	pids="$pids $!"
done < "$contributors_dir/release-authors"

failed=false
for pid in $pids; do
	wait "$pid" || failed=true
done
[ "$failed" = false ] || exit 1

for first_file in "$contributors_dir"/first-*.tsv; do
	[ -e "$first_file" ] || continue
	while IFS="	" read -r login first; do
		if jq -e --arg login "$login" --argjson number "$first" \
		    '.[] | select(.number == $number and
		        .user.login == $login)' \
		    "$contributors_dir/release-prs.json" >/dev/null; then
			printf '%s\t%s\n' "$login" "$first"
		fi
	done < "$first_file"
done | sort -f > "$contributors_dir/new-contributors.tsv"

if [ -s "$contributors_dir/new-contributors.tsv" ]; then
	printf '### New contributors\n\n' \
		> "$contributors_dir/new-contributors.md"
	while IFS="	" read -r login number; do
		printf -- '- [@%s](%s/%s) contributed for the first time in [PR #%s](%s/%s/pulls/%s).\n' \
			"$login" "$forgejo_url" "$login" "$number" \
			"$forgejo_url" "$repository" "$number"
	done < "$contributors_dir/new-contributors.tsv" \
		>> "$contributors_dir/new-contributors.md"
fi

# Place first-time contributors immediately below the release heading, before
# the categorized changes.  Keep the same layout when there are none.
{
	head -n 1 "$attributed"
	printf '\n'
	if [ -s "$contributors_dir/new-contributors.md" ]; then
		cat "$contributors_dir/new-contributors.md"
		printf '\n'
	fi
	tail -n +2 "$attributed" | sed '1{/^$/d;}'
} > "$release"

mkdir -p "$(dirname "$release_notes")"
cp "$release" "$release_notes"

if grep -Fq "## [$version]" "$changelog"; then
	echo "CHANGELOG already contains release $version" >&2
	exit 1
fi

{
	printf '# Changelog\n\n'
	cat "$release"
	printf '\n'
	tail -n +2 "$changelog" | sed '1{/^$/d;}'
} > "$updated"
mv "$updated" "$changelog"

# Keep generated release notes consistent with the repository's Markdown.
tools/format-markdown.py "$release_notes" "$changelog"
