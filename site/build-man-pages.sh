#!/bin/sh

set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 output-directory base-url" >&2
	exit 1
fi

output=$1
base_url=$2
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=$("$repo_root/tools/cow-version-str.sh")
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
	  <link rel="icon" href="../assets/Only-CoW.svg" type="image/svg+xml">
	  <link rel="stylesheet" href="../styles.css">
	  <link rel="stylesheet" href="../man.css">
	  <script src="../script.js" defer></script>
	</head>
	<body class="manual-page">
	  <a class="skip-link" href="#manual-content">Skip to manual</a>
	  <header class="desktop-panel">
	    <nav aria-label="Primary navigation">
	      <a href="../">Home</a>
	      <a href="./">Manuals</a>
	      <a href="https://codeberg.org/thomasadam/cow">Source</a>
	      <a href="https://codeberg.org/thomasadam/cow/wiki/Home">Documentation</a>
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
	  <p>Manuals generated from the current CoW source tree, version ${version}.</p>
	</header>
	<ul class="manual-index">
	EOF

for source in "$repo_root"/man/*.scd.in; do
	filename=$(basename -- "$source")
	source_relative=${source#"$repo_root"/}
	page=${filename%.scd.in}
	name=${page%.*}
	section=${page##*.}
	roff=$tmpdir/$page
	html_roff=$tmpdir/$page.html.roff
	raw_fragment=$tmpdir/$page.raw.html
	fragment=$tmpdir/$page.html

	if ! source_date_epoch=$(git -C "$repo_root" log -1 --format=%ct \
	    -- "$source_relative"); then
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

	printf '  <li><a href="%s.html"><code>%s(%s)</code></a></li>\n' \
	    "$page" "$name" "$section" >> "$index"
done

cat >> "$index" <<-EOF
	</ul>
	EOF
page_end >> "$index"

printf 'Generated manual pages in %s for %s\n' "$output" "$base_url"
