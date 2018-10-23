/*
 * There are a number of oddities with dealing with XWayland, and
 * its behavior change depending on if you are using rootless mode
 * or not.
 *
 * With 'normal' mode it behaves as a dumb (and buggy) wl_shell
 * client that basically ignored everything.
 *
 * With 'rootless' mode, it creates compositor surfaces and uses
 * them directly - being basically the only client to do so. The
 * job then is to pair these surfaces based on a window property
 * and just treat them as something altogether special by adding
 * a custom window-manager.
 *
 * The process we're using is that whenever a compositor surface
 * tries to commit, we check if we are running with xwayland and
 * then fires up a window manager whi
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>

static FILE* wmfd_output = NULL;
static pid_t xwl_wm_pid = -1;

static int wmfd_input = -1;
static char wmfd_inbuf[256];
static size_t wmfd_ofs = 0;

/* "known" mapped windows, we trigger the search etc. when a buffer
 * is commited without a known backing on the compositor, and try
 * to 'pair' it with ones that we have been told about */
struct xwl_window {
	uint32_t id;
	uint32_t surface_id;
	bool paired;
	bool subsurface;
};

/* just linear search in a fixed buffer for now, scaling problems
 * are elsewhere for quite some time to come

#include "../uthash.h"
struct window {
	uint32_t id;
	xcb_window_t wnd;
	UT_hash_handle hh;
};
static struct window* windows;
*/

static struct xwl_window xwl_windows[256];
static struct xwl_window* xwl_find(uint32_t id)
{
	for (size_t i = 0; i < COUNT_OF(xwl_windows); i++)
		if (xwl_windows[i].id == id)
			return &xwl_windows[i];

	return NULL;
}

static struct xwl_window* xwl_find_surface(uint32_t id)
{
	for (size_t i = 0; i < COUNT_OF(xwl_windows); i++)
		if (xwl_windows[i].surface_id == id)
			return &xwl_windows[i];

	return NULL;
}

static struct xwl_window* xwl_find_alloc(uint32_t id)
{
	struct xwl_window* wnd = xwl_find(id);
	if (wnd)
		return wnd;

	wnd = xwl_find(0);
	return wnd;
}

static void process_input(const char* msg)
{
	trace(TRACE_XWL, "wm->%s", msg);
	struct arg_arr* cmd = arg_unpack(msg);
	if (!cmd){
		trace(TRACE_XWL, "malformed message: %s", msg);
		return;
	}

/* map : id ( window should be made visibile )
 * unmap : id ( window should be made invisible )
 * configure : id ( resize/reposition )
 */
	const char* arg;
	if (!arg_lookup(cmd, "kind", 0, &arg)){
		trace(TRACE_XWL, "malformed argument: %s, missing kind", msg);
		goto cleanup;
	}
	else if (strcmp(arg, "surface") == 0){
		if (!arg_lookup(cmd, "id", 0, &arg)){
			trace(TRACE_XWL, "malformed surface argument: missing id");
			goto cleanup;
		}
		uint32_t id = strtoul(arg, NULL, 10);
		if (!arg_lookup(cmd, "surface_id", 0, &arg)){
			trace(TRACE_XWL, "malformed surface argument: missing surface id");
			goto cleanup;
		}
		uint32_t surface_id = strtoul(arg, NULL, 10);
		trace(TRACE_XWL, "surface id:%"PRIu32"-%"PRIu32, id, surface_id);
		struct xwl_window* wnd = xwl_find_alloc(id);
		if (!wnd)
			goto cleanup;
		wnd->surface_id = surface_id;
		wnd->id = id;
		wnd->paired = true;
	}
	else if (strcmp(arg, "map") == 0){
		if (!arg_lookup(cmd, "id", 0, &arg)){
			trace(TRACE_XWL, "malformed map argument: missing id");
			goto cleanup;
		}
		uint32_t id = strtoul(arg, NULL, 10);
		trace(TRACE_XWL, "map id:%"PRIu32, id);
		struct xwl_window* wnd = xwl_find_alloc(id);
		if (!wnd)
			goto cleanup;

		wnd->id = id;
		if (arg_lookup(cmd, "type", 0, &arg)){
			trace(TRACE_XWL, "mapped with type %s", arg);
			wnd->subsurface = true;
		}
	}
	else if (strcmp(arg, "unmap") == 0){

	}
	else if (strcmp(arg, "configure") == 0){
/* currently the xcb_configure_request on the other side blindly
 * accepts their idea of the configure as to the size, it will be
 * reflected on the next buffer commit, what we can do here is
 * VIEWPORT- hint the x/y */
		trace(TRACE_XWL, "configure");
	}

cleanup:
	arg_cleanup(cmd);
}

/*
 * Process / update the incoming pipe, or spawn/respawn the WM if it doesn't
 * exist. This will synch the map- table of known surface IDs that we want to
 * pair with surfaces.
 */
static void xwl_check_wm()
{
	if (xwl_wm_pid == -1){
		trace(TRACE_XWL, "spawning 'arcan-xwayland-wm'");
		int p2c_pipe[2];
		int c2p_pipe[2];
		if (-1 == pipe(p2c_pipe))
			return;

		if (-1 == pipe(c2p_pipe)){
			close(p2c_pipe[0]);
			close(p2c_pipe[1]);
			return;
		}

		wmfd_input = c2p_pipe[0];
		wmfd_output = fdopen(p2c_pipe[1], "w");

		xwl_wm_pid = fork();

		if (-1 == xwl_wm_pid){
			fprintf(stderr, "Couldn't spawn wm- process (fork failed)\n");
			exit(EXIT_FAILURE);
		}

/* child, close, dup spawn */
		if (!xwl_wm_pid){
			char* const argv[] = {"arcan-xwayland-wm", NULL};
			close(p2c_pipe[1]);
			close(c2p_pipe[0]);
			dup2(p2c_pipe[0], STDIN_FILENO);
			dup2(c2p_pipe[1], STDOUT_FILENO);
			close(p2c_pipe[1]);
			close(c2p_pipe[0]);
			execvp("arcan-xwayland-wm", argv);
			execv("arcan-xwayland-wm", argv);
			exit(EXIT_FAILURE);
		}

/* want the input-pipe to work non-blocking here */
		int flags = fcntl(wmfd_input, F_GETFL);
			if (-1 != flags)
				fcntl(wmfd_input, F_SETFL, flags | O_NONBLOCK);

/* drop child write end, parent read end as the wm process owns these now */
		close(c2p_pipe[1]);
		close(p2c_pipe[0]);
	}

/* populate inbuffer, look for linefeed */
	char inbuf[256];
	ssize_t nr = read(wmfd_input, inbuf, 256);
	if (-1 == nr){
		if (errno != EAGAIN && errno != EINTR){
			fclose(wmfd_output);
			close(wmfd_input);
			wmfd_ofs = 0;
			kill(xwl_wm_pid, SIGKILL);
			waitpid(xwl_wm_pid, NULL, 0);
			xwl_wm_pid = -1;
			wmfd_input = -1;
			trace(TRACE_XWL, "arcan-xwayland-wm died");
		}
		return;
	}

/* check the new input for a linefeed character, or flush to the buffer */
	for (size_t i = 0; i < nr; i++){
		if (inbuf[i] == '\n'){
			wmfd_inbuf[wmfd_ofs] = '\0';
			process_input(wmfd_inbuf);
			wmfd_ofs = 0;
		}
/* accept crop on overflow (though no command should be this long) */
		else {
			wmfd_inbuf[wmfd_ofs] = inbuf[i];
			wmfd_ofs = (wmfd_ofs + 1) % sizeof(wmfd_inbuf);
		}
	}
}

static bool xwlsurf_shmifev_handler(
	struct comp_surf* surf, struct arcan_event* ev)
{
	if (ev->category != EVENT_TARGET || !wmfd_output)
		return false;

/* translate relevant non-input shmif events to the text- based
 * format used with the wm- process */

	struct xwl_window* wnd =
		xwl_find_surface(wl_resource_get_id(surf->shell_res));
	if (!wnd)
		return false;

	switch (ev->tgt.kind){
	case TARGET_COMMAND_DISPLAYHINT:{
		int rw = ev->tgt.ioevs[0].iv;
		int rh = ev->tgt.ioevs[1].iv;
		int dw = rw - (int)surf->acon.w;
		int dh = rh - (int)surf->acon.h;
		if (rw > 0 && rh > 0 && (dw != 0 || dh != 0)){
			trace(TRACE_XWL, "displayhint: %"PRIu32",%"PRIu32,
				ev->tgt.ioevs[0].iv, ev->tgt.ioevs[1].iv);

			fprintf(wmfd_output,
				"id=%"PRIu32":kind=resize:width=%"PRIu32":height=%"PRIu32"\n",
				(uint32_t) wnd->id,
				(uint32_t) abs(ev->tgt.ioevs[0].iv),
				(uint32_t) abs(ev->tgt.ioevs[1].iv)
			);
			fflush(wmfd_output);
		}
		return true;
	}
/* write to the xwl_wm_fd:
 * configure:id=%d:...
 * focus:id=%d
 */
	break;
	default:
	break;
	}

	return false;
}

static bool xwl_defer_handler(
	struct surface_request* req, struct arcan_shmif_cont* con)
{
	if (!req || !con){
		return false;
	}

	struct comp_surf* surf = wl_resource_get_user_data(req->target);
	surf->acon = *con;
	surf->cookie = 0xfeedface;
	surf->shell_res = req->target;
	surf->dispatch = xwlsurf_shmifev_handler;
	surf->id = wl_resource_get_id(surf->shell_res);

	return true;
}

static struct xwl_window*
	lookup_surface(struct comp_surf* surf, struct wl_resource* res)
{
	if (!wl.use_xwayland)
		return NULL;

/* always start by synching against pending from wm as the surface + atom
 * mapping might be done there before we actually get to this stage */
	xwl_check_wm();
	uint32_t id = wl_resource_get_id(res);
	struct xwl_window* wnd = xwl_find_surface(id);
	if (!wnd){
		wnd = xwl_find(0);
		if (!wnd){
			trace(TRACE_XWL, "out-of-memory");
			return NULL;
		}
		wnd->surface_id = id;
	}
	else if (!wnd->paired){
		trace(TRACE_XWL, "paired %"PRIu32, id);
		wnd->paired = true;
	}

	return wnd;
}

static bool xwl_pair_surface(struct comp_surf* surf, struct wl_resource* res)
{
/* do we know of a matching xwayland- provided surface? */
	struct xwl_window* wnd = lookup_surface(surf, res);
	if (!wnd)
		return false;

/* if so, allocate the corresponding arcan- side resource */
	return request_surface(surf->client, &(struct surface_request){
/* SEGID should be X11, but need to patch durden as well */
			.segid = wnd->subsurface ? SEGID_MEDIA : SEGID_APPLICATION,
			.target = res,
			.trace = "xwl",
			.dispatch = xwl_defer_handler,
			.client = surf->client,
			.source = surf,
			.tag = NULL
	}, 'X');
}