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
#if !defined(COW_ATTRS_H)
#define COW_ATTRS_H

/*
 * Compiler attribute macros, shared between cow and its companion
 * utilities (cowpager, cowbar, ...).  Carved out of compat.h so that
 * tools which don't need cow's full portability shim can pull in just
 * the attribute spellings without dragging in input-event-codes,
 * libbsd fallbacks, etc.
 */

#ifndef __unused
#define __unused __attribute__((__unused__))
#endif

#ifndef printflike1
#define printflike1 __attribute__((format(printf, 1, 2)))
#endif
#ifndef printflike2
#define printflike2 __attribute__((format(printf, 2, 3)))
#endif

#ifndef nitems
#define nitems(n) (sizeof(n) / sizeof(*(n)))
#endif

#endif /* COW_ATTRS_H */
