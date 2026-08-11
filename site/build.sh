#!/bin/sh

set -eu

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
