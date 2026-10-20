#!/bin/sh
set -eu

tool=$1
case "$tool" in
    /*) ;;
    *) tool="$(pwd)/$tool" ;;
esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export XDG_DATA_HOME="$tmp/user"
export XDG_DATA_DIRS="$tmp/system"
export XDG_CURRENT_DESKTOP=CoW
export XDG_CONFIG_HOME="$tmp/config-user"
export XDG_CONFIG_DIRS="$tmp/config-system:$tmp/config-other"
unset XDG_MENU_PREFIX
export LC_ALL=C
mkdir -p "$tmp/user/applications" "$tmp/system/applications/nested"

entry()
{
    printf '[Desktop Entry]\nType=Application\nName=%s\nExec=true\n' "$2" > "$1"
}

entry "$tmp/system/applications/masked.desktop" Masked
printf '[Desktop Entry]\nHidden=true\n' > "$tmp/user/applications/masked.desktop"
entry "$tmp/system/applications/override.desktop" Old
entry "$tmp/user/applications/override.desktop" Replacement
entry "$tmp/system/applications/nested/tool.desktop" Nested
entry "$tmp/system/applications/hidden.desktop" Hidden
printf 'NoDisplay=true\n' >> "$tmp/system/applications/hidden.desktop"
entry "$tmp/system/applications/other.desktop" Other
printf 'OnlyShowIn=Other;\n' >> "$tmp/system/applications/other.desktop"
entry "$tmp/system/applications/not.desktop" Excluded
printf 'NotShowIn=CoW;\n' >> "$tmp/system/applications/not.desktop"
entry "$tmp/system/applications/missing.desktop" Missing
printf 'TryExec=/definitely/not/a/program\n' >> "$tmp/system/applications/missing.desktop"
entry "$tmp/system/applications/visible.desktop" Visible
printf 'OnlyShowIn=Other;CoW;\n' >> "$tmp/system/applications/visible.desktop"
entry "$tmp/system/applications/Visible.desktop" CaseSensitive
ln -s .. "$tmp/system/applications/nested/cycle"

"$tool" > "$tmp/result"
grep -q '"Name".*"Replacement"' "$tmp/result"
grep -q '"id".*"nested-tool.desktop"' "$tmp/result"
grep -q '"Name".*"Visible"' "$tmp/result"
test "$(grep -c '"id"' "$tmp/result")" -eq 4
if grep -Eq '"Name".*"(Masked|Old|Hidden|Other|Excluded|Missing)"' "$tmp/result"; then
    exit 1
fi

# Locale fallback must prefer language+modifier over an unrelated locale.
printf 'Name[fr]=Francais\nName[fr@latin]=Latin\n' >> "$tmp/user/applications/override.desktop"
LC_ALL=fr_CA.UTF-8@latin "$tool" > "$tmp/localized"
grep -q '"Name".*"Latin"' "$tmp/localized"
"$tool" --help > /dev/null
if "$tool" --invalid > /dev/null 2>&1; then
    exit 1
fi

printf 'Categories=Utility;Test;\n' >> "$tmp/user/applications/override.desktop"
cat > "$tmp/test.menu" <<'EOF'
<!DOCTYPE Menu PUBLIC "-//freedesktop//DTD Menu 1.0//EN"
 "http://www.freedesktop.org/standards/menu-spec/menu-1.0.dtd">
<Menu>
  <Name>Applications</Name>
  <DefaultAppDirs/>
  <Menu>
    <Name>Tools</Name>
    <Include><Category>Utility</Category></Include>
    <Exclude><All/></Exclude>
    <Include><And><Category>Test</Category><Not><Filename>missing.desktop</Filename></Not></And></Include>
  </Menu>
  <Menu><Name>Hidden</Name><Deleted/><Include><All/></Include></Menu>
  <Menu><Name>Empty</Name><Include><Category>NoSuchCategory</Category></Include></Menu>
</Menu>
EOF
"$tool" --menu "$tmp/test.menu" > "$tmp/menu-result"
actual=$(sed -n '/"menu":/,$p' "$tmp/menu-result" | tr -d ' \t\r\n')
expected='"menu":{"id":"Applications","name":"Applications","applications":[],"menus":[{"id":"Applications/Tools","name":"Tools","applications":[{"id":"override.desktop","file":"'"$tmp"'/user/applications/override.desktop"}],"menus":[],"entries":[{"type":"application","id":"override.desktop","file":"'"$tmp"'/user/applications/override.desktop"}]},{"id":"Applications/Empty","name":"Empty","applications":[],"menus":[],"entries":[]}],"entries":[{"type":"menu","id":"Applications/Tools"}]}}'
test "$actual" = "$expected"

# Unsupported constructs must fail even in an empty or deleted menu.
for element in LegacyDir; do
    printf '<Menu><Name>Test</Name><Deleted/><%s/></Menu>\n' "$element" > "$tmp/bad.menu"
    if "$tool" --menu "$tmp/bad.menu" > "$tmp/bad-result" 2> /dev/null; then
        exit 1
    fi
    test ! -s "$tmp/bad-result"
done

# Child moves run first; parent moves may precede their source in XML.
# Chained moves create intermediate menus and merge into existing targets.
cat > "$tmp/moves.menu" <<'EOF'
<Menu><Name>Apps</Name><DefaultAppDirs/>
  <Move><Old>Source/Renamed</Old><New>New/Target</New>
    <Old>Absent</Old><New>Ghost</New>
    <Old>New/Target</Old><New>Dest</New>
    <Old>Dest</Old><New>Dest</New></Move>
  <Menu><Name>Source</Name>
    <Menu><Name>Leaf</Name><Include><Filename>override.desktop</Filename></Include></Menu>
    <Move><Old>Leaf</Old><New>Renamed</New></Move>
  </Menu>
  <Menu><Name>Dest</Name><Include><Filename>nested-tool.desktop</Filename></Include></Menu>
</Menu>
EOF
cat > "$tmp/moves-expected.menu" <<'EOF'
<Menu><Name>Apps</Name><DefaultAppDirs/>
  <Menu><Name>Source</Name></Menu>
  <Menu><Name>Dest</Name>
    <Include><Filename>override.desktop</Filename></Include>
    <Include><Filename>nested-tool.desktop</Filename></Include>
  </Menu>
  <Menu><Name>New</Name></Menu>
</Menu>
EOF
"$tool" --menu "$tmp/moves.menu" --format cow > "$tmp/moved"
"$tool" --menu "$tmp/moves-expected.menu" --format cow > "$tmp/expected"
cmp "$tmp/moved" "$tmp/expected"

for move in \
    '<Move/>' \
    '<Move><Old>A</Old></Move>' \
    '<Move><New>A</New><Old>B</Old></Move>' \
    '<Move><Old>A</Old><New>A/B</New></Move>' \
    '<Move><Old>A</Old><New>/B</New></Move>' \
    '<Move><Old>A</Old><New>B//C</New></Move>'; do
    printf '<Menu><Name>Apps</Name><Menu><Name>A</Name></Menu>%s</Menu>' \
        "$move" > "$tmp/bad-move.menu"
    if "$tool" --menu "$tmp/bad-move.menu" > "$tmp/bad-move-result" 2>/dev/null; then
        exit 1
    fi
    test ! -s "$tmp/bad-move-result"
done
cat > "$tmp/bad.menu" <<'EOF'
<!DOCTYPE Menu [<!ENTITY secret SYSTEM "file:///etc/passwd">]>
<Menu><Name>&secret;</Name></Menu>
EOF
if "$tool" --menu "$tmp/bad.menu" > "$tmp/bad-result" 2> /dev/null; then
    exit 1
fi

mkdir -p "$tmp/allocation"
entry "$tmp/allocation/a.desktop" Allocated
entry "$tmp/allocation/b.desktop" ExcludedButAllocated
entry "$tmp/allocation/c.desktop" Unallocated
cat > "$tmp/allocation.menu" <<'EOF'
<Menu>
  <Name>Apps</Name><AppDir>allocation</AppDir>
  <Menu><Name>Other</Name><OnlyUnallocated/><Include><All/></Include></Menu>
  <Menu>
    <Name>Regular</Name><OnlyUnallocated/><NotOnlyUnallocated/>
    <Include><Filename>a.desktop</Filename><Filename>b.desktop</Filename></Include>
    <Exclude><Filename>b.desktop</Filename></Exclude>
  </Menu>
  <Menu><Name>Deleted</Name><Deleted/><Include><Filename>c.desktop</Filename></Include></Menu>
</Menu>
EOF
"$tool" --menu "$tmp/allocation.menu" > "$tmp/menu-result"
actual=$(sed -n '/"menu":/,$p' "$tmp/menu-result" | tr -d ' \t\r\n')
expected=$(tr -d ' \t\r\n' <<EOF
"menu":{"id":"Apps","name":"Apps","applications":[],"menus":[
  {"id":"Apps/Other","name":"Other",
   "applications":[{"id":"c.desktop","file":"$tmp/allocation/c.desktop"}],"menus":[],
   "entries":[{"type":"application","id":"c.desktop","file":"$tmp/allocation/c.desktop"}]},
  {"id":"Apps/Regular","name":"Regular",
   "applications":[{"id":"a.desktop","file":"$tmp/allocation/a.desktop"}],"menus":[],
   "entries":[{"type":"application","id":"a.desktop","file":"$tmp/allocation/a.desktop"}]}
],"entries":[{"type":"menu","id":"Apps/Other"},{"type":"menu","id":"Apps/Regular"}]}}
EOF
)
test "$actual" = "$expected"

mkdir -p "$tmp/apps-a" "$tmp/apps-b" "$tmp/apps-mask"
entry "$tmp/apps-a/override.desktop" CustomA
entry "$tmp/apps-b/override.desktop" CustomB
printf '[Desktop Entry]\nHidden=true\n' > "$tmp/apps-mask/override.desktop"
cat > "$tmp/scopes.menu" <<'EOF'
<Menu>
  <Name>Apps</Name><DefaultAppDirs/><AppDir>apps-a</AppDir>
  <Include><Filename>override.desktop</Filename></Include>
  <Menu>
    <Name>Override</Name><AppDir>apps-b</AppDir>
    <Include><Filename>override.desktop</Filename></Include>
  </Menu>
  <Menu>
    <Name>Sibling</Name><Include><Filename>override.desktop</Filename></Include>
  </Menu>
  <Menu>
    <Name>Default</Name><AppDir>apps-b</AppDir><DefaultAppDirs/>
    <Include><Filename>override.desktop</Filename></Include>
  </Menu>
  <Menu>
    <Name>Masked</Name><AppDir>apps-mask</AppDir>
    <Include><Filename>override.desktop</Filename></Include>
  </Menu>
</Menu>
EOF
"$tool" --menu "$tmp/scopes.menu" > "$tmp/menu-result"
grep -q '"version":.*2' "$tmp/menu-result"
actual=$(sed -n '/"menu":/,$p' "$tmp/menu-result" | tr -d ' \t\r\n')
expected=$(tr -d ' \t\r\n' <<EOF
"menu": {
  "id": "Apps", "name": "Apps",
  "applications": [{"id":"override.desktop","file":"$tmp/apps-a/override.desktop"}],
  "menus": [
    {"id":"Apps/Override","name":"Override",
     "applications":[{"id":"override.desktop","file":"$tmp/apps-b/override.desktop"}],"menus":[],
     "entries":[{"type":"application","id":"override.desktop","file":"$tmp/apps-b/override.desktop"}]},
    {"id":"Apps/Sibling","name":"Sibling",
     "applications":[{"id":"override.desktop","file":"$tmp/apps-a/override.desktop"}],"menus":[],
     "entries":[{"type":"application","id":"override.desktop","file":"$tmp/apps-a/override.desktop"}]},
    {"id":"Apps/Default","name":"Default",
     "applications":[{"id":"override.desktop","file":"$tmp/user/applications/override.desktop"}],"menus":[],
     "entries":[{"type":"application","id":"override.desktop","file":"$tmp/user/applications/override.desktop"}]},
    {"id":"Apps/Masked","name":"Masked","applications":[],"menus":[],"entries":[]}
  ],
  "entries":[{"type":"menu","id":"Apps/Default"},{"type":"menu","id":"Apps/Override"},
    {"type":"menu","id":"Apps/Sibling"},
    {"type":"application","id":"override.desktop","file":"$tmp/apps-a/override.desktop"}]
}}
EOF
)
test "$actual" = "$expected"
test "$(grep -c '"Name".*"CustomA"' "$tmp/menu-result")" -eq 1
test "$(grep -c '"Name".*"CustomB"' "$tmp/menu-result")" -eq 1
(cd "$tmp" && "$tool" --menu scopes.menu) > "$tmp/relative-result"
sed '/"menu_file":/d' "$tmp/menu-result" > "$tmp/absolute-content"
sed '/"menu_file":/d' "$tmp/relative-result" > "$tmp/relative-content"
cmp "$tmp/absolute-content" "$tmp/relative-content"

mkdir -p "$tmp/user/desktop-directories" "$tmp/system/desktop-directories" "$tmp/custom"
printf '[Desktop Entry]\nType=Directory\nName=System\n' > "$tmp/system/desktop-directories/tools.directory"
printf '[Desktop Entry]\nType=Directory\nName=User\nName[fr]=Outils\nIcon=tools\n' > "$tmp/user/desktop-directories/tools.directory"
printf '[Desktop Entry]\nType=Directory\nName=Custom\n' > "$tmp/custom/tools.directory"
printf '[Desktop Entry]\nType=Directory\nName=Secret\nHidden=true\n' > "$tmp/custom/hidden.directory"
cat > "$tmp/labels.menu" <<'EOF'
<Menu>
  <Name>Applications</Name><DefaultDirectoryDirs/>
  <Menu>
    <Name>StableID</Name>
    <Directory>tools.directory</Directory><Directory>missing.directory</Directory>
  </Menu>
  <Menu>
    <Name>Custom</Name><DirectoryDir>custom</DirectoryDir>
    <Menu><Name>Inherited</Name><Directory>tools.directory</Directory></Menu>
    <Menu><Name>Hidden</Name><Directory>hidden.directory</Directory></Menu>
  </Menu>
  <Menu><Name>Sibling</Name><Directory>tools.directory</Directory></Menu>
</Menu>
EOF
LC_ALL=fr_CA.UTF-8 "$tool" --menu "$tmp/labels.menu" > "$tmp/menu-result"
actual=$(sed -n '/"menu":/,$p' "$tmp/menu-result" | tr -d ' \t\r\n')
expected='"menu":{"id":"Applications","name":"Applications","applications":[],"menus":[{"id":"Applications/StableID","name":"Outils","icon":"tools","comment":"","applications":[],"menus":[],"entries":[]},{"id":"Applications/Custom","name":"Custom","applications":[],"menus":[{"id":"Applications/Custom/Inherited","name":"Custom","icon":"","comment":"","applications":[],"menus":[],"entries":[]}],"entries":[]},{"id":"Applications/Sibling","name":"Outils","icon":"tools","comment":"","applications":[],"menus":[],"entries":[]}],"entries":[]}}'
test "$actual" = "$expected"
test ! -s "$tmp/bad-result"

printf '<Menu><Name>Empty</Name><Include><All/></Include></Menu>\n' > "$tmp/empty.menu"
"$tool" --menu "$tmp/empty.menu" > "$tmp/menu-result"
actual=$(sed -n '/"menu":/,$p' "$tmp/menu-result" | tr -d ' \t\r\n')
test "$actual" = '"menu":{"id":"Empty","name":"Empty","applications":[],"menus":[],"entries":[]}}'
printf '<Menu><Name>Gone</Name><Deleted/></Menu>\n' > "$tmp/empty.menu"
"$tool" --menu "$tmp/empty.menu" > "$tmp/menu-result"
grep -q '"menu":.*null' "$tmp/menu-result"

printf '<Menu><Name>Test</Name><Menu><Name>Same</Name></Menu><Menu><Name>Same</Name></Menu></Menu>\n' > "$tmp/duplicates.menu"
"$tool" --menu "$tmp/duplicates.menu" > "$tmp/menu-result"
test "$(grep -c '"id".*"Test/Same"' "$tmp/menu-result")" -eq 1

mkdir -p "$tmp/merged" "$tmp/merge-dir"
cat > "$tmp/merged/shared.menu" <<'EOF'
<Menu>
  <Name>Ignored</Name><AppDir>../allocation</AppDir>
  <Menu><Name>Shared</Name><Include><Filename>b.desktop</Filename></Include></Menu>
</Menu>
EOF
cat > "$tmp/merge-dir/10-extra.menu" <<'EOF'
<Menu>
  <Name>Ignored</Name>
  <Menu><Name>FromDirectory</Name><Include><Filename>c.desktop</Filename></Include></Menu>
</Menu>
EOF
printf 'ignored text' > "$tmp/merge-dir/not-a-menu.txt"
cat > "$tmp/merge.menu" <<'EOF'
<Menu>
  <Name>Apps</Name><AppDir>allocation</AppDir>
  <Menu><Name>Shared</Name><Include><Filename>a.desktop</Filename></Include></Menu>
  <MergeFile type="path">merged/shared.menu</MergeFile>
  <MergeDir>merge-dir</MergeDir>
  <Menu><Name>Shared</Name><Exclude><Filename>a.desktop</Filename></Exclude></Menu>
  <MergeFile>missing.menu</MergeFile><MergeDir>missing-dir</MergeDir>
</Menu>
EOF
"$tool" --menu "$tmp/merge.menu" > "$tmp/menu-result"
actual=$(sed -n '/"menu":/,$p' "$tmp/menu-result" | tr -d ' \t\r\n')
expected=$(tr -d ' \t\r\n' <<EOF
"menu":{"id":"Apps","name":"Apps","applications":[],"menus":[
  {"id":"Apps/FromDirectory","name":"FromDirectory",
   "applications":[{"id":"c.desktop","file":"$tmp/allocation/c.desktop"}],"menus":[],
   "entries":[{"type":"application","id":"c.desktop","file":"$tmp/allocation/c.desktop"}]},
  {"id":"Apps/Shared","name":"Shared",
   "applications":[{"id":"b.desktop","file":"$tmp/allocation/b.desktop"}],"menus":[],
   "entries":[{"type":"application","id":"b.desktop","file":"$tmp/allocation/b.desktop"}]}
],"entries":[{"type":"menu","id":"Apps/FromDirectory"},{"type":"menu","id":"Apps/Shared"}]}}
EOF
)
test "$actual" = "$expected"

# Repeated merge directives are effective only at their final position.
cat > "$tmp/repeated.menu" <<'EOF'
<Menu>
  <Name>Apps</Name><MergeFile>merged/shared.menu</MergeFile>
  <Menu><Name>Shared</Name><Exclude><All/></Exclude></Menu>
  <MergeFile>merged/shared.menu</MergeFile>
</Menu>
EOF
"$tool" --menu "$tmp/repeated.menu" > "$tmp/menu-result"
grep -q '"file".*/allocation/b.desktop"' "$tmp/menu-result"
test "$(grep -c '"id".*"Apps/Shared"' "$tmp/menu-result")" -eq 2

printf '<Menu><Name>Cycle</Name><MergeFile>cycle.menu</MergeFile></Menu>' > "$tmp/cycle.menu"
ln -s cycle.menu "$tmp/alias.menu"
printf '<Menu><Name>Cycle</Name><MergeFile>alias.menu</MergeFile></Menu>' > "$tmp/cycle.menu"
if "$tool" --menu "$tmp/cycle.menu" > "$tmp/bad-result" 2> "$tmp/error"; then
    exit 1
fi
test ! -s "$tmp/bad-result"
grep -q 'recursive menu include' "$tmp/error"

printf '<Menu><Name>Test</Name><MergeFile type="bogus">unused</MergeFile></Menu>' > "$tmp/bad.menu"
if "$tool" --menu "$tmp/bad.menu" > "$tmp/bad-result" 2> /dev/null; then
    exit 1
fi
printf '<Menu><Name>Test</Name><MergeFile>malformed.menu</MergeFile></Menu>' > "$tmp/bad.menu"
printf '<Menu>' > "$tmp/malformed.menu"
if "$tool" --menu "$tmp/bad.menu" > "$tmp/bad-result" 2> /dev/null; then
    exit 1
fi
test ! -s "$tmp/bad-result"
mkfifo "$tmp/pipe.menu"
if "$tool" --menu "$tmp/pipe.menu" > "$tmp/bad-result" 2> /dev/null; then
    exit 1
fi

# A non-cyclic chain is bounded too.
i=0
while test "$i" -lt 65; do
    next=$((i + 1))
    printf '<Menu><Name>Deep</Name><MergeFile>deep-%s.menu</MergeFile></Menu>' "$next" > "$tmp/deep-$i.menu"
    i=$next
done
printf '<Menu><Name>End</Name></Menu>' > "$tmp/deep-65.menu"
if "$tool" --menu "$tmp/deep-0.menu" > "$tmp/bad-result" 2> "$tmp/error"; then
    exit 1
fi
test ! -s "$tmp/bad-result"
grep -q 'limit exceeded' "$tmp/error"

mkdir "$tmp/amp&dir"
entry "$tmp/amp&dir/amp.desktop" Ampersand
printf '<Menu><Name>Apps</Name><AppDir>amp&amp;dir</AppDir><Include><All/></Include></Menu>' > "$tmp/amp.menu"
"$tool" --menu "$tmp/amp.menu" > "$tmp/menu-result"
grep -q '"file".*/amp&dir/amp.desktop"' "$tmp/menu-result"

mkdir -p "$tmp/config-user/menus/applications-merged" \
    "$tmp/config-system/menus/applications-merged" "$tmp/config-other/menus"
cat > "$tmp/config-other/menus/cow-applications.menu" <<'EOF'
<Menu>
  <Name>System</Name><AppDir>../../allocation</AppDir>
  <Menu><Name>Tools</Name><Include><Filename>a.desktop</Filename></Include></Menu>
</Menu>
EOF
cat > "$tmp/config-system/menus/cow-applications.menu" <<'EOF'
<Menu>
  <Name>Ignored</Name><MergeFile type="parent"/>
  <Menu><Name>Tools</Name><Include><Filename>b.desktop</Filename></Include></Menu>
</Menu>
EOF
cat > "$tmp/config-user/menus/cow-applications.menu" <<'EOF'
<Menu>
  <Name>User</Name><MergeFile type="parent">ignored-filename</MergeFile>
  <DefaultMergeDirs/>
  <Menu><Name>Tools</Name><Exclude><Filename>b.desktop</Filename></Exclude></Menu>
</Menu>
EOF
cat > "$tmp/config-system/menus/applications-merged/10-tools.menu" <<'EOF'
<Menu><Name>Ignored</Name><Menu><Name>Tools</Name>
  <Exclude><Filename>a.desktop</Filename></Exclude>
</Menu></Menu>
EOF
cat > "$tmp/config-user/menus/applications-merged/20-tools.menu" <<'EOF'
<Menu><Name>Ignored</Name><Menu><Name>Tools</Name>
  <Include><Filename>c.desktop</Filename></Include>
</Menu></Menu>
EOF
XDG_MENU_PREFIX=cow- "$tool" --menu "$tmp/config-user/menus/cow-applications.menu" > "$tmp/menu-result"
actual=$(sed -n '/"menu":/,$p' "$tmp/menu-result" | tr -d ' \t\r\n')
expected=$(tr -d ' \t\r\n' <<EOF
"menu":{"id":"User","name":"User","applications":[],"menus":[
  {"id":"User/Tools","name":"Tools",
   "applications":[{"id":"c.desktop","file":"$tmp/allocation/c.desktop"}],"menus":[],
   "entries":[{"type":"application","id":"c.desktop","file":"$tmp/allocation/c.desktop"}]}
],"entries":[{"type":"menu","id":"User/Tools"}]}}
EOF
)
test "$actual" = "$expected"

# Parent lookup outside the configuration path is a no-op, not a literal include.
printf '<Menu><Name>Outside</Name><MergeFile type="parent">malformed.menu</MergeFile></Menu>' > "$tmp/outside.menu"
"$tool" --menu "$tmp/outside.menu" > /dev/null

# Automatic discovery uses the same precedence and merging as explicit input.
XDG_MENU_PREFIX=cow- "$tool" --menu > "$tmp/menu-result"
grep -q '"menu_source":.*"discovered"' "$tmp/menu-result"
grep -q '"menu_file":.*config-user/menus/cow-applications.menu"' "$tmp/menu-result"
actual=$(sed -n '/"menu":/,$p' "$tmp/menu-result" | tr -d ' \t\r\n')
test "$actual" = "$expected"

# A missing prefixed menu uses the fallback, not an unprefixed system menu.
printf '<Menu>' > "$tmp/config-system/menus/applications.menu"
XDG_MENU_PREFIX=absent- "$tool" --menu --fallback "$tmp/outside.menu" > "$tmp/menu-result"
grep -q '"menu_source":.*"fallback"' "$tmp/menu-result"
grep -q '"name":.*"Outside"' "$tmp/menu-result"

# Existing invalid menus must never silently select the fallback.
printf '<Menu>' > "$tmp/config-user/menus/cow-applications.menu"
if XDG_MENU_PREFIX=cow- "$tool" --menu --fallback "$tmp/outside.menu" > "$tmp/bad-result" 2>/dev/null; then
    exit 1
fi
test ! -s "$tmp/bad-result"
if XDG_MENU_PREFIX=../ "$tool" --menu --fallback "$tmp/outside.menu" > "$tmp/bad-result" 2>/dev/null; then
    exit 1
fi
if "$tool" --fallback "$tmp/outside.menu" > "$tmp/bad-result" 2>/dev/null; then
    exit 1
fi

# Explicit positions are reserved before Merge, including later references.
entry "$tmp/allocation/a.desktop" Zulu
entry "$tmp/allocation/b.desktop" Beta
entry "$tmp/allocation/c.desktop" Alpha
cat > "$tmp/layout.menu" <<'EOF'
<Menu>
  <Name>Apps</Name><AppDir>allocation</AppDir><Include><All/></Include>
  <Menu><Name>Empty</Name></Menu>
  <Menu><Name>Hidden</Name><Deleted/></Menu>
  <Layout><Merge type="all"/></Layout>
  <Layout>
    <Separator/><Filename>missing.desktop</Filename>
    <Merge type="all"/><Separator/><Separator/>
    <Filename>a.desktop</Filename><Filename>a.desktop</Filename>
    <Menuname show_empty="true">Empty</Menuname>
    <Menuname>Hidden</Menuname><Separator/>
  </Layout>
</Menu>
EOF
"$tool" --menu "$tmp/layout.menu" > "$tmp/menu-result"
root_entries=$(printf '\t\t"entries":')
actual=$(sed -n "/^$root_entries/,\$p" "$tmp/menu-result" | tr -d ' \t\r\n')
expected=$(tr -d ' \t\r\n' <<EOF
"entries":[
  {"type":"application","id":"c.desktop","file":"$tmp/allocation/c.desktop"},
  {"type":"application","id":"b.desktop","file":"$tmp/allocation/b.desktop"},
  {"type":"separator"},
  {"type":"application","id":"a.desktop","file":"$tmp/allocation/a.desktop"},
  {"type":"menu","id":"Apps/Empty"}
]}}
EOF
)
test "$actual" = "$expected"

# Empty Layout falls back to the inherited DefaultLayout, not XML order.
cat > "$tmp/layout.menu" <<'EOF'
<Menu>
  <Name>Apps</Name><AppDir>allocation</AppDir>
  <DefaultLayout show_empty="true">
    <Filename>c.desktop</Filename><Merge type="all"/>
  </DefaultLayout>
  <Menu><Name>Child</Name><Include><All/></Include><Layout/></Menu>
  <Menu><Name>Empty</Name></Menu>
</Menu>
EOF
"$tool" --menu "$tmp/layout.menu" > "$tmp/menu-result"
actual=$(sed -n '/"menu":/,$p' "$tmp/menu-result" | tr -d ' \t\r\n')
expected=$(tr -d ' \t\r\n' <<EOF
"menu":{"id":"Apps","name":"Apps","applications":[],"menus":[
  {"id":"Apps/Child","name":"Child","applications":[
    {"id":"a.desktop","file":"$tmp/allocation/a.desktop"},
    {"id":"b.desktop","file":"$tmp/allocation/b.desktop"},
    {"id":"c.desktop","file":"$tmp/allocation/c.desktop"}
  ],"menus":[],"entries":[
    {"type":"application","id":"c.desktop","file":"$tmp/allocation/c.desktop"},
    {"type":"application","id":"b.desktop","file":"$tmp/allocation/b.desktop"},
    {"type":"application","id":"a.desktop","file":"$tmp/allocation/a.desktop"}
  ]},
  {"id":"Apps/Empty","name":"Empty","applications":[],"menus":[],"entries":[]}
],"entries":[{"type":"menu","id":"Apps/Child"},{"type":"menu","id":"Apps/Empty"}]}}
EOF
)
test "$actual" = "$expected"

for layout in \
    '<Layout><Merge type="bad"/></Layout>' \
    '<Layout><Merge type="files"/></Layout>' \
    '<Layout><Merge type="all"/><Merge type="menus"/></Layout>' \
    '<Layout><Menuname>A/B</Menuname><Merge type="all"/></Layout>' \
    '<DefaultLayout inline="maybe"/>' \
    '<DefaultLayout inline_limit="-1"/>' \
    '<DefaultLayout inline_limit="999999999999999999999"/>' \
    '<Layout><Separator>bad</Separator><Merge type="all"/></Layout>'; do
    printf '<Menu><Name>Bad</Name><Deleted/>%s</Menu>' "$layout" > "$tmp/bad.menu"
    if "$tool" --menu "$tmp/bad.menu" > "$tmp/bad-result" 2>/dev/null; then
        exit 1
    fi
    test ! -s "$tmp/bad-result"
done

# Inlining inherits defaults but respects per-child limits and aliases.
cat > "$tmp/inline.menu" <<'EOF'
<Menu>
  <Name>Apps</Name><AppDir>allocation</AppDir>
  <DefaultLayout inline="true" inline_limit="0"/>
  <Menu><Name>Alias</Name><Include><Filename>a.desktop</Filename></Include></Menu>
  <Menu><Name>Heading</Name><Include><Filename>b.desktop</Filename></Include></Menu>
  <Menu><Name>Large</Name><Include><All/></Include></Menu>
  <Menu><Name>Outer</Name>
    <Menu><Name>Nested</Name><Include><Filename>c.desktop</Filename></Include></Menu>
    <Layout><Menuname inline="false">Nested</Menuname><Merge type="all"/></Layout>
  </Menu>
  <Layout>
    <Menuname inline_alias="true">Alias</Menuname>
    <Menuname>Heading</Menuname>
    <Menuname inline_limit="1">Large</Menuname>
    <Menuname inline_header="false">Outer</Menuname>
    <Merge type="all"/>
  </Layout>
</Menu>
EOF
"$tool" --menu "$tmp/inline.menu" > "$tmp/menu-result"
actual=$(sed -n "/^$root_entries/,\$p" "$tmp/menu-result" | tr -d ' \t\r\n')
expected=$(tr -d ' \t\r\n' <<EOF
"entries":[
  {"type":"application","id":"a.desktop","file":"$tmp/allocation/a.desktop","name":"Alias"},
  {"type":"header","name":"Heading"},
  {"type":"application","id":"b.desktop","file":"$tmp/allocation/b.desktop"},
  {"type":"menu","id":"Apps/Large"},
  {"type":"menu","id":"Apps/Outer/Nested"}
]}}
EOF
)
test "$actual" = "$expected"
"$tool" --menu "$tmp/inline.menu" --format cow > "$tmp/inline.conf"
grep -q 'Alias.*--launch.*a.desktop' "$tmp/inline.conf"
grep -q 'Heading title' "$tmp/inline.conf"
grep -q 'Nested.*popup .*Apps/Outer/Nested' "$tmp/inline.conf"
