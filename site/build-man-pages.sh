#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
source_root=$repo_root
git_root=$repo_root
git_ref=HEAD
compare_ref=
compare_label=
version=
channel="the current CoW source tree"
link_prefix=../
manuals_href=./

usage()
{
	echo "usage: $0 [options] output-directory base-url" >&2
	echo "  --source-root directory" >&2
	echo "  --git-root directory" >&2
	echo "  --git-ref ref" >&2
	echo "  --compare-ref ref" >&2
	echo "  --compare-label description" >&2
	echo "  --version version" >&2
	echo "  --channel description" >&2
	echo "  --link-prefix prefix" >&2
	echo "  --manuals-href href" >&2
	exit 1
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--source-root|--git-root|--git-ref|--compare-ref|--compare-label|--version|--channel|--link-prefix|--manuals-href)
		[ "$#" -ge 2 ] || usage
		case "$1" in
		--source-root) source_root=$2 ;;
		--git-root) git_root=$2 ;;
		--git-ref) git_ref=$2 ;;
		--compare-ref) compare_ref=$2 ;;
		--compare-label) compare_label=$2 ;;
		--version) version=$2 ;;
		--channel) channel=$2 ;;
		--link-prefix) link_prefix=$2 ;;
		--manuals-href) manuals_href=$2 ;;
		esac
		shift 2
		;;
	--)
		shift
		break
		;;
	-*) usage ;;
	*) break ;;
	esac
done

[ "$#" -eq 2 ] || usage
output=$1
base_url=$2

if [ -z "$version" ]; then
	version=$("$source_root/tools/cow-version-str.sh")
fi
if [ -n "$compare_ref" ]; then
	git -C "$git_root" rev-parse --verify --quiet \
	    "$compare_ref^{commit}" >/dev/null || {
		echo "unknown comparison ref: $compare_ref" >&2
		exit 1
	}
	if [ -z "$compare_label" ]; then
		compare_label=$compare_ref
	fi
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

mkdir -p "$output"

page_start()
{
	title=$1
	cat <<-EOF
	<!doctype html>
	<html lang="en">
	<head>
	  <meta charset="utf-8">
	  <meta name="viewport" content="width=device-width, initial-scale=1">
	  <meta name="theme-color" content="#b8b8b8">
	  <title>${title} — CoW manual</title>
	  <link rel="icon" href="${link_prefix}assets/Only-CoW.svg" type="image/svg+xml">
	  <link rel="stylesheet" href="${link_prefix}styles.css">
	  <link rel="stylesheet" href="${link_prefix}man.css">
	  <script src="${link_prefix}script.js" defer></script>
	</head>
	<body class="manual-page">
	  <a class="skip-link" href="#manual-content">Skip to manual</a>
	  <header class="desktop-panel">
	    <nav aria-label="Primary navigation">
	      <a href="${link_prefix}">Home</a>
	      <a href="${manuals_href}">Manuals</a>
	      <a href="https://codeberg.org/cow-wm/cow">Source</a>
	      <a href="https://codeberg.org/cow-wm/cow/wiki/Home">Documentation</a>
	    </nav>
	  </header>
	  <main class="manual-main">
	    <section class="window manual-window" aria-labelledby="manual-title">
	      <div class="titlebar">
	        <button class="titlebar-button shade" type="button" data-window-action="shade" aria-label="Shade this manual" aria-expanded="true"></button>
	        <span class="window-title" id="manual-title">${title}</span>
	        <button class="titlebar-button minimise" type="button" data-window-action="minimise" aria-label="Minimise this manual" aria-expanded="true"></button>
	        <button class="titlebar-button maximise" type="button" data-window-action="maximise" aria-label="Maximise this manual" aria-pressed="false"></button>
	      </div>
	      <div class="window-content manual-content" id="manual-content">
	EOF
}

page_end()
{
	cat <<-EOF
	      </div>
	    </section>
	  </main>
	  <aside class="window-tray" aria-label="Closed manuals" hidden>
	    <span>Windows:</span>
	    <div class="window-tray-items"></div>
	  </aside>
	</body>
	</html>
	EOF
}

index=$output/index.html
page_start "Manual pages" > "$index"
cat >> "$index" <<-EOF
	<header class="manual-index-header">
	  <h2>CoW manual pages</h2>
	  <p>Manuals generated from ${channel}, version ${version}.</p>
	EOF

if [ -n "$compare_ref" ]; then
	printf '  <p class="manual-change-key">Manuals with a highlighted background have changed since %s.</p>\n' \
	    "$compare_label" >> "$index"
fi
cat >> "$index" <<-EOF
	</header>
	<ul class="manual-index">
	EOF

set -- "$source_root"/man/*.md.in
if [ ! -e "$1" ]; then
	set -- "$source_root"/man/*.scd.in
fi

for source do
	filename=$(basename -- "$source")
	source_relative=${source#"$source_root"/}
	case "$filename" in
	*.md.in) page=${filename%.md.in}; source_format=markdown ;;
	*.scd.in) page=${filename%.scd.in}; source_format=scdoc ;;
	*) continue ;;
	esac
	name=${page%.*}
	section=${page##*.}
	markdown=$tmpdir/$page.md
	roff=$tmpdir/$page.roff
	html_roff=$tmpdir/$page.html.roff
	raw_fragment=$tmpdir/$page.raw.html
	fragment=$tmpdir/$page.html

	if ! source_date=$(git -C "$git_root" log -1 --format=%cs \
	    "$git_ref" -- "$source_relative") || [ -z "$source_date" ]; then
		source_date=$(date -u +%Y-%m-%d)
	fi
	if [ "$source_format" = markdown ]; then
		awk -v version="$version" -v date="$source_date" '
			{ gsub(/@VERSION@/, version); gsub(/@DATE@/, date); print }
		' "$source" > "$markdown"
		lowdown -t html "$markdown" > "$raw_fragment"
		awk '
			BEGIN {
				split("cow cow-module-cmd cowbar cowbuttons cowclock cowdiag cowiconman cowident cowpager cowrearrange moocow", manuals)
			}
			{
				for (i in manuals) {
					from = "<strong>" manuals[i] "</strong>\\(1\\)"
					to = "<a href=\"" manuals[i] ".1.html\"><strong>" manuals[i] "</strong>(1)</a>"
					gsub(from, to)
				}
				print
			}
		' "$raw_fragment" > "$fragment"
	else
		if ! source_date_epoch=$(git -C "$git_root" log -1 --format=%ct \
		    "$git_ref" -- "$source_relative") ||
		    [ -z "$source_date_epoch" ]; then
			source_date_epoch=$(date +%s)
		fi
		awk -v version="$version" '{ gsub(/@VERSION@/, version); print }' \
		    "$source" | SOURCE_DATE_EPOCH=$source_date_epoch scdoc > "$roff"
		awk '
			$0 == ".nf" { in_literal = 1 }
			in_literal && $0 == "" {
				print "COW_HTML_BLANK_LINE_SENTINEL"
				next
			}
			{ print }
			$0 == ".fi" { in_literal = 0 }
		' "$roff" > "$html_roff"
		mandoc -Thtml -Ofragment,man=%N.%S.html "$html_roff" > "$raw_fragment"
		awk '
			/<pre([[:space:]>])/ { in_pre = 1 }
			!(in_pre && /^[[:space:]]*<br[[:space:]]*\/>[[:space:]]*$/) {
				gsub(/COW_HTML_BLANK_LINE_SENTINEL/, "")
				print
			}
			/<\/pre>/ { in_pre = 0 }
		' "$raw_fragment" > "$fragment"
	fi

	page_start "$name($section)" > "$output/$page.html"
	if [ "$source_format" = markdown ]; then
		printf '        <article class="manual-text">\n' >> "$output/$page.html"
	fi
	cat "$fragment" >> "$output/$page.html"
	if [ "$source_format" = markdown ]; then
		printf '        </article>\n' >> "$output/$page.html"
	fi
	page_end >> "$output/$page.html"

	class=
	label="$name($section)"
	if [ -n "$compare_ref" ] &&
	    ! git -C "$git_root" diff --quiet "$compare_ref" "$git_ref" -- \
	    "$source_relative"; then
		class=' class="manual-index-changed"'
		label="$label, changed since $compare_label"
	fi
	printf '  <li><a%s href="%s.html" aria-label="%s"><code>%s(%s)</code></a></li>\n' \
	    "$class" "$page" "$label" "$name" "$section" >> "$index"
done

cat >> "$index" <<-EOF
	</ul>
	EOF
page_end >> "$index"

printf 'Generated manual pages in %s for %s\n' "$output" "$base_url"
