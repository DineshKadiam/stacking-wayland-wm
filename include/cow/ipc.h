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
#if !defined(COW_IPC_H)
#define COW_IPC_H

#include <stdbool.h>

#define COW_IPC_MAX_CLIENTS 32
#define COW_IPC_BUF_SIZE 4096
#define COW_IPC_MAX_ARGS 64

struct cow_ipc_client {
	int	      fd;
	char	      buf[COW_IPC_BUF_SIZE];
	int	      buf_len;
	bool	      active;
	bool	      responded;
	struct event *ev;
};

struct cow_ipc {
	int		      server_fd;
	struct event	     *server_ev;
	char		     *socket_path;
	char		     *config_path;
	struct cow_ipc_client clients[COW_IPC_MAX_CLIENTS];
	int		      num_clients;
	bool		      startup_mode;
};

extern struct cow_ipc cow_ipc;

/*
 * Initialize the IPC command socket.
 * Creates a Unix domain socket at $XDG_RUNTIME_DIR/cow-cmd.sock.
 */
bool cow_ipc_init(void);

/*
 * Shut down IPC: close all clients, close server, unlink socket.
 */
void cow_ipc_fini(void);

/*
 * Dispatch a single command line.
 * If client_fd >= 0, send JSON response to that fd.
 * If client_fd < 0 (config file mode), log errors to stderr.
 * Returns true if the command was recognized.
 */
bool cow_ipc_dispatch(const char *, int);

/*
 * Load a line-based config file by dispatching each line as a command.
 * Exec commands are deferred until cow_exec_run_deferred() is called.
 * Returns true on success.
 */
bool cow_ipc_load_config(const char *);
bool cow_ipc_include_config(const char *, int);

/*
 * Look up a command by name.  Returns NULL if not found.
 */
/*
 * IPC response helpers (shared with cmd-get.c).
 */
struct cJSON;

void cow_ipc_respond(int, const char *);
void cow_ipc_respond_error(int, const char *);
void cow_ipc_respond_data(int, struct cJSON *);

#endif
