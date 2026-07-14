#include <gtk/gtk.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	GtkWidget *dialog;
	GtkWidget *content;
	GtkWidget *label;
	GtkWidget *entry;
	const char *text;
	int response;

	if (!gtk_init_check(&argc, &argv)) {
		fprintf(stderr, "ERROR: Could not initialize GTK3 entry dialog.\n");
		return 1;
	}

	dialog = gtk_dialog_new_with_buttons(
	    "Insert Text",
	    NULL,
	    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
	    "_Cancel",
	    GTK_RESPONSE_CANCEL,
	    "_OK",
	    GTK_RESPONSE_OK,
	    NULL);

	gtk_window_set_default_size(GTK_WINDOW(dialog), 520, -1);
	gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

	content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	gtk_container_set_border_width(GTK_CONTAINER(content), 12);

	label = gtk_label_new("Type text and press Enter:");
	gtk_widget_set_halign(label, GTK_ALIGN_START);

	entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);

	gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 6);
	gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 6);

	gtk_widget_show_all(dialog);
	gtk_widget_grab_focus(entry);

	response = gtk_dialog_run(GTK_DIALOG(dialog));
	if (response != GTK_RESPONSE_OK) {
		gtk_widget_destroy(dialog);
		return 1;
	}

	text = gtk_entry_get_text(GTK_ENTRY(entry));
	if (!text || !*text) {
		gtk_widget_destroy(dialog);
		return 1;
	}

	fputs(text, stdout);
	fputc('\n', stdout);
	fflush(stdout);

	gtk_widget_destroy(dialog);
	return 0;
}
