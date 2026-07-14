/*
 * warpd - strict X11/AT-SPI matching with Chromium process-family support.
 */
#include "atspi-x11-detector.h"

#include <at-spi-2.0/atspi/atspi.h>
#include <glib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const char *config_get(const char *key);
extern int config_get_int(const char *key);

#ifdef WARPD_X

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

extern Display *dpy;

typedef struct {
	Window xid;
	guint pid;
	gchar *title;
	gchar *wm_class;
	gchar *process_name;
	gchar *exe_name;
	gint x;
	gint y;
	gint w;
	gint h;
} X11Target;

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

static gchar *read_trimmed_file(const gchar *path)
{
	gchar *data = NULL;
	gsize length = 0;

	if (!g_file_get_contents(path, &data, &length, NULL) || !data)
		return NULL;
	g_strstrip(data);
	return data;
}

static gchar *proc_comm(guint pid)
{
	gchar path[64];

	if (!pid)
		return NULL;
	snprintf(path, sizeof(path), "/proc/%u/comm", pid);
	return read_trimmed_file(path);
}

static gchar *proc_exe_name(guint pid)
{
	gchar path[64];
	gchar target[PATH_MAX + 1];
	ssize_t length;
	gchar *slash;

	if (!pid)
		return NULL;
	snprintf(path, sizeof(path), "/proc/%u/exe", pid);
	length = readlink(path, target, PATH_MAX);
	if (length <= 0)
		return NULL;
	target[length] = '\0';
	slash = strrchr(target, '/');
	return g_strdup(slash ? slash + 1 : target);
}

static guint proc_ppid(guint pid)
{
	gchar path[64];
	gchar *stat = NULL;
	gsize length = 0;
	gchar *end;
	char state = 0;
	unsigned int parent = 0;

	if (!pid)
		return 0;
	snprintf(path, sizeof(path), "/proc/%u/stat", pid);
	if (!g_file_get_contents(path, &stat, &length, NULL) || !stat)
		return 0;
	end = strrchr(stat, ')');
	if (end)
		sscanf(end + 2, "%c %u", &state, &parent);
	g_free(stat);
	return parent;
}

static gboolean pid_is_ancestor(guint ancestor, guint pid)
{
	guint current = pid;

	if (!ancestor || !pid)
		return FALSE;
	for (gint depth = 0; depth < 24 && current > 1; depth++) {
		if (current == ancestor)
			return TRUE;
		current = proc_ppid(current);
		if (!current)
			break;
	}
	return FALSE;
}

static gboolean processes_related(guint left, guint right)
{
	if (!left || !right)
		return FALSE;
	return left == right || pid_is_ancestor(left, right) ||
	       pid_is_ancestor(right, left);
}

static gchar *fold_text(const gchar *text)
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
	a = fold_text(left);
	b = fold_text(right);
	if (strlen(a) >= 3 && strlen(b) >= 3) {
		if (g_strcmp0(a, b) == 0)
			score = 360;
		else if (strstr(a, b) || strstr(b, a))
			score = 240;
	}
	g_free(a);
	g_free(b);
	return score;
}

static gboolean names_equal(const gchar *left, const gchar *right)
{
	return left && right && *left && *right &&
	       g_ascii_strcasecmp(left, right) == 0;
}

static gchar *x11_text_property(Window window, const char *name)
{
	Atom property;
	Atom actual_type = None;
	int actual_format = 0;
	unsigned long count = 0;
	unsigned long remaining = 0;
	unsigned char *data = NULL;
	gchar *result = NULL;

	property = XInternAtom(dpy, name, True);
	if (property == None)
		return NULL;
	if (XGetWindowProperty(
		dpy, window, property, 0, 4096, False, AnyPropertyType,
		&actual_type, &actual_format, &count, &remaining, &data) == Success &&
	    data && actual_format == 8 && count > 0)
		result = g_strndup((const gchar *)data, count);
	if (data)
		XFree(data);
	return result;
}

static guint x11_window_pid(Window window)
{
	Atom property;
	Atom actual_type = None;
	int actual_format = 0;
	unsigned long count = 0;
	unsigned long remaining = 0;
	unsigned char *data = NULL;
	guint pid = 0;

	property = XInternAtom(dpy, "_NET_WM_PID", True);
	if (property == None)
		return 0;
	if (XGetWindowProperty(
		dpy, window, property, 0, 1, False, XA_CARDINAL,
		&actual_type, &actual_format, &count, &remaining, &data) == Success &&
	    data && actual_format == 32 && count == 1)
		pid = (guint)(*(unsigned long *)data);
	if (data)
		XFree(data);
	return pid;
}

static gboolean x11_target_read(X11Target *target)
{
	Window root;
	Window child = None;
	Atom property;
	Atom actual_type = None;
	int actual_format = 0;
	unsigned long count = 0;
	unsigned long remaining = 0;
	unsigned char *data = NULL;
	XWindowAttributes attributes;
	XClassHint class_hint;
	char *legacy_title = NULL;

	memset(target, 0, sizeof(*target));
	if (!dpy)
		return FALSE;
	root = DefaultRootWindow(dpy);
	property = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
	if (property == None)
		return FALSE;
	if (XGetWindowProperty(
		dpy, root, property, 0, 1, False, XA_WINDOW,
		&actual_type, &actual_format, &count, &remaining, &data) != Success ||
	    !data || actual_format != 32 || count != 1) {
		if (data)
			XFree(data);
		return FALSE;
	}
	target->xid = *(Window *)data;
	XFree(data);
	if (target->xid == None || target->xid == root)
		return FALSE;

	target->pid = x11_window_pid(target->xid);
	target->process_name = proc_comm(target->pid);
	target->exe_name = proc_exe_name(target->pid);
	target->title = x11_text_property(target->xid, "_NET_WM_NAME");
	if ((!target->title || !*target->title) &&
	    XFetchName(dpy, target->xid, &legacy_title)) {
		g_free(target->title);
		target->title = g_strdup(legacy_title);
		XFree(legacy_title);
	}

	memset(&class_hint, 0, sizeof(class_hint));
	if (XGetClassHint(dpy, target->xid, &class_hint)) {
		target->wm_class = g_strdup_printf(
		    "%s %s",
		    class_hint.res_name ? class_hint.res_name : "",
		    class_hint.res_class ? class_hint.res_class : "");
		if (class_hint.res_name)
			XFree(class_hint.res_name);
		if (class_hint.res_class)
			XFree(class_hint.res_class);
	}

	if (XGetWindowAttributes(dpy, target->xid, &attributes)) {
		target->w = attributes.width;
		target->h = attributes.height;
		if (!XTranslateCoordinates(
			dpy, target->xid, root, 0, 0,
			&target->x, &target->y, &child)) {
			target->x = attributes.x;
			target->y = attributes.y;
		}
	}
	return TRUE;
}

static void x11_target_clear(X11Target *target)
{
	g_free(target->title);
	g_free(target->wm_class);
	g_free(target->process_name);
	g_free(target->exe_name);
	memset(target, 0, sizeof(*target));
}

static void accessible_rect(
    AtspiAccessible *accessible,
    gint *x, gint *y, gint *w, gint *h)
{
	AtspiComponent *component;
	AtspiRect *rect;

	*x = *y = *w = *h = 0;
	component = atspi_accessible_get_component(accessible);
	if (!component)
		return;
	rect = atspi_component_get_extents(
	    component, ATSPI_COORD_TYPE_SCREEN, NULL);
	if (rect) {
		*x = rect->x;
		*y = rect->y;
		*w = rect->width;
		*h = rect->height;
		g_free(rect);
	}
	g_object_unref(component);
}

static gint geometry_score(AtspiAccessible *window, const X11Target *target)
{
	gint x, y, w, h;
	gint left, top, right, bottom;
	gint64 intersection;
	gint64 smaller;
	gint score;

	accessible_rect(window, &x, &y, &w, &h);
	if (w <= 0 || h <= 0 || target->w <= 0 || target->h <= 0)
		return 0;
	left = MAX(x, target->x);
	top = MAX(y, target->y);
	right = MIN(x + w, target->x + target->w);
	bottom = MIN(y + h, target->y + target->h);
	if (right <= left || bottom <= top)
		return -180;
	intersection = (gint64)(right - left) * (bottom - top);
	smaller = MIN((gint64)w * h, (gint64)target->w * target->h);
	if (smaller <= 0)
		return 0;
	score = (gint)(((double)intersection / (double)smaller) * 320.0);
	if (ABS(x - target->x) <= 16 && ABS(y - target->y) <= 16)
		score += 50;
	if (ABS(w - target->w) <= 32 && ABS(h - target->h) <= 32)
		score += 50;
	return score;
}

static gboolean state_has(AtspiStateSet *states, AtspiStateType state)
{
	return states && atspi_state_set_contains(states, state);
}

static AtspiAccessible *match_atspi_window(void)
{
	AtspiAccessible *desktop;
	AtspiAccessible *best = NULL;
	X11Target target;
	gboolean have_target;
	gint best_score = -100000;
	gint geometry_only = 0;
	gchar *best_app = NULL;
	gchar *best_window = NULL;
	gchar *best_identity = NULL;
	guint best_pid = 0;
	GPtrArray *diagnostics = g_ptr_array_new_with_free_func(g_free);

	have_target = x11_target_read(&target);
	if (have_target) {
		fprintf(
		    stderr,
		    "AT-SPI: X11 target xid=0x%lx pid=%u process='%s' exe='%s' title='%s' class='%s' rect=%d,%d %dx%d\n",
		    (unsigned long)target.xid,
		    target.pid,
		    target.process_name ? target.process_name : "",
		    target.exe_name ? target.exe_name : "",
		    target.title ? target.title : "",
		    target.wm_class ? target.wm_class : "",
		    target.x, target.y, target.w, target.h);
	}

	desktop = atspi_get_desktop(0);
	if (!desktop) {
		x11_target_clear(&target);
		g_ptr_array_free(diagnostics, TRUE);
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
			gchar *candidate_comm;
			gchar *candidate_exe;
			guint window_pid;
			gboolean active;
			gboolean visible;
			gboolean pid_match = FALSE;
			gboolean family_match = FALSE;
			gboolean related_match = FALSE;
			gboolean identity_match = FALSE;
			gint title_score = 0;
			gint class_score = 0;
			gint process_score = 0;
			gint geo_score = 0;
			gint score = 0;
			GString *identity;
			gint wx, wy, ww, wh;

			if (!window)
				continue;
			states = atspi_accessible_get_state_set(window);
			active = state_has(states, ATSPI_STATE_ACTIVE);
			visible = state_has(states, ATSPI_STATE_SHOWING) ||
			          state_has(states, ATSPI_STATE_VISIBLE);
			window_name = atspi_accessible_get_name(window, NULL);
			window_pid = atspi_accessible_get_process_id(window, NULL);
			if (!window_pid)
				window_pid = app_pid;
			candidate_comm = proc_comm(window_pid);
			candidate_exe = proc_exe_name(window_pid);
			accessible_rect(window, &wx, &wy, &ww, &wh);

			if (diagnostics->len < 24 && (visible || active)) {
				g_ptr_array_add(
				    diagnostics,
				    g_strdup_printf(
				        "AT-SPI: candidate app='%s' window='%s' pid=%u process='%s' exe='%s' active=%d visible=%d rect=%d,%d %dx%d",
				        app_name ? app_name : "",
				        window_name ? window_name : "",
				        window_pid,
				        candidate_comm ? candidate_comm : "",
				        candidate_exe ? candidate_exe : "",
				        active, visible, wx, wy, ww, wh));
			}

			identity = g_string_new(NULL);
			if (have_target) {
				pid_match = target.pid &&
				            (window_pid == target.pid || app_pid == target.pid);
				related_match = processes_related(target.pid, window_pid) ||
				                processes_related(target.pid, app_pid);
				family_match =
				    names_equal(target.process_name, candidate_comm) ||
				    names_equal(target.exe_name, candidate_exe);
				title_score = text_match_score(window_name, target.title);
				class_score = MAX(
				    text_match_score(app_name, target.wm_class),
				    text_match_score(window_name, target.wm_class));
				process_score = MAX(
				    MAX(text_match_score(candidate_comm, target.process_name),
				        text_match_score(candidate_exe, target.exe_name)),
				    MAX(text_match_score(app_name, target.process_name),
				        text_match_score(window_name, target.process_name)));
				identity_match = pid_match || related_match || family_match ||
				                 title_score >= 240 || class_score >= 240 ||
				                 process_score >= 240;

				if (pid_match) {
					score += 1000;
					g_string_append(identity, "pid,");
				}
				if (related_match) {
					score += 760;
					g_string_append(identity, "process-tree,");
				}
				if (family_match) {
					score += 620;
					g_string_append(identity, "process-family,");
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
					g_string_append(identity, "process-name,");
				}
				geo_score = geometry_score(window, &target);
				if (!identity_match) {
					if (geo_score > 0)
						geometry_only++;
					g_string_free(identity, TRUE);
					g_free(candidate_comm);
					g_free(candidate_exe);
					g_free(window_name);
					if (states)
						g_object_unref(states);
					g_object_unref(window);
					continue;
				}
				score += geo_score;
				if (active)
					score += 80;
			} else {
				identity_match = active;
				if (active) {
					score = 1200;
					g_string_append(identity, "active,");
				}
			}

			if (!visible && !active && geo_score < 80) {
				g_string_free(identity, TRUE);
				g_free(candidate_comm);
				g_free(candidate_exe);
				g_free(window_name);
				if (states)
					g_object_unref(states);
				g_object_unref(window);
				continue;
			}
			if (visible)
				score += 25;

			if (identity_match && score > best_score) {
				if (best)
					g_object_unref(best);
				best = g_object_ref(window);
				best_score = score;
				g_free(best_app);
				g_free(best_window);
				g_free(best_identity);
				best_app = g_strdup(app_name ? app_name : "");
				best_window = g_strdup(window_name ? window_name : "");
				best_identity = g_string_free(identity, FALSE);
				identity = NULL;
				best_pid = window_pid;
			}

			if (identity)
				g_string_free(identity, TRUE);
			g_free(candidate_comm);
			g_free(candidate_exe);
			g_free(window_name);
			if (states)
				g_object_unref(states);
			g_object_unref(window);
		}
		g_free(app_name);
		g_object_unref(app);
	}
	g_object_unref(desktop);

	if (best && best_score < 240) {
		g_object_unref(best);
		best = NULL;
	}
	if (best) {
		fprintf(
		    stderr,
		    "AT-SPI: matched app='%s' window='%s' pid=%u identity='%s' score=%d\n",
		    best_app ? best_app : "",
		    best_window ? best_window : "",
		    best_pid,
		    best_identity ? best_identity : "",
		    best_score);
	} else {
		fprintf(
		    stderr,
		    "AT-SPI: no identity match for X11 active window; ignored %d geometry-only candidate(s)\n",
		    geometry_only);
		for (guint i = 0; i < diagnostics->len; i++)
			fprintf(stderr, "%s\n", (gchar *)g_ptr_array_index(diagnostics, i));
	}

	g_ptr_array_free(diagnostics, TRUE);
	g_free(best_app);
	g_free(best_window);
	g_free(best_identity);
	x11_target_clear(&target);
	return best;
}

static gboolean element_visible(AtspiAccessible *accessible)
{
	AtspiStateSet *states = atspi_accessible_get_state_set(accessible);
	gboolean visible;

	if (!states)
		return TRUE;
	visible = state_has(states, ATSPI_STATE_SHOWING) ||
	          state_has(states, ATSPI_STATE_VISIBLE);
	g_object_unref(states);
	return visible;
}

static gboolean role_interactive(const gchar *role)
{
	static const gchar *roles[] = {
		"push button", "button", "toggle button", "check box",
		"radio button", "link", "entry", "password text",
		"combo box", "menu item", "check menu item", "radio menu item",
		"page tab", "spin button", "slider", "scroll bar",
		"tree item", "list item", "table cell", NULL,
	};

	if (!role)
		return FALSE;
	for (gint i = 0; roles[i]; i++)
		if (g_ascii_strcasecmp(role, roles[i]) == 0)
			return TRUE;
	return FALSE;
}

static gboolean role_container(const gchar *role)
{
	static const gchar *roles[] = {
		"application", "frame", "dialog", "panel", "section",
		"html container", "document web", "document frame", "paragraph",
		"filler", "grouping", "scroll pane", NULL,
	};

	if (!role)
		return TRUE;
	for (gint i = 0; roles[i]; i++)
		if (g_ascii_strcasecmp(role, roles[i]) == 0)
			return TRUE;
	return FALSE;
}

static gboolean has_action(AtspiAccessible *accessible)
{
	AtspiAction *action = atspi_accessible_get_action(accessible);

	if (!action)
		return FALSE;
	g_object_unref(action);
	return TRUE;
}

static gboolean has_editable_text(AtspiAccessible *accessible)
{
	AtspiEditableText *editable =
	    atspi_accessible_get_editable_text(accessible);

	if (!editable)
		return FALSE;
	g_object_unref(editable);
	return TRUE;
}

static gboolean accessible_interactive(
    AtspiAccessible *accessible,
    const gchar *role)
{
	AtspiStateSet *states;
	gboolean state_interactive = FALSE;

	if (role_interactive(role) || has_action(accessible) ||
	    has_editable_text(accessible))
		return TRUE;
	if (role_container(role))
		return FALSE;
	states = atspi_accessible_get_state_set(accessible);
	if (states) {
		state_interactive = state_has(states, ATSPI_STATE_FOCUSABLE) ||
		                    state_has(states, ATSPI_STATE_SELECTABLE) ||
		                    state_has(states, ATSPI_STATE_EDITABLE);
		g_object_unref(states);
	}
	return state_interactive;
}

static gboolean overlaps_window(
    const CollectContext *context,
    gint x, gint y, gint w, gint h)
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

static char *copy_glib_text(gchar *text, const char *fallback)
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

static void collect_elements(
    AtspiAccessible *node,
    gint depth,
    CollectContext *context)
{
	gint x, y, w, h;
	gint child_count;
	gchar *role;
	gboolean visible;

	if (!node || depth > context->max_depth ||
	    (gint)context->elements->len >= context->max_elements)
		return;

	visible = element_visible(node);
	accessible_rect(node, &x, &y, &w, &h);
	role = atspi_accessible_get_role_name(node, NULL);
	if (visible && w >= context->min_width && h >= context->min_height &&
	    (gint64)w * h >= context->min_area &&
	    overlaps_window(context, x, y, w, h) &&
	    accessible_interactive(node, role)) {
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
				element->name = copy_glib_text(
				    name, role ? role : "UI element");
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
			collect_elements(child, depth + 1, context);
			g_object_unref(child);
		}
	}
}

static void free_element(struct ui_element *element)
{
	if (!element)
		return;
	free(element->name);
	free(element->role);
	free(element);
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
	window = match_atspi_window();
	if (!window) {
		result->error = -1;
		snprintf(
		    result->error_msg, sizeof(result->error_msg),
		    "No identity-matched AT-SPI window for the X11 active window");
		return result;
	}

	memset(&context, 0, sizeof(context));
	context.elements = g_ptr_array_new();
	context.seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	context.max_depth = MAX(config_get_int("ui_max_depth"), 12);
	context.max_elements = MAX(config_get_int("ui_max_elements"), 1);
	context.min_width = MAX(config_get_int("ui_min_width"), 3);
	context.min_height = MAX(config_get_int("ui_min_height"), 3);
	context.min_area = MAX(config_get_int("ui_min_area"), 16);
	accessible_rect(
	    window,
	    &context.win_x, &context.win_y,
	    &context.win_w, &context.win_h);
	collect_elements(window, 0, &context);
	g_object_unref(window);

	if (context.elements->len == 0) {
		result->error = -2;
		snprintf(
		    result->error_msg, sizeof(result->error_msg),
		    "Matched AT-SPI window exposed no interactive elements");
		g_hash_table_destroy(context.seen);
		g_ptr_array_free(context.elements, TRUE);
		return result;
	}

	result->elements = calloc(context.elements->len, sizeof(*result->elements));
	if (!result->elements) {
		result->error = -3;
		snprintf(result->error_msg, sizeof(result->error_msg),
		         "Memory allocation failed");
		for (guint i = 0; i < context.elements->len; i++)
			free_element(g_ptr_array_index(context.elements, i));
		g_hash_table_destroy(context.seen);
		g_ptr_array_free(context.elements, TRUE);
		return result;
	}

	for (guint i = 0; i < context.elements->len; i++) {
		struct ui_element *element = g_ptr_array_index(context.elements, i);
		result->elements[i] = *element;
		free(element);
	}
	result->count = context.elements->len;
	result->error = 0;
	fprintf(
	    stderr,
	    "AT-SPI: collected %zu interactive elements from matched window\n",
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
		snprintf(result->error_msg, sizeof(result->error_msg),
		         "X11 is not available");
	}
	return result;
}

#endif
