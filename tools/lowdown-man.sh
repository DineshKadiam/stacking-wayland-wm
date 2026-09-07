#!/bin/sh

# Lowdown forces roff tables to expand across the available line width.  This
# adds large leading gaps inside otherwise left-aligned columns.  Keep its
# column alignment, restore a compact layout, and disable roff justification
# while rendering each table.

set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 lowdown input" >&2
	exit 2
fi

"$1" -s -t man "$2" |
while IFS= read -r line || [ -n "$line" ]; do
	case $line in
	.TS)
		printf '.na\n.TS\n'
		;;
	'tab(|) expand allbox;')
		printf 'tab(|) allbox;\n'
		;;
	.TE)
		printf '.TE\n.ad b\n'
		;;
	*)
		printf '%s\n' "$line"
		;;
	esac
done
