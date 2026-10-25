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

phase_started=$(date +%s)
phase_done()
{
	now=$(date +%s)
	printf 'changelog: %s: %ss\n' "$1" "$((now - phase_started))" >&2
	phase_started=$now
}

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
pids=
workers=0
cleanup()
{
	# Let bounded requests finish before removing their output directory.
	for pid in $pids; do
		wait "$pid" 2>/dev/null || :
	done
	rm -f "$raw" "$formatted" "$attributed" "$release" "$updated"
	rm -rf "$contributors_dir"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

header=$(printf '## [%s](%s/%s/releases/tag/%s) - %s' \
	"$version" "$forgejo_url" "$repository" "$version" \
	"$(date +%Y-%m-%d)")
header="$header
"

set --
if [ -n "${RNA_WORKDIR:-}" ]; then
	set -- --workdir "$RNA_WORKDIR"
fi
attempt=1
delay=5
rna_errors="$contributors_dir/rna-errors"
while :; do
	printf 'changelog: release-notes-assistant attempt %s/3\n' "$attempt" >&2
	# RNA can leave incomplete output when an API request fails.
	: > "$raw"
	if "$assistant" "$@" \
		--config .release-notes-assistant.yaml \
		--forgejo-url "$forgejo_url" \
		--repository "$repository" \
		--token "${GITEA_TOKEN:-}" \
		--tag-from-version "$tag_format" \
		--render-header "$header" \
		--storage file \
		--storage-location "$raw" \
		release "$version" 2> "$rna_errors"; then
		cat "$rna_errors" >&2
		break
	else
		status=$?
	fi
	cat "$rna_errors" >&2
	# Retry only recognized transient failures, not arbitrary RNA panics or
	# configuration/authentication errors. Our contributor curl calls below
	# have their own request-level retries.
	if [ "$attempt" -ge 3 ] || ! grep -Eq \
	    'status (408|429|500|502|503|504)([[:space:]]|$)|i/o timeout|TLS handshake timeout|connection reset by peer|Temporary failure in name resolution' \
	    "$rna_errors"; then
		exit "$status"
	fi
	printf 'changelog: transient RNA failure; retrying in %ss\n' "$delay" >&2
	sleep "$delay"
	attempt=$((attempt + 1))
	delay=$((delay * 2))
done
phase_done 'release-notes-assistant'

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
api_get()
{
	destination=$1
	endpoint=$2
	shift 2
	if [ -n "$token" ]; then
		set -- -H "Authorization: token $token" "$@"
	fi
	# Use a file so curl can discard partial output before retrying.
	curl -fsSL -G --connect-timeout 10 --max-time 30 \
		--retry 2 --retry-max-time 60 --output "$destination" \
		"$@" "$forgejo_url/api/v1/repos/$repository/$endpoint"
}

api_get_oldest_pulls()
{
	api_get "$3" pulls \
		--data-urlencode state=closed \
		--data-urlencode poster="$1" \
		--data-urlencode sort=oldest \
		--data-urlencode limit=50 \
		--data-urlencode page="$2"
}

wait_workers()
{
	failed=false
	for pid in $pids; do
		wait "$pid" || failed=true
	done
	[ "$failed" = false ] || exit 1
	pids=
	workers=0
}

# The raw RNA output contains all PRs selected for the previous-tag..HEAD
# release range, including entries which formatting later excludes.
sed -n 's#^  - \[PR\]([^)]*/pulls/\([0-9][0-9]*\)).*#\1#p' \
	"$raw" | sort -nu |
jq -Rsc 'split("\n") | map(select(length > 0) | tonumber)' \
	> "$contributors_dir/release-numbers.json"

wanted=$(cat "$contributors_dir/release-numbers.json")
# Bulk fetch recent PRs, but do not walk unrelated repository history.
# Older PRs updated since the release are resolved individually below.
page_limit=$(( ($(jq length "$contributors_dir/release-numbers.json") + 49) / 50 ))
: > "$contributors_dir/matched.jsonl"
page=1
while [ "$wanted" != "[]" ] && [ "$page" -le "$page_limit" ]; do
	page_file="$contributors_dir/page-$page.json"
	api_get "$page_file" pulls \
		--data-urlencode state=closed \
		--data-urlencode sort=recentupdate \
		--data-urlencode limit=50 --data-urlencode page="$page"
	jq -c --argjson wanted "$wanted" \
		'.[] | select(.number as $n | $wanted | index($n))' \
		"$page_file" >> "$contributors_dir/matched.jsonl"
	wanted=$(jq -c --argjson wanted "$wanted" \
		'$wanted - map(.number)' "$page_file")
	[ "$(jq 'length' "$page_file")" -lt 50 ] && break
	page=$((page + 1))
done

for number in $(printf '%s\n' "$wanted" | jq -r '.[]'); do
	api_get "$contributors_dir/pull-$number.json" "pulls/$number" &
	pids="$pids $!"
	workers=$((workers + 1))
	if [ "$workers" -eq 4 ]; then
		wait_workers
	fi
done
wait_workers
for number in $(printf '%s\n' "$wanted" | jq -r '.[]'); do
	jq -ce --argjson number "$number" \
		'select(.number == $number and (.user.login | type == "string"))' \
		"$contributors_dir/pull-$number.json" >> "$contributors_dir/matched.jsonl"
done
jq -s 'unique_by(.number)' "$contributors_dir/matched.jsonl" \
	> "$contributors_dir/release-prs.json"
phase_done 'release PR lookup and formatting'
jq -r '.[] | select(.user.login != null) |
    [.number, .user.login] | @tsv' "$contributors_dir/release-prs.json" \
	> "$contributors_dir/authors.tsv"

# Attribute every rendered pull-request entry to its opener.  Using the
# Forgejo account avoids trying to infer identities from commit email
# addresses, which is unreliable for rebased and maintainer-edited PRs.
awk -F '\t' -v forgejo_url="$forgejo_url" '
	FILENAME == ARGV[1] {
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

while read -r login; do
	(
		page=1
		first=
		while :; do
			page_file="$contributors_dir/oldest-$login-$page.json"
			api_get_oldest_pulls "$login" "$page" "$page_file"
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
	workers=$((workers + 1))
	if [ "$workers" -eq 4 ]; then
		wait_workers
	fi
done < "$contributors_dir/release-authors"

wait_workers
phase_done 'first-contributor lookup'
requests=$(find "$contributors_dir" -name 'page-*.json' -o \
	-name 'pull-*.json' -o -name 'oldest-*.json' | wc -l)
printf 'changelog: contributor API requests: %s (excluding retries)\n' "$requests" >&2

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
phase_done 'changelog assembly and Markdown formatting'
