/*
 * Copyright (c) 2026 Thomas Adam
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#if !defined(COW_COMPAT_H)
#define COW_COMPAT_H

/*
 * Portability shim.  All platform feature-detection lives in the meson
 * build system, which writes the results into build_config.h (one
 * HAVE_<THING> = 0|1 per probe).  This header then picks the right
 * native or bsd/-prefixed include, and provides inline fallbacks for
 * the handful of libc functions that are still missing.
 */

#include "build_config.h"

#include <stdlib.h>
#include <string.h>

/*
 * sys/queue.h.  Prefer the native header when it ships the full BSD
 * API (TAILQ_FOREACH_SAFE etc.).  Glibc's stub does not, so on Linux
 * with libbsd installed we fall back to <bsd/sys/queue.h>.
 */
#if HAVE_SYS_QUEUE_H && HAVE_TAILQ_FOREACH_SAFE
#include <sys/queue.h>
#elif HAVE_BSD_SYS_QUEUE_H
#include <bsd/sys/queue.h>
#elif HAVE_SYS_QUEUE_H
#include <sys/queue.h>
#error "<sys/queue.h> is missing TAILQ_FOREACH_SAFE; install libbsd"
#else
#error "no sys/queue.h found -- install libbsd (Linux) or a BSD libc"
#endif

/* sys/tree.h -- native path on BSDs, bsd/ fallback via libbsd. */
#if HAVE_SYS_TREE_H
#include <sys/tree.h>
#elif HAVE_BSD_SYS_TREE_H
#include <bsd/sys/tree.h>
#else
#error "no sys/tree.h found -- install libbsd (Linux) or a BSD libc"
#endif

/* libutil.h -- for fparseln(). */
#if HAVE_LIBUTIL_H
#include <libutil.h>
#elif HAVE_BSD_LIBUTIL_H
#include <bsd/libutil.h>
#else
#error "no libutil.h found -- install libbsd (Linux) or libutil (BSD)"
#endif

/*
 * bsd/ overlays for string.h / stdlib.h.  On a BSD host these are
 * already declared by the native <string.h>/<stdlib.h> above; on
 * Linux we pick them up from the libbsd headers here.  We include
 * both unconditionally when libbsd is available because its headers
 * guard against collision with a native declaration.
 */
#if HAVE_BSD_STRING_H
#include <bsd/string.h>
#endif
#if HAVE_BSD_STDLIB_H
#include <bsd/stdlib.h>
#endif

/* Input event codes (BTN_*, KEY_*). */
#if HAVE_LINUX_INPUT_EVENT_CODES_H
#include <linux/input-event-codes.h>
#elif HAVE_DEV_EVDEV_INPUT_EVENT_CODES_H
#include <dev/evdev/input-event-codes.h>
#else
#error "no input-event-codes.h found -- install evdev-proto (FreeBSD)"
#endif

/*
 * Attribute macros live in their own header so cow's companion utilities
 * can pull them in without dragging in the rest of this portability
 * shim.
 */
#include "cow/attrs.h"

#endif
