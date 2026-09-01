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
#include <cJSON.h>
#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "cow/compat.h"
#include "cow/module.h"
#include "cow/util.h"

#include "proc.h"

#ifndef COW_VERSION
#define COW_VERSION "unknown"
#endif

#define PROG "cowdiag"
#define HISTORY_LEN 60
#define GRAPH_METRIC_COUNT 4
#define MIN_WIDTH 62
#define MIN_HEIGHT 22

enum colour_pair {
	COLOUR_BORDER = 1,
	COLOUR_TITLE,
	COLOUR_LIVE,
	COLOUR_WARNING,
	COLOUR_ERROR,
	COLOUR_GRAPH_COW,
	COLOUR_GRAPH_RIVER,
	COLOUR_GRAPH_RSS,
	COLOUR_GRAPH_MANAGE,
	COLOUR_GRAPH_RENDER,
	COLOUR_SCALE,
};

struct history {
	double values[HISTORY_LEN];
	int next;
	int count;
};

struct process_sample {
	long pid;
	uint64_t cpu_us;
	uint64_t rss_bytes;
	uint64_t peak_bytes;
	uint64_t previous_cpu_us;
	uint64_t previous_at_us;
	double cpu_percent;
	bool cpu_valid;
	bool rss_valid;
};

struct rates {
	uint64_t manage_cycles;
	uint64_t render_cycles;
	uint64_t at_us;
	double manage_per_second;
	double render_per_second;
};

struct app {
	int status_fd;
	char status_buf[COW_MODULE_STATUS_LINE_MAX];
	size_t status_len;
	cJSON *status;
	cJSON *diag;
	struct process_sample cow;
	struct process_sample river;
	struct rates rates;
	struct history cow_cpu;
	struct history river_cpu;
	struct history cow_rss;
	struct history manage_wait;
	struct history render_wait;
	uint64_t last_connect_attempt_us;
	uint64_t cow_uptime_ms;
	uint64_t manage_wait_samples;
	uint64_t render_wait_samples;
	unsigned interval_ms;
	int graph_metric;
	bool connected;
	bool paused;
	bool help;
	bool graph_only;
};

static volatile sig_atomic_t running = 1;
static bool colours_enabled;

static void
colour_on(WINDOW *window, int pair)
{
	if (colours_enabled)
		wattron(window, COLOR_PAIR(pair));
}

static void
colour_off(WINDOW *window, int pair)
{
	if (colours_enabled)
		wattroff(window, COLOR_PAIR(pair));
}

static void
handle_signal(__unused int sig)
{
	running = 0;
}

static uint64_t
mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL +
	    (uint64_t)ts.tv_nsec / 1000ULL;
}

static cJSON *
object_item(const cJSON *obj, const char *name)
{
	return obj == NULL ? NULL :
	    cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);
}

static double
json_number(const cJSON *obj, const char *name, double fallback)
{
	cJSON *item = object_item(obj, name);
	return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static void
history_add(struct history *history, double value)
{
	history->values[history->next] = value;
	history->next = (history->next + 1) % HISTORY_LEN;
	if (history->count < HISTORY_LEN)
		history->count++;
}

static void
history_clear(struct app *app)
{
	app->cow_cpu = (struct history){ 0 };
	app->river_cpu = (struct history){ 0 };
	app->cow_rss = (struct history){ 0 };
	app->manage_wait = (struct history){ 0 };
	app->render_wait = (struct history){ 0 };
}

static void
process_update(struct process_sample *proc, long pid, uint64_t cpu_us,
    uint64_t rss, uint64_t peak, bool rss_valid, uint64_t now)
{
	if (proc->pid != pid) {
		*proc = (struct process_sample){ .pid = pid };
	}
	if (proc->previous_at_us != 0 && now > proc->previous_at_us &&
	    cpu_us >= proc->previous_cpu_us) {
		proc->cpu_percent =
		    (double)(cpu_us - proc->previous_cpu_us) * 100.0 /
		    (double)(now - proc->previous_at_us);
		proc->cpu_valid = true;
	}
	proc->cpu_us = cpu_us;
	proc->rss_bytes = rss;
	proc->peak_bytes = peak;
	proc->rss_valid = rss_valid;
	proc->previous_cpu_us = cpu_us;
	proc->previous_at_us = now;
}

static bool
status_connect(struct app *app)
{
	int fd = cow_module_status_connect_quiet(PROG);
	if (fd < 0)
		return false;
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(fd);
		return false;
	}
	app->status_fd = fd;
	app->status_len = 0;
	return true;
}

static void
status_disconnect(struct app *app)
{
	if (app->status_fd >= 0)
		close(app->status_fd);
	app->status_fd = -1;
	app->status_len = 0;
}

static void
status_parse_lines(struct app *app)
{
	for (;;) {
		char *newline = memchr(app->status_buf, '\n', app->status_len);
		if (newline == NULL)
			break;
		size_t len = (size_t)(newline - app->status_buf);
		app->status_buf[len] = '\0';
		cJSON *root = cJSON_Parse(app->status_buf);
		if (root != NULL) {
			cJSON_Delete(app->status);
			app->status = root;
		}
		size_t consumed = len + 1;
		memmove(app->status_buf, app->status_buf + consumed,
		    app->status_len - consumed);
		app->status_len -= consumed;
	}
}

static void
status_pump(struct app *app)
{
	if (app->status_fd < 0)
		return;
	for (;;) {
		if (app->status_len == sizeof(app->status_buf)) {
			status_disconnect(app);
			return;
		}
		ssize_t n = read(app->status_fd,
		    app->status_buf + app->status_len,
		    sizeof(app->status_buf) - app->status_len);
		if (n > 0) {
			app->status_len += (size_t)n;
			status_parse_lines(app);
			continue;
		}
		if (n == 0) {
			status_disconnect(app);
			return;
		}
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			status_disconnect(app);
		return;
	}
}

static cJSON *
request_diagnostics(void)
{
	int fd = cow_module_cmd_connect_quiet(PROG);
	if (fd < 0)
		return NULL;
	if (!cow_module_cmd_send(fd, "show diagnostics", true)) {
		close(fd);
		return NULL;
	}
	char *response = cow_module_read_response(fd,
	    COW_MODULE_RESPONSE_INITIAL, true);
	close(fd);
	if (response == NULL)
		return NULL;
	cJSON *root = cJSON_Parse(response);
	free(response);
	if (root == NULL)
		return NULL;
	cJSON *data = object_item(root, "data");
	cJSON *copy = cJSON_IsObject(data) ? cJSON_Duplicate(data, true) : NULL;
	cJSON_Delete(root);
	return copy;
}

static void
rates_update(struct app *app, uint64_t now)
{
	cJSON *manage = object_item(app->diag, "manage");
	cJSON *render = object_item(app->diag, "render");
	uint64_t manage_cycles = (uint64_t)json_number(manage, "cycles", 0);
	uint64_t render_cycles = (uint64_t)json_number(render, "cycles", 0);
	if (app->rates.at_us != 0 && now > app->rates.at_us) {
		double seconds = (double)(now - app->rates.at_us) / 1000000.0;
		if (manage_cycles >= app->rates.manage_cycles)
			app->rates.manage_per_second =
			    (manage_cycles - app->rates.manage_cycles) / seconds;
		if (render_cycles >= app->rates.render_cycles)
			app->rates.render_per_second =
			    (render_cycles - app->rates.render_cycles) / seconds;
	}
	app->rates.manage_cycles = manage_cycles;
	app->rates.render_cycles = render_cycles;
	app->rates.at_us = now;
}

static bool
sample(struct app *app)
{
	uint64_t now = mono_us();
	cJSON *diag = request_diagnostics();
	if (diag == NULL) {
		app->connected = false;
		return false;
	}
	app->connected = true;
	cJSON_Delete(app->diag);
	app->diag = diag;

	cJSON *process = object_item(diag, "process");
	long cow_pid = (long)json_number(process, "pid", -1);
	uint64_t uptime = (uint64_t)json_number(process, "uptime_ms", 0);
	uint64_t cow_cpu = (uint64_t)json_number(process, "user_cpu_us", 0) +
	    (uint64_t)json_number(process, "system_cpu_us", 0);
	uint64_t cow_peak =
	    (uint64_t)json_number(process, "peak_rss_bytes", 0);
	struct cowdiag_proc cow_proc;
	bool cow_rss_valid = cowdiag_proc_read(cow_pid, &cow_proc);
	uint64_t cow_rss = cow_rss_valid ? cow_proc.rss_bytes : 0;
	long old_cow_pid = app->cow.pid;
	bool new_session = old_cow_pid > 0 &&
	    (old_cow_pid != cow_pid || uptime < app->cow_uptime_ms);
	process_update(&app->cow, cow_pid, cow_cpu, cow_rss, cow_peak,
	    cow_rss_valid, now);
	app->cow_uptime_ms = uptime;
	if (new_session) {
		history_clear(app);
		app->rates = (struct rates){ 0 };
		app->manage_wait_samples = 0;
		app->render_wait_samples = 0;
	}

	if (app->river.pid <= 0)
		app->river.pid = cowdiag_river_pid();
	struct cowdiag_proc river_proc;
	if (app->river.pid > 0 && cowdiag_proc_read(app->river.pid,
		&river_proc)) {
		process_update(&app->river, app->river.pid, river_proc.cpu_us,
		    river_proc.rss_bytes, river_proc.peak_bytes, true, now);
	} else {
		app->river = (struct process_sample){
			.pid = cowdiag_river_pid(),
		};
	}

	rates_update(app, now);
	if (app->cow.cpu_valid)
		history_add(&app->cow_cpu, app->cow.cpu_percent);
	if (app->river.cpu_valid)
		history_add(&app->river_cpu, app->river.cpu_percent);
	if (app->cow.rss_valid)
		history_add(&app->cow_rss,
		    (double)app->cow.rss_bytes / (1024.0 * 1024.0));
	cJSON *manage = object_item(diag, "manage");
	cJSON *render = object_item(diag, "render");
	cJSON *wait = object_item(manage, "request_wait");
	cJSON *after = object_item(render, "after_manage");
	uint64_t samples = (uint64_t)json_number(wait, "samples", 0);
	double value = json_number(wait, "last_us", -1);
	if (samples > app->manage_wait_samples && value >= 0)
		history_add(&app->manage_wait, value);
	app->manage_wait_samples = samples;
	samples = (uint64_t)json_number(after, "samples", 0);
	value = json_number(after, "last_us", -1);
	if (samples > app->render_wait_samples && value >= 0)
		history_add(&app->render_wait, value);
	app->render_wait_samples = samples;
	return true;
}

static void
format_bytes(char *buf, size_t size, uint64_t bytes, bool valid)
{
	if (!valid) {
		snprintf(buf, size, "n/a");
	} else if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
		snprintf(buf, size, "%.2f GiB",
		    (double)bytes / (1024.0 * 1024.0 * 1024.0));
	} else {
		snprintf(buf, size, "%.1f MiB",
		    (double)bytes / (1024.0 * 1024.0));
	}
}

static void
format_time_us(char *buf, size_t size, double us)
{
	if (us < 0)
		snprintf(buf, size, "n/a");
	else if (us >= 1000000.0)
		snprintf(buf, size, "%.2f s", us / 1000000.0);
	else if (us >= 1000.0)
		snprintf(buf, size, "%.2f ms", us / 1000.0);
	else
		snprintf(buf, size, "%.0f us", us);
}

static WINDOW *
panel(int y, int x, int height, int width, const char *title)
{
	WINDOW *window = newwin(height, width, y, x);
	colour_on(window, COLOUR_BORDER);
	box(window, 0, 0);
	colour_off(window, COLOUR_BORDER);
	colour_on(window, COLOUR_TITLE);
	wattron(window, A_BOLD);
	mvwprintw(window, 0, 2, " %s ", title);
	wattroff(window, A_BOLD);
	colour_off(window, COLOUR_TITLE);
	return window;
}

static void
draw_processes(WINDOW *window, const struct app *app)
{
	char rss[32], peak[32];

	mvwprintw(window, 1, 2, "%-7s %7s %9s %11s %11s",
	    "process", "pid", "CPU", "RSS", "peak RSS");
	format_bytes(rss, sizeof(rss), app->cow.rss_bytes,
	    app->cow.rss_valid);
	format_bytes(peak, sizeof(peak), app->cow.peak_bytes,
	    app->cow.peak_bytes > 0);
	char cpu[16];
	if (app->cow.cpu_valid)
		snprintf(cpu, sizeof(cpu), "%.2f%%", app->cow.cpu_percent);
	else
		snprintf(cpu, sizeof(cpu), "n/a");
	mvwprintw(window, 2, 2, "%-7s %7ld %9s %11s %11s", "CoW",
	    app->cow.pid, cpu, rss, peak);
	format_bytes(rss, sizeof(rss), app->river.rss_bytes,
	    app->river.rss_valid);
	format_bytes(peak, sizeof(peak), app->river.peak_bytes,
	    app->river.peak_bytes > 0);
	if (app->river.cpu_valid)
		snprintf(cpu, sizeof(cpu), "%.2f%%", app->river.cpu_percent);
	else
		snprintf(cpu, sizeof(cpu), "n/a");
	mvwprintw(window, 3, 2, "%-7s %7ld %9s %11s %11s", "River",
	    app->river.pid, cpu, rss, peak);
}

static void
draw_session(WINDOW *window, const struct app *app)
{
	cJSON *process = object_item(app->diag, "process");
	long uptime = (long)json_number(process, "uptime_ms", 0) / 1000;
	int windows = (int)json_number(app->status, "total_windows", 0);
	cJSON *outputs = object_item(app->status, "outputs");
	int output_count = cJSON_IsArray(outputs) ? cJSON_GetArraySize(outputs) : 0;
	int desks = 0;
	if (cJSON_IsArray(outputs)) {
		cJSON *output;
		cJSON_ArrayForEach(output, outputs) {
			cJSON *list = object_item(output, "desks");
			if (cJSON_IsArray(list))
				desks += cJSON_GetArraySize(list);
		}
	}
	cJSON *focused = object_item(app->status, "focused");
	cJSON *title = object_item(focused, "title");
	mvwprintw(window, 1, 2, "uptime %ldd %02ld:%02ld:%02ld", uptime / 86400,
	    uptime / 3600 % 24, uptime / 60 % 60, uptime % 60);
	mvwprintw(window, 2, 2, "windows %d   outputs %d   desks %d",
	    windows, output_count, desks);
	mvwprintw(window, 3, 2, "focus: ");
	const char *focus_title = cJSON_IsString(title) ?
	    title->valuestring : "none";
	mvwaddnstr(window, 3, 9, focus_title, getmaxx(window) - 11);
}

static void
draw_timing(WINDOW *window, const cJSON *root, const char *series_name,
	const char *handoff_name, double rate)
{
	cJSON *duration = object_item(root, series_name);
	cJSON *handoff = object_item(root, handoff_name);
	char last[24], average[24], maximum[24], handoff_last[24];
	format_time_us(last, sizeof(last),
	    json_number(duration, "last_us", -1));
	format_time_us(average, sizeof(average),
	    json_number(duration, "average_us", -1));
	format_time_us(maximum, sizeof(maximum),
	    json_number(duration, "maximum_us", -1));
	format_time_us(handoff_last, sizeof(handoff_last),
	    json_number(handoff, "last_us", -1));
	mvwprintw(window, 1, 2, "rate %.2f/s   cycles %.0f", rate,
	    json_number(root, "cycles", 0));
	mvwprintw(window, 2, 2, "work  last %-10s avg %-10s max %s",
	    last, average, maximum);
	mvwprintw(window, 3, 2, "handoff last %s", handoff_last);
}

static const struct history *
selected_history(const struct app *app)
{
	switch (app->graph_metric) {
	case 0: return &app->cow_cpu;
	case 1: return &app->cow_rss;
	case 2: return &app->manage_wait;
	default: return &app->render_wait;
	}
}

static const struct history *
secondary_history(const struct app *app)
{
	return app->graph_metric == 0 ? &app->river_cpu : NULL;
}

static const char *
graph_name(int metric)
{
	static const char *names[] = {
		"CPU", "CoW RSS", "Manage wait", "Render wait",
	};
	return names[metric];
}

static void
format_graph_value(char *buf, size_t size, int metric, double value)
{
	if (metric == 0)
		snprintf(buf, size, "%.2f%%", value);
	else if (metric == 1)
		snprintf(buf, size, "%.2f MiB", value);
	else
		format_time_us(buf, size, value);
}

static int
graph_colour(int metric)
{
	static const int colours[] = {
		COLOUR_GRAPH_COW,
		COLOUR_GRAPH_RSS,
		COLOUR_GRAPH_MANAGE,
		COLOUR_GRAPH_RENDER,
	};
	return colours[metric];
}

static bool
history_range(const struct history *history, double *minimum,
    double *maximum)
{
	if (history == NULL || history->count == 0)
		return false;
	int start = (history->next - history->count + HISTORY_LEN) %
	    HISTORY_LEN;
	for (int i = 0; i < history->count; i++) {
		double value = history->values[(start + i) % HISTORY_LEN];
		if (i == 0 || value < *minimum)
			*minimum = value;
		if (i == 0 || value > *maximum)
			*maximum = value;
	}
	return true;
}

static double
history_latest(const struct history *history)
{
	return history->values[
	    (history->next - 1 + HISTORY_LEN) % HISTORY_LEN];
}

static double
minimum_graph_span(int metric, double value)
{
	if (metric == 0)
		return 0.1;
	if (metric == 1)
		return 0.1;
	if (value < 1000.0)
		return 2.0;
	if (value < 1000000.0)
		return 20.0;
	return 20000.0;
}

static void
graph_scale(int metric, double observed_minimum, double observed_maximum,
    double *scale_minimum, double *scale_maximum)
{
	double midpoint = (observed_minimum + observed_maximum) / 2.0;
	double span = observed_maximum - observed_minimum;
	double minimum_span = minimum_graph_span(metric, midpoint);
	if (span < minimum_span) {
		observed_minimum = midpoint - minimum_span / 2.0;
		observed_maximum = midpoint + minimum_span / 2.0;
		span = minimum_span;
	}
	double padding = span * 0.1;
	*scale_minimum = observed_minimum - padding;
	if (*scale_minimum < 0)
		*scale_minimum = 0;
	*scale_maximum = observed_maximum + padding;
}

static unsigned char
braille_bit(int x, int y)
{
	static const unsigned char dots[2][4] = {
		{ 1U << 0, 1U << 1, 1U << 2, 1U << 6 },
		{ 1U << 3, 1U << 4, 1U << 5, 1U << 7 },
	};
	return dots[x & 1][y & 3];
}

static void
braille_set(unsigned char *cells, int cell_width, int x, int y)
{
	if (x < 0 || y < 0)
		return;
	cells[(y / 4) * cell_width + x / 2] |= braille_bit(x, y);
}

static void
braille_line(unsigned char *cells, int cell_width, int x0, int y0,
    int x1, int y1)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int error = dx + dy;
	for (;;) {
		braille_set(cells, cell_width, x0, y0);
		if (x0 == x1 && y0 == y1)
			break;
		int twice = 2 * error;
		if (twice >= dy) {
			error += dy;
			x0 += sx;
		}
		if (twice <= dx) {
			error += dx;
			y0 += sy;
		}
	}
}

static void
utf8_braille(char out[4], unsigned char dots)
{
	unsigned value = 0x2800U + dots;
	out[0] = (char)(0xe0U | (value >> 12));
	out[1] = (char)(0x80U | ((value >> 6) & 0x3fU));
	out[2] = (char)(0x80U | (value & 0x3fU));
	out[3] = '\0';
}

static void
rasterize_history(unsigned char *cells, int cell_width, int cell_height,
    const struct history *history, double scale_minimum, double scale_maximum)
{
	int pixel_width = cell_width * 2;
	int pixel_height = cell_height * 4;
	int start = (history->next - history->count + HISTORY_LEN) %
	    HISTORY_LEN;
	int previous_x = 0, previous_y = 0;
	for (int i = 0; i < history->count; i++) {
		double value = history->values[(start + i) % HISTORY_LEN];
		int x = history->count == 1 ? pixel_width - 1 :
		    i * (pixel_width - 1) / (history->count - 1);
		int y = (int)((scale_maximum - value) * (pixel_height - 1) /
		    (scale_maximum - scale_minimum));
		if (y < 0)
			y = 0;
		else if (y >= pixel_height)
			y = pixel_height - 1;
		if (i == 0)
			braille_set(cells, cell_width, x, y);
		else
			braille_line(cells, cell_width, previous_x, previous_y,
			    x, y);
		previous_x = x;
		previous_y = y;
	}
}

static void
render_cells(WINDOW *window, const unsigned char *cells, int cell_width,
    int cell_height, int plot_x, int colour, bool clear)
{
	colour_on(window, colour);
	for (int y = 0; y < cell_height; y++) {
		for (int x = 0; x < cell_width; x++) {
			unsigned char dots = cells[y * cell_width + x];
			if (dots == 0) {
				if (clear)
					mvwaddch(window, y + 2, plot_x + x, ' ');
				continue;
			}
			char glyph[4];
			utf8_braille(glyph, dots);
			mvwaddstr(window, y + 2, plot_x + x, glyph);
		}
	}
	colour_off(window, colour);
}

static void
draw_history_graph(WINDOW *window, const struct app *app)
{
	const struct history *history = selected_history(app);
	const struct history *secondary = secondary_history(app);
	int height = getmaxy(window), width = getmaxx(window);
	char current[24] = "n/a", secondary_current[24] = "n/a";
	double minimum = 0, maximum = 0;
	bool have_samples = history_range(history, &minimum, &maximum);
	if (have_samples)
		format_graph_value(current, sizeof(current), app->graph_metric,
		    history_latest(history));
	if (secondary != NULL && secondary->count > 0) {
		double secondary_minimum, secondary_maximum;
		(void)history_range(secondary, &secondary_minimum,
		    &secondary_maximum);
		if (!have_samples || secondary_minimum < minimum)
			minimum = secondary_minimum;
		if (!have_samples || secondary_maximum > maximum)
			maximum = secondary_maximum;
		have_samples = true;
		format_graph_value(secondary_current, sizeof(secondary_current),
		    app->graph_metric, history_latest(secondary));
	}
	colour_on(window, graph_colour(app->graph_metric));
	wattron(window, A_BOLD);
	mvwprintw(window, 1, 2, "%s", graph_name(app->graph_metric));
	wattroff(window, A_BOLD);
	colour_off(window, graph_colour(app->graph_metric));
	if (secondary != NULL) {
		wprintw(window, "  ");
		colour_on(window, COLOUR_GRAPH_COW);
		wattron(window, A_BOLD);
		wprintw(window, "CoW %s", current);
		wattroff(window, A_BOLD);
		colour_off(window, COLOUR_GRAPH_COW);
		wprintw(window, "  ");
		colour_on(window, COLOUR_GRAPH_RIVER);
		wattron(window, A_BOLD);
		wprintw(window, "River %s", secondary_current);
		wattroff(window, A_BOLD);
		colour_off(window, COLOUR_GRAPH_RIVER);
		wprintw(window, "  n=%d/%d", history->count, secondary->count);
	} else {
		char observed_minimum[24] = "n/a", observed_maximum[24] = "n/a";
		if (have_samples) {
			format_graph_value(observed_minimum,
			    sizeof(observed_minimum), app->graph_metric, minimum);
			format_graph_value(observed_maximum,
			    sizeof(observed_maximum), app->graph_metric, maximum);
		}
		wprintw(window, "  cur ");
		colour_on(window, graph_colour(app->graph_metric));
		wattron(window, A_BOLD);
		wprintw(window, "%s", current);
		wattroff(window, A_BOLD);
		colour_off(window, graph_colour(app->graph_metric));
		wprintw(window, "  min %s  max %s  n=%d", observed_minimum,
		    observed_maximum, history->count);
	}

	if (!have_samples)
		return;

	double scale_minimum, scale_maximum;
	graph_scale(app->graph_metric, minimum, maximum, &scale_minimum,
	    &scale_maximum);

	char minimum_text[24], middle_text[24], maximum_text[24];
	format_graph_value(minimum_text, sizeof(minimum_text),
	    app->graph_metric, scale_minimum);
	format_graph_value(middle_text, sizeof(middle_text),
	    app->graph_metric, (scale_minimum + scale_maximum) / 2.0);
	format_graph_value(maximum_text, sizeof(maximum_text),
	    app->graph_metric, scale_maximum);

	int label_width = (int)strlen(minimum_text);
	if ((int)strlen(middle_text) > label_width)
		label_width = (int)strlen(middle_text);
	if ((int)strlen(maximum_text) > label_width)
		label_width = (int)strlen(maximum_text);
	int plot_x = label_width + 4;
	int cell_width = width - plot_x - 1;
	int cell_height = height - 4;

	if (cell_width <= 0 || cell_height <= 0)
		return;
	colour_on(window, COLOUR_SCALE);
	wattron(window, A_DIM);

	for (int y = 0; y < cell_height; y++)
		mvwaddch(window, y + 2, plot_x - 1, ACS_VLINE);
	mvwprintw(window, 2, 1, "%*s ", label_width, maximum_text);
	mvwaddch(window, 2, plot_x - 1, ACS_LTEE);

	int middle_y = 2 + (cell_height - 1) / 2;
	mvwprintw(window, middle_y, 1, "%*s ", label_width, middle_text);
	mvwaddch(window, middle_y, plot_x - 1, ACS_LTEE);
	mvwprintw(window, height - 3, 1, "%*s ", label_width, minimum_text);
	mvwaddch(window, height - 3, plot_x - 1, ACS_LTEE);
	wattroff(window, A_DIM);
	colour_off(window, COLOUR_SCALE);

	unsigned char *cells = calloc((size_t)cell_width * cell_height, 1);
	if (cells == NULL)
		return;
	if (history->count > 0) {
		rasterize_history(cells, cell_width, cell_height, history,
		    scale_minimum, scale_maximum);
	}
	render_cells(window, cells, cell_width, cell_height, plot_x,
	    graph_colour(app->graph_metric), true);

	if (secondary != NULL && secondary->count > 0) {
		memset(cells, 0, (size_t)cell_width * cell_height);
		rasterize_history(cells, cell_width, cell_height, secondary,
		    scale_minimum, scale_maximum);
		render_cells(window, cells, cell_width, cell_height, plot_x,
		    COLOUR_GRAPH_RIVER, false);
	}
	free(cells);
	colour_on(window, COLOUR_SCALE);
	wattron(window, A_DIM);
	mvwprintw(window, height - 2, plot_x, "oldest");
	mvwprintw(window, height - 2, width - 8, "newest");
	wattroff(window, A_DIM);
	colour_off(window, COLOUR_SCALE);
}

static void
draw_dashboard(struct app *app)
{
	erase();
	int rows, cols;
	getmaxyx(stdscr, rows, cols);

	if (rows < MIN_HEIGHT || cols < MIN_WIDTH) {
		mvprintw(0, 0, "cowdiag: terminal must be at least %dx%d (%dx%d)",
		    MIN_WIDTH, MIN_HEIGHT, cols, rows);
		refresh();
		return;
	}
	colour_on(stdscr, COLOUR_TITLE);
	attron(A_BOLD);
	mvprintw(0, 1, "cowdiag %s", COW_VERSION);
	attroff(A_BOLD);
	colour_off(stdscr, COLOUR_TITLE);

	const char *state = app->paused ? "PAUSED" :
	    app->connected ? "LIVE" : "OFFLINE";
	int state_colour = app->paused ? COLOUR_WARNING :
	    app->connected ? COLOUR_LIVE : COLOUR_ERROR;
	move(0, cols - 25);
	colour_on(stdscr, state_colour);
	attron(A_BOLD);
	printw("%s", state);
	attroff(A_BOLD);
	colour_off(stdscr, state_colour);
	printw("  refresh %ums", app->interval_ms);
	attron(A_DIM);

	if (app->help)
		mvprintw(rows - 1, 1,
		    "q quit  Space pause  r reset  Tab/Arrows graph  g expand  ? help");
	else
		mvprintw(rows - 1, 1,
		    "q quit   Space pause   Tab graph   g expand   ? help");
	attroff(A_DIM);

	/*
	 * Commit stdscr before the panel windows. wgetch() refreshes stdscr;
	 * leaving its blank body dirty would erase the panels on the next input
	 * poll and leave only the header/footer visible.
	 */
	wnoutrefresh(stdscr);
	if (app->graph_only) {
		WINDOW *history = panel(1, 0, rows - 2, cols,
		    "History: Tab/Left/Right selects metric");
		draw_history_graph(history, app);
		wnoutrefresh(history);
		delwin(history);
		doupdate();
		return;
	}

	bool wide = cols >= 100;
	if (wide) {
		int left = cols / 2;
		WINDOW *proc = panel(1, 0, 6, left, "Processes");
		WINDOW *session = panel(1, left, 6, cols - left, "Session");
		WINDOW *manage = panel(7, 0, 6, left, "Manage: CoW / River");
		WINDOW *render = panel(7, left, 6, cols - left,
		    "Render: CoW / River");
		WINDOW *history = panel(13, 0, rows - 14, cols,
		    "History: Tab/Left/Right selects metric");
		draw_processes(proc, app);
		draw_session(session, app);
		draw_timing(manage, object_item(app->diag, "manage"),
		    "duration", "request_wait", app->rates.manage_per_second);
		draw_timing(render, object_item(app->diag, "render"),
		    "duration", "after_manage", app->rates.render_per_second);
		draw_history_graph(history, app);
		wnoutrefresh(proc); wnoutrefresh(session);
		wnoutrefresh(manage); wnoutrefresh(render);
		wnoutrefresh(history);
		delwin(proc); delwin(session); delwin(manage);
		delwin(render); delwin(history);
	} else {
		WINDOW *proc = panel(1, 0, 6, cols, "Processes");
		WINDOW *session = panel(7, 0, 5, cols, "Session");
		WINDOW *manage = panel(12, 0, 5, cols, "Manage: CoW / River");
		WINDOW *render = panel(17, 0, 5, cols, "Render: CoW / River");
		draw_processes(proc, app);
		draw_session(session, app);
		draw_timing(manage, object_item(app->diag, "manage"),
		    "duration", "request_wait", app->rates.manage_per_second);
		draw_timing(render, object_item(app->diag, "render"),
		    "duration", "after_manage", app->rates.render_per_second);
		wnoutrefresh(proc); wnoutrefresh(session);
		wnoutrefresh(manage); wnoutrefresh(render);
		delwin(proc); delwin(session); delwin(manage); delwin(render);
	}
	doupdate();
}

static cJSON *
combined_json(const struct app *app)
{
	cJSON *root = cJSON_CreateObject();
	time_t now = time(NULL);
	char timestamp[32];
	struct tm tm;
	gmtime_r(&now, &tm);
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tm);
	cJSON_AddStringToObject(root, "timestamp", timestamp);
	cJSON_AddBoolToObject(root, "connected", app->connected);
	cJSON_AddItemToObject(root, "state", app->status != NULL ?
	    cJSON_Duplicate(app->status, true) : cJSON_CreateNull());
	cJSON_AddItemToObject(root, "diagnostics", app->diag != NULL ?
	    cJSON_Duplicate(app->diag, true) : cJSON_CreateNull());
	cJSON *processes = cJSON_AddObjectToObject(root, "processes");
	const struct process_sample *samples[] = { &app->cow, &app->river };
	const char *names[] = { "cow", "river" };
	for (int i = 0; i < 2; i++) {
		cJSON *process = cJSON_AddObjectToObject(processes, names[i]);
		if (samples[i]->pid > 0)
			cJSON_AddNumberToObject(process, "pid", samples[i]->pid);
		else
			cJSON_AddNullToObject(process, "pid");
		if (samples[i]->cpu_valid)
			cJSON_AddNumberToObject(process, "cpu_percent",
			    samples[i]->cpu_percent);
		else
			cJSON_AddNullToObject(process, "cpu_percent");
		if (samples[i]->rss_valid)
			cJSON_AddNumberToObject(process, "rss_bytes",
			    (double)samples[i]->rss_bytes);
		else
			cJSON_AddNullToObject(process, "rss_bytes");
		if (samples[i]->peak_bytes > 0)
			cJSON_AddNumberToObject(process, "peak_rss_bytes",
			    (double)samples[i]->peak_bytes);
		else
			cJSON_AddNullToObject(process, "peak_rss_bytes");
	}
	cJSON *rates = cJSON_AddObjectToObject(root, "rates");
	cJSON_AddNumberToObject(rates, "manage_per_second",
	    app->rates.manage_per_second);
	cJSON_AddNumberToObject(rates, "render_per_second",
	    app->rates.render_per_second);
	return root;
}

static void
print_json_sample(const struct app *app)
{
	cJSON *root = combined_json(app);
	char *text = cJSON_PrintUnformatted(root);
	if (text != NULL) {
		puts(text);
		fflush(stdout);
		cJSON_free(text);
	}
	cJSON_Delete(root);
}

static void
wait_interval(unsigned milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (long)(milliseconds % 1000) * 1000000L,
	};

	while (nanosleep(&delay, &delay) < 0 && errno == EINTR && running)
		;
}

static int
json_mode(struct app *app, bool watch)
{
	app->status_fd = -1;
	(void)status_connect(app);
	status_pump(app);
	if (!sample(app))
		return EXIT_FAILURE;
	wait_interval(app->interval_ms);
	status_pump(app);
	if (!running || !sample(app))
		return EXIT_FAILURE;
	print_json_sample(app);
	while (watch && running) {
		wait_interval(app->interval_ms);
		status_pump(app);
		(void)sample(app);
		print_json_sample(app);
	}
	return EXIT_SUCCESS;
}

static int
tui_mode(struct app *app)
{
	setlocale(LC_ALL, "");
	initscr();
	cbreak();
	noecho();
	keypad(stdscr, true);
	curs_set(0);
	timeout(100);
	if (has_colors()) {
		start_color();
		use_default_colors();
		init_pair(COLOUR_BORDER, COLOR_CYAN, -1);
		init_pair(COLOUR_TITLE, COLOR_CYAN, -1);
		init_pair(COLOUR_LIVE, COLOR_GREEN, -1);
		init_pair(COLOUR_WARNING, COLOR_YELLOW, -1);
		init_pair(COLOUR_ERROR, COLOR_RED, -1);
		init_pair(COLOUR_GRAPH_COW, COLOR_GREEN, -1);
		init_pair(COLOUR_GRAPH_RIVER, COLOR_MAGENTA, -1);
		init_pair(COLOUR_GRAPH_RSS, COLOR_CYAN, -1);
		init_pair(COLOUR_GRAPH_MANAGE, COLOR_YELLOW, -1);
		init_pair(COLOUR_GRAPH_RENDER, COLOR_RED, -1);
		init_pair(COLOUR_SCALE, -1, -1);
		colours_enabled = true;
	}
	uint64_t next_sample = 0;

	while (running) {
		uint64_t now = mono_us();
		if (app->status_fd < 0 &&
		    now - app->last_connect_attempt_us >= 1000000ULL) {
			app->last_connect_attempt_us = now;
			(void)status_connect(app);
		}
		status_pump(app);
		if (!app->paused && now >= next_sample) {
			(void)sample(app);
			next_sample = now + app->interval_ms * 1000ULL;
			draw_dashboard(app);
		}
		int key = getch();
		switch (key) {
		case 'q': case 'Q': case 27:
			running = 0;
			break;
		case ' ':
			app->paused = !app->paused;
			if (!app->paused)
				next_sample = 0;
			draw_dashboard(app);
			break;
		case 'r': case 'R':
			history_clear(app);
			draw_dashboard(app);
			break;
		case '?':
			app->help = !app->help;
			draw_dashboard(app);
			break;
		case '\t': case KEY_RIGHT:
			app->graph_metric =
			    (app->graph_metric + 1) % GRAPH_METRIC_COUNT;
			draw_dashboard(app);
			break;
		case KEY_LEFT:
			app->graph_metric = (app->graph_metric +
			    GRAPH_METRIC_COUNT - 1) % GRAPH_METRIC_COUNT;
			draw_dashboard(app);
			break;
		case '1': case '2': case '3': case '4':
			app->graph_metric = key - '1';
			draw_dashboard(app);
			break;
		case 'g': case 'G':
			app->graph_only = !app->graph_only;
			draw_dashboard(app);
			break;
		case KEY_RESIZE:
			draw_dashboard(app);
			break;
		}
	}
	endwin();
	return EXIT_SUCCESS;
}

static void
usage(FILE *stream)
{
	fprintf(stream,
	    "usage: cowdiag [-hvw] [-i milliseconds] [-j]\n"
	    "       cowdiag -j -w [-i milliseconds]\n");
}

int
main(int argc, char **argv)
{
	struct app app = {
		.status_fd = -1,
		.interval_ms = 1000,
		.river = { .pid = -1 },
		.cow = { .pid = -1 },
	};
	bool json = false, watch = false;
	int ch;
	while ((ch = getopt(argc, argv, "hi:jvw")) != -1) {
		switch (ch) {
		case 'i': {
			char *end;
			long interval = strtol(optarg, &end, 10);
			if (*optarg == '\0' || *end != '\0' || interval < 100 ||
			    interval > 60000) {
				fprintf(stderr,
				    "cowdiag: interval must be 100..60000 ms\n");
				return EXIT_FAILURE;
			}
			app.interval_ms = (unsigned)interval;
			break;
		}
		case 'j': json = true; break;
		case 'w': watch = true; break;
		case 'v': printf("cowdiag %s\n", COW_VERSION); return 0;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return EXIT_FAILURE;
		}
	}

	if (optind != argc || (watch && !json)) {
		usage(stderr);
		return EXIT_FAILURE;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	int result = json ? json_mode(&app, watch) : tui_mode(&app);
	status_disconnect(&app);
	cJSON_Delete(app.status);
	cJSON_Delete(app.diag);
	return result;
}
