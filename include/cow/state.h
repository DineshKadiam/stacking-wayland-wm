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
#if !defined(COW_STATE_H)
#define COW_STATE_H

#include <stdbool.h>

/*
 * Save the current window manager state to a file.
 * State includes: per-view position, sheet assignment, flags, stacking,
 * maximize/shade state, titlebar position/squeeze, keyboard layout, focus, and
 * per-output current sheet.
 *
 * Returns true on success.
 */
bool cow_state_save(void);

/*
 * Attempt to restore window state from the saved state file.
 * Matches views by their river identifier string.
 * Called after all initial window/output events have been received.
 *
 * Returns true if state was restored (even partially).
 */
bool cow_state_restore(void);

/*
 * Return the default state file path.
 */
const char *cow_state_path(void);

#endif
