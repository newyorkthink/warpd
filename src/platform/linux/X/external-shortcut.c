/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * Add tap-hold semantics to normal-mode mouse buttons without depending on a
 * particular application, shortcut, input remapper, or physical device.
 *
 * warpd generates mouse clicks with XTest, which is above evdev/uinput in the
 * Linux input stack. Low-level remappers therefore cannot observe those
 * synthetic clicks. Handle the hold decision inside warpd instead: a short
 * press is returned to normal mode as the original button key, while a long
 * press sends the configured desktop shortcut after releasing warpd's XInput
 * keyboard grab.
 */

#include "X.h"

extern int external_shortcut_exit_requested;

struct button_hold_state {
	int active;
	int button;
	uint64_t started_us;
	struct input_event source_press;
	struct input_event action;
};

static struct button_hold_state hold_state;

static int translate_smart_hint_activation(struct input_event *ev)
{
	struct input_event effective;

	if (!ev || !ev->pressed)
		return 0;

	/*
	 * A virtual/uinput keyboard can report incomplete effective modifiers while
	 * warpd owns its XIGrabDevice. Merge the event state with the modifier
	 * presses tracked by input.c before matching the activation chord.
	 */
	effective = *ev;
	effective.mods |= x_active_mods;

	if (input_eq(&effective, config_get("smart_hint_activation_key")) != 2)
		return 0;

	return input_parse_string(ev, config_get("smart_hint")) == 0;
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

static void send_modifier(uint8_t mod, int pressed)
{
	KeySym sym = NoSymbol;

	switch (mod) {
	case PLATFORM_MOD_CONTROL:
		sym = XK_Control_L;
		break;
	case PLATFORM_MOD_SHIFT:
		sym = XK_Shift_L;
		break;
	case PLATFORM_MOD_ALT:
		sym = XK_Alt_L;
		break;
	case PLATFORM_MOD_META:
		sym = XK_Super_L;
		break;
	}

	if (sym != NoSymbol)
		XTestFakeKeyEvent(
		    dpy, XKeysymToKeycode(dpy, sym), pressed, CurrentTime);
}

static void send_shortcut(const struct input_event *action)
{
	if (action->mods & PLATFORM_MOD_CONTROL)
		send_modifier(PLATFORM_MOD_CONTROL, True);
	if (action->mods & PLATFORM_MOD_SHIFT)
		send_modifier(PLATFORM_MOD_SHIFT, True);
	if (action->mods & PLATFORM_MOD_ALT)
		send_modifier(PLATFORM_MOD_ALT, True);
	if (action->mods & PLATFORM_MOD_META)
		send_modifier(PLATFORM_MOD_META, True);

	XTestFakeKeyEvent(dpy, action->code, True, CurrentTime);
	XTestFakeKeyEvent(dpy, action->code, False, CurrentTime);

	if (action->mods & PLATFORM_MOD_META)
		send_modifier(PLATFORM_MOD_META, False);
	if (action->mods & PLATFORM_MOD_ALT)
		send_modifier(PLATFORM_MOD_ALT, False);
	if (action->mods & PLATFORM_MOD_SHIFT)
		send_modifier(PLATFORM_MOD_SHIFT, False);
	if (action->mods & PLATFORM_MOD_CONTROL)
		send_modifier(PLATFORM_MOD_CONTROL, False);

	XSync(dpy, False);
}

static struct input_event *trigger_button_hold(void)
{
	static struct input_event exit_event;
	struct input_event action = hold_state.action;

	hold_state.active = 0;

	/*
	 * The shortcut must be emitted after releasing the XIGrabDevice grabs;
	 * otherwise warpd would immediately consume its own synthetic key events.
	 */
	x_input_ungrab_keyboard();
	send_shortcut(&action);

	external_shortcut_exit_requested = 1;

	/* Unwind the active normal-mode loop through its normal exit path. */
	if (input_parse_string(&exit_event, config_get("exit")) < 0)
		input_parse_string(&exit_event, "esc");

	return &exit_event;
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
		int wait_timeout =
		    pending_wait_timeout(timeout, hold_timeout_us);

		ev = x_input_next_event(wait_timeout);

		if (ev && !ev->pressed &&
		    ev->code == hold_state.source_press.code) {
			/*
			 * Released before the threshold: reproduce the original button
			 * key press now, so normal mode performs one ordinary click.
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

	if (!ev || !ev->pressed || hold_timeout_us == 0)
		return ev;

	/*
	 * config_input_match respects the current mode whitelist. Therefore this
	 * only intercepts normal-mode "buttons" bindings and leaves hint/grid/etc.
	 * input untouched.
	 */
	int button = config_input_match(ev, "buttons");
	if (button && button_hold_action(button, &hold_state.action)) {
		hold_state.active = 1;
		hold_state.button = button;
		hold_state.started_us = get_time_us();
		hold_state.source_press = *ev;

		/* Suppress the immediate click until tap versus hold is known. */
		return NULL;
	}

	return ev;
}
