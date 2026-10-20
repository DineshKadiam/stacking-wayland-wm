#define main cowbuttons_main
#include "../modules/cowbuttons/src/cowbuttons.c"
#undef main

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%d: %s\n", __LINE__, #expr); \
		exit(1); \
	} \
} while (0)

int
main(int argc, char **argv)
{
	CHECK(argc == 3 || argc == 4);
	style_init();
	CHECK(cow_module_config_instances_init(&config_instances, PROG,
	    SHOW_INDEX_ENV));
	cJSON *candidates = cJSON_Parse("{\"applications\":["
	    "{\"id\":\"console.desktop\",\"Name\":\"A console\",\"Categories\":\"TextEditor;\",\"Terminal\":true,\"available\":true},"
	    "{\"id\":\"z.desktop\",\"Name\":\"Z editor\",\"Categories\":\"TextEditor;\",\"available\":true},"
	    "{\"id\":\"a.desktop\",\"Name\":\"A editor\",\"Categories\":\"TextEditor;\",\"available\":true},"
	    "{\"id\":\"missing.desktop\",\"Name\":\"Missing\",\"available\":false}]}");
	CHECK(candidates != NULL);
	cJSON *chosen = application_select(candidates, "TextEditor", NULL, "foot");
	CHECK(!strcmp(application_string(chosen, "id"), "a.desktop"));
	chosen = application_select(candidates, "TextEditor", "z.desktop", "foot");
	CHECK(!strcmp(application_string(chosen, "id"), "z.desktop"));
	cJSON_AddBoolToObject(chosen, "selected", true);
	chosen = application_select(candidates, "TextEditor", "z.desktop", "foot");
	CHECK(!strcmp(application_string(chosen, "id"), "a.desktop"));
	CHECK(application_select(candidates, "AudioVideo", NULL, "foot") == NULL);
	CHECK(application_select(candidates, NULL, "missing.desktop", "foot") == chosen);
	cJSON_Delete(candidates);
	if (command_exists("xdg-mime")) {
		char *preferred = application_default("TextEditor");
		CHECK(preferred && !strcmp(preferred, "TextEditor.desktop"));
		free(preferred);
	}
	instance_output = xstrdup("DP-1");
	CHECK(load_config(argv[1]));
	CHECK(!strcmp(instance_title, "cowbuttons:Starter:DP-1"));
	CHECK(cells[0].indicator && cells[0].panel_dir == PANEL_UP);
	if (argc == 4)
		CHECK(strstr(cells[1].command[CLICK_LEFT][CLICK_SINGLE], argv[3]) != NULL);
	char *command = cell_command(&cells[0], "starter-applications #{panel_title}");
	CHECK(command && strstr(command, "let panel_title = \"cowbuttons:Starter:DP-1\""));
	free(command);
	CHECK(lock_instance() == 1);
	pid_t child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		CHECK(lock_instance() == 0);
		xstrset(&instance_output, "DP-10");
		build_instance_title();
		CHECK(lock_instance() == 1);
		_exit(0);
	}
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	char socket_path[PATH_MAX];
	CHECK(cow_module_cmd_path(socket_path, sizeof(socket_path), PROG));
	int socket_file = open(socket_path, O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(socket_file >= 0);
	CHECK(instance_policy_current(true));
	CHECK(instance_policy_current(false));
	CHECK(unlink(socket_path) == 0);
	int replacement = open(socket_path, O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(replacement >= 0);
	CHECK(!instance_policy_current(false));
	CHECK(instance_policy_current(true));
	CHECK(instance_policy_current(false));
	close(socket_file);
	close(replacement);
	CHECK(unlink(socket_path) == 0);
	close(instance_lock);
	instance_lock = -1;
	cJSON *snapshot = cJSON_Parse("{\"outputs\":["
	    "{\"name\":\"DP-2\",\"current\":true,\"current_desk\":1},"
	    "{\"name\":\"DP-1\",\"number\":1,\"current\":false,\"current_desk\":2,"
	    "\"desks\":[{\"nr\":0,\"name\":\"Work\"},{\"nr\":2,\"name\":\"Chat\"}]}]}");
	state_from_json(&current_state, snapshot);
	CHECK(!strcmp(current_state.current_output, "DP-1"));
	CHECK(!strcmp(current_state.current_desk, "2"));
	CHECK(!strcmp(current_state.desk_list, "Work Chat"));
	for (int i = 0; i < n_cells; i++) {
		if (cells[i].type != CELL_DESKS)
			continue;
		int x, y, w, h;
		desk_rect(&cells[i], 1, 2, &x, &y, &w, &h);
		ptr_x = x + w / 2;
		ptr_y = y + h / 2;
		char *action = cell_command(&cells[i],
		    "desk -t #{output_selector} -d #{desk_nr}");
		CHECK(action && strstr(action, "let desk_nr = 2"));
		CHECK(strstr(action, "let output_selector = \"@1\""));
		free(action);
	}
	cJSON_Delete(snapshot);
	cJSON_Delete(desk_output);
	desk_output = NULL;
	/* An output with a similar name must not keep this panel alive. */
	running = 1;
	handle_status_line("{\"outputs\":[{\"name\":\"DP-10\"}]}", NULL);
	CHECK(!running);
	running = 1;
	handle_status_line("{\"outputs\":[]}", NULL);
	CHECK(!running);
	int launchers = 0, editor = 0, window_lists = 0, externals = 0;
	int pagers = 0, desk_cells = 0;
	for (int i = 0; i < n_cells; i++) {
		struct cell *c = &cells[i];
		if (c->type == CELL_EXTERNAL)
			externals++;
		if (c->type == CELL_DESKS)
			desk_cells++;
		if (c->external.match_title &&
		    !strcmp(c->external.match_title, "cowpager:StarterPages:DP-1")) {
			pagers++;
			CHECK(c->external.respawn);
		}
		if (c->external.match_title &&
		    strstr(c->external.match_title, "-terminal")) {
			CHECK(!strcmp(c->external.match_title,
			    "cowbuttons:Starter:DP-1-terminal"));
			CHECK(c->external.respawn);
		}
		if (c->external.match_title &&
		    strstr(c->external.match_title, "-tasks"))
			CHECK(!strcmp(c->external.match_title,
			    "cowbuttons:Starter:DP-1-tasks"));
		const char *action = c->command[CLICK_LEFT][CLICK_SINGLE];
		if (action != NULL && !strcmp(action, "window-list"))
			window_lists++;
		if (action == NULL || strstr(action, "cow-appmenu --launch") == NULL)
			continue;
		CHECK(strcmp(c->label, "Missing editor"));
		if (!strcmp(c->label, "An \"Editor\""))
			editor++;
		CHECK(cfg.col_w[c->col] == 40);
		CHECK(c->style.image_size == 24);
		CHECK(c->style.icon_only);
		CHECK(c->style.image_pos == IMAGE_CENTER);
		for (int j = 0; j < i; j++) {
			const char *prior = cells[j].command[CLICK_LEFT][CLICK_SINGLE];
			CHECK(prior == NULL || strcmp(action, prior));
		}
		launchers++;
	}
	CHECK(launchers == atoi(argv[2]));
	CHECK(desk_cells == 1);
	CHECK(pagers == (command_exists("cowpager") ? 1 : 0));
	if (launchers == 0)
		CHECK(externals == 0 && window_lists == 1);
	if (launchers == 1)
		CHECK(editor == 1);

	/* Opening must not turn a delayed collapsed configure into a resize. */
	struct wl_array states;
	wl_array_init(&states);
	expanded_w = 1280;
	expanded_h = 96;
	selected_panels[0].open = false;
	reopening_w = target_width();
	selected_panels[0].open = true;
	CHECK(reopening_w < minimum_extent(true));
	xdg_toplevel_configure(NULL, NULL, reopening_w, 96, &states);
	CHECK(expanded_w == 1280);
	xdg_toplevel_configure(NULL, NULL, minimum_extent(true), 96, &states);
	CHECK(expanded_w == 1280);
	xdg_toplevel_configure(NULL, NULL, 1280, 96, &states);
	CHECK(reopening_w == 0);
	xdg_toplevel_configure(NULL, NULL, 900, 96, &states);
	CHECK(expanded_w == 900);
	wl_array_release(&states);
	return 0;
}
