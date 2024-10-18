#include <fcntl.h>
#include <xcb/randr.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

#include "process.h"
#include "x.h"

extern struct session *ps_g;

int spawn_picomling(struct x_connection *c) {
	int dev_null = open("/dev/null", O_RDWR);
	if (dev_null < 0) {
		log_error("Failed to open /dev/null");
		return -1;
	}
	int screen = 0;
	auto new_c = xcb_connect(NULL, &screen);
	if (xcb_connection_has_error(new_c)) {
		log_error("Failed to open new connection");
		close(dev_null);
		xcb_disconnect(new_c);
		return -1;
	}

	pid_t pid = fork();
	if (pid == -1) {
		log_error("Failed to fork");
		return -1;
	}

	if (pid != 0) {
		close(dev_null);
		// Close the connection on the parent's side so `xcb_disconnect` won't
		// shut it down.
		close(xcb_get_file_descriptor(new_c));
		xcb_disconnect(new_c);
		return 1;
	}

	// Prevent child from using parent's X connection
	ps_g = NULL;

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	dup2(dev_null, STDIN_FILENO);
	dup2(dev_null, STDOUT_FILENO);
	dup2(dev_null, STDERR_FILENO);

	setsid();

	x_connection_init_xcb(c, new_c, screen);

	xcb_prefetch_extension_data(c->c, &xcb_randr_id);
	xcb_prefetch_extension_data(c->c, &xcb_render_id);

	// Initialize the X RandR extension.
	auto extension = xcb_get_extension_data(c->c, &xcb_randr_id);
	if (!extension || !extension->present) {
		log_error("The X server doesn't have the X RandR extension.");

		return -1;
	}

	c->e.has_randr = true;
	c->e.randr_event = extension->first_event;

	// Initialize the X Render extension.
	extension = xcb_get_extension_data(c->c, &xcb_render_id);
	if (!extension || !extension->present) {
		log_error("The X server doesn't have the X Render extension.");

		return -1;
	}

	c->e.render_error = extension->first_error;

	return 0;
}
