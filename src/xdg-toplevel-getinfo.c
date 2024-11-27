/*
 * Copyright (C) 2024 Bardia Moshiri
 * SPDX-License-Identifier: GPL-3.0+
 * Author: Bardia Moshiri <fakeshell@bardia.tech>
 */

#include <stdio.h>
#include <math.h>
#include <gio/gio.h>
#include "xdg-shell-client-protocol.h"

#define OSK_BUS_NAME "sm.puri.OSK0"
#define OSK_OBJECT_PATH "/sm/puri/OSK0"
#define OSK_INTERFACE "sm.puri.OSK0"

struct client_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    int width;
    int height;
    int running;
};

/* we could get this from org.gnome.Mutter.DisplayConfig or wlr-output-management-unstable-v1
 * but the dbus one is a crazy a((ssss)a(siiddada{sv})a{sv}) variant and the wlroots one needs the screen to be on */
static double
get_phosh_scale(void)
{
    GSettings *settings;
    GVariant *config;
    double scale = -1.0;

    settings = g_settings_new("sm.puri.phosh.monitors");
    if (!settings) {
        g_debug("Could not get phosh monitor settings");
        return scale;
    }

    config = g_settings_get_value(settings, "config");
    if (!config) {
        g_debug("Could not get config value");
        g_object_unref(settings);
        return scale;
    }

    GVariant *monitor = g_variant_lookup_value(config, "HWCOMPOSER-1", NULL);
    if (!monitor) {
        g_debug("Could not find HWCOMPOSER-1");
        g_variant_unref(config);
        g_object_unref(settings);
        return scale;
    }

    GVariant *scale_variant = g_variant_lookup_value(monitor, "scale", NULL);
    if (scale_variant) {
        scale = g_variant_get_double(scale_variant);
        g_debug("Found scale (direct): %f", scale);
        g_variant_unref(scale_variant);

        if (scale != floor(scale)) {
            scale = ceil(scale);
            g_debug("Rounded up to: %f", scale);
        }
    } else {
        g_debug("Could not find scale value");
    }

    g_variant_unref(monitor);
    g_variant_unref(config);
    g_object_unref(settings);

    return scale;
}

static void
xdg_toplevel_configure(void *data,
                       struct xdg_toplevel *xdg_toplevel,
                       int32_t width,
                       int32_t height,
                       struct wl_array *states)
{
    struct client_state *state = data;

    if (width == 0 || height == 0) {
        g_debug("Compositor is deferring size decision to us");
        return;
    }

    double scale = get_phosh_scale();
    if (scale < 0) {
        g_debug("Failed to get phosh scale");
    } else {
        state->width = (int)(width * scale);
        state->height = (int)(height * scale);
        g_debug("Base dimensions from compositor: %dx%d", width, height);
        g_print("%dx%d\n", state->width, state->height);
    }

    state->running = 0;
}

static void
xdg_toplevel_close(void *data,
                   struct xdg_toplevel *xdg_toplevel)
{
    struct client_state *state = data;
    state->running = 0;
}

static const struct
xdg_toplevel_listener xdg_toplevel_listener =
{
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

static void
xdg_surface_configure(void *data,
                      struct xdg_surface *xdg_surface,
                      uint32_t serial)
{
    struct client_state *state = data;
    xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct
xdg_surface_listener xdg_surface_listener =
{
    .configure = xdg_surface_configure,
};

static void
xdg_wm_base_ping(void *data,
                 struct xdg_wm_base *xdg_wm_base,
                 uint32_t serial)
{
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct
xdg_wm_base_listener xdg_wm_base_listener =
{
    .ping = xdg_wm_base_ping,
};

static void
registry_global(void *data,
                struct wl_registry *registry,
                uint32_t name,
                const char *interface,
                uint32_t version)
{
    struct client_state *state = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->xdg_wm_base = wl_registry_bind(registry, name,
                                             &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(state->xdg_wm_base,
                                &xdg_wm_base_listener, state);
    }
}

static void
registry_global_remove(void *data,
                       struct wl_registry *registry,
                       uint32_t name)
{
    // Handle removal of global objects
}

static const struct
wl_registry_listener registry_listener =
{
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* keyboard being open will make xdg toplevel return the wrong value, this is what happens in android too */
static void
ensure_keyboard_hidden(void)
{
    GError *error = NULL;
    GVariant *result;
    GDBusConnection *connection;

    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error != NULL) {
        g_debug("Error connecting to session bus: %s", error->message);
        g_error_free(error);
        return;
    }

    result = g_dbus_connection_call_sync(
        connection,
        OSK_BUS_NAME,
        OSK_OBJECT_PATH,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", OSK_INTERFACE, "Visible"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error
    );

    if (error != NULL) {
        g_debug("Error getting keyboard visibility: %s", error->message);
        g_error_free(error);
        g_object_unref(connection);
        return;
    }

    GVariant *variant;
    g_variant_get(result, "(v)", &variant);
    gboolean is_visible = g_variant_get_boolean(variant);
    g_variant_unref(variant);
    g_variant_unref(result);

    if (is_visible) {
        g_debug("Keyboard is visible. Setting to hidden");

        GVariant *param = g_variant_new("(b)", FALSE);
        result = g_dbus_connection_call_sync(
            connection,
            OSK_BUS_NAME,
            OSK_OBJECT_PATH,
            OSK_INTERFACE,
            "SetVisible",
            param,
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            NULL,
            &error
        );

        if (error != NULL) {
            g_debug("Error setting keyboard visibility: %s", error->message);
            g_error_free(error);
        } else if (result != NULL) {
            g_variant_unref(result);
            g_debug("Keyboard hidden successfully");
        }
    } else {
        g_debug("Keyboard is already hidden");
    }

    g_object_unref(connection);
    usleep(100000);
}

int
main(int argc,
     char *argv[])
{
    ensure_keyboard_hidden();

    struct client_state state = { 0 };

    state.display = wl_display_connect(NULL);
    if (!state.display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);

    if (!state.compositor || !state.xdg_wm_base) {
        fprintf(stderr, "Compositor doesn't support required interfaces\n");
        return 1;
    }

    state.surface = wl_compositor_create_surface(state.compositor);
    if (!state.surface) {
        fprintf(stderr, "Failed to create surface\n");
        return 1;
    }

    state.xdg_surface = xdg_wm_base_get_xdg_surface(state.xdg_wm_base,
                                                   state.surface);
    if (!state.xdg_surface) {
        fprintf(stderr, "Failed to create XDG surface\n");
        return 1;
    }
    xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);

    state.toplevel = xdg_surface_get_toplevel(state.xdg_surface);
    if (!state.toplevel) {
        fprintf(stderr, "Failed to create toplevel\n");
        return 1;
    }
    xdg_toplevel_add_listener(state.toplevel, &xdg_toplevel_listener, &state);

    wl_surface_commit(state.surface);
    wl_display_roundtrip(state.display);

    state.running = 1;
    for (int i = 0; i < 10 && state.running; i++) {
        wl_display_roundtrip(state.display);
    }

    if (state.toplevel)
        xdg_toplevel_destroy(state.toplevel);
    if (state.xdg_surface)
        xdg_surface_destroy(state.xdg_surface);
    if (state.surface)
        wl_surface_destroy(state.surface);
    if (state.xdg_wm_base)
        xdg_wm_base_destroy(state.xdg_wm_base);
    if (state.compositor)
        wl_compositor_destroy(state.compositor);
    wl_registry_destroy(state.registry);
    wl_display_disconnect(state.display);

    return 0;
}
