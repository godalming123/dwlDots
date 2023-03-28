// See LICENSE file for copyright and license details
#include <libinput.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_input_inhibitor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "util.h"

// macros
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define VISIBLEON(C, M)         ((M) && (C)->mon == (M) && ((C)->tags & (M)->tagset[(M)->seltags]))
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define END(A)                  ((A) + LENGTH(A))
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
#define TAGMASK                 ((1 << LENGTH(tags)) - 1)
#define IDLE_NOTIFY_ACTIVITY    wlr_idle_notify_activity(idle, seat), wlr_idle_notifier_v1_notify_activity(idle_notifier, seat)

/* enums */
enum { CurNormal, CurPressed, CurMove, CurResize }; /* cursor */
enum { XDGShell, LayerShell }; /* client types */
enum { LyrBg, LyrBottom, LyrTop, LyrOverlay, LyrTile, LyrFloat, LyrFS, LyrDragIcon, LyrBlock, NUM_LAYERS }; /* scene layers */

#include "structs.h"

/* function declarations */
static void destroylayersurfacenotify(struct wl_listener *listener, void *data);
static void destroylocksurface(struct wl_listener *listener, void *data);
static void destroynotify(struct wl_listener *listener, void *data);
static void destroysessionlock(struct wl_listener *listener, void *data);
static Monitor *dirtomon(enum wlr_direction dir);
static void focusclient(Client *c, int lift);
static void focusmon(int mon);
static void focusstack(int relativeWindow);
static Client *focustop(Monitor *m);
static void fullscreennotify(struct wl_listener *listener, void *data);
static void incnmaster(int num);
static void keypress(struct wl_listener *listener, void *data);
static void keypressmod(struct wl_listener *listener, void *data);
static int keyrepeat(void *data);
static void killclient(void);
static void maplayersurfacenotify(struct wl_listener *listener, void *data);
static void mapnotify(struct wl_listener *listener, void *data);
static void maximizenotify(struct wl_listener *listener, void *data);
static void motionnotify(uint32_t time);
static void moveresize(unsigned int movementType);
static void pointerfocus(Client *c, struct wlr_surface *surface,
		double sx, double sy, uint32_t time);
static void printstatus(void);
static void quit(void);
static void rendermon(struct wl_listener *listener, void *data);
static void resize(Client *c, struct wlr_box geo, int interact);
static void setfloating(Client *c, int floating);
static void setfullscreen(Client *c, int fullscreen);
static void setlayout(const Layout *newLayout);
static void setmon(Client *c, Monitor *m, unsigned int newtags);
static void togglefloating(void);
static void togglefullscreen(void);
static void unlocksession(struct wl_listener *listener, void *data);
static void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
static void unmapnotify(struct wl_listener *listener, void *data);
static void updatetitle(struct wl_listener *listener, void *data);
static Monitor *xytomon(double x, double y);
static struct wlr_scene_node *xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny);

/* variables */
static bool ignoreNextKeyrelease = false;
static const char *cursor_image = "left_ptr";
static pid_t child_pid = -1;
static int locked;
static void *exclusive_focus;
static struct wl_display *dpy;
static struct wlr_backend *backend;
static struct wlr_scene *scene;
static struct wlr_scene_tree *layers[NUM_LAYERS];
static struct wlr_renderer *drw;
static struct wlr_allocator *alloc;
static struct wlr_compositor *compositor;

static struct wlr_xdg_shell *xdg_shell;
static struct wlr_xdg_activation_v1 *activation;
static struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
static struct wl_list clients; /* tiling order */
static struct wl_list fstack; /* focus order */
static struct wlr_idle *idle;
static struct wlr_idle_notifier_v1 *idle_notifier;
static struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
static struct wlr_input_inhibit_manager *input_inhibit_mgr;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_output_manager_v1 *output_mgr;
static struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;

static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *cursor_mgr;

static struct wlr_session_lock_manager_v1 *session_lock_mgr;
static struct wlr_scene_rect *locked_bg;
static struct wlr_session_lock_v1 *cur_lock;

static struct wlr_seat *seat;
static struct wl_list keyboards;
static unsigned int cursor_mode;
static Client *grabc;
static int grabcx, grabcy; /* client-relative */

static struct wlr_output_layout *output_layout;
static struct wlr_box sgeom;
static struct wl_list mons;
static Monitor *selmon;

static void chvt(unsigned int vt) {
	wlr_session_change_vt(wlr_backend_get_session(backend), vt);
}

void focusstack(int relativeWindow) {
	/* Focus the next or previous client (in tiling order) on selmon */
	Client *c, *sel = focustop(selmon);
	if (!sel || sel->isfullscreen)
		return;
	if (relativeWindow > 0) {
		wl_list_for_each(c, &sel->link, link) {
			if (&c->link == &clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon))
				break; /* found it */
		}
	} else {
		wl_list_for_each_reverse(c, &sel->link, link) {
			if (&c->link == &clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon))
				break; /* found it */
		}
	}
	/* If only one client is visible on selmon, then c == sel */
	focusclient(c, 1);
}

static void monocle(Monitor *m) {
	Client *c;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		resize(c, m->w, 0);
	}
	if ((c = focustop(m)))
		wlr_scene_node_raise_to_top(&c->scene->node);
}

static void tile(Monitor *m) {
	unsigned int i, n = 0, mw, my, ty;
	Client *c;

	wl_list_for_each(c, &clients, link)
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			n++;
	if (n == 0)
		return;

	if (n > m->nmaster)
		mw = m->nmaster ? m->w.width * m->mfact : 0;
	else
		mw = m->w.width;

	i = my = ty = 0;
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		if (i < m->nmaster) {
			resize(c, (struct wlr_box){.x = m->w.x, .y = m->w.y + my, .width = mw,
				.height = (m->w.height - my) / (MIN(n, m->nmaster) - i)}, 0);
			my += c->geom.height;
		} else {
			resize(c, (struct wlr_box){.x = m->w.x + mw, .y = m->w.y + ty,
				.width = m->w.width - mw, .height = (m->w.height - ty) / (n - i)}, 0);
			ty += c->geom.height;
		}
		i++;
	}
}

/* configuration, allows nested code to access above variables */
#include "config.h"

/* compile-time check if all tags fit into an unsigned int bit array. */
struct NumTags { char limitexceeded[LENGTH(tags) > 31 ? -1 : 1]; };

/* attempt to encapsulate suck into one file */
#include "client.h"

/* function implementations */
static void applybounds(Client *c, struct wlr_box *bbox) {
	if (!c->isfullscreen) {
		struct wlr_box min = {0}, max = {0};
		client_get_size_hints(c, &max, &min);
		/* try to set size hints */
		c->geom.width = MAX(min.width, c->geom.width);
		c->geom.height = MAX(min.height, c->geom.height);
		/* Some clients set their max size to INT_MAX, which does not violate the
		 * protocol but it's unnecesary, as they can set their max size to zero. */
		if (max.width > 0 && !(0 > INT_MAX - max.width)) /* Checks for overflow */
			c->geom.width = MIN(max.width, c->geom.width);
		if (max.height > 0 && !(0 > INT_MAX - max.height)) /* Checks for overflow */
			c->geom.height = MIN(max.height, c->geom.height);
	}

	if (c->geom.x >= bbox->x + bbox->width)
		c->geom.x = bbox->x + bbox->width - c->geom.width;
	if (c->geom.y >= bbox->y + bbox->height)
		c->geom.y = bbox->y + bbox->height - c->geom.height;
	if (c->geom.x + c->geom.width <= bbox->x)
		c->geom.x = bbox->x;
	if (c->geom.y + c->geom.height <= bbox->y)
		c->geom.y = bbox->y;
}

static void checkidleinhibitor(struct wlr_surface *exclude) {
	int inhibited = 0;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &idle_inhibit_mgr->inhibitors, link) {
		struct wlr_surface *surface = wlr_surface_get_root_surface(inhibitor->surface);
		struct wlr_scene_tree *tree = surface->data;
		if (exclude != surface && (bypass_surface_visibility || (!tree
			|| tree->node.enabled))) {
			inhibited = 1;
			break;
		}
	}

	wlr_idle_set_enabled(idle, NULL, !inhibited);
	wlr_idle_notifier_v1_set_inhibited(idle_notifier, inhibited);
}

static void arrange(Monitor *m) {
	Client *c;
	wl_list_for_each(c, &clients, link)
		if (c->mon == m)
			wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON(c, m));

	wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
			(c = focustop(m)) && c->isfullscreen);

	if (m && m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);
	motionnotify(0);
	checkidleinhibitor(NULL);
}

static void arrangelayer(Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive) {
	LayerSurface *layersurface;
	struct wlr_box full_area = m->m;

	wl_list_for_each(layersurface, list, link) {
		struct wlr_layer_surface_v1 *wlr_layer_surface = layersurface->layer_surface;
		struct wlr_layer_surface_v1_state *state = &wlr_layer_surface->current;

		if (exclusive != (state->exclusive_zone > 0))
			continue;

		wlr_scene_layer_surface_v1_configure(layersurface->scene_layer, &full_area, usable_area);
		wlr_scene_node_set_position(&layersurface->popups->node,
				layersurface->scene->node.x, layersurface->scene->node.y);
		layersurface->geom.x = layersurface->scene->node.x;
		layersurface->geom.y = layersurface->scene->node.y;
	}
}

static void arrangelayers(Monitor *m) {
	int i;
	struct wlr_box usable_area = m->m;
	uint32_t layers_above_shell[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	LayerSurface *layersurface;
	if (!m->wlr_output->enabled)
		return;

	/* Arrange exclusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 1);

	if (memcmp(&usable_area, &m->w, sizeof(struct wlr_box))) {
		m->w = usable_area;
		arrange(m);
	}

	/* Arrange non-exlusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 0);

	/* Find topmost keyboard interactive layer, if such a layer exists */
	for (i = 0; i < LENGTH(layers_above_shell); i++) {
		wl_list_for_each_reverse(layersurface,
				&m->layers[layers_above_shell[i]], link) {
			if (!locked && layersurface->layer_surface->current.keyboard_interactive
					&& layersurface->mapped) {
				/* Deactivate the focused client. */
				focusclient(NULL, 0);
				exclusive_focus = layersurface;
				client_notify_enter(layersurface->layer_surface->surface, wlr_seat_get_keyboard(seat));
				return;
			}
		}
	}
}

static void cleanupkeyboard(struct wl_listener *listener, void *data) {
	Keyboard *kb = wl_container_of(listener, kb, destroy);

	wl_event_source_remove(kb->key_repeat_source);
	wl_list_remove(&kb->link);
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	free(kb);
}

static void closemon(Monitor *m) {
	/* update selmon if needed and
	 * move closed monitor's clients to the focused one */
	Client *c;
	if (wl_list_empty(&mons)) {
		selmon = NULL;
	} else if (m == selmon) {
		int nmons = wl_list_length(&mons), i = 0;
		do /* don't switch to disabled mons */
			selmon = wl_container_of(mons.next, selmon, link);
		while (!selmon->wlr_output->enabled && i++ < nmons);
	}

	wl_list_for_each(c, &clients, link) {
		if (c->isfloating && c->geom.x > m->m.width)
			resize(c, (struct wlr_box){.x = c->geom.x - m->w.width, .y = c->geom.y,
				.width = c->geom.width, .height = c->geom.height}, 0);
		if (c->mon == m)
			setmon(c, selmon, c->tags);
	}
	focusclient(focustop(selmon), 1);
	printstatus();
}

static void cleanupmon(struct wl_listener *listener, void *data) {
	Monitor *m = wl_container_of(listener, m, destroy);
	LayerSurface *l, *tmp;
	int i;

	for (i = 0; i <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; i++)
		wl_list_for_each_safe(l, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(l->layer_surface);

	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->link);
	m->wlr_output->data = NULL;
	wlr_output_layout_remove(output_layout, m->wlr_output);
	wlr_scene_output_destroy(m->scene_output);
	wlr_scene_node_destroy(&m->fullscreen_bg->node);

	closemon(m);
	free(m);
}

static void commitlayersurfacenotify(struct wl_listener *listener, void *data) {
	LayerSurface *layersurface = wl_container_of(listener, layersurface, surface_commit);
	struct wlr_layer_surface_v1 *wlr_layer_surface = layersurface->layer_surface;
	struct wlr_output *wlr_output = wlr_layer_surface->output;

	/* For some reason this layersurface have no monitor, this can be because
	 * its monitor has just been destroyed */
	if (!wlr_output || !(layersurface->mon = wlr_output->data))
		return;

	if (layers[wlr_layer_surface->current.layer] != layersurface->scene->node.parent) {
		wlr_scene_node_reparent(&layersurface->scene->node,
				layers[wlr_layer_surface->current.layer]);
		wlr_scene_node_reparent(&layersurface->popups->node,
				layers[wlr_layer_surface->current.layer]);
		wl_list_remove(&layersurface->link);
		wl_list_insert(&layersurface->mon->layers[wlr_layer_surface->current.layer],
				&layersurface->link);
	}
	if (wlr_layer_surface->current.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP)
		wlr_scene_node_reparent(&layersurface->popups->node, layers[LyrTop]);

	if (wlr_layer_surface->current.committed == 0
			&& layersurface->mapped == wlr_layer_surface->mapped)
		return;
	layersurface->mapped = wlr_layer_surface->mapped;

	arrangelayers(layersurface->mon);
}

static void commitnotify(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, commit);
	struct wlr_box box = {0};
	client_get_geometry(c, &box);

	if (c->mon && !wlr_box_empty(&box) && (box.width != c->geom.width
			|| box.height != c->geom.height))
		c->isfloating ? resize(c, c->geom, 1) : arrange(c->mon);

	/* mark a pending resize as completed */
	if (c->resize && c->resize <= c->xdg_surface->current.configure_serial)
		c->resize = 0;
}

static void createkeyboard(struct wlr_keyboard *keyboard) {
	struct xkb_context *context;
	struct xkb_keymap *keymap;
	Keyboard *kb = keyboard->data = ecalloc(1, sizeof(*kb));
	kb->wlr_keyboard = keyboard;

	/* Prepare an XKB keymap and assign it to the keyboard. */
	context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	keymap = xkb_keymap_new_from_names(context, &xkb_rules,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(keyboard, repeat_rate, repeat_delay);

	/* Here we set up listeners for keyboard events. */
	LISTEN(&keyboard->events.modifiers, &kb->modifiers, keypressmod);
	LISTEN(&keyboard->events.key, &kb->key, keypress);
	LISTEN(&keyboard->base.events.destroy, &kb->destroy, cleanupkeyboard);

	wlr_seat_set_keyboard(seat, keyboard);

	kb->key_repeat_source = wl_event_loop_add_timer(
			wl_display_get_event_loop(dpy), keyrepeat, kb);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&keyboards, &kb->link);
}

static void createlocksurface(struct wl_listener *listener, void *data) {
	SessionLock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	Monitor *m = lock_surface->output->data;
	struct wlr_scene_tree *scene_tree = lock_surface->surface->data =
		wlr_scene_subsurface_tree_create(lock->scene, lock_surface->surface);
	m->lock_surface = lock_surface;

	wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
	wlr_session_lock_surface_v1_configure(lock_surface, m->m.width, m->m.height);

	LISTEN(&lock_surface->events.destroy, &m->destroy_lock_surface, destroylocksurface);

	if (m == selmon)
		client_notify_enter(lock_surface->surface, wlr_seat_get_keyboard(seat));
}

static void createpointer(struct wlr_pointer *pointer) {
	if (wlr_input_device_is_libinput(&pointer->base)) {
		struct libinput_device *libinput_device = (struct libinput_device*)
			wlr_libinput_get_device_handle(&pointer->base);

		if (libinput_device_config_tap_get_finger_count(libinput_device)) {
			libinput_device_config_tap_set_enabled(libinput_device, tap_to_click);
			libinput_device_config_tap_set_drag_enabled(libinput_device, tap_and_drag);
			libinput_device_config_tap_set_drag_lock_enabled(libinput_device, drag_lock);
			libinput_device_config_tap_set_button_map(libinput_device, button_map);
		}

		if (libinput_device_config_scroll_has_natural_scroll(libinput_device))
			libinput_device_config_scroll_set_natural_scroll_enabled(libinput_device, natural_scrolling);

		if (libinput_device_config_dwt_is_available(libinput_device))
			libinput_device_config_dwt_set_enabled(libinput_device, disable_while_typing);

		if (libinput_device_config_left_handed_is_available(libinput_device))
			libinput_device_config_left_handed_set(libinput_device, left_handed);

		if (libinput_device_config_middle_emulation_is_available(libinput_device))
			libinput_device_config_middle_emulation_set_enabled(libinput_device, middle_button_emulation);

		if (libinput_device_config_scroll_get_methods(libinput_device) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method (libinput_device, scroll_method);
		
		if (libinput_device_config_click_get_methods(libinput_device) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
			libinput_device_config_click_set_method (libinput_device, click_method);

		if (libinput_device_config_send_events_get_modes(libinput_device))
			libinput_device_config_send_events_set_mode(libinput_device, send_events_mode);

		if (libinput_device_config_accel_is_available(libinput_device)) {
			libinput_device_config_accel_set_profile(libinput_device, accel_profile);
			libinput_device_config_accel_set_speed(libinput_device, accel_speed);
		}
	}

	wlr_cursor_attach_input_device(cursor, &pointer->base);
}

void destroylayersurfacenotify(struct wl_listener *listener, void *data) {
	LayerSurface *layersurface = wl_container_of(listener, layersurface, destroy);

	wl_list_remove(&layersurface->link);
	wl_list_remove(&layersurface->destroy.link);
	wl_list_remove(&layersurface->map.link);
	wl_list_remove(&layersurface->unmap.link);
	wl_list_remove(&layersurface->surface_commit.link);
	wlr_scene_node_destroy(&layersurface->scene->node);
	free(layersurface);
}

void destroylock(SessionLock *lock, int unlock) {
	wlr_seat_keyboard_notify_clear_focus(seat);
	if ((locked = !unlock))
		goto destroy;

	wlr_scene_node_set_enabled(&locked_bg->node, 0);

	focusclient(focustop(selmon), 0);
	motionnotify(0);

destroy:
	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);

	wlr_scene_node_destroy(&lock->scene->node);
	cur_lock = NULL;
	free(lock);
}

void destroylocksurface(struct wl_listener *listener, void *data) {
	Monitor *m = wl_container_of(listener, m, destroy_lock_surface);
	struct wlr_session_lock_surface_v1 *surface, *lock_surface = m->lock_surface;

	m->lock_surface = NULL;
	wl_list_remove(&m->destroy_lock_surface.link);

	if (lock_surface->surface == seat->keyboard_state.focused_surface) {
		if (locked && cur_lock && !wl_list_empty(&cur_lock->surfaces)) {
			surface = wl_container_of(cur_lock->surfaces.next, surface, link);
			client_notify_enter(surface->surface, wlr_seat_get_keyboard(seat));
		} else if (!locked) {
			focusclient(focustop(selmon), 1);
		} else {
			wlr_seat_keyboard_clear_focus(seat);
		}
	}
}

void destroynotify(struct wl_listener *listener, void *data) {
	/* Called when the surface is destroyed and should never be shown again. */
	Client *c = wl_container_of(listener, c, destroy);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->fullscreen.link);
	free(c);
}

void destroysessionlock(struct wl_listener *listener, void *data) {
	SessionLock *lock = wl_container_of(listener, lock, destroy);
	destroylock(lock, 0);
}

Monitor * dirtomon(enum wlr_direction dir) {
	struct wlr_output *next;
	if (!wlr_output_layout_get(output_layout, selmon->wlr_output))
		return selmon;
	if ((next = wlr_output_layout_adjacent_output(output_layout,
			dir, selmon->wlr_output, selmon->m.x, selmon->m.y)))
		return next->data;
	if ((next = wlr_output_layout_farthest_output(output_layout,
			dir ^ (WLR_DIRECTION_LEFT|WLR_DIRECTION_RIGHT),
			selmon->wlr_output, selmon->m.x, selmon->m.y)))
		return next->data;
	return selmon;
}

void focusclient(Client *c, int lift) {
	struct wlr_surface *old = seat->keyboard_state.focused_surface;

	if (locked)
		return;

	/* Raise client in stacking order if requested */
	if (c && lift)
		wlr_scene_node_raise_to_top(&c->scene->node);

	if (c && c->xdg_surface->surface == old)
		return;

	/* Put the new client atop the focus stack and select its monitor */
	if (c) {
		wl_list_remove(&c->flink);
		wl_list_insert(&fstack, &c->flink);
		selmon = c->mon;
		c->isurgent = 0;
		client_restack_surface(c);
	}

	/* Deactivate old client if focus is changing */
	if (old && (!c || c->xdg_surface->surface != old)) {
		/* If an overlay is focused, don't focus or activate the client,
		 * but only update its position in fstack to render its border with focuscolor
		 * and focus it after the overlay is closed. */
		Client *w = NULL;
		LayerSurface *l = NULL;
		int type = toplevel_from_wlr_surface(old, &w, &l);
		// Do not do anything more if the client if a layer surface on the top layer
		if (type == LayerShell && l->scene->node.enabled && l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
			return;
		/* Don't deactivate old client if the new one wants focus, as this causes issues with winecfg
		 * and probably other clients */
		else if (w)
			client_activate_surface(old, 0);
	}
	printstatus();

	if (!c) {
		/* With no client, all we have left is to clear focus */
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}

	/* Change cursor surface */
	motionnotify(0);

	/* Have a client, so focus its top-level wlr_surface */
	client_notify_enter(c->xdg_surface->surface, wlr_seat_get_keyboard(seat));

	/* Activate the new client */
	client_activate_surface(c->xdg_surface->surface, 1);
}

void focusmon(int mon) {
	int i = 0, nmons = wl_list_length(&mons);
	if (nmons)
		do /* don't switch to disabled mons */
			selmon = dirtomon(mon);
		while (!selmon->wlr_output->enabled && i++ < nmons);
	focusclient(focustop(selmon), 1);
}

// returns the client that is topmost for a given monitor
Client * focustop(Monitor *m) {
	Client *c;
	wl_list_for_each(c, &fstack, flink)
		if (VISIBLEON(c, m))
			return c;
	return NULL;
}

void fullscreennotify(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, fullscreen);
	setfullscreen(c, client_wants_fullscreen(c));
}

void incnmaster(int num) {
	if (!num || !selmon)
		return;
	selmon->nmaster = MAX(selmon->nmaster + num, 0);
	arrange(selmon);
}

// This event is raised when a key is pressed or released.
void keypress(struct wl_listener *listener, void *data) {
	Keyboard *kb = wl_container_of(listener, kb, key);
	struct wlr_keyboard_key_event *event = data;

	// Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	// Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);
	// get a list of mods pressed
	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
	
	// notify of idle activity
	IDLE_NOTIFY_ACTIVITY;

	/* On _press_ if there is no active screen locker,
	 * attempt to process a compositor keybinding. */
	int handled = 0;
	if (!locked && !input_inhibit_mgr->active_inhibitor) {
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			// if the alt modifier is being held and you press a key that is not alt
			if ((mods&WLR_MODIFIER_ALT) == WLR_MODIFIER_ALT && keycode != 64)
				// then we need to ignore the next keyrelease
				ignoreNextKeyrelease = true;
			
			// now handle the keypress
			handled = handleKeypress(mods, syms[nsyms-1]);
		}
		else if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
			// if the alt modifier is being held and you release it
			if (mods == WLR_MODIFIER_ALT && syms[0] == 65513) {
				if (!ignoreNextKeyrelease) {
					if (fork() == 0) {
						system(menucmd);
						exit(EXIT_SUCCESS);
					}
				}
				handled = 1;
				ignoreNextKeyrelease = false;
			}
		}
	}
	
	if (handled && kb->wlr_keyboard->repeat_info.delay > 0) {
		kb->mods = mods;
		kb->keysyms = syms;
		kb->nsyms = nsyms;
		wl_event_source_timer_update(kb->key_repeat_source, kb->wlr_keyboard->repeat_info.delay);
	} else {
		kb->nsyms = 0;
		wl_event_source_timer_update(kb->key_repeat_source, 0);
	}

	if (!handled) {
		/* Pass unhandled keycodes along to the client. */
		wlr_seat_set_keyboard(seat, kb->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

void keypressmod(struct wl_listener *listener, void *data) {
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	Keyboard *kb = wl_container_of(listener, kb, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(seat, kb->wlr_keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(seat,
		&kb->wlr_keyboard->modifiers);
}

int keyrepeat(void *data) {
	Keyboard *kb = data;
	int i;
	if (kb->nsyms && kb->wlr_keyboard->repeat_info.rate > 0) {
		wl_event_source_timer_update(kb->key_repeat_source,
				1000 / kb->wlr_keyboard->repeat_info.rate);

		for (i = 0; i < kb->nsyms; i++)
			handleKeypress(kb->mods, kb->keysyms[i]);
	}

	return 0;
}

void killclient(void) {
	Client *sel = focustop(selmon);
	if (sel)
		client_send_close(sel);
}

void maplayersurfacenotify(struct wl_listener *listener, void *data) {
	LayerSurface *l = wl_container_of(listener, l, map);
	wlr_surface_send_enter(l->layer_surface->surface, l->mon->wlr_output);
	motionnotify(0);
}

void mapnotify(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	Client *p, *w, *c = wl_container_of(listener, c, map);
	Monitor *m;

	/* Create scene tree for this client and its border */
	c->scene = wlr_scene_tree_create(layers[LyrTile]);
	wlr_scene_node_set_enabled(&c->scene->node, c->type != XDGShell);
	c->scene_surface = c->type == XDGShell
			? wlr_scene_xdg_surface_create(c->scene, c->xdg_surface)
			: wlr_scene_subsurface_tree_create(c->scene, c->xdg_surface->surface);
	if (c->xdg_surface->surface) {
		c->xdg_surface->surface->data = c->scene;
		/* Ideally we should do this in createnotify{,x11} but at that moment
		* wlr_xwayland_surface doesn't have wlr_surface yet. */
		LISTEN(&c->xdg_surface->surface->events.commit, &c->commit, commitnotify);
	}
	c->scene->node.data = c->scene_surface->node.data = c;

	/* Initialize client geometry with room for border */
	client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	client_get_geometry(c, &c->geom);

	/* Insert this client into client lists. */
	wl_list_insert(&clients, &c->link);
	wl_list_insert(&fstack, &c->flink);

	/* Set initial monitor, tags, floating status, and focus:
	 * we always consider floating, clients that have parent and thus
	 * we set the same tags and monitor than its parent, if not
	 * try to apply rules for them */
	 /* TODO: https://github.com/djpohly/dwl/pull/334#issuecomment-1330166324 */
	if (c->type == XDGShell && (p = client_get_parent(c))) {
		c->isfloating = 1;
		wlr_scene_node_reparent(&c->scene->node, layers[LyrFloat]);
		setmon(c, p->mon, p->tags);
	} else {
		c->isfloating = client_is_float_type(c);
		setmon(c, selmon, 0);
		wlr_scene_node_reparent(&c->scene->node, layers[c->isfloating ? LyrFloat : LyrTile]);
	}
	printstatus();

unset_fullscreen:
	m = c->mon ? c->mon : xytomon(c->geom.x, c->geom.y);
	wl_list_for_each(w, &clients, link)
		if (w != c && w->isfullscreen && m == w->mon && (w->tags & c->tags))
			setfullscreen(w, 0);
}

void maximizenotify(struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on
	 * client-side decorations. dwl doesn't support maximization, but
	 * to conform to xdg-shell protocol we still must send a configure.
	 * wlr_xdg_surface_schedule_configure() is used to send an empty reply. */
	Client *c = wl_container_of(listener, c, maximize);
	wlr_xdg_surface_schedule_configure(c->xdg_surface);
}

void motionnotify(uint32_t time) {
	double sx = 0, sy = 0;
	Client *c = NULL, *w = NULL;
	LayerSurface *l = NULL;
	int type;
	struct wlr_surface *surface = NULL;
	struct wlr_drag_icon *icon;

	/* time is 0 in internal calls meant to restore pointer focus. */
	if (time) {
		IDLE_NOTIFY_ACTIVITY;

		/* Update selmon (even while dragging a window) */
		if (sloppyfocus)
			selmon = xytomon(cursor->x, cursor->y);
	}

	/* Update drag icon's position if any */
	if (seat->drag && (icon = seat->drag->icon))
		wlr_scene_node_set_position(icon->data, cursor->x + icon->surface->sx,
				cursor->y + icon->surface->sy);
	/* If we are currently grabbing the mouse, handle and return */
	if (cursor_mode == CurMove) {
		/* Move the grabbed client to the new position. */
		resize(grabc, (struct wlr_box){.x = cursor->x - grabcx, .y = cursor->y - grabcy,
			.width = grabc->geom.width, .height = grabc->geom.height}, 1);
		return;
	} else if (cursor_mode == CurResize) {
		resize(grabc, (struct wlr_box){.x = grabc->geom.x, .y = grabc->geom.y,
			.width = cursor->x - grabc->geom.x, .height = cursor->y - grabc->geom.y}, 1);
		return;
	}

	/* Find the client under the pointer and send the event along. */
	xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);

	if (cursor_mode == CurPressed && !seat->drag) {
		if ((type = toplevel_from_wlr_surface(
				 seat->pointer_state.focused_surface, &w, &l)) >= 0) {
			c = w;
			surface = seat->pointer_state.focused_surface;
			sx = cursor->x - (type == LayerShell ? l->geom.x : w->geom.x);
			sy = cursor->y - (type == LayerShell ? l->geom.y : w->geom.y);
		}
	}

	/* If there's no client surface under the cursor, set the cursor image to a
	 * default. This is what makes the cursor image appear when you move it
	 * off of a client or over its border. */
	if (!surface && !seat->drag && (!cursor_image || strcmp(cursor_image, "left_ptr")))
		wlr_xcursor_manager_set_cursor_image(cursor_mgr, (cursor_image = "left_ptr"), cursor);

	pointerfocus(c, surface, sx, sy, time);
}

void moveresize(unsigned int movementType) {
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
	if (!grabc || grabc->isfullscreen)
		return;

	/* Float the window and tell motionnotify to grab it */
	setfloating(grabc, 1);
	switch (cursor_mode = movementType) {
	case CurMove:
		grabcx = cursor->x - grabc->geom.x;
		grabcy = cursor->y - grabc->geom.y;
		wlr_xcursor_manager_set_cursor_image(cursor_mgr, (cursor_image = "fleur"), cursor);
		break;
	case CurResize:
		/* Doesn't work for X11 output - the next absolute motion event
		 * returns the cursor to where it started */
		wlr_cursor_warp_closest(cursor, NULL,
				grabc->geom.x + grabc->geom.width,
				grabc->geom.y + grabc->geom.height);
		wlr_xcursor_manager_set_cursor_image(cursor_mgr,
				(cursor_image = "bottom_right_corner"), cursor);
		break;
	}
}

void pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,	uint32_t time) {
	struct timespec now;
	int internal_call = !time;

	if (sloppyfocus && !internal_call && c)
		focusclient(c, 0);

	/* If surface is NULL, clear pointer focus */
	if (!surface) {
		wlr_seat_pointer_notify_clear_focus(seat);
		return;
	}

	if (internal_call) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
	}

	/* Let the client know that the mouse cursor has entered one
	 * of its surfaces, and make keyboard focus follow if desired.
	 * wlroots makes this a no-op if surface is already focused */
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

void printstatus(void) {
	Monitor *m = NULL;
	Client *c;
	unsigned int occ, urg, sel;
	const char *appid, *title;

	wl_list_for_each(m, &mons, link) {
		occ = urg = 0;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m)
				continue;
			occ |= c->tags;
			if (c->isurgent)
				urg |= c->tags;
		}
		if ((c = focustop(m))) {
			title = client_get_title(c);
			appid = client_get_appid(c);
			printf("%s title %s\n", m->wlr_output->name, title);
			printf("%s appid %s\n", m->wlr_output->name, appid);
			printf("%s fullscreen %u\n", m->wlr_output->name, c->isfullscreen);
			printf("%s floating %u\n", m->wlr_output->name, c->isfloating);
			sel = c->tags;
		} else {
			printf("%s title \n", m->wlr_output->name);
			printf("%s appid \n", m->wlr_output->name);
			printf("%s fullscreen \n", m->wlr_output->name);
			printf("%s floating \n", m->wlr_output->name);
			sel = 0;
		}

		printf("%s selmon %u\n", m->wlr_output->name, m == selmon);
		printf("%s tags %u %u %u %u\n", m->wlr_output->name, occ, m->tagset[m->seltags],
				sel, urg);
		printf("%s layout %s\n", m->wlr_output->name, m->lt[m->sellt]->symbol);
	}
	fflush(stdout);
}

void quit(void) {
	wl_display_terminate(dpy);
}

void rendermon(struct wl_listener *listener, void *data) {
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	Monitor *m = wl_container_of(listener, m, frame);
	Client *c;
	struct timespec now;

	/* Render if no XDG clients have an outstanding resize and are visible on
	 * this monitor. */
	wl_list_for_each(c, &clients, link)
		if (c->resize && !c->isfloating && client_is_rendered_on_mon(c, m) && !client_is_stopped(c))
			goto skip;
	if (!wlr_scene_output_commit(m->scene_output))
		return;
skip:
	/* Let clients know a frame has been rendered */
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);
}

void resize(Client *c, struct wlr_box geo, int interact) {
	struct wlr_box *bbox = interact ? &sgeom : &c->mon->w;
	client_set_bounds(c, geo.width, geo.height);
	c->geom = geo;
	applybounds(c, bbox);

	/* Update scene-graph */
	wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
	wlr_scene_node_set_position(&c->scene_surface->node, 0, 0);

	/* this is a no-op if size hasn't changed */
	c->resize = client_set_size(c, c->geom.width,
			c->geom.height);
}

void setfloating(Client *c, int floating) {
	c->isfloating = floating;
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfloating ? LyrFloat : LyrTile]);
	arrange(c->mon);
	printstatus();
}

void setfullscreen(Client *c, int fullscreen) {
	c->isfullscreen = fullscreen;
	if (!c->mon)
		return;
	client_set_fullscreen(c, fullscreen);
	wlr_scene_node_reparent(&c->scene->node, layers[fullscreen
			? LyrFS : c->isfloating ? LyrFloat : LyrTile]);

	if (fullscreen) {
		c->prev = c->geom;
		resize(c, c->mon->m, 0);
	} else {
		/* restore previous size instead of arrange for floating windows since
		 * client positions are set by the user and cannot be recalculated */
		resize(c, c->prev, 0);
	}
	arrange(c->mon);
	printstatus();
}

void setlayout(const Layout *newLayout) {
	if (!selmon)
		return;
	if (!newLayout || newLayout != selmon->lt[selmon->sellt])
		selmon->sellt ^= 1;
	if (newLayout)
		selmon->lt[selmon->sellt] = newLayout;
	/* TODO change layout symbol? */
	arrange(selmon);
	printstatus();
}

void setmon(Client *c, Monitor *m, unsigned int newtags) {
	Monitor *oldmon = c->mon;

	if (oldmon == m)
		return;
	c->mon = m;
	c->prev = c->geom;

	/* TODO leave/enter is not optimal but works */
	if (oldmon) {
		wlr_surface_send_leave(c->xdg_surface->surface, oldmon->wlr_output);
		arrange(oldmon);
	}
	if (m) {
		/* Make sure window actually overlaps with the monitor */
		resize(c, c->geom, 0);
		wlr_surface_send_enter(c->xdg_surface->surface, m->wlr_output);
		c->tags = newtags ? newtags : m->tagset[m->seltags]; /* assign tags of target monitor */
		setfullscreen(c, c->isfullscreen); /* This will call arrange(c->mon) */
	}
	focusclient(focustop(selmon), 1);
}

void tag(unsigned int newTag) {
	Client *sel = focustop(selmon);
	if (sel && newTag & TAGMASK) {
		sel->tags = newTag & TAGMASK;
		focusclient(focustop(selmon), 1);
		arrange(selmon);
	}
	printstatus();
}

static void tagmon(int tagToSwitch) {
	Client *sel = focustop(selmon);
	if (sel)
		setmon(sel, dirtomon(tagToSwitch), 0);
}

void togglefloating(void) {
	Client *sel = focustop(selmon);
	/* return if fullscreen */
	if (sel && !sel->isfullscreen)
		setfloating(sel, !sel->isfloating);
}

void togglefullscreen(void) {
	Client *sel = focustop(selmon);
	if (sel)
		setfullscreen(sel, !sel->isfullscreen);
}

static void toggletag(unsigned int tagToBeToggled) {
	unsigned int newtags;
	Client *sel = focustop(selmon);
	if (!sel)
		return;
	newtags = sel->tags ^ (tagToBeToggled & TAGMASK);
	if (newtags) {
		sel->tags = newtags;
		focusclient(focustop(selmon), 1);
		arrange(selmon);
	}
	printstatus();
}

static void toggleview(unsigned int tagToBeToggled) {
	unsigned int newtagset = selmon ? selmon->tagset[selmon->seltags] ^ (tagToBeToggled & TAGMASK) : 0;

	if (newtagset) {
		selmon->tagset[selmon->seltags] = newtagset;
		focusclient(focustop(selmon), 1);
		arrange(selmon);
	}
	printstatus();
}

void unlocksession(struct wl_listener *listener, void *data) {
	SessionLock *lock = wl_container_of(listener, lock, unlock);
	destroylock(lock, 1);
}

void unmaplayersurfacenotify(struct wl_listener *listener, void *data) {
	LayerSurface *layersurface = wl_container_of(listener, layersurface, unmap);

	layersurface->mapped = 0;
	wlr_scene_node_set_enabled(&layersurface->scene->node, 0);
	if (layersurface == exclusive_focus)
		exclusive_focus = NULL;
	if (layersurface->layer_surface->output
			&& (layersurface->mon = layersurface->layer_surface->output->data))
		arrangelayers(layersurface->mon);
	if (layersurface->layer_surface->surface ==
			seat->keyboard_state.focused_surface)
		focusclient(focustop(selmon), 1);
	motionnotify(0);
}

void unmapnotify(struct wl_listener *listener, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	Client *c = wl_container_of(listener, c, unmap);
	if (c == grabc) {
		cursor_mode = CurNormal;
		grabc = NULL;
	}

	wl_list_remove(&c->link);
	setmon(c, NULL, 0);
	wl_list_remove(&c->flink);

	wl_list_remove(&c->commit.link);
	wlr_scene_node_destroy(&c->scene->node);
	printstatus();
	motionnotify(0);
}

void updatetitle(struct wl_listener *listener, void *data) {
	Client *c = wl_container_of(listener, c, set_title);
	if (c == focustop(c->mon))
		printstatus();
}

static void view(unsigned int newVeiw) {
	if (!selmon || (newVeiw & TAGMASK) == selmon->tagset[selmon->seltags])
		return;
	selmon->seltags ^= 1; /* toggle sel tagset */
	if (newVeiw & TAGMASK)
		selmon->tagset[selmon->seltags] = newVeiw & TAGMASK;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

Monitor * xytomon(double x, double y) {
	struct wlr_output *o = wlr_output_layout_output_at(output_layout, x, y);
	return o ? o->data : NULL;
}

struct wlr_scene_node * xytonode(double x, double y, struct wlr_surface **psurface, Client **pc, LayerSurface **pl, double *nx, double *ny) {
	struct wlr_scene_node *node, *pnode;
	struct wlr_surface *surface = NULL;
	Client *c = NULL;
	LayerSurface *l = NULL;
	const int *layer;
	int focus_order[] = { LyrBlock, LyrOverlay, LyrTop, LyrFS, LyrFloat, LyrTile, LyrBottom, LyrBg };

	for (layer = focus_order; layer < END(focus_order); layer++) {
		if ((node = wlr_scene_node_at(&layers[*layer]->node, x, y, nx, ny))) {
			if (node->type == WLR_SCENE_NODE_BUFFER)
				surface = wlr_scene_surface_from_buffer(
						wlr_scene_buffer_from_node(node))->surface;
			/* Walk the tree to find a node that knows the client */
			for (pnode = node; pnode && !c; pnode = &pnode->parent->node)
				c = pnode->data;
			if (c && c->type == LayerShell) {
				c = NULL;
				l = pnode->data;
			}
		}
		if (surface)
			break;
	}

	if (psurface) *psurface = surface;
	if (pc) *pc = c;
	if (pl) *pl = l;
	return node;
}

//////////////////////////////
// SOME DEFINITIONS FOR DWL //
//////////////////////////////

#include "listeners.h"

// SIGNALS //
static void quitsignal(int signo) { quit();}

//////////////////////////
// MAIN CODE TO RUN DWL //
//////////////////////////

static void setup(void) {
	struct sigaction sa_term = {.sa_flags = SA_RESTART, .sa_handler = quitsignal};
	struct sigaction sa_sigchld = {
		.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART,
		.sa_handler = SIG_IGN,
	};
	sigemptyset(&sa_term.sa_mask);
	sigemptyset(&sa_sigchld.sa_mask);

	// The Wayland display is managed by libwayland. It handles accepting
	// clients from the Unix socket, manging Wayland globals, and so on.
	dpy = wl_display_create();

	/* Set up signal handlers */
	sigaction(SIGCHLD, &sa_sigchld, NULL);
	sigaction(SIGINT, &sa_term, NULL);
	sigaction(SIGTERM, &sa_term, NULL);

	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. The NULL argument here optionally allows you
	 * to pass in a custom renderer if wlr_renderer doesn't meet your needs. The
	 * backend uses the renderer, for example, to fall back to software cursors
	 * if the backend does not support hardware cursors (some older GPUs
	 * don't). */
	if (!(backend = wlr_backend_autocreate(dpy)))
		die("Couldn't create backend");

	/* Initialize the scene graph used to lay out windows */
	scene = wlr_scene_create();
	layers[LyrBg]       = wlr_scene_tree_create(&scene->tree);
	layers[LyrBottom]   = wlr_scene_tree_create(&scene->tree);
	layers[LyrTile]     = wlr_scene_tree_create(&scene->tree);
	layers[LyrFloat]    = wlr_scene_tree_create(&scene->tree);
	layers[LyrFS]       = wlr_scene_tree_create(&scene->tree);
	layers[LyrTop]      = wlr_scene_tree_create(&scene->tree);
	layers[LyrOverlay]  = wlr_scene_tree_create(&scene->tree);
	layers[LyrDragIcon] = wlr_scene_tree_create(&scene->tree);
	layers[LyrBlock]    = wlr_scene_tree_create(&scene->tree);

	/* Create a renderer with the default implementation */
	if (!(drw = wlr_renderer_autocreate(backend)))
		die("Couldn't create renderer");
	wlr_renderer_init_wl_display(drw, dpy);

	/* Create a default allocator */
	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't create allocator");

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the setsel() function. */
	compositor = wlr_compositor_create(dpy, drw);
	wlr_export_dmabuf_manager_v1_create(dpy);
	wlr_screencopy_manager_v1_create(dpy);
	wlr_data_control_manager_v1_create(dpy);
	wlr_data_device_manager_create(dpy);
	wlr_gamma_control_manager_v1_create(dpy);
	wlr_primary_selection_v1_device_manager_create(dpy);
	wlr_viewporter_create(dpy);
	wlr_single_pixel_buffer_manager_v1_create(dpy);
	wlr_subcompositor_create(dpy);

	/* Initializes the interface used to implement urgency hints */
	activation = wlr_xdg_activation_v1_create(dpy);
	wl_signal_add(&activation->events.request_activate, &request_activate);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	output_layout = wlr_output_layout_create();
	wl_signal_add(&output_layout->events.change, &layout_change);
	wlr_xdg_output_manager_v1_create(dpy, output_layout);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&mons);
	wl_signal_add(&backend->events.new_output, &new_output);

	/* Set up our client lists and the xdg-shell. The xdg-shell is a
	 * Wayland protocol which is used for application windows. For more
	 * detail on shells, refer to the article:
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&clients);
	wl_list_init(&fstack);

	idle = wlr_idle_create(dpy);
	idle_notifier = wlr_idle_notifier_v1_create(dpy);

	idle_inhibit_mgr = wlr_idle_inhibit_v1_create(dpy);
	wl_signal_add(&idle_inhibit_mgr->events.new_inhibitor, &idle_inhibitor_create);

	layer_shell = wlr_layer_shell_v1_create(dpy);
	wl_signal_add(&layer_shell->events.new_surface, &new_layer_shell_surface);

	xdg_shell = wlr_xdg_shell_create(dpy, 4);
	wl_signal_add(&xdg_shell->events.new_surface, &new_xdg_surface);

	input_inhibit_mgr = wlr_input_inhibit_manager_create(dpy);
	session_lock_mgr = wlr_session_lock_manager_v1_create(dpy);
	wl_signal_add(&session_lock_mgr->events.new_lock, &session_lock_create_lock);
	wl_signal_add(&session_lock_mgr->events.destroy, &session_lock_mgr_destroy);
	locked_bg = wlr_scene_rect_create(layers[LyrBlock], sgeom.width, sgeom.height,
			(float [4]){0.1, 0.1, 0.1, 1.0});
	wlr_scene_node_set_enabled(&locked_bg->node, 0);

	/* Use decoration protocols to negotiate server-side decorations */
	wlr_server_decoration_manager_set_default_mode(
		wlr_server_decoration_manager_create(dpy),
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(dpy);
	wl_signal_add(&xdg_decoration_mgr->events.new_toplevel_decoration, &new_xdg_decoration);

	// Creates a cursor, which is a wlroots utility
	// for tracking the cursor image shown on screen.
	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). Scaled cursors will be loaded with each output. */
	cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in
	 * Drew Devault's input handling blog post:
	 *
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * Note: more comments are sprinkled throughout the notify functions above.
	 */
	wl_signal_add(&cursor->events.motion,          &cursor_motion);
	wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
	wl_signal_add(&cursor->events.button,          &cursor_button);
	wl_signal_add(&cursor->events.axis,            &cursor_axis);
	wl_signal_add(&cursor->events.frame,           &cursor_frame);

	/* Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend. */
	wl_list_init(&keyboards);
	wl_signal_add(&backend->events.new_input, &new_input);
	virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(dpy);
	wl_signal_add(&virtual_keyboard_mgr->events.new_virtual_keyboard,
			&new_virtual_keyboard);
	seat = wlr_seat_create(dpy, "seat0");
	wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
	wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
	wl_signal_add(&seat->events.request_set_primary_selection, &request_set_psel);
	wl_signal_add(&seat->events.request_start_drag, &request_start_drag);
	wl_signal_add(&seat->events.start_drag, &start_drag);

	output_mgr = wlr_output_manager_v1_create(dpy);
	wl_signal_add(&output_mgr->events.apply, &output_mgr_apply);
	wl_signal_add(&output_mgr->events.test, &output_mgr_test);

	wlr_scene_set_presentation(scene, wlr_presentation_create(dpy, backend));
}


static void rundwl(void) {
	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(dpy);
	struct sigaction sa = {.sa_flags = SA_RESTART, .sa_handler = SIG_IGN};
	sigemptyset(&sa.sa_mask);
	if (!socket)
		die("startup: display_add_socket_auto");
	setenv("WAYLAND_DISPLAY", socket, 1);

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(backend))
		die("startup: backend_start");

	/* Now that the socket exists and the backend is started, run the startup command */
	int piperw[2];
	if (pipe(piperw) < 0)
		die("startup: pipe:");
	if ((child_pid = fork()) < 0)
		die("startup: fork:");
	if (child_pid == 0) {
		dup2(piperw[0], STDIN_FILENO);
		close(piperw[0]);
		close(piperw[1]);
		execl("/bin/sh", "/bin/sh", "-c", "somebar", NULL);
		die("startup: execl:");
	}
	dup2(piperw[1], STDOUT_FILENO);
	close(piperw[1]);
	close(piperw[0]);

	/* If nobody is reading the status output, don't terminate */
	sigaction(SIGPIPE, &sa, NULL);
	printstatus();

	/* At this point the outputs are initialized, choose initial selmon based on
	 * cursor position, and set default cursor image */
	selmon = xytomon(cursor->x, cursor->y);

	/* TODO hack to get cursor to display in its initial location (100, 100)
	 * instead of (0, 0) and then jumping. still may not be fully
	 * initialized, as the image/coordinates are not transformed for the
	 * monitor when displayed here */
	wlr_cursor_warp_closest(cursor, NULL, cursor->x, cursor->y);
	wlr_xcursor_manager_set_cursor_image(cursor_mgr, cursor_image, cursor);

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wl_display_run(dpy);
}

static void cleanup(void) {
	wl_display_destroy_clients(dpy);
	if (child_pid > 0) {
		kill(child_pid, SIGTERM);
		waitpid(child_pid, NULL, 0);
	}
	wlr_backend_destroy(backend);
	wlr_renderer_destroy(drw);
	wlr_allocator_destroy(alloc);
	wlr_xcursor_manager_destroy(cursor_mgr);
	wlr_cursor_destroy(cursor);
	wlr_output_layout_destroy(output_layout);
	wlr_seat_destroy(seat);
	wl_display_destroy(dpy);
}

int main(int argc, char *argv[]) {
	for (int _=1; _<argc; _++) {
		if (!strcmp(argv[_], "about"))
			die("Godalming123's DWL dotfiles based on DWL 0.4");
		else
			die("Use the `about` command to see info.", argv[0]);
	}

	/* Wayland requires XDG_RUNTIME_DIR for creating its communications socket */
	if (!getenv("XDG_RUNTIME_DIR"))
		die("XDG_RUNTIME_DIR must be set");

	setup();
	rundwl();
	cleanup();
	return EXIT_SUCCESS;
}
