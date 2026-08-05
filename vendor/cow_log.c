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
#include "cow/log.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static struct {
	FILE		   *stream;
	bool		    close_stream;
	bool		    colour;
	bool		    file_output;
	enum cow_log_level level;
} logger = {
	.stream = NULL,
	.level  = COW_LOG_INFO,
};

static const char *level_names[] = {
	"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL",
};

static const char *level_colours[] = {
	"\033[94m", "\033[36m", "\033[32m",
	"\033[33m", "\033[31m", "\033[35m",
};

static const char *
source_path(const char *file)
{
	while (file[0] == '.' && file[1] == '.' && file[2] == '/')
		file += 3;
	return (file);
}

bool
cow_log_level_parse(const char *value, enum cow_log_level *level)
{
	if (value == NULL || level == NULL)
		return (false);
	for (int i = COW_LOG_TRACE; i <= COW_LOG_FATAL; i++) {
		if (strcasecmp(value, level_names[i]) == 0) {
			*level = i;
			return (true);
		}
	}
	return (false);
}

bool
cow_log_colour_parse(const char *value, enum cow_log_colour *colour)
{
	if (value == NULL || colour == NULL)
		return (false);
	if (strcasecmp(value, "auto") == 0)
		*colour = COW_LOG_COLOUR_AUTO;
	else if (strcasecmp(value, "always") == 0)
		*colour = COW_LOG_COLOUR_ALWAYS;
	else if (strcasecmp(value, "never") == 0)
		*colour = COW_LOG_COLOUR_NEVER;
	else
		return (false);
	return (true);
}

int
cow_log_init(const char *path, enum cow_log_level level,
    enum cow_log_colour colour)
{
	cow_log_fini();

	logger.stream = stderr;
	logger.level  = level;
	if (path != NULL && strcmp(path, "-") != 0) {
		logger.stream = fopen(path, "a");
		if (logger.stream == NULL)
			return (-1);
		(void)fcntl(fileno(logger.stream), F_SETFD, FD_CLOEXEC);
		logger.close_stream = true;
		logger.file_output  = true;
	}

	if (colour == COW_LOG_COLOUR_ALWAYS) {
		logger.colour = true;
	} else if (colour == COW_LOG_COLOUR_NEVER || logger.file_output ||
	    getenv("NO_COLOR") != NULL) {
		logger.colour = false;
	} else {
		logger.colour = isatty(fileno(logger.stream)) != 0;
	}
	return (0);
}

void
cow_log_fini(void)
{
	if (logger.close_stream && logger.stream != NULL)
		fclose(logger.stream);
	logger.stream	      = NULL;
	logger.close_stream = false;
	logger.colour	      = false;
	logger.file_output  = false;
}

void
cow_log_vwrite(enum cow_log_level level, const char *file, int line,
    const char *function, const char *fmt, va_list ap)
{
	if (level < logger.level)
		return;
	FILE *stream = logger.stream != NULL ? logger.stream : stderr;

	struct timeval tv;
	struct tm      tm;
	char	       timestamp[64];
	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &tm);
	if (logger.file_output) {
		strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm);
	} else {
		strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &tm);
	}

	if (logger.colour) {
		fprintf(stream, "%s.%03ld %s%-5s\033[0m \033[90m[%s:%d %s]"
		    "\033[0m ", timestamp, tv.tv_usec / 1000,
		    level_colours[level], level_names[level], source_path(file),
		    line, function);
	} else {
		fprintf(stream, "%s.%03ld %-5s [%s:%d %s] ", timestamp,
		    tv.tv_usec / 1000, level_names[level], source_path(file), line,
		    function);
	}
	vfprintf(stream, fmt, ap);
	fputc('\n', stream);
	fflush(stream);
}

void
cow_log_write(enum cow_log_level level, const char *file, int line,
    const char *function, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	cow_log_vwrite(level, file, line, function, fmt, ap);
	va_end(ap);
}
