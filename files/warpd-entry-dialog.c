#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

static void apply_dark_theme(void)
{
	GtkSettings *settings;
	GtkCssProvider *provider;
	GdkScreen *screen;
	GError *error = NULL;
	const char *css =
	    "dialog, window {"
	    "  background-color: #171717;"
	    "  color: #f2f2f2;"
	    "}"
	    "label {"
	    "  color: #f2f2f2;"
	    "}"
	    "textview, textview text {"
	    "  background-color: #252525;"
	    "  color: #ffffff;"
	    "  caret-color: #ffffff;"
	    "}"
	    "scrolledwindow {"
	    "  border: 1px solid #555555;"
	    "}"
	    "button {"
	    "  background-image: none;"
	    "  background-color: #303030;"
	    "  color: #ffffff;"
	    "  border-color: #555555;"
	    "}"
	    "button:hover {"
	    "  background-color: #3d3d3d;"
	    "}";

	settings = gtk_settings_get_default();
	if (settings)
		g_object_set(
		    settings,
		    "gtk-application-prefer-dark-theme",
		    TRUE,
		    NULL);

	screen = gdk_screen_get_default();
	if (!screen)
		return;

	provider = gtk_css_provider_new();
	if (!gtk_css_provider_load_from_data(provider, css, -1, &error)) {
		fprintf(
		    stderr,
		    "WARNING: Could not load dark GTK theme: %s\n",
		    error ? error->message : "unknown error");
		g_clear_error(&error);
		g_object_unref(provider);
		return;
	}

	gtk_style_context_add_provider_for_screen(
	    screen,
	    GTK_STYLE_PROVIDER(provider),
	    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);
}

static gboolean editor_key_press(
    GtkWidget *widget,
    GdkEventKey *event,
    gpointer user_data)
{
	GtkDialog *dialog = GTK_DIALOG(user_data);
	(void)widget;

	if (event->keyval == GDK_KEY_Escape) {
		gtk_dialog_response(dialog, GTK_RESPONSE_CANCEL);
		return TRUE;
	}

	if ((event->state & GDK_CONTROL_MASK) &&
	    (event->keyval == GDK_KEY_Return ||
	     event->keyval == GDK_KEY_KP_Enter)) {
		gtk_dialog_response(dialog, GTK_RESPONSE_OK);
		return TRUE;
	}

	return FALSE;
}

static gchar *get_initial_text(void)
{
	const char *environment_text = getenv("WARPD_ENTRY_TEXT");
	GtkClipboard *primary;
	gchar *primary_text;

	if (environment_text && *environment_text)
		return g_strdup(environment_text);

	/* GTK negotiates UTF8_STRING correctly with terminals and browsers. */
	primary = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
	primary_text = gtk_clipboard_wait_for_text(primary);
	return primary_text;
}

int main(int argc, char **argv)
{
	GtkWidget *dialog;
	GtkWidget *content;
	GtkWidget *label;
	GtkWidget *scrolled;
	GtkWidget *text_view;
	GtkTextBuffer *buffer;
	GtkTextIter start;
	GtkTextIter end;
	GtkTextIter cursor;
	gchar *initial_text;
	gchar *text;
	int response;

	if (!gtk_init_check(&argc, &argv)) {
		fprintf(stderr, "ERROR: Could not initialize GTK3 text editor.\n");
		return 1;
	}

	apply_dark_theme();

	dialog = gtk_dialog_new_with_buttons(
	    "Edit Text",
	    NULL,
	    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
	    "_Cancel",
	    GTK_RESPONSE_CANCEL,
	    "_OK",
	    GTK_RESPONSE_OK,
	    NULL);

	gtk_window_set_default_size(GTK_WINDOW(dialog), 760, 480);
	gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

	content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_container_set_border_width(GTK_CONTAINER(content), 12);

	label = gtk_label_new(
	    "Edit text below. Enter inserts a new line; Ctrl+Enter submits; Esc cancels.");
	gtk_widget_set_halign(label, GTK_ALIGN_START);

	scrolled = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(
	    GTK_SCROLLED_WINDOW(scrolled),
	    GTK_POLICY_AUTOMATIC,
	    GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scrolled, TRUE);
	gtk_widget_set_hexpand(scrolled, TRUE);

	text_view = gtk_text_view_new();
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text_view), 10);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text_view), 10);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text_view), 10);
	gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(text_view), 10);
	gtk_container_add(GTK_CONTAINER(scrolled), text_view);

	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
	initial_text = get_initial_text();
	if (initial_text && *initial_text) {
		gtk_text_buffer_set_text(buffer, initial_text, -1);
		gtk_text_buffer_get_end_iter(buffer, &cursor);
		gtk_text_buffer_place_cursor(buffer, &cursor);
	}
	g_free(initial_text);

	g_signal_connect(
	    text_view,
	    "key-press-event",
	    G_CALLBACK(editor_key_press),
	    dialog);

	gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 6);
	gtk_box_pack_start(GTK_BOX(content), scrolled, TRUE, TRUE, 6);

	gtk_widget_show_all(dialog);
	gtk_widget_grab_focus(text_view);

	response = gtk_dialog_run(GTK_DIALOG(dialog));
	if (response != GTK_RESPONSE_OK) {
		gtk_widget_destroy(dialog);
		return 1;
	}

	gtk_text_buffer_get_bounds(buffer, &start, &end);
	text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
	if (!text || !*text) {
		g_free(text);
		gtk_widget_destroy(dialog);
		return 1;
	}

	fwrite(text, 1, strlen(text), stdout);
	fflush(stdout);
	g_free(text);

	gtk_widget_destroy(dialog);
	return 0;
}
