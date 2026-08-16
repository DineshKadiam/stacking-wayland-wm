#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output=${1:-_site}
base_url=${2:-http://localhost:8000/}
hugo=${HUGO:-hugo}

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

"$hugo" \
	--source site \
	--destination "$destination" \
	--baseURL "$base_url" \
	--gc \
	--minify

sh "$script_dir/build-man-pages.sh" "$destination/man" "$base_url"
