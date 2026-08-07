/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * Allow desktop shortcuts that are not consumed by the active warpd mode to
 * escape the XInput keyboard grab. This is intentionally independent of the
 * shortcut, application, input remapper, and physical/virtual input device.
 */

#include "X.h"

extern int external_shortcut_exit_requested;

struct input_event *__real_x_input_next_event(int timeout);

static int is_modifier_key(uint8_t code)
{
	KeySym sym = XKeycodeToKeysym(dpy, code, 0);

	return IsModifierKey(sym);
}

static int is_warpd_binding(struct input_event *ev)
{
	struct config_entry *ent;

	for (ent = config; ent; ent = ent->next) {
		char buf[sizeof ent->value];
		char *tok;

		if (!ent->whitelisted ||
		    (ent->type != OPT_KEY && ent->type != OPT_BUTTON) ||
		    !strcmp(ent->value, "unbind"))
			continue;

		snprintf(buf, sizeof buf, "%s", ent->value);

		for (tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
			int match = input_eq(ev, tok);

			if ((ent->type == OPT_KEY && match == 2) ||
			    (ent->type == OPT_BUTTON && match != 0))
				return 1;
		}
	}

	return 0;
}

static int is_standalone_shortcut_key(uint8_t code)
{
	KeySym sym = XKeycodeToKeysym(dpy, code, 0);

	if (IsFunctionKey(sym) || IsMiscFunctionKey(sym))
		return 1;

	/*
	 * XF86 multimedia/system keys use the 0x1008FFxx keysym range.
	 * Keep this numeric check local so the build does not gain another header
	 * dependency merely for XF86 key names.
	 */
	return sym >= 0x1008FF00 && sym <= 0x1008FFFF;
}

static uint8_t effective_mods(struct input_event *ev)
{
	/*
	 * Some virtual/uinput keyboards report incomplete effective modifiers on
	 * the final key event while warpd owns the XInput device. warpd already
	 * tracks modifier key presses independently in x_active_mods, so merge the
	 * two views instead of trusting either one alone.
	 */
	return ev->mods | x_active_mods;
}

static int should_passthrough(struct input_event *ev)
{
	struct input_event effective;

	if (!ev || !ev->pressed)
		return 0;

	/* A modifier press is only the beginning of a chord, not the shortcut. */
	if (is_modifier_key(ev->code))
		return 0;

	effective = *ev;
	effective.mods = effective_mods(ev);

	/* Never steal a key that the current warpd mode is configured to use. */
	if (is_warpd_binding(&effective))
		return 0;

	/*
	 * Modified chords are desktop-shortcut candidates. Unmodified function,
	 * system, and multimedia keys are also safe candidates. Plain text stays
	 * inside warpd because hint modes intentionally consume arbitrary text.
	 */
	return effective.mods != 0 || is_standalone_shortcut_key(ev->code);
}

static void modifier_keysyms(uint8_t mod, KeySym *left, KeySym *right)
{
	switch (mod) {
	case PLATFORM_MOD_CONTROL:
		*left = XK_Control_L;
		*right = XK_Control_R;
		break;
	case PLATFORM_MOD_SHIFT:
		*left = XK_Shift_L;
		*right = XK_Shift_R;
		break;
	case PLATFORM_MOD_ALT:
		*left = XK_Alt_L;
		*right = XK_Alt_R;
		break;
	case PLATFORM_MOD_META:
		*left = XK_Super_L;
		*right = XK_Super_R;
		break;
	default:
		*left = NoSymbol;
		*right = NoSymbol;
		break;
	}
}

static int keycode_is_down(const char keymap[32], KeyCode code)
{
	if (!code)
		return 0;

	return 0x01 & keymap[code / 8] >> (code % 8);
}

static int modifier_is_down(const char keymap[32], uint8_t mod)
{
	KeySym left_sym;
	KeySym right_sym;
	KeyCode left;
	KeyCode right;

	modifier_keysyms(mod, &left_sym, &right_sym);
	if (left_sym == NoSymbol)
		return 0;

	left = XKeysymToKeycode(dpy, left_sym);
	right = XKeysymToKeycode(dpy, right_sym);

	return keycode_is_down(keymap, left) || keycode_is_down(keymap, right);
}

static KeyCode modifier_replay_keycode(uint8_t mod)
{
	KeySym left_sym;
	KeySym right_sym;

	modifier_keysyms(mod, &left_sym, &right_sym);
	(void)right_sym;

	return left_sym == NoSymbol ? 0 : XKeysymToKeycode(dpy, left_sym);
}

static void replay_shortcut_and_exit(struct input_event *ev)
{
	static const uint8_t modifier_order[] = {
		PLATFORM_MOD_CONTROL,
		PLATFORM_MOD_SHIFT,
		PLATFORM_MOD_ALT,
		PLATFORM_MOD_META,
	};
	KeyCode synthetic_modifiers[sizeof modifier_order];
	size_t nr_synthetic_modifiers = 0;
	uint8_t mods = effective_mods(ev);
	uint8_t code = ev->code;
	char keymap[32];
	size_t i;

	x_input_ungrab_keyboard();

	/*
	 * XIGrabDevice can detach a slave keyboard from its master while warpd owns
	 * it. After the grab is released, a virtual/remapped keyboard's modifier
	 * state is therefore not guaranteed to be visible to the desktop at the
	 * exact instant the final shortcut key is replayed.
	 *
	 * Query the real X11 key state after ungrabbing and synthesize only the
	 * modifiers that are actually missing. This preserves physical modifiers,
	 * works with virtual/uinput keyboards, and does not depend on any specific
	 * shortcut or application.
	 */
	XQueryKeymap(dpy, keymap);

	for (i = 0; i < sizeof modifier_order; i++) {
		uint8_t mod = modifier_order[i];
		KeyCode modifier_code;

		if (!(mods & mod) || modifier_is_down(keymap, mod))
			continue;

		modifier_code = modifier_replay_keycode(mod);
		if (!modifier_code)
			continue;

		XTestFakeKeyEvent(dpy, modifier_code, True, CurrentTime);
		synthetic_modifiers[nr_synthetic_modifiers++] = modifier_code;
	}

	/* Ensure a fresh key press even if the grabbed source still reports it down. */
	XTestFakeKeyEvent(dpy, code, False, CurrentTime);
	XTestFakeKeyEvent(dpy, code, True, CurrentTime);
	XTestFakeKeyEvent(dpy, code, False, CurrentTime);

	while (nr_synthetic_modifiers) {
		KeyCode modifier_code =
		    synthetic_modifiers[--nr_synthetic_modifiers];
		XTestFakeKeyEvent(dpy, modifier_code, False, CurrentTime);
	}

	XSync(dpy, False);

	external_shortcut_exit_requested = 1;

	/*
	 * Return warpd's configured exit event so whichever sub-mode currently
	 * owns the input loop unwinds normally. mode_loop then sees the external
	 * shortcut flag and ends the whole active session.
	 */
	if (input_parse_string(ev, config_get("exit")) < 0)
		input_parse_string(ev, "esc");
}

struct input_event *__wrap_x_input_next_event(int timeout)
{
	struct input_event *ev = __real_x_input_next_event(timeout);

	if (should_passthrough(ev))
		replay_shortcut_and_exit(ev);

	return ev;
}
