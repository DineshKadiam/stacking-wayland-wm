/*
 * Copyright (c) 2020 rxi
 * Copyright (c) 2026 Thomas Adam
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Derived from rxi/log.c and adapted for CoW: namespaced symbols, automatic
 * terminal colour, millisecond timestamps, function names, and one selectable
 * output destination.
 */
#ifndef COW_LOG_H
#define COW_LOG_H

#include <stdarg.h>
#include <stdbool.h>

enum cow_log_level {
	COW_LOG_TRACE,
	COW_LOG_DEBUG,
	COW_LOG_INFO,
	COW_LOG_WARN,
	COW_LOG_ERROR,
	COW_LOG_FATAL,
};

enum cow_log_colour {
	COW_LOG_COLOUR_AUTO,
	COW_LOG_COLOUR_ALWAYS,
	COW_LOG_COLOUR_NEVER,
};

bool cow_log_level_parse(const char *, enum cow_log_level *);
bool cow_log_colour_parse(const char *, enum cow_log_colour *);
int  cow_log_init(const char *, enum cow_log_level, enum cow_log_colour);
void cow_log_fini(void);
void cow_log_write(enum cow_log_level, const char *, int, const char *,
    const char *, ...) __attribute__((format(printf, 5, 6)));
void cow_log_vwrite(enum cow_log_level, const char *, int, const char *,
    const char *, va_list) __attribute__((format(printf, 5, 0)));

#define cow_log_trace(...) \
	cow_log_write(COW_LOG_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define cow_log_debug(...) \
	cow_log_write(COW_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define cow_log_info(...) \
	cow_log_write(COW_LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define cow_log_warn(...) \
	cow_log_write(COW_LOG_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define cow_log_error(...) \
	cow_log_write(COW_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define cow_log_fatal(...) \
	cow_log_write(COW_LOG_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

#endif
