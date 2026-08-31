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

for source in "$source_root"/man/*.scd.in; do
	filename=$(basename -- "$source")
	source_relative=${source#"$source_root"/}
	page=${filename%.scd.in}
	name=${page%.*}
	section=${page##*.}
	roff=$tmpdir/$page
	html_roff=$tmpdir/$page.html.roff
	raw_fragment=$tmpdir/$page.raw.html
	fragment=$tmpdir/$page.html

	if ! source_date_epoch=$(git -C "$git_root" log -1 --format=%ct \
	    "$git_ref" -- "$source_relative") || [ -z "$source_date_epoch" ]; then
		source_date_epoch=$(date +%s)
	fi
	awk -v version="$version" '{ gsub(/@VERSION@/, version); print }' \
	    "$source" | SOURCE_DATE_EPOCH=$source_date_epoch scdoc > "$roff"
	# mandoc discards physical blank lines inside roff no-fill regions. Keep
	# them as text through conversion, then remove the marker from the HTML.
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
	# mandoc also emits redundant standalone <br/> elements in some <pre>
	# blocks. The source newlines already provide the required line breaks.
	awk '
		/<pre([[:space:]>])/ { in_pre = 1 }
		!(in_pre && /^[[:space:]]*<br[[:space:]]*\/>[[:space:]]*$/) {
			gsub(/COW_HTML_BLANK_LINE_SENTINEL/, "")
			print
		}
		/<\/pre>/ { in_pre = 0 }
	' "$raw_fragment" > "$fragment"

	page_start "$name($section)" > "$output/$page.html"
	cat "$fragment" >> "$output/$page.html"
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
