/*
 * warpd - strict X11-aware AT-SPI window matching.
 *
 * Geometry is only a secondary score. An AT-SPI window must first match the
 * X11 active window by PID, title, WM_CLASS/application name, or process name.
 */
#include "atspi-x11-detector.h"

#include <at-spi-2.0/atspi/atspi.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *config_get(const char *key);
extern int config_get_int(const char *key);

#ifdef WARPD_X

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdint.h>

extern Display *dpy;

typedef struct {
	Window xid;
	guint pid;
	gchar *title;
	gchar *wm_class;
	gchar *process_name;
	gint x;
	gint y;
	gint w;
	gint h;
} X11ActiveWindowInfo;

typedef struct {
	GPtrArray *elements;
	GHashTable *seen;
	gint win_x;
	gint win_y;
	gint win_w;
	gint win_h;
	gint max_depth;
	gint max_elements;
	gint min_width;
	gint min_height;
	gint min_area;
} CollectContext;

static void get_accessible_rect(
    AtspiAccessible *accessible,
    gint *x,
    gint *y,
    gint *w,
    gint *h)
{
	AtspiComponent *component;
	AtspiRect *rect;

	*x = 0;
	*y = 0;
	*w = 0;
	*h = 0;
	component = atspi_accessible_get_component(accessible);
	if (!component)
		return;

	rect = atspi_component_get_extents(
	    component,
	    ATSPI_COORD_TYPE_SCREEN,
	    NULL);
	if (rect) {
		*x = rect->x;
		*y = rect->y;
		*w = rect->width;
		*h = rect->height;
		g_free(rect);
	}
	g_object_unref(component);
}

static gchar *casefold_text(const gchar *text)
{
	gchar *folded;

	if (!text)
		return g_strdup("");
	folded = g_utf8_validate(text, -1, NULL)
	             ? g_utf8_casefold(text, -1)
	             : g_ascii_strdown(text, -1);
	g_strstrip(folded);
	return folded;
}

static gint text_match_score(const gchar *left, const gchar *right)
{
	gchar *a;
	gchar *b;
	gint score = 0;

	if (!left || !right || !*left || !*right)
		return 0;
	a = casefold_text(left);
	b = casefold_text(right);
	if (strlen(a) >= 3 && strlen(b) >= 3) {
		if (g_strcmp0(a, b) == 0)
			score = 420;
		else if (strstr(a, b) || strstr(b, a))
			score = 260;
	}
	g_free(a);
	g_free(b);
	return score;
}

static gchar *read_process_name(guint pid)
{
	gchar *path;
	gchar *contents = NULL;

	if (!pid)
		return NULL;
	path = g_strdup_printf("/proc/%u/comm", pid);
	if (!g_file_get_contents(path, &contents, NULL, NULL)) {
		g_free(path);
		return NULL;
	}
	g_free(path);
	g_strstrip(contents);
	return contents;
}

static gchar *x11_get_text_property(Window window, const char *property_name)
{
	Atom property;
	Atom actual_type = None;
	int actual_format = 0;
	unsigned long item_count = 0;
	unsigned long bytes_after = 0;
	unsigned char *data = NULL;
	gchar *text = NULL;

	property = XInternAtom(dpy, property_name, True);
	if (property == None)
		return NULL;
	if (XGetWindowProperty(
		dpy, window, property, 0, 4096, False, AnyPropertyType,
		&actual_type, &actual_format, &item_count, &bytes_after,
		&data) == Success &&
	    data && actual_format == 8 && item_count > 0)
		text = g_strndup((const gchar *)data, item_count);
	if (data)
		XFree(data);
	return text;
}

static guint x11_get_window_pid(Window window)
{
	Atom property;
	Atom actual_type = None;
	int actual_format = 0;
	unsigned long item_count = 0;
	unsigned long bytes_after = 0;
	unsigned char *data = NULL;
	guint pid = 0;

	property = XInternAtom(dpy, "_NET_WM_PID", True);
	if (property == None)
		return 0;
	if (XGetWindowProperty(
		dpy, window, property, 0, 1, False, XA_CARDINAL,
		&actual_type, &actual_format, &item_count, &bytes_after,
		&data) == Success &&
	    data && actual_format == 32 && item_count == 1)
		pid = (guint)(*(unsigned long *)data);
	if (data)
		XFree(data);
	return pid;
}

static gboolean x11_get_active_window_info(X11ActiveWindowInfo *info)
{
	Window root;
	Atom property;
	Atom actual_type = None;
	int actual_format = 0;
	unsigned long item_count = 0;
	unsigned long bytes_after = 0;
	unsigned char *data = NULL;
	XWindowAttributes attributes;
	Window child = None;
	XClassHint class_hint;
	char *legacy_title = NULL;

	memset(info, 0, sizeof(*info));
	if (!dpy)
		return FALSE;
	root = DefaultRootWindow(dpy);
	property = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
	if (property == None)
		return FALSE;
	if (XGetWindowProperty(
		dpy, root, property, 0, 1, False, XA_WINDOW,
		&actual_type, &actual_format, &item_count, &bytes_after,
		&data) != Success ||
	    !data || actual_format != 32 || item_count != 1) {
		if (data)
			XFree(data);
		return FALSE;
	}

	info->xid = *(Window *)data;
	XFree(data);
	if (info->xid == None || info->xid == root)
		return FALSE;

	info->pid = x11_get_window_pid(info->xid);
	info->process_name = read_process_name(info->pid);
	info->title = x11_get_text_property(info->xid, "_NET_WM_NAME");
	if ((!info->title || !*info->title) &&
	    XFetchName(dpy, info->xid, &legacy_title)) {
		g_free(info->title);
		info->title = g_strdup(legacy_title);
		XFree(legacy_title);
	}

	memset(&class_hint, 0, sizeof(class_hint));
	if (XGetClassHint(dpy, info->xid, &class_hint)) {
		info->wm_class = g_strdup_printf(
		    "%s %s",
		    class_hint.res_name ? class_hint.res_name : "",
		    class_hint.res_class ? class_hint.res_class : "");
		if (class_hint.res_name)
			XFree(class_hint.res_name);
		if (class_hint.res_class)
			XFree(class_hint.res_class);
	}

	if (XGetWindowAttributes(dpy, info->xid, &attributes)) {
		info->w = attributes.width;
		info->h = attributes.height;
		if (!XTranslateCoordinates(
			dpy, info->xid, root, 0, 0,
			&info->x, &info->y, &child)) {
			info->x = attributes.x;
			info->y = attributes.y;
		}
	}
	return TRUE;
}

static void x11_window_info_clear(X11ActiveWindowInfo *info)
{
	g_free(info->title);
	g_free(info->wm_class);
	g_free(info->process_name);
	memset(info, 0, sizeof(*info));
}

static gint geometry_match_score(
    AtspiAccessible *window,
    const X11ActiveWindowInfo *target)
{
	gint x;
	gint y;
	gint w;
	gint h;
	gint left;
	gint top;
	gint right;
	gint bottom;
	gint64 intersection;
	gint64 smaller_area;
	gint score;

	get_accessible_rect(window, &x, &y, &w, &h);
	if (w <= 0 || h <= 0 || target->w <= 0 || target->h <= 0)
		return 0;
	left = MAX(x, target->x);
	top = MAX(y, target->y);
	right = MIN(x + w, target->x + target->w);
	bottom = MIN(y + h, target->y + target->h);
	if (right <= left || bottom <= top)
		return -180;

	intersection = (gint64)(right - left) * (bottom - top);
	smaller_area = MIN((gint64)w * h, (gint64)target->w * target->h);
	if (smaller_area <= 0)
		return 0;
	score = (gint)(((double)intersection / (double)smaller_area) * 260.0);
	if (ABS(x - target->x) <= 12 && ABS(y - target->y) <= 12)
		score += 40;
	if (ABS(w - target->w) <= 24 && ABS(h - target->h) <= 24)
		score += 40;
	return score;
}

static gboolean state_contains(AtspiStateSet *states, AtspiStateType state)
{
	return states && atspi_state_set_contains(states, state);
}

static AtspiAccessible *find_best_atspi_window(void)
{
	AtspiAccessible *desktop;
	AtspiAccessible *best = NULL;
	AtspiAccessible *sole_visible = NULL;
	X11ActiveWindowInfo target;
	gboolean have_target;
	gint best_score = -100000;
	gint visible_count = 0;
	gint geometry_only_ignored = 0;
	gchar *best_app_name = NULL;
	gchar *best_window_name = NULL;
	gchar *best_identity = NULL;
	guint best_pid = 0;

	have_target = x11_get_active_window_info(&target);
	if (have_target) {
		fprintf(
		    stderr,
		    "AT-SPI: X11 target xid=0x%lx pid=%u process='%s' title='%s' class='%s' rect=%d,%d %dx%d\n",
		    (unsigned long)target.xid,
		    target.pid,
		    target.process_name ? target.process_name : "",
		    target.title ? target.title : "",
		    target.wm_class ? target.wm_class : "",
		    target.x, target.y, target.w, target.h);
	}

	desktop = atspi_get_desktop(0);
	if (!desktop) {
		x11_window_info_clear(&target);
		return NULL;
	}

	for (gint i = 0; i < atspi_accessible_get_child_count(desktop, NULL); i++) {
		AtspiAccessible *app =
		    atspi_accessible_get_child_at_index(desktop, i, NULL);
		gchar *app_name;
		guint app_pid;

		if (!app)
			continue;
		app_name = atspi_accessible_get_name(app, NULL);
		app_pid = atspi_accessible_get_process_id(app, NULL);

		for (gint j = 0; j < atspi_accessible_get_child_count(app, NULL); j++) {
			AtspiAccessible *window =
			    atspi_accessible_get_child_at_index(app, j, NULL);
			AtspiStateSet *states;
			gchar *window_name;
			guint window_pid;
			gboolean active;
			gboolean visible;
			gboolean pid_match = FALSE;
			gboolean identity_match = FALSE;
			gint title_score = 0;
			gint class_score = 0;
			gint process_score = 0;
			gint geometry_score = 0;
			gint score = 0;
			GString *identity;

			if (!window)
				continue;
			states = atspi_accessible_get_state_set(window);
			active = state_contains(states, ATSPI_STATE_ACTIVE);
			visible = state_contains(states, ATSPI_STATE_SHOWING) ||
			          state_contains(states, ATSPI_STATE_VISIBLE);
			window_name = atspi_accessible_get_name(window, NULL);
			window_pid = atspi_accessible_get_process_id(window, NULL);
			if (!window_pid)
				window_pid = app_pid;

			identity = g_string_new(NULL);
			if (have_target) {
				pid_match = target.pid &&
				            (window_pid == target.pid || app_pid == target.pid);
				title_score = text_match_score(window_name, target.title);
				class_score = MAX(
				    text_match_score(app_name, target.wm_class),
				    text_match_score(window_name, target.wm_class));
				process_score = MAX(
				    text_match_score(app_name, target.process_name),
				    text_match_score(window_name, target.process_name));
				identity_match = pid_match || title_score >= 240 ||
				                 class_score >= 240 || process_score >= 240;

				if (pid_match) {
					score += 900;
					g_string_append(identity, "pid,");
				}
				if (title_score > 0) {
					score += title_score;
					g_string_append(identity, "title,");
				}
				if (class_score > 0) {
					score += class_score;
					g_string_append(identity, "class,");
				}
				if (process_score > 0) {
					score += process_score;
					g_string_append(identity, "process,");
				}
				geometry_score = geometry_match_score(window, &target);
				if (!identity_match) {
					if (geometry_score > 0)
						geometry_only_ignored++;
					g_string_free(identity, TRUE);
					g_free(window_name);
					if (states)
						g_object_unref(states);
					g_object_unref(window);
					continue;
				}
				score += geometry_score;
				if (active)
					score += 80;
			} else {
				identity_match = active;
				if (active) {
					score += 1200;
					g_string_append(identity, "active,");
				}
			}

			if (!visible && !active) {
				g_string_free(identity, TRUE);
				g_free(window_name);
				if (states)
					g_object_unref(states);
				g_object_unref(window);
				continue;
			}
			if (visible)
				score += 25;

			if (!have_target && visible) {
				visible_count++;
				if (sole_visible)
					g_object_unref(sole_visible);
				sole_visible = g_object_ref(window);
			}

			if (identity_match && score > best_score) {
				if (best)
					g_object_unref(best);
				best = g_object_ref(window);
				best_score = score;
				g_free(best_app_name);
				g_free(best_window_name);
				g_free(best_identity);
				best_app_name = g_strdup(app_name ? app_name : "");
				best_window_name = g_strdup(window_name ? window_name : "");
				best_identity = g_string_free(identity, FALSE);
				identity = NULL;
				best_pid = window_pid;
			}

			if (identity)
				g_string_free(identity, TRUE);
			g_free(window_name);
			if (states)
				g_object_unref(states);
			g_object_unref(window);
		}
		g_free(app_name);
		g_object_unref(app);
	}
	g_object_unref(desktop);

	if (have_target) {
		if (best && best_score < 240) {
			g_object_unref(best);
			best = NULL;
		}
	} else if (!best && visible_count == 1 && sole_visible) {
		best = g_object_ref(sole_visible);
	}

	if (best) {
		fprintf(
		    stderr,
		    "AT-SPI: matched app='%s' window='%s' pid=%u identity='%s' score=%d\n",
		    best_app_name ? best_app_name : "",
		    best_window_name ? best_window_name : "",
		    best_pid,
		    best_identity ? best_identity : "",
		    best_score);
	} else if (have_target) {
		fprintf(
		    stderr,
		    "AT-SPI: no identity match for X11 active window; ignored %d geometry-only candidate(s)\n",
		    geometry_only_ignored);
	} else {
		fprintf(stderr, "AT-SPI: no active AT-SPI window found\n");
	}

	if (sole_visible)
		g_object_unref(sole_visible);
	g_free(best_app_name);
	g_free(best_window_name);
	g_free(best_identity);
	x11_window_info_clear(&target);
	return best;
}

static gboolean accessible_is_visible(AtspiAccessible *accessible)
{
	AtspiStateSet *states = atspi_accessible_get_state_set(accessible);
	gboolean visible;

	if (!states)
		return FALSE;
	visible = atspi_state_set_contains(states, ATSPI_STATE_SHOWING) &&
	          atspi_state_set_contains(states, ATSPI_STATE_VISIBLE);
	g_object_unref(states);
	return visible;
}

static gboolean role_name_is_interactive(const gchar *role)
{
	static const gchar *roles[] = {
		"push button", "toggle button", "check box", "radio button",
		"link", "entry", "password text", "combo box", "menu item",
		"check menu item", "radio menu item", "page tab", "spin button",
		"slider", "scroll bar", "tree item", "list item", "table cell",
		NULL,
	};

	if (!role)
		return FALSE;
	for (gint i = 0; roles[i]; i++) {
		if (g_ascii_strcasecmp(role, roles[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

static gboolean accessible_has_action(AtspiAccessible *accessible)
{
	AtspiAction *action = atspi_accessible_get_action(accessible);
	if (!action)
		return FALSE;
	g_object_unref(action);
	return TRUE;
}

static gboolean accessible_has_editable_text(AtspiAccessible *accessible)
{
	AtspiEditableText *editable =
	    atspi_accessible_get_editable_text(accessible);
	if (!editable)
		return FALSE;
	g_object_unref(editable);
	return TRUE;
}

static gboolean role_is_generic_container(const gchar *role)
{
	static const gchar *roles[] = {
		"application", "frame", "dialog", "panel", "section",
		"html container", "document web", "document frame", "paragraph",
		"filler", "grouping", "scroll pane", NULL,
	};

	if (!role)
		return TRUE;
	for (gint i = 0; roles[i]; i++) {
		if (g_ascii_strcasecmp(role, roles[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

static gboolean accessible_is_interactive(
    AtspiAccessible *accessible,
    const gchar *role)
{
	AtspiStateSet *states;
	gboolean state_interactive = FALSE;

	if (role_name_is_interactive(role) ||
	    accessible_has_action(accessible) ||
	    accessible_has_editable_text(accessible))
		return TRUE;
	if (role_is_generic_container(role))
		return FALSE;

	states = atspi_accessible_get_state_set(accessible);
	if (states) {
		state_interactive =
		    atspi_state_set_contains(states, ATSPI_STATE_FOCUSABLE) ||
		    atspi_state_set_contains(states, ATSPI_STATE_SELECTABLE) ||
		    atspi_state_set_contains(states, ATSPI_STATE_EDITABLE);
		g_object_unref(states);
	}
	return state_interactive;
}

static gboolean rect_overlaps_window(
    const CollectContext *context,
    gint x,
    gint y,
    gint w,
    gint h)
{
	gint right;
	gint bottom;

	if (context->win_w <= 0 || context->win_h <= 0)
		return TRUE;
	right = MIN(x + w, context->win_x + context->win_w);
	bottom = MIN(y + h, context->win_y + context->win_h);
	return right > MAX(x, context->win_x) &&
	       bottom > MAX(y, context->win_y);
}

static char *duplicate_glib_text(gchar *text, const char *fallback)
{
	char *copy;

	if (!text || !*text) {
		g_free(text);
		return strdup(fallback ? fallback : "");
	}
	copy = strdup(text);
	g_free(text);
	return copy;
}

static void free_ui_element_pointer(gpointer data)
{
	struct ui_element *element = data;

	if (!element)
		return;
	free(element->name);
	free(element->role);
	free(element);
}

static void collect_accessible_elements(
    AtspiAccessible *node,
    gint depth,
    CollectContext *context)
{
	gint x;
	gint y;
	gint w;
	gint h;
	gint child_count;
	gchar *role;

	if (!node || depth > context->max_depth ||
	    (gint)context->elements->len >= context->max_elements)
		return;
	if (depth > 0 && !accessible_is_visible(node))
		return;

	get_accessible_rect(node, &x, &y, &w, &h);
	role = atspi_accessible_get_role_name(node, NULL);
	if (w >= context->min_width && h >= context->min_height &&
	    (gint64)w * h >= context->min_area &&
	    rect_overlaps_window(context, x, y, w, h) &&
	    accessible_is_interactive(node, role)) {
		gchar *key = g_strdup_printf("%d:%d:%d:%d", x, y, w, h);

		if (!g_hash_table_contains(context->seen, key)) {
			struct ui_element *element = calloc(1, sizeof(*element));
			gchar *name = atspi_accessible_get_name(node, NULL);

			if (element) {
				if (!name || !*name) {
					g_free(name);
					name = atspi_accessible_get_description(node, NULL);
				}
				element->x = x;
				element->y = y;
				element->w = w;
				element->h = h;
				element->name = duplicate_glib_text(
				    name,
				    role ? role : "UI element");
				element->role = strdup(role ? role : "element");
				g_ptr_array_add(context->elements, element);
				g_hash_table_add(context->seen, key);
			} else {
				g_free(name);
				g_free(key);
			}
		} else {
			g_free(key);
		}
	}
	g_free(role);

	child_count = atspi_accessible_get_child_count(node, NULL);
	for (gint i = 0; i < child_count; i++) {
		AtspiAccessible *child;

		if ((gint)context->elements->len >= context->max_elements)
			break;
		child = atspi_accessible_get_child_at_index(node, i, NULL);
		if (child) {
			collect_accessible_elements(child, depth + 1, context);
			g_object_unref(child);
		}
	}
}

static int compare_ui_elements(const void *left, const void *right)
{
	const struct ui_element *a = left;
	const struct ui_element *b = right;

	if (a->y != b->y)
		return a->y < b->y ? -1 : 1;
	if (a->x != b->x)
		return a->x < b->x ? -1 : 1;
	return 0;
}

int atspi_x11_is_available(void)
{
	return dpy != NULL;
}

struct ui_detection_result *atspi_x11_detect_ui_elements(void)
{
	struct ui_detection_result *result = calloc(1, sizeof(*result));
	AtspiAccessible *window;
	CollectContext context;

	if (!result)
		return NULL;
	atspi_init();
	window = find_best_atspi_window();
	if (!window) {
		result->error = -1;
		snprintf(
		    result->error_msg,
		    sizeof(result->error_msg),
		    "No identity-matched AT-SPI window for the X11 active window");
		return result;
	}

	memset(&context, 0, sizeof(context));
	context.elements = g_ptr_array_new_with_free_func(free_ui_element_pointer);
	context.seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	context.max_depth = MAX(config_get_int("ui_max_depth"), 1);
	context.max_elements = MAX(config_get_int("ui_max_elements"), 1);
	context.min_width = MAX(config_get_int("ui_min_width"), 1);
	context.min_height = MAX(config_get_int("ui_min_height"), 1);
	context.min_area = MAX(config_get_int("ui_min_area"), 1);
	get_accessible_rect(
	    window,
	    &context.win_x,
	    &context.win_y,
	    &context.win_w,
	    &context.win_h);
	collect_accessible_elements(window, 0, &context);
	g_object_unref(window);

	if (context.elements->len == 0) {
		result->error = -2;
		snprintf(
		    result->error_msg,
		    sizeof(result->error_msg),
		    "Identity-matched AT-SPI window exposed no interactive elements");
		g_hash_table_destroy(context.seen);
		g_ptr_array_free(context.elements, TRUE);
		return result;
	}

	result->elements = calloc(context.elements->len, sizeof(*result->elements));
	if (!result->elements) {
		result->error = -3;
		snprintf(
		    result->error_msg,
		    sizeof(result->error_msg),
		    "Memory allocation failed");
		g_hash_table_destroy(context.seen);
		g_ptr_array_free(context.elements, TRUE);
		return result;
	}

	for (guint i = 0; i < context.elements->len; i++) {
		struct ui_element *element = g_ptr_array_index(context.elements, i);
		result->elements[i] = *element;
		element->name = NULL;
		element->role = NULL;
	}
	result->count = context.elements->len;
	result->error = 0;
	qsort(
	    result->elements,
	    result->count,
	    sizeof(*result->elements),
	    compare_ui_elements);
	fprintf(
	    stderr,
	    "AT-SPI: collected %zu interactive elements from identity-matched window\n",
	    result->count);
	g_hash_table_destroy(context.seen);
	g_ptr_array_free(context.elements, TRUE);
	return result;
}

#else

int atspi_x11_is_available(void)
{
	return 0;
}

struct ui_detection_result *atspi_x11_detect_ui_elements(void)
{
	struct ui_detection_result *result = calloc(1, sizeof(*result));
	if (result) {
		result->error = -1;
		snprintf(
		    result->error_msg,
		    sizeof(result->error_msg),
		    "X11 is not available");
	}
	return result;
}

#endif
