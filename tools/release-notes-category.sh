#!/bin/sh

set -eu

labels=$(jq -r '
    if .IsBackportedFrom and (.BackportedFrom | length > 0) then
        .BackportedFrom[0]
    else
        .Pr
    end
    | .labels[]?.name
')

has_label()
{
	printf '%s\n' "$labels" | grep -Fqx -- "$1"
}

if has_label skip/changelog; then
	printf '99 Excluded'
elif has_label type/new-feature || has_label type/feature-request; then
	printf '00 Added'
elif has_label type/bug; then
	printf '10 Fixed'
elif has_label type/augmentation; then
	printf '20 Changed'
elif has_label config/remove-cmd; then
	printf '30 Removed'
elif has_label relates/docs; then
	printf '40 Documentation'
elif has_label type/internal-improvement; then
	printf '50 Internal improvements'
else
	printf '90 Other changes'
fi
