/*
 * warpd - enable the desktop accessibility service before AT-SPI detection.
 */
#include "atspi-status.h"

#include <gio/gio.h>
#include <stdio.h>

#define A11Y_BUS_NAME "org.a11y.Bus"
#define A11Y_BUS_PATH "/org/a11y/bus"
#define A11Y_STATUS_INTERFACE "org.a11y.Status"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"

static gboolean ensure_bus_started(GDBusConnection *connection)
{
	GError *error = NULL;
	GVariant *reply;

	reply = g_dbus_connection_call_sync(
	    connection,
	    A11Y_BUS_NAME,
	    A11Y_BUS_PATH,
	    A11Y_BUS_NAME,
	    "GetAddress",
	    NULL,
	    G_VARIANT_TYPE("(s)"),
	    G_DBUS_CALL_FLAGS_NONE,
	    3000,
	    NULL,
	    &error);
	if (!reply) {
		fprintf(
		    stderr,
		    "AT-SPI: unable to start accessibility bus: %s\n",
		    error ? error->message : "unknown error");
		g_clear_error(&error);
		return FALSE;
	}

	g_variant_unref(reply);
	return TRUE;
}

static gboolean read_enabled(
    GDBusConnection *connection,
    gboolean *enabled)
{
	GError *error = NULL;
	GVariant *reply;
	GVariant *property = NULL;

	reply = g_dbus_connection_call_sync(
	    connection,
	    A11Y_BUS_NAME,
	    A11Y_BUS_PATH,
	    DBUS_PROPERTIES_INTERFACE,
	    "Get",
	    g_variant_new("(ss)", A11Y_STATUS_INTERFACE, "IsEnabled"),
	    G_VARIANT_TYPE("(v)"),
	    G_DBUS_CALL_FLAGS_NONE,
	    3000,
	    NULL,
	    &error);
	if (!reply) {
		fprintf(
		    stderr,
		    "AT-SPI: unable to read org.a11y.Status.IsEnabled: %s\n",
		    error ? error->message : "unknown error");
		g_clear_error(&error);
		return FALSE;
	}

	g_variant_get(reply, "(v)", &property);
	if (!property || !g_variant_is_of_type(property, G_VARIANT_TYPE_BOOLEAN)) {
		fprintf(stderr, "AT-SPI: IsEnabled returned an invalid value\n");
		if (property)
			g_variant_unref(property);
		g_variant_unref(reply);
		return FALSE;
	}

	*enabled = g_variant_get_boolean(property);
	g_variant_unref(property);
	g_variant_unref(reply);
	return TRUE;
}

static gboolean write_enabled(GDBusConnection *connection)
{
	GError *error = NULL;
	GVariant *reply;

	reply = g_dbus_connection_call_sync(
	    connection,
	    A11Y_BUS_NAME,
	    A11Y_BUS_PATH,
	    DBUS_PROPERTIES_INTERFACE,
	    "Set",
	    g_variant_new(
	        "(ssv)",
	        A11Y_STATUS_INTERFACE,
	        "IsEnabled",
	        g_variant_new_boolean(TRUE)),
	    G_VARIANT_TYPE("()"),
	    G_DBUS_CALL_FLAGS_NONE,
	    3000,
	    NULL,
	    &error);
	if (!reply) {
		fprintf(
		    stderr,
		    "AT-SPI: unable to enable org.a11y.Status.IsEnabled: %s\n",
		    error ? error->message : "unknown error");
		g_clear_error(&error);
		return FALSE;
	}

	g_variant_unref(reply);
	return TRUE;
}

int atspi_ensure_global_accessibility(void)
{
	static gboolean reported_enabled = FALSE;
	GDBusConnection *connection;
	GError *error = NULL;
	gboolean enabled = FALSE;
	gboolean success = FALSE;

	connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	if (!connection) {
		fprintf(
		    stderr,
		    "AT-SPI: unable to connect to the session D-Bus: %s\n",
		    error ? error->message : "unknown error");
		g_clear_error(&error);
		return 0;
	}

	if (!ensure_bus_started(connection))
		goto out;
	if (!read_enabled(connection, &enabled))
		goto out;

	if (enabled) {
		if (!reported_enabled) {
			fprintf(stderr, "AT-SPI: global accessibility status is enabled\n");
			reported_enabled = TRUE;
		}
		success = TRUE;
		goto out;
	}

	fprintf(
	    stderr,
	    "AT-SPI: global accessibility status is disabled; enabling it now\n");
	if (!write_enabled(connection))
		goto out;

	/* Allow the launcher to update GSettings and notify running applications. */
	g_usleep(300 * 1000);
	if (!read_enabled(connection, &enabled) || !enabled) {
		fprintf(stderr, "AT-SPI: accessibility status did not become enabled\n");
		goto out;
	}

	fprintf(
	    stderr,
	    "AT-SPI: global accessibility enabled; restart already-running Firefox/Vivaldi once\n");
	reported_enabled = TRUE;
	success = TRUE;

out:
	g_object_unref(connection);
	return success ? 1 : 0;
}
