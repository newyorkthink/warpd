/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * X11 input handling while warpd owns the keyboard grab.
 *
 * This adapter keeps three behaviours in one place:
 *   1. Desktop shortcuts that are not used by the active warpd mode are
 *      replayed after releasing the XInput grab, then the warpd session exits.
 *   2. Normal-mode mouse-button keys can use tap/hold semantics. A short press
 *      remains the configured mouse click; a configured long press replays its
 *      shortcut and then resumes the same warpd Normal Mode session.
 *   3. The daemon Smart Hint activation chord can still enter Smart Hint while
 *      Normal Mode is active, including when the chord comes from a virtual or
 *      uinput keyboard whose modifier state is incomplete under XIGrabDevice.
 */

#include "X.h"

extern int external_shortcut_exit_requested;

struct button_hold_state {
	int active;
	uint64_t started_us;
	struct input_event source_press;
	struct input_event action;
};

static struct button_hold_state hold_state;

static uint8_t effective_mods(struct input_event *ev)
{
	/*
	 * Virtual/uinput keyboards can report incomplete effective modifiers on the
	 * final key event while warpd owns XIGrabDevice. Merge the XInput event with
	 * the modifier presses tracked independently by input.c.
	 */
	return ev->mods | x_active_mods;
}

static int translate_smart_hint_activation(struct input_event *ev)
{
	struct input_event effective;

	if (!ev || !ev->pressed)
		return 0;

	effective = *ev;
	effective.mods = effective_mods(ev);

	if (input_eq(&effective, config_get("smart_hint_activation_key")) != 2)
		return 0;

	/* Convert the daemon activation chord into Normal Mode's Smart Hint key. */
	return input_parse_string(ev, config_get("smart_hint")) == 0;
}

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

	/* XF86 multimedia/system keys occupy the 0x1008FFxx keysym range. */
	return sym >= 0x1008FF00 && sym <= 0x1008FFFF;
}

static int should_passthrough(struct input_event *ev)
{
	struct input_event effective;

	if (!ev || !ev->pressed)
		return 0;

	/* A modifier press starts a chord; it is not a complete shortcut itself. */
	if (is_modifier_key(ev->code))
		return 0;

	effective = *ev;
	effective.mods = effective_mods(ev);

	/* Never steal a key that the currently active warpd mode is using. */
	if (is_warpd_binding(&effective))
		return 0;

	/*
	 * Modified chords are desktop-shortcut candidates. Unmodified function,
	 * system and multimedia keys are also safe candidates. Plain text remains
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

static void replay_shortcut(const struct input_event *action)
{
	static const uint8_t modifier_order[] = {
		PLATFORM_MOD_CONTROL,
		PLATFORM_MOD_SHIFT,
		PLATFORM_MOD_ALT,
		PLATFORM_MOD_META,
	};
	KeyCode synthetic_modifiers[sizeof modifier_order];
	size_t nr_synthetic_modifiers = 0;
	char keymap[32];
	size_t i;

	/*
	 * After XIUngrabDevice, a virtual keyboard's modifier state may not yet be
	 * visible to the desktop. Query the real X11 state and synthesize only the
	 * modifiers that are missing, so physically held modifiers are preserved.
	 */
	XQueryKeymap(dpy, keymap);

	for (i = 0; i < sizeof modifier_order; i++) {
		uint8_t mod = modifier_order[i];
		KeyCode modifier_code;

		if (!(action->mods & mod) || modifier_is_down(keymap, mod))
			continue;

		modifier_code = modifier_replay_keycode(mod);
		if (!modifier_code)
			continue;

		XTestFakeKeyEvent(dpy, modifier_code, True, CurrentTime);
		synthetic_modifiers[nr_synthetic_modifiers++] = modifier_code;
	}

	/* Force a fresh press even if the grabbed source still reports the key down. */
	XTestFakeKeyEvent(dpy, action->code, False, CurrentTime);
	XTestFakeKeyEvent(dpy, action->code, True, CurrentTime);
	XTestFakeKeyEvent(dpy, action->code, False, CurrentTime);

	while (nr_synthetic_modifiers) {
		KeyCode modifier_code =
		    synthetic_modifiers[--nr_synthetic_modifiers];
		XTestFakeKeyEvent(dpy, modifier_code, False, CurrentTime);
	}

	XSync(dpy, False);
}

static void replay_shortcut_and_exit(struct input_event *ev)
{
	struct input_event action = *ev;

	action.mods = effective_mods(ev);

	x_input_ungrab_keyboard();
	replay_shortcut(&action);

	external_shortcut_exit_requested = 1;

	/* Return the configured exit event so the active sub-mode unwinds normally. */
	if (input_parse_string(ev, config_get("exit")) < 0)
		input_parse_string(ev, "esc");
}

static int button_hold_action(int button, struct input_event *action)
{
	char buf[64];
	char *tok;
	int idx = 1;

	snprintf(buf, sizeof buf, "%s", config_get("button_hold_keys"));

	for (tok = strtok(buf, " "); tok; tok = strtok(NULL, " "), idx++) {
		if (idx != button)
			continue;

		if (!strcmp(tok, "unbind"))
			return 0;

		if (input_parse_string(action, tok) < 0) {
			fprintf(stderr,
				"ERROR: invalid button hold shortcut: %s\n", tok);
			return 0;
		}

		return 1;
	}

	return 0;
}

static struct input_event *trigger_button_hold(void)
{
	struct input_event action = hold_state.action;

	hold_state.active = 0;

	/*
	 * The hold shortcut must escape warpd's XIGrabDevice grab. Unlike a normal
	 * external desktop shortcut, this path deliberately re-grabs the keyboard
	 * afterwards so the existing Normal Mode pointer remains active.
	 */
	x_input_ungrab_keyboard();
	replay_shortcut(&action);
	x_input_grab_keyboard();

	return NULL;
}

static int pending_wait_timeout(int timeout, uint64_t timeout_us)
{
	uint64_t elapsed_us = get_time_us() - hold_state.started_us;
	uint64_t remaining_us;
	uint64_t remaining_ms;

	if (elapsed_us >= timeout_us)
		return 1;

	remaining_us = timeout_us - elapsed_us;
	remaining_ms = (remaining_us + 999) / 1000;
	if (remaining_ms == 0)
		remaining_ms = 1;

	if (timeout == 0 || remaining_ms < (uint64_t)timeout)
		return (int)remaining_ms;

	return timeout;
}

struct input_event *x_input_next_event_passthrough(int timeout)
{
	static struct input_event tap_event;
	struct input_event *ev;
	int hold_ms = config_get_int("button_hold_timeout");
	uint64_t hold_timeout_us = hold_ms > 0 ? (uint64_t)hold_ms * 1000 : 0;

	if (hold_state.active) {
		int wait_timeout = pending_wait_timeout(timeout, hold_timeout_us);

		ev = x_input_next_event(wait_timeout);

		if (ev && !ev->pressed &&
		    ev->code == hold_state.source_press.code) {
			/*
			 * Released before the threshold: reproduce the original button
			 * key press so Normal Mode performs exactly one ordinary click.
			 */
			tap_event = hold_state.source_press;
			tap_event.pressed = 1;
			hold_state.active = 0;
			return &tap_event;
		}

		if (ev)
			return ev;

		if (get_time_us() - hold_state.started_us >= hold_timeout_us)
			return trigger_button_hold();

		return NULL;
	}

	ev = x_input_next_event(timeout);
	if (translate_smart_hint_activation(ev))
		return ev;

	if (!ev)
		return NULL;

	/*
	 * config_input_match respects the active mode whitelist, so tap/hold is
	 * applied only to Normal Mode's configured mouse-button keys.
	 */
	if (ev->pressed && hold_timeout_us != 0) {
		int button = config_input_match(ev, "buttons");

		if (button && button_hold_action(button, &hold_state.action)) {
			hold_state.active = 1;
			hold_state.started_us = get_time_us();
			hold_state.source_press = *ev;

			/* Wait until release or timeout before deciding tap versus hold. */
			return NULL;
		}
	}

	if (should_passthrough(ev))
		replay_shortcut_and_exit(ev);

	return ev;
}
