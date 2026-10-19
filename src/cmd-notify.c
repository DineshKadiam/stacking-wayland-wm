#include "cow/cmd.h"
#include "cow/notification.h"

static enum cmd_retval cmd_notify_exec(struct cmd *, struct cmd_q *);

struct cmd_entry cmd_notify_entry = {
	"notify",
	"dn:t:",
	0,
	1,
	"notify [-n name] [-t milliseconds] text | notify -n name -d",
	cmd_notify_exec,
	CMD_OWN_TS,
};

static enum cmd_retval
cmd_notify_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args *args = self->args;
	const char *name = args_get(args, 'n');
	bool dismiss = args_has(args, 'd');
	if ((name != NULL && *name == '\0') ||
	    (dismiss && (name == NULL || args->argc != 0 || args_has(args, 't'))) ||
	    (!dismiss && args->argc != 1)) {
		cmdq_error(cmdq, "usage: %s", self->entry->usage);
		return CMD_RETURN_ERROR;
	}
	if (dismiss) {
		cow_notification_dismiss(name);
		return CMD_RETURN_NORMAL;
	}
	int timeout = 3000;
	if (args_has(args, 't')) {
		char *cause = NULL;
		timeout = args_strtonum(args, 't', 0, 86400000, &cause);
		if (cause != NULL) {
			cmdq_error(cmdq, "notify: timeout %s", cause);
			xfree(cause);
			return CMD_RETURN_ERROR;
		}
	}
	cow_notification_show(name != NULL ? name : "default", NULL,
	    args->argv[0], cmd_ctx_target_window(cmdq),
	    cmd_ctx_target_output(cmdq), timeout);
	return CMD_RETURN_NORMAL;
}
