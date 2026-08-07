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

static int should_passthrough(struct input_event *ev)
{
	if (!ev || !ev->pressed)
		return 0;

	/* A modifier press is only the beginning of a chord, not the shortcut. */
	if (is_modifier_key(ev->code))
		return 0;

	/* Never steal a key that the current warpd mode is configured to use. */
	if (is_warpd_binding(ev))
		return 0;

	/*
	 * Modified chords are desktop-shortcut candidates. Unmodified function,
	 * system, and multimedia keys are also safe candidates. Plain text stays
	 * inside warpd because hint modes intentionally consume arbitrary text.
	 */
	return ev->mods != 0 || is_standalone_shortcut_key(ev->code);
}

static void replay_shortcut_and_exit(struct input_event *ev)
{
	uint8_t code = ev->code;

	x_input_ungrab_keyboard();

	/*
	 * The original key press was consumed by the XInput grab. Re-emit a fresh
	 * press after releasing the grab. Modifier state remains in the X server,
	 * so the desktop receives the same chord without hard-coded modifiers.
	 */
	XTestFakeKeyEvent(dpy, code, False, CurrentTime);
	XTestFakeKeyEvent(dpy, code, True, CurrentTime);
	XTestFakeKeyEvent(dpy, code, False, CurrentTime);
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
