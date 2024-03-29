/*
 * Attempt to consolidate unavoidable suck into one file, away from dwl.c.  This
 * file is not meant to be pretty.  We use a .h file with static inline
 * functions instead of a separate .c module, or function pointers like sway, so
 * that they will simply compile out if the chosen #defines leave them unused.
 */

/* Leave these functions first; they're used in the others */
static inline void client_get_size_hints(Client *c, struct wlr_box *max, struct wlr_box *min) {
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_xdg_toplevel_state *state;
	toplevel = c->xdg_surface->toplevel;
	state = &toplevel->current;
	max->width = state->max_width;
	max->height = state->max_height;
	min->width = state->min_width;
	min->height = state->min_height;
}

static inline int toplevel_from_wlr_surface(struct wlr_surface *s, Client **pc, LayerSurface **pl) {
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_surface *root_surface;
	struct wlr_layer_surface_v1 *layer_surface;
	Client *c = NULL;
	LayerSurface *l = NULL;
	int type = -1;

	if (!s)
		return type;
	root_surface = wlr_surface_get_root_surface(s);


	if (wlr_surface_is_layer_surface(root_surface)
			&& (layer_surface = wlr_layer_surface_v1_from_wlr_surface(root_surface))) {
		l = layer_surface->data;
		type = LayerShell;
		goto end;
	}

	if (wlr_surface_is_xdg_surface(root_surface)
			&& (xdg_surface = wlr_xdg_surface_from_wlr_surface(root_surface))) {
		while (1) {
			switch (xdg_surface->role) {
			case WLR_XDG_SURFACE_ROLE_POPUP:
				if (!xdg_surface->popup->parent)
					return -1;
				else if (!wlr_surface_is_xdg_surface(xdg_surface->popup->parent))
					return toplevel_from_wlr_surface(xdg_surface->popup->parent, pc, pl);

				xdg_surface = wlr_xdg_surface_from_wlr_surface(xdg_surface->popup->parent);
				break;
			case WLR_XDG_SURFACE_ROLE_TOPLEVEL:
				c = xdg_surface->data;
				type = c->type;
				goto end;
			case WLR_XDG_SURFACE_ROLE_NONE:
				return -1;
			}
		}
	}

end:
	if (pl)
		*pl = l;
	if (pc)
		*pc = c;
	return type;
}

/* The others */
static inline void client_activate_surface(struct wlr_surface *s, int activated) {
	struct wlr_xdg_surface *surface;
	if (wlr_surface_is_xdg_surface(s)
			&& (surface = wlr_xdg_surface_from_wlr_surface(s))
			&& surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		wlr_xdg_toplevel_set_activated(surface->toplevel, activated);
}

static inline uint32_t client_set_bounds(Client *c, int32_t width, int32_t height) {
	if (c->xdg_surface->client->shell->version >=
			XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION && width >= 0 && height >= 0)
		return wlr_xdg_toplevel_set_bounds(c->xdg_surface->toplevel, width, height);
	return 0;
}

static inline void client_for_each_surface(Client *c, wlr_surface_iterator_func_t fn, void *data) {
	wlr_surface_for_each_surface(c->xdg_surface->surface, fn, data);
	wlr_xdg_surface_for_each_popup_surface(c->xdg_surface, fn, data);
}

static inline const char * client_get_appid(Client *c) {
	return c->xdg_surface->toplevel->app_id;
}

static inline void client_get_geometry(Client *c, struct wlr_box *geom) {
	wlr_xdg_surface_get_geometry(c->xdg_surface, geom);
}

static inline Client * client_get_parent(Client *c) {
	Client *p = NULL;
	if (c->xdg_surface->toplevel->parent)
		toplevel_from_wlr_surface(c->xdg_surface->toplevel->parent->base->surface, &p, NULL);

	return p;
}

static inline const char * client_get_title(Client *c) {
	return c->xdg_surface->toplevel->title;
}

static inline int client_is_float_type(Client *c) {
	struct wlr_box min = {0}, max = {0};
	client_get_size_hints(c, &max, &min);
	return (
		(min.width > 0 || min.height > 0 || max.width > 0 || max.height > 0) &&
		(min.width == max.width || min.height == max.height));
}

static inline int client_is_mapped(Client *c) {
	return c->xdg_surface->mapped;
}

static inline int client_is_rendered_on_mon(Client *c, Monitor *m) {
	/* This is needed for when you don't want to check formal assignment,
	 * but rather actual displaying of the pixels.
	 * Usually VISIBLEON suffices and is also faster. */
	struct wlr_surface_output *s;
	if (!c->scene->node.enabled)
		return 0;
	wl_list_for_each(s, &c->xdg_surface->surface->current_outputs, link)
		if (s->output == m->wlr_output)
			return 1;
	return 0;
}

static inline int client_is_stopped(Client *c) {
	int pid;
	siginfo_t in = {0};

	wl_client_get_credentials(c->xdg_surface->client->client, &pid, NULL, NULL);
	if (waitid(P_PID, pid, &in, WNOHANG|WCONTINUED|WSTOPPED|WNOWAIT) < 0) {
		/* This process is not our child process, while is very unluckely that
		 * it is stopped, in order to do not skip frames assume that it is. */
		if (errno == ECHILD)
			return 1;
	} else if (in.si_pid) {
		if (in.si_code == CLD_STOPPED || in.si_code == CLD_TRAPPED)
			return 1;
		if (in.si_code == CLD_CONTINUED)
			return 0;
	}

	return 0;
}

static inline void client_notify_enter(struct wlr_surface *s, struct wlr_keyboard *kb) {
	if (kb)
		wlr_seat_keyboard_notify_enter(seat, s, kb->keycodes,
				kb->num_keycodes, &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(seat, s, NULL, 0, NULL);
}

static inline void client_restack_surface(Client *c) {
	return;
}

static inline void client_send_close(Client *c) {
	wlr_xdg_toplevel_send_close(c->xdg_surface->toplevel);
}

static inline void client_set_fullscreen(Client *c, int fullscreen) {
	wlr_xdg_toplevel_set_fullscreen(c->xdg_surface->toplevel, fullscreen);
}

static inline uint32_t client_set_size(Client *c, uint32_t width, uint32_t height) {
	if (width == c->xdg_surface->toplevel->current.width
			&& height ==c->xdg_surface->toplevel->current.height)
		return 0;
	return wlr_xdg_toplevel_set_size(c->xdg_surface->toplevel, width, height);
}

static inline void client_set_tiled(Client *c, uint32_t edges) {
	wlr_xdg_toplevel_set_tiled(c->xdg_surface->toplevel, edges);
}

static inline struct wlr_surface * client_surface_at(Client *c, double cx, double cy, double *sx, double *sy) {
	return wlr_xdg_surface_surface_at(c->xdg_surface, cx, cy, sx, sy);
}

static inline int client_wants_fullscreen(Client *c) {
	return c->xdg_surface->toplevel->requested.fullscreen;
}
