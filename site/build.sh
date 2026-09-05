#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
output=${1:-_site}
base_url=${2:-http://localhost:8000/}
hugo=${HUGO:-hugo}
release_preview_file=${COW_RELEASE_PREVIEW:-}

case "$output" in
	""|/|.)
		echo "refusing unsafe output directory: $output" >&2
		exit 1
		;;
esac

case "$output" in
	/*) destination=$output ;;
	*) destination=$(pwd)/$output ;;
esac

development_version=$(CDPATH= cd -- "$repo_root" && \
	"$repo_root/tools/cow-version-str.sh")
release_preview=
release_preview_version=
release_preview_summary=
release_preview_changes=0
release_preview_contributors=0
if [ -n "$release_preview_file" ]; then
	if [ ! -s "$release_preview_file" ]; then
		echo "release preview is missing or empty: $release_preview_file" >&2
		exit 1
	fi
	release_preview=$(cat "$release_preview_file")
	release_preview_version=$(sed -n \
		's/^## \[\([^]]*\)\].*/\1/p' "$release_preview_file")
	release_preview_summary=$(awk '
		/^- \[PR\]/ {
			print
			if (++entries == 3)
				exit
		}
	' "$release_preview_file")
	release_preview_changes=$(grep -c '^- \[PR\]' \
		"$release_preview_file" || true)
	release_preview_contributors=$(awk '
		/^### New contributors$/ { contributors = 1; next }
		contributors && /^### / { contributors = 0 }
		contributors && /^- / { count++ }
		END { print count + 0 }
	' "$release_preview_file")
fi
release_tag=$(git -C "$repo_root" for-each-ref --merged=HEAD \
	--sort=-creatordate --format='%(refname:short)' refs/tags |
	awk '/^[0-9]+(\.[0-9]+)*$/ { print; exit }')
release_date=
if [ -n "$release_tag" ]; then
	release_date=$(git -C "$repo_root" for-each-ref \
		--format='%(creatordate:format:%B %Y)' \
		"refs/tags/$release_tag")
fi

HUGO_PARAMS_DEVELOPMENTVERSION=$development_version \
HUGO_PARAMS_RELEASEVERSION=$release_tag \
HUGO_PARAMS_RELEASEDATE=$release_date \
HUGO_PARAMS_RELEASEPREVIEW="$release_preview" \
HUGO_PARAMS_RELEASEPREVIEWVERSION="$release_preview_version" \
HUGO_PARAMS_RELEASEPREVIEWSUMMARY="$release_preview_summary" \
HUGO_PARAMS_RELEASEPREVIEWCHANGES=$release_preview_changes \
HUGO_PARAMS_RELEASEPREVIEWCONTRIBUTORS=$release_preview_contributors \
"$hugo" \
	--source site \
	--destination "$destination" \
	--baseURL "$base_url" \
	--cleanDestinationDir \
	--gc \
	--minify

set -- \
	--version "$development_version" \
	--channel "the development branch" \
	--link-prefix ../../ \
	--manuals-href ../
if [ -n "$release_tag" ]; then
	set -- "$@" \
		--compare-ref "$release_tag" \
		--compare-label "release $release_tag"
fi
sh "$script_dir/build-man-pages.sh" "$@" \
	"$destination/man/development" "$base_url"

if [ -n "$release_tag" ]; then
	release_source=$(mktemp -d)
	trap 'rm -rf "$release_source"' EXIT HUP INT TERM
	git -C "$repo_root" archive "$release_tag" man |
		tar -x -C "$release_source"
	sh "$script_dir/build-man-pages.sh" \
		--source-root "$release_source" \
		--git-root "$repo_root" \
		--git-ref "$release_tag" \
		--version "$release_tag" \
		--channel "release ${release_tag}" \
		--link-prefix ../../ \
		--manuals-href ../ \
		"$destination/man/release" "$base_url"
fi
