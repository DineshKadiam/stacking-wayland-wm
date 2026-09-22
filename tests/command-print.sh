#!/bin/sh
# Copyright (c) 2026 Thomas Adam
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

set -eu

test_dir=$(mktemp -d)
trap 'rm -f "$test_dir/config" "$test_dir/log"; rmdir "$test_dir"' EXIT HUP INT TERM

# Exercise truncation around the 512-byte command-log buffer. Commands are
# formatted even when trace logging is disabled. Bindings are only stored;
# no external programs are launched by this configuration.
awk 'BEGIN {
    text = ""
    for (n = 1; n <= 650; n++) {
        text = text "a"
        if (n >= 480) {
            print "bind L+a exec " text
            print "bind -x " text " L+b nop"
        }
    }
    print "alias " text " nop"
    print text
}' > "$test_dir/config"

# Stop after configuration loading, before any connection to River.
result=0
XDG_RUNTIME_DIR="$test_dir" WAYLAND_DISPLAY=missing \
    "$1" -c "$test_dir/config" > "$test_dir/log" 2>&1 || result=$?
if [ "$result" -ne 1 ] ||
    ! grep -q 'config loaded from' "$test_dir/log" ||
    ! grep -q 'failed to connect to Wayland display' "$test_dir/log"; then
    cat "$test_dir/log" >&2
    exit 1
fi
