#!/bin/sh
#
# cow-version-str: emits the version of cow which is building.
#		    If this is a release build, then the tag name is chomped
#		    to remove extraneous git information.
#
#		    If it's a developer build, it's left as-is.
#
#
#
# Intended to be called from meson.
set -e

COW_RELEASE=no
VERSION=0.4

[ -d ".git" -a "$COW_RELEASE" = "no" ] || { echo "$VERSION" && exit 0 ; }

git describe --always --long --dirty --tags || echo "$VERSION"

