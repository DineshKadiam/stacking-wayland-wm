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
#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <wayland-client.h>

#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/un.h>
#include <sys/ucred.h>
#include <sys/user.h>
#elif defined(__OpenBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#elif defined(__NetBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#endif

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
static uint64_t
pages_to_bytes(int64_t pages)
{
	long page_size = sysconf(_SC_PAGESIZE);
	return pages > 0 && page_size > 0 ?
	    (uint64_t)pages * (uint64_t)page_size : 0;
}
#endif

#if defined(__OpenBSD__) || defined(__NetBSD__)
static uint64_t
timeval_parts_to_us(uint64_t seconds, uint64_t microseconds)
{
	return seconds * 1000000ULL + microseconds;
}
#endif

bool
cowdiag_proc_parse_stat(char *line, long ticks, uint64_t *cpu_us)
{
#if defined(__linux__)
	char *rest = strrchr(line, ')');
	if (rest == NULL || rest[1] != ' ' || ticks <= 0)
		return false;
	rest += 2;
	char *save = NULL;
	char *token = strtok_r(rest, " ", &save);
	int field = 3;
	unsigned long long user_ticks = 0, system_ticks = 0;
	while (token != NULL) {
		if (field == 14)
			user_ticks = strtoull(token, NULL, 10);
		else if (field == 15) {
			system_ticks = strtoull(token, NULL, 10);
			*cpu_us = (user_ticks + system_ticks) * 1000000ULL /
			    (uint64_t)ticks;
			return true;
		}
		field++;
		token = strtok_r(NULL, " ", &save);
	}
#else
	(void)line;
	(void)ticks;
	(void)cpu_us;
#endif
	return false;
}

bool
cowdiag_proc_read(long pid, struct cowdiag_proc *sample)
{
	*sample = (struct cowdiag_proc){ 0 };
#if defined(__linux__)
	char path[64], line[4096];
	FILE *fp;

	snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
	fp = fopen(path, "r");
	if (fp == NULL)
		return false;
	if (fgets(line, sizeof(line), fp) == NULL) {
		fclose(fp);
		return false;
	}
	fclose(fp);
	if (!cowdiag_proc_parse_stat(line, sysconf(_SC_CLK_TCK),
		&sample->cpu_us))
		return false;

	snprintf(path, sizeof(path), "/proc/%ld/statm", pid);
	fp = fopen(path, "r");
	unsigned long pages = 0, resident = 0;
	if (fp == NULL || fscanf(fp, "%lu %lu", &pages, &resident) != 2) {
		if (fp != NULL)
			fclose(fp);
		return false;
	}
	fclose(fp);
	(void)pages;
	sample->rss_bytes = pages_to_bytes((int64_t)resident);

	snprintf(path, sizeof(path), "/proc/%ld/status", pid);
	fp = fopen(path, "r");
	if (fp != NULL) {
		while (fgets(line, sizeof(line), fp) != NULL) {
			unsigned long kib;
			if (sscanf(line, "VmHWM: %lu kB", &kib) == 1) {
				sample->peak_bytes = kib * 1024ULL;
				break;
			}
		}
		fclose(fp);
	}
	return true;
#elif defined(__FreeBSD__)
	struct kinfo_proc proc;
	int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid };
	size_t len = sizeof(proc);

	if (sysctl(mib, 4, &proc, &len, NULL, 0) == -1 || len == 0)
		return false;
	sample->cpu_us = proc.ki_runtime;
	sample->rss_bytes = pages_to_bytes(proc.ki_rssize);
	if (proc.ki_rusage.ru_maxrss > 0)
		sample->peak_bytes =
		    (uint64_t)proc.ki_rusage.ru_maxrss * 1024ULL;
	return true;
#elif defined(__OpenBSD__)
	struct kinfo_proc proc;
	int mib[] = {
		CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid,
		(int)sizeof(proc), 1,
	};
	size_t len = sizeof(proc);

	if (sysctl(mib, 6, &proc, &len, NULL, 0) == -1 || len == 0)
		return false;
	if (proc.p_uvalid) {
		sample->cpu_us =
		    timeval_parts_to_us(proc.p_uutime_sec,
			proc.p_uutime_usec) +
		    timeval_parts_to_us(proc.p_ustime_sec,
			proc.p_ustime_usec);
	} else {
		sample->cpu_us = timeval_parts_to_us(proc.p_rtime_sec,
		    proc.p_rtime_usec);
	}
	sample->rss_bytes = pages_to_bytes(proc.p_vm_rssize);
	if (proc.p_uvalid && proc.p_uru_maxrss > 0)
		sample->peak_bytes = proc.p_uru_maxrss * 1024ULL;
	return true;
#elif defined(__NetBSD__)
	struct kinfo_proc2 proc;
	int mib[] = {
		CTL_KERN, KERN_PROC2, KERN_PROC_PID, (int)pid,
		(int)sizeof(proc), 1,
	};
	size_t len = sizeof(proc);

	if (sysctl(mib, 6, &proc, &len, NULL, 0) == -1 || len == 0)
		return false;
	if (proc.p_uvalid) {
		sample->cpu_us =
		    timeval_parts_to_us(proc.p_uutime_sec,
			proc.p_uutime_usec) +
		    timeval_parts_to_us(proc.p_ustime_sec,
			proc.p_ustime_usec);
	} else {
		sample->cpu_us = timeval_parts_to_us(proc.p_rtime_sec,
		    proc.p_rtime_usec);
	}
	sample->rss_bytes = pages_to_bytes(proc.p_vm_rssize);
	if (proc.p_uvalid && proc.p_uru_maxrss > 0)
		sample->peak_bytes = proc.p_uru_maxrss * 1024ULL;
	return true;
#else
	(void)pid;
	return false;
#endif
}

long
cowdiag_river_pid(void)
{
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL)
		return -1;
	int fd = wl_display_get_fd(display);
	long pid = -1;
#if defined(__linux__)
	struct ucred credentials;
	socklen_t len = sizeof(credentials);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &len) == 0)
		pid = credentials.pid;
#elif defined(__FreeBSD__)
	struct xucred credentials;
	socklen_t len = sizeof(credentials);
	if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &credentials, &len) == 0 &&
	    credentials.cr_version == XUCRED_VERSION)
		pid = credentials.cr_pid;
#elif defined(__OpenBSD__)
	struct sockpeercred credentials;
	socklen_t len = sizeof(credentials);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &len) == 0)
		pid = credentials.pid;
#elif defined(__NetBSD__)
	struct unpcbid credentials;
	socklen_t len = sizeof(credentials);
	if (getsockopt(fd, SOL_LOCAL, LOCAL_PEEREID, &credentials, &len) == 0)
		pid = credentials.unp_pid;
#else
	(void)fd;
#endif
	wl_display_disconnect(display);
	return pid;
}
