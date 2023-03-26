#include <unistd.h>               // for fork functions
#include <wlr/types/wlr_output.h> // for monitor configuration
#include <libinput.h>             // for mouse + trackpad configuration

// for my keyboard configuration
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

//#include "structs.h"

// BASICS
static const int sloppyfocus               = 1;         // focus follows mouse
static const char *tags[] = { "1", "2", "3", "4", "5"}; // tagging

// To conform the xdg-protocol, set the alpha to zero to restore the old behavior
static const float fullscreen_bg[]         = {0.18, 0.2, 0.25, 1.0};

// 1 means idle inhibitors will disable idle tracking even if it's surface isn't visible
static const int bypass_surface_visibility = 0;


// LAYOUTS
static void twoWindows(Monitor *m);

static const Layout layouts[] = {
	// symbol     arrange function
	{ "mon",      monocle },
	{ "tle",      tile },
	{ "tws",      twoWindows },
};

static void moveClientToSide(Client *c, bool onLeft) {
	resize(c, (struct wlr_box) {
		.x = c->mon->w.x + (onLeft ? 0 : c->mon->w.width*0.5),
		.y = c->mon->w.y,
		.width = c->mon->w.width * 0.5,
		.height = c->mon->w.height}, 0);
}

static bool leftWindowHasFocus = true;

void twoWindows(Monitor *m) {
	Client *c;
	
	// first get all the clients that match the criterea to be tiled
	wl_list_for_each(c, &fstack, flink)
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			moveClientToSide(c, leftWindowHasFocus);
}

// MONITORS
static const MonitorRule monrules[] = {
	// name mfact nmaster scale layout       rotate/reflect              X  Y
	{ NULL, 0.5,  1,      1,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL, 0, 0 },
};

// KEYBOARD
static const struct xkb_rule_names xkb_rules = {
	// can specify fields: rules, model, layout, variant, options */
	// example:
	// .options = "ctrl:nocaps",
	.layout = "gb",
	.options = "",
};

static const int repeat_rate = 25;
static const int repeat_delay = 600;

/* TRACKPAD */
static const int tap_to_click = 1;
static const int tap_and_drag = 1;
static const int drag_lock = 1;
static const int disable_while_typing = 1;
static const int middle_button_emulation = 0;
// You can choose between: LIBINPUT_CONFIG_SCROLL_(NO_SCROLL, 2FG, EDGE, ON_BUTTON_DOWN)
static const enum libinput_config_scroll_method scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;
// You can choose between: LIBINPUT_CONFIG_CLICK_METHOD_(NONE, BUTTON_AREAS, CLICKFINGER)
static const enum libinput_config_click_method click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;

/* MOUSE */
static const int natural_scrolling = 0;
static const int left_handed = 0;
// You can choose between: LIBINPUT_CONFIG_SEND_EVENTS_(ENABLED, DISABLED, DISABLED_ON_EXTERNAL_MOUSE)
static const uint32_t send_events_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;

// You can choose between: LIBINPUT_CONFIG_ACCEL_PROFILE_(FLAT, ADEPTIVE)
static const enum libinput_config_accel_profile accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
static const double accel_speed = 0.3;
/* You can choose between:
LIBINPUT_CONFIG_TAP_MAP_LRM -- 1/2/3 finger tap maps to left/right/middle
LIBINPUT_CONFIG_TAP_MAP_LMR -- 1/2/3 finger tap maps to left/middle/right
*/
static const enum libinput_config_tap_button_map button_map = LIBINPUT_CONFIG_TAP_MAP_LRM;

/* KEYBINDINGS */
#define MODKEY WLR_MODIFIER_ALT

#define TAGKEYS(KEY,SKEY,TAG) \
	{ MODKEY,                                      KEY,            view,            {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_CTRL,                    KEY,            toggleview,      {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_SHIFT,                   SKEY,           tag,             {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT, SKEY,           toggletag,       {.ui = 1 << TAG} }

/* commands */
static const char *menucmd =
	"tofi-drun "
	"--drun-launch=true "

	// size
	"--width=100% "
	"--height=25 "

	// position
	"--anchor=bottom "

	// margins
	"--margin-left=0 "
	"--margin-right=0 "
	"--margin-top=0 "
	"--margin-bottom=0 "

	// padding
	"--selection-background-padding=10 "
	"--padding-left=10 "
	"--padding-right=10 "
	"--padding-top=3 "
	"--padding-bottom=3 "

	// colors
	"--text-color=#FFFFFF "
	"--selection-color=#000000 "
	"--background-color=#2e3440 "
	"--selection-background=#88c0d0 "

	// results
	"--result-spacing=20 "
	"--num-results=12 "

	// styling
	"--border-width=0 "
	"--outline-width=0 "
	"--corner-radius=0 "
	"--selection-background-corner-radius=6 "

	// other options
	"--terminal=alcritty "
	"--prompt-text=' > ' "
	"--horizontal=true "
	"--font-size=12";

static void run(char *cmdTxt) {
	if (fork() == 0) {
		system(cmdTxt);
		exit(EXIT_SUCCESS);
	}
}

int handleKeypress(uint32_t mods, xkb_keysym_t sym) {
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 */
	// 	Note that Shift changes certain key codes: c -> C, 2 -> quotedbl, etc.
	
	if (mods == WLR_MODIFIER_ALT) {
		switch (sym) {
			case XKB_KEY_Return: run("alacritty");       break;
			case XKB_KEY_q:      killclient();           break;
			case XKB_KEY_d:      focusstack(+1);         break;
			case XKB_KEY_a:      focusstack(-1);         break;
			case XKB_KEY_space:  togglefloating();       break;
			case XKB_KEY_f:      togglefullscreen();     break;
			case XKB_KEY_m:      setlayout(&layouts[0]); break;
			case XKB_KEY_t:      setlayout(&layouts[1]); break;
			case XKB_KEY_2:      setlayout(&layouts[2]); break;
			case XKB_KEY_Escape: quit();                 break;
			default: return 0; break;
		}
	}
	else if (mods == (WLR_MODIFIER_ALT|WLR_MODIFIER_SHIFT)) {
		switch (sym) {
			case XKB_KEY_S: leftWindowHasFocus = !leftWindowHasFocus; break;
		}
	}
	else if (mods == (WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT)) {
		// under wayland we must handle virtual terminals ourselfs here I only
		// have 6 because I use runnit and that only has 6 virtual terminals
		switch (sym) {
			case XKB_KEY_XF86Switch_VT_1: chvt(1); break;
			case XKB_KEY_XF86Switch_VT_2: chvt(2); break;
			case XKB_KEY_XF86Switch_VT_3: chvt(3); break;
			case XKB_KEY_XF86Switch_VT_4: chvt(4); break;
			case XKB_KEY_XF86Switch_VT_5: chvt(5); break;
			case XKB_KEY_XF86Switch_VT_6: chvt(6); break;
			default: return 0; break;
		}
	}
	else {
		return 0;
	}

	return 1;
}

int handleMousePress(unsigned int mod, unsigned int button) {
	if (mod == MODKEY) {
		switch (button) {
			case BTN_LEFT:   moveresize(CurMove); break;
			case BTN_MIDDLE: togglefullscreen(); break;
			case BTN_RIGHT:  moveresize(CurResize); break;
			default:         return 0; break; // did not handle the button press
		}
	}
	else {
		return 0; // did not handle the button press
	}
	return 1;
}

// static const Key keys[] = {
// 	/* modifier                  key                            function          argument */
// 	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_a,                     incnmaster,       {.i = +1} },
// 	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_d,                     incnmaster,       {.i = -1} },
// 	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_A,                     setmfact,         {.f = -0.05} },
// 	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_D,                     setmfact,         {.f = +0.05} },
// 	TAGKEYS(                     XKB_KEY_1, XKB_KEY_exclam,                       0),
// 	TAGKEYS(                     XKB_KEY_2, XKB_KEY_quotedbl,                     1),
// 	TAGKEYS(                     XKB_KEY_3, XKB_KEY_sterling,                     2),
// 	TAGKEYS(                     XKB_KEY_4, XKB_KEY_dollar,                       3),
// 	TAGKEYS(                     XKB_KEY_5, XKB_KEY_percent,                      4),
//};

