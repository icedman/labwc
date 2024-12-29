#include "labwc.h"

#include <stdio.h>
#include <stdlib.h>

#include <gio/gio.h>
#include <glib.h>

typedef struct {
	struct server *server;

	GMainContext *context;
	GMainLoop *loop;
	guint owner_id;
	void *timer;
	int interval;

	GDBusConnection *connection;
	gchar *property_message;
	gint property_count;
} DBusService;

static DBusService *dbus;

static GDBusNodeInfo *introspection_data = NULL;

const gchar *introspection_xml =
	"<node>"
	"  <interface name='com.dwl.DBus.Interface'>"
	"    <method name='FocusWindow'>"
	"      <arg type='s' name='window' direction='in'/>"
	"      <arg type='s' name='window' direction='out'/>"
	"    </method>"
	"    <method name='GetWindows'>"
	"      <arg type='s' name='windows' direction='out'/>"
	"    </method>"
	"    <property name='Message' type='s' access='readwrite'/>"
	"    <property name='Count' type='i' access='read'/>"
	"    <signal name='WindowFocused'>"
	"      <arg type='s' name='window' direction='out'/>"
	"    </signal>"
	"    <signal name='WindowOpened'>"
	"      <arg type='s' name='window' direction='out'/>"
	"    </signal>"
	"    <signal name='WindowClosed'>"
	"      <arg type='s' name='window' direction='out'/>"
	"    </signal>"
	"  </interface>"
	"</node>";

#define SERVICE_NAME "com.dwl.DBus"
#define OBJECT_PATH "/com/dwl/DBus"
#define INTERFACE_NAME "com.dwl.DBus.Interface"

static void dbus_on_name_acquired(
	GDBusConnection *connection, const gchar *name, gpointer user_data);
static void dbus_on_name_lost(
	GDBusConnection *connection, const gchar *name, gpointer user_data);
static void dbus_on_bus_acquired(
	GDBusConnection *connection, const gchar *name, gpointer user_data);
int dbus_service_update(void *data);
int dbus_service_init(struct server *server);
void dbus_service_cleanup(void);

// Method handler
static void dbus_handle_method_call(GDBusConnection *connection,
	const gchar *sender, const gchar *object_path,
	const gchar *interface_name, const gchar *method_name,
	GVariant *parameters, GDBusMethodInvocation *invocation,
	gpointer user_data);
// Property handlers
static GVariant *dbus_get_property(GDBusConnection *connection,
	const gchar *sender, const gchar *object_path,
	const gchar *interface_name, const gchar *property_name, GError **error,
	gpointer user_data);
static gboolean dbus_set_property(GDBusConnection *connection,
	const gchar *sender, const gchar *object_path,
	const gchar *interface_name, const gchar *property_name,
	GVariant *value, GError **error, gpointer user_data);

// Emit the MessageChanged signal
void dbus_emit_client_signal(const char *signal, void *c);

// VTable
static const GDBusInterfaceVTable interface_vtable = {
	.method_call = dbus_handle_method_call,
	.get_property = dbus_get_property,
	.set_property = dbus_set_property,
};

GString *
gstring_append_client_json(GString *gstring, struct view *view)
{
	const char *fmt =
		"{ \"id\": \"0x%x\", \"title\": \"%s\", \"app_id\": \"%s\" }";

	GString *gstringTemp = g_string_new("");

	const char *appid = view_get_string_prop(view, "app_id");
	const char *title = view_get_string_prop(view, "title");

	g_string_assign(gstringTemp, title);
	g_string_replace(gstringTemp, "\"", "'", 0);
	g_string_append_printf(gstring, fmt, view, gstringTemp->str, appid);

	g_string_free(gstringTemp, TRUE);
	return gstring;
}

static void
dbus_handle_method_call(GDBusConnection *connection, const gchar *sender,
	const gchar *object_path, const gchar *interface_name,
	const gchar *method_name, GVariant *parameters,
	GDBusMethodInvocation *invocation, gpointer user_data)
{
	if (g_strcmp0(method_name, "GetWindows") == 0) {
		const char *response = "Hello from D-Bus!";
		g_print("HelloWorld method called by %s\n", sender);

		GString *gstring = g_string_new("[");

		struct view *view = NULL;
		wl_list_for_each(view, &dbus->server->views, link) {
			gstring_append_client_json(gstring, view);
			if (view->link.next != &dbus->server->views) {
				g_string_append_printf(gstring, ",");
			}
		}
		g_string_append_printf(gstring, "]");
		g_dbus_method_invocation_return_value(
			invocation, g_variant_new("(s)", gstring->str));
		g_string_free(gstring, TRUE);
	} else if (g_strcmp0(method_name, "FocusWindow") == 0) {
		const gchar *window;
		g_variant_get(parameters, "(s)", &window);

		uintptr_t address = strtol(window, NULL, 16); // Base 16 for

		struct view *view = NULL;
		wl_list_for_each(view, &dbus->server->views, link) {
			if ((uintptr_t)view == address) {
				desktop_focus_view(view, /*raise*/ true);
			}
		}
		// g_print("focus %s\n", window);
		g_dbus_method_invocation_return_value(
			invocation, g_variant_new("(s)", window));
		g_free(window);

	} else {
		g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
			G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method: %s",
			method_name);
	}
}

static GVariant *
dbus_get_property(GDBusConnection *connection, const gchar *sender,
	const gchar *object_path, const gchar *interface_name,
	const gchar *property_name, GError **error, gpointer user_data)
{
	if (g_strcmp0(property_name, "Message") == 0) {
		return g_variant_new_string(dbus->property_message
						    ? dbus->property_message
						    : "Default Message");
	} else if (g_strcmp0(property_name, "Count") == 0) {
		dbus->property_count = 0;
		// Monitor *m = NULL;
		// wl_list_for_each(m, &mons, link) {
		// 	dbus->property_count++;
		// }

		// Client *c = NULL;
		// wl_list_for_each(c, &clients, link) {
		// 	dbus->property_count++;
		// }

		return g_variant_new_int32(dbus->property_count);
	}
	return NULL; // Property not found
}

static gboolean
dbus_set_property(GDBusConnection *connection, const gchar *sender,
	const gchar *object_path, const gchar *interface_name,
	const gchar *property_name, GVariant *value, GError **error,
	gpointer user_data)
{
	if (g_strcmp0(property_name, "Message") == 0) {
		g_free(dbus->property_message);
		dbus->property_message =
			g_strdup(g_variant_get_string(value, NULL));
		return TRUE;
	}
	g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY,
		"The 'Count' property is read-only.");
	return FALSE;
}

void
dbus_emit_client_signal(const char *signal, void *c)
{
	g_print("emit %s\n", signal);
	GString *gstring = g_string_new("");
	gstring_append_client_json(gstring, c);

	g_dbus_connection_emit_signal(dbus->connection,
		NULL,		// No sender (broadcast to all clients)
		OBJECT_PATH,	// Object path
		INTERFACE_NAME, // Interface name
		signal,		// Signal name
		g_variant_new("(s)", gstring->str), // Arguments
		NULL);				    // No error
	g_string_free(gstring, TRUE);
}

static void
dbus_on_name_acquired(
	GDBusConnection *connection, const gchar *name, gpointer user_data)
{
	g_print("Service name '%s' acquired.\n", SERVICE_NAME);
}

static void
dbus_on_name_lost(
	GDBusConnection *connection, const gchar *name, gpointer user_data)
{
	g_print("Service name '%s' lost.\n", SERVICE_NAME);
}

static void
dbus_on_bus_acquired(
	GDBusConnection *connection, const gchar *name, gpointer user_data)
{
	GError *error = NULL;

	// Register the object
	guint registration_id =
		g_dbus_connection_register_object(connection, OBJECT_PATH,
			introspection_data->interfaces[0], &interface_vtable,
			NULL, // user data
			NULL, // user data free function
			&error);

	if (registration_id == 0) {
		g_printerr("Failed to register object: %s\n", error->message);
		g_error_free(error);
	}

	dbus->connection = connection;
}

int
dbus_service_update(void *data)
{
	g_main_context_iteration(dbus->context, FALSE);
	wl_event_source_timer_update(dbus->timer, dbus->interval);
	return 0;
}

int
dbus_service_init(struct server *server)
{
	dbus = malloc(sizeof(DBusService));

	DBusService *d = dbus;
	d->server = server;
	d->interval = 150;
	d->property_message = NULL;
	d->property_count = 0;

	d->loop = g_main_loop_new(NULL, FALSE);
	d->context = g_main_loop_get_context(d->loop);

	dbus->timer = wl_event_loop_add_timer(
		server->wl_event_loop, dbus_service_update, dbus);

	// Create introspection data
	introspection_data =
		g_dbus_node_info_new_for_xml(introspection_xml, NULL);

	// Acquire the bus name
	d->owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, SERVICE_NAME,
		G_BUS_NAME_OWNER_FLAGS_NONE, dbus_on_bus_acquired,
		dbus_on_name_acquired, dbus_on_name_lost, NULL, NULL);

	wl_event_source_timer_update(dbus->timer, dbus->interval);
	return 0;
}

void
dbus_service_cleanup(void)
{
	g_bus_unown_name(dbus->owner_id);
	g_main_loop_unref(dbus->loop);
	g_dbus_node_info_unref(introspection_data);
	free(dbus);
	dbus = NULL;
}