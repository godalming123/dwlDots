#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_pointer.h>

// WL_LISTENER's //
// These are a wlroots struct with a .notify property
// that takes a function that accepts some args and
// returns nothing. They can then be used with the
// wl_signal_add function which takes an event and a
// pointer to a wl_listener and causes the listener
// function to be ran the next time the event triggers.

static void axisNotify(struct wl_listener *listener, void *data) {
	// This event is forwarded by the cursor when a pointer emits an axis event,
	// for example when you move the scroll wheel.
	struct wlr_pointer_axis_event *event = data;
	IDLE_NOTIFY_ACTIVITY;
	/* TODO: allow usage of scroll whell for mousebindings, it can be implemented
	 * checking the event's orientation and the delta of the event */
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(seat,
		event->time_msec, event->orientation, event->delta,
		event->delta_discrete, event->source);
}

static void buttonPress(struct wl_listener *listener, void *data) {
	struct wlr_pointer_button_event *event = data;
	struct wlr_keyboard *keyboard;
	uint32_t mods;
	Client *c;

	IDLE_NOTIFY_ACTIVITY;

	switch (event->state) {
		case WLR_BUTTON_PRESSED:
			cursor_mode = CurPressed;
			if (locked)
				break;

			/* Change focus if the button was _pressed_ over a client */
			xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
			if (c)
				focusclient(c, 1);

			keyboard = wlr_seat_get_keyboard(seat);
			mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
			ignoreNextKeyrelease=true;
			if (handleMousePress(mods, event->button))
				return;
		break;
		case WLR_BUTTON_RELEASED:
			/* If you released any buttons, we exit interactive move/resize mode. */
			if (!locked && cursor_mode != CurNormal && cursor_mode != CurPressed) {
				cursor_mode = CurNormal;
				/* Clear the pointer focus, this way if the cursor is over a surface
				 * we will send an enter event after which the client will provide us
				 * a cursor surface */
				wlr_seat_pointer_clear_focus(seat);
				motionnotify(0);
				/* Drop the window off on its new monitor */
				selmon = xytomon(cursor->x, cursor->y);
				setmon(grabc, selmon, 0);
				return;
			} else {
				cursor_mode = CurNormal;
			}
		break;
	}
	/* If the event wasn't handled by the compositor, notify the client with
	 * pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(seat,
			event->time_msec, event->button, event->state);
}

static void cursorFrame(struct wl_listener *listener, void *data) {
	// This event is forwarded by the cursor when a pointer emits an frame
	// event. Frame events are sent after regular pointer events to group
	// multiple events together. For instance, two axis events may happen at the
	// same time, in which case a frame event won't be sent in between.
	
	// Notify the client with pointer focus of the frame event.
	wlr_seat_pointer_notify_frame(seat);
}

static void motionRelative(struct wl_listener *listener, void *data) {
	// This event is forwarded by the cursor when a pointer
	// emits a _relative_ pointer motion event (i.e. a delta)
	struct wlr_pointer_motion_event *event = data;

	// The cursor doesn't move unless we tell it to. The cursor automatically
	// handles constraining the motion to the output layout, as well as any
	// special configuration applied for the specific input device which
	// generated the event. You can pass NULL for the device if you want to move
	// the cursor around without any input.
	wlr_cursor_move(cursor, &event->pointer->base, event->delta_x, event->delta_y);
	motionnotify(event->time_msec);
}

static void motionAbsolute(struct wl_listener *listener, void *data) {
	// This event is forwarded by the cursor when a pointer emits an _absolute_
	// motion event, from 0..1 on each axis. This happens, for example, when
	// wlroots is running under a Wayland window rather than KMS+DRM, and you
	// move the mouse over the window. You could enter the window from any edge,
	// so we have to warp the mouse there. There is also some hardware which
	// emits these events.
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);
	motionnotify(event->time_msec);
}

static void destroyDragIcon(struct wl_listener *listener, void *data) {
	struct wlr_drag_icon *icon = data;
	wlr_scene_node_destroy(icon->data);
	/* Focus enter isn't sent during drag, so refocus the focused node. */
	focusclient(focustop(selmon), 1);
	motionnotify(0);
}

static void createIdleInhibitor(struct wl_listener *listener, void *data);

static void destroyIdleInhibitor(struct wl_listener *listener, void *data) {
	// `data` is the wlr_surface of the idle inhibitor being destroyed,
	// at this point the idle inhibitor is still in the list of the manager
	checkidleinhibitor(wlr_surface_get_root_surface(data));
}

static void updateMons(struct wl_listener *listener, void *data) {
	// Called whenever the output layout changes: adding or removing a
	// monitor, changing an output's mode or position, etc. This is where
	// the change officially happens and we update geometry, window
	// positions, focus, and the stored configuration in wlroots'
	// output-manager implementation.
	struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
	Client *c;
	struct wlr_output_configuration_head_v1 *config_head;
	Monitor *m;

	// First remove the disabled monitors from the layout
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
		config_head->state.enabled = 0;
		/* Remove this output from the layout to avoid cursor enter inside it */
		wlr_output_layout_remove(output_layout, m->wlr_output);
		closemon(m);
		memset(&m->m, 0, sizeof(m->m));
		memset(&m->w, 0, sizeof(m->w));
	}
	// Insert outputs that need to
	wl_list_for_each(m, &mons, link)
		if (m->wlr_output->enabled && !wlr_output_layout_get(output_layout, m->wlr_output))
			wlr_output_layout_add_auto(output_layout, m->wlr_output);

	// Now that we update the output layout we can get its box
	wlr_output_layout_get_box(output_layout, NULL, &sgeom);

	/* Make sure the clients are hidden when dwl is locked */
	wlr_scene_node_set_position(&locked_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(locked_bg, sgeom.width, sgeom.height);

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);

		/* Get the effective monitor geometry to use for surfaces */
		wlr_output_layout_get_box(output_layout, m->wlr_output, &(m->m));
		wlr_output_layout_get_box(output_layout, m->wlr_output, &(m->w));
		wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);

		wlr_scene_node_set_position(&m->fullscreen_bg->node, m->m.x, m->m.y);
		wlr_scene_rect_set_size(m->fullscreen_bg, m->m.width, m->m.height);

		if (m->lock_surface) {
			struct wlr_scene_tree *scene_tree = m->lock_surface->surface->data;
			wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
			wlr_session_lock_surface_v1_configure(m->lock_surface, m->m.width, m->m.height);
		}

		/* Calculate the effective monitor geometry to use for clients */
		arrangelayers(m);
		/* Don't move clients to the left output when plugging monitors */
		arrange(m);

		config_head->state.enabled = 1;
		config_head->state.mode = m->wlr_output->current_mode;
		config_head->state.x = m->m.x;
		config_head->state.y = m->m.y;
	}

	if (selmon && selmon->wlr_output->enabled) {
		wl_list_for_each(c, &clients, link)
			if (!c->mon && client_is_mapped(c))
				setmon(c, selmon, c->tags);
		if (selmon->lock_surface)
			client_notify_enter(selmon->lock_surface->surface,
				wlr_seat_get_keyboard(seat));
	}

	wlr_output_manager_v1_set_configuration(output_mgr, config);
}

static void inputDevice(struct wl_listener *listener, void *data) {
	// This event is raised by the backend when a new input device becomes available
	struct wlr_input_device *device = data;
	uint32_t caps;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		createkeyboard(wlr_keyboard_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		createpointer(wlr_pointer_from_input_device(device));
		break;
	default:
		/* TODO handle other input device types */
		break;
	}

	// We need to let the wlr_seat know what our capabilities are, which is
	// communiciated to the client. In dwl we always have a cursor, even if
	// there are no pointer devices, so we always include that capability.
	
	// TODO do we actually require a cursor?
	caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(seat, caps);
}

static void createLayerSurface(struct wl_listener *listener, void *data) {
	struct wlr_layer_surface_v1 *wlr_layer_surface = data;
	LayerSurface *layersurface;
	struct wlr_layer_surface_v1_state old_state;

	if (!wlr_layer_surface->output)
		wlr_layer_surface->output = selmon ? selmon->wlr_output : NULL;

	if (!wlr_layer_surface->output)
		wlr_layer_surface_v1_destroy(wlr_layer_surface);

	layersurface = ecalloc(1, sizeof(LayerSurface));
	layersurface->type = LayerShell;
	LISTEN(&wlr_layer_surface->surface->events.commit,
			&layersurface->surface_commit, commitlayersurfacenotify);
	LISTEN(&wlr_layer_surface->events.destroy, &layersurface->destroy,
			destroylayersurfacenotify);
	LISTEN(&wlr_layer_surface->events.map, &layersurface->map,
			maplayersurfacenotify);
	LISTEN(&wlr_layer_surface->events.unmap, &layersurface->unmap,
			unmaplayersurfacenotify);

	layersurface->layer_surface = wlr_layer_surface;
	layersurface->mon = wlr_layer_surface->output->data;
	wlr_layer_surface->data = layersurface;

	layersurface->scene_layer = wlr_scene_layer_surface_v1_create(
			layers[wlr_layer_surface->pending.layer], wlr_layer_surface);
	layersurface->scene = layersurface->scene_layer->tree;
	layersurface->popups = wlr_layer_surface->surface->data =
			wlr_scene_tree_create(layers[wlr_layer_surface->pending.layer]);

	layersurface->scene->node.data = layersurface;

	wl_list_insert(&layersurface->mon->layers[wlr_layer_surface->pending.layer],
		&layersurface->link);

	// Temporarily set the layer's current state to pending
	// so that we can easily arrange it
	old_state = wlr_layer_surface->current;
	wlr_layer_surface->current = wlr_layer_surface->pending;
	layersurface->mapped = 1;
	arrangelayers(layersurface->mon);
	wlr_layer_surface->current = old_state;
}

static void createMon(struct wl_listener *listener, void *data) {
	// This event is raised by the backend when a new output (aka a display or
	// monitor) becomes available.
	struct wlr_output *wlr_output = data;
	const MonitorRule *r;
	size_t i;
	Monitor *m = wlr_output->data = ecalloc(1, sizeof(*m));
	m->wlr_output = wlr_output;

	wlr_output_init_render(wlr_output, alloc, drw);

	// Initialize monitor state using configured rules
	for (i = 0; i < LENGTH(m->layers); i++)
		wl_list_init(&m->layers[i]);
	m->tagset[0] = m->tagset[1] = 1;
	for (r = monrules; r < END(monrules); r++) {
		if (!r->name || strstr(wlr_output->name, r->name)) {
			m->mfact = r->mfact;
			m->nmaster = r->nmaster;
			wlr_output_set_scale(wlr_output, r->scale);
			wlr_xcursor_manager_load(cursor_mgr, r->scale);
			m->lt[0] = m->lt[1] = r->lt;
			wlr_output_set_transform(wlr_output, r->rr);
			m->m.x = r->x;
			m->m.y = r->y;
			break;
		}
	}

	// The mode is a tuple of (width, height, refresh rate), and each
	// monitor supports only a specific set of modes. We just pick the
	// monitor's preferred mode; a more sophisticated compositor would let
	// the user configure it.
	wlr_output_set_mode(wlr_output, wlr_output_preferred_mode(wlr_output));

	// Set up event listeners
	LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
	LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);

	wlr_output_enable(wlr_output, 1);
	if (!wlr_output_commit(wlr_output))
		return;

	// Try to enable adaptive sync, note that not all monitors support it.
	// wlr_output_commit() will deactivate it in case it cannot be enabled.
	wlr_output_enable_adaptive_sync(wlr_output, 1);
	wlr_output_commit(wlr_output);

	wl_list_insert(&mons, &m->link);
	printstatus();

	// The xdg-protocol specifies:
	//
	// If the fullscreened surface is not opaque, the compositor must make
	// sure that other screen content not part of the same surface tree (made
	// up of subsurfaces, popups or similarly coupled surfaces) are not
	// visible below the fullscreened surface.
	//
	// updateMons() will resize and set correct position
	m->fullscreen_bg = wlr_scene_rect_create(layers[LyrFS], 0, 0, fullscreen_bg);
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

	// Adds this to the output layout in the order it was configured in.
	//
	// The output layout utility automatically adds a wl_output global to the
	// display, which Wayland clients can see to find out information about the
	// output (such as DPI, scale factor, manufacturer, etc).
	m->scene_output = wlr_scene_output_create(scene, wlr_output);
	if (m->m.x < 0 || m->m.y < 0)
		wlr_output_layout_add_auto(output_layout, wlr_output);
	else
		wlr_output_layout_add(output_layout, wlr_output, m->m.x, m->m.y);
}

static void virtualKeyboard(struct wl_listener *listener, void *data) {
	struct wlr_virtual_keyboard_v1 *keyboard = data;
	createkeyboard(&keyboard->keyboard);
}

static void createDecoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *dec = data;
	wlr_xdg_toplevel_decoration_v1_set_mode(dec, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void createNotify(struct wl_listener *listener, void *data) {
	// This event is raised when wlr_xdg_shell receives a new xdg surface from a
	// client, either a toplevel (application window) or popup,
	// or when wlr_layer_shell receives a new popup from a layer.
	// If you want to do something tricky with popups you should check if
	// its parent is wlr_xdg_shell or wlr_layer_shell
	struct wlr_xdg_surface *xdg_surface = data;
	Client *c = NULL;
	LayerSurface *l = NULL;

	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_box box;
		int type = toplevel_from_wlr_surface(xdg_surface->surface, &c, &l);
		if (!xdg_surface->popup->parent || type < 0)
			return;
		xdg_surface->surface->data = wlr_scene_xdg_surface_create(
				xdg_surface->popup->parent->data, xdg_surface);
		if ((l && !l->mon) || (c && !c->mon))
			return;
		box = type == LayerShell ? l->mon->m : c->mon->w;
		box.x -= (type == LayerShell ? l->geom.x : c->geom.x);
		box.y -= (type == LayerShell ? l->geom.y : c->geom.y);
		wlr_xdg_popup_unconstrain_from_box(xdg_surface->popup, &box);
		return;
	} else if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_NONE)
		return;

	/* Allocate a Client for this surface */
	c = xdg_surface->data = ecalloc(1, sizeof(*c));
	c->xdg_surface = xdg_surface;

	LISTEN(&xdg_surface->events.map, &c->map, mapnotify);
	LISTEN(&xdg_surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&xdg_surface->events.destroy, &c->destroy, destroynotify);
	LISTEN(&xdg_surface->toplevel->events.set_title, &c->set_title, updatetitle);
	LISTEN(&xdg_surface->toplevel->events.request_fullscreen, &c->fullscreen,
			fullscreennotify);
	LISTEN(&xdg_surface->toplevel->events.request_maximize, &c->maximize,
			maximizenotify);
}

static void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test) {
	// Called when a client such as wlr-randr requests a change in output
	// configuration. This is only one way that the layout can be changed,
	// so any Monitor information should be updated by updateMons() after an
	// output_layout.change event, not here.
	struct wlr_output_configuration_head_v1 *config_head;
	int ok = 1;

	wl_list_for_each(config_head, &config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		Monitor *m = wlr_output->data;

		wlr_output_enable(wlr_output, config_head->state.enabled);
		if (!config_head->state.enabled)
			goto apply_or_test;
		if (config_head->state.mode)
			wlr_output_set_mode(wlr_output, config_head->state.mode);
		else
			wlr_output_set_custom_mode(wlr_output,
					config_head->state.custom_mode.width,
					config_head->state.custom_mode.height,
					config_head->state.custom_mode.refresh);

		// Don't move monitors if position wouldn't change, this to avoid
		// wlroots marking the output as manually configured
		if (m->m.x != config_head->state.x || m->m.y != config_head->state.y)
			wlr_output_layout_move(output_layout, wlr_output,
					config_head->state.x, config_head->state.y);
		wlr_output_set_transform(wlr_output, config_head->state.transform);
		wlr_output_set_scale(wlr_output, config_head->state.scale);
		wlr_output_enable_adaptive_sync(wlr_output,
				config_head->state.adaptive_sync_enabled);

apply_or_test:
		if (test) {
			ok &= wlr_output_test(wlr_output);
			wlr_output_rollback(wlr_output);
		} else {
			ok &= wlr_output_commit(wlr_output);
		}
	}

	if (ok)
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);

	// TODO: use a wrapper function?
	updateMons(NULL, NULL);
}

static void outputMgrApply(struct wl_listener *listener, void *data) {
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 0);
}

static void outputMgrTest(struct wl_listener *listener, void *data) {
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 1);
}

static void urgent(struct wl_listener *listener, void *data) {
	// Called when a client sends a sends a signal
	// to the wayland server that it is urgent
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	Client *c = NULL;
	toplevel_from_wlr_surface(event->surface, &c, NULL);
	if (c && c != focustop(selmon)) {
		c->isurgent = 1;
		printstatus();
	}
}

static void setCursor(struct wl_listener *listener, void *data) {
	// This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	// If we're "grabbing" the cursor, don't use the client's image, we will
	// restore it after "grabbing" sending a leave event, followed by a enter
	// event, which will result in the client requesting set the cursor surface
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	cursor_image = NULL;
	// This can be sent by any client, so we check to make sure this one is
	// actually has pointer focus first. If so, we can tell the cursor to
	// use the provided surface as the cursor image. It will set the
	// hardware cursor on the output that it's currently on and continue to
	// do so as the cursor moves between outputs.
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_surface(cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
}

static void requestStartDrag(struct wl_listener *listener, void *data) {
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(seat, event->origin,
			event->serial))
		wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

static void setPSel(struct wl_listener *listener, void *data) {
	// This event is raised by the seat when a client wants to set the selection,
	// usually when the user copies something. wlroots allows compositors to
	// ignore such requests if they so choose, but in dwl we always honor
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

static void setSel(struct wl_listener *listener, void *data) {
	// This event is raised by the seat when a client wants to set the selection,
	// usually when the user copies something. wlroots allows compositors to
	// ignore such requests if they so choose, but in dwl we always honor
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(seat, event->source, event->serial);
}

static void lockSession(struct wl_listener *listener, void *data) {
	struct wlr_session_lock_v1 *session_lock = data;
	SessionLock *lock;
	wlr_scene_node_set_enabled(&locked_bg->node, 1);
	if (cur_lock) {
		wlr_session_lock_v1_destroy(session_lock);
		return;
	}
	lock = ecalloc(1, sizeof(*lock));
	focusclient(NULL, 0);

	lock->scene = wlr_scene_tree_create(layers[LyrBlock]);
	cur_lock = lock->lock = session_lock;
	locked = 1;
	session_lock->data = lock;

	LISTEN(&session_lock->events.new_surface, &lock->new_surface, createlocksurface);
	LISTEN(&session_lock->events.destroy, &lock->destroy, destroysessionlock);
	LISTEN(&session_lock->events.unlock, &lock->unlock, unlocksession);

	wlr_session_lock_v1_send_locked(session_lock);
}

static void destroySessionMgr(struct wl_listener *listener, void *data);
static void startDrag(struct wl_listener *listener, void *data);

static struct wl_listener cursor_axis              = {.notify = axisNotify};           // EG: scrolling
static struct wl_listener cursor_button            = {.notify = buttonPress};
static struct wl_listener cursor_frame             = {.notify = cursorFrame};          // marks 1 section of cursor motion
static struct wl_listener cursor_motion            = {.notify = motionRelative};       // when you move the cursor
static struct wl_listener cursor_motion_absolute   = {.notify = motionAbsolute};
static struct wl_listener drag_icon_destroy        = {.notify = destroyDragIcon};
static struct wl_listener idle_inhibitor_create    = {.notify = createIdleInhibitor};  // These programs run screenlockers at idle
static struct wl_listener idle_inhibitor_destroy   = {.notify = destroyIdleInhibitor};
static struct wl_listener layout_change            = {.notify = updateMons};
static struct wl_listener new_input                = {.notify = inputDevice};
static struct wl_listener new_layer_shell_surface  = {.notify = createLayerSurface};
static struct wl_listener new_output               = {.notify = createMon};
static struct wl_listener new_virtual_keyboard     = {.notify = virtualKeyboard};
static struct wl_listener new_xdg_decoration       = {.notify = createDecoration};
static struct wl_listener new_xdg_surface          = {.notify = createNotify};
static struct wl_listener output_mgr_apply         = {.notify = outputMgrApply};
static struct wl_listener output_mgr_test          = {.notify = outputMgrTest};
static struct wl_listener request_activate         = {.notify = urgent};
static struct wl_listener request_cursor           = {.notify = setCursor};
static struct wl_listener request_start_drag       = {.notify = requestStartDrag};
static struct wl_listener request_set_psel         = {.notify = setPSel};
static struct wl_listener request_set_sel          = {.notify = setSel};
static struct wl_listener session_lock_create_lock = {.notify = lockSession};
static struct wl_listener session_lock_mgr_destroy = {.notify = destroySessionMgr};
static struct wl_listener start_drag               = {.notify = startDrag};

// functions that use listener declarations

void createIdleInhibitor(struct wl_listener *listener, void *data) {
	struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
	wl_signal_add(&idle_inhibitor->events.destroy, &idle_inhibitor_destroy);

	checkidleinhibitor(NULL);
}

void destroySessionMgr(struct wl_listener *listener, void *data) {
	wl_list_remove(&session_lock_create_lock.link);
	wl_list_remove(&session_lock_mgr_destroy.link);
}

void startDrag(struct wl_listener *listener, void *data) {
	struct wlr_drag *drag = data;

	if (!drag->icon)
		return;

	drag->icon->data = wlr_scene_subsurface_tree_create(layers[LyrDragIcon], drag->icon->surface);
	motionnotify(0);
	wl_signal_add(&drag->icon->events.destroy, &drag_icon_destroy);
}

