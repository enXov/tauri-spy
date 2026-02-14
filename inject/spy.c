/*
 * libspy.so — WebKitGTK DevTools injection library
 *
 * Loaded via LD_PRELOAD to enable developer extras in Tauri release builds.
 * Hooks gtk_main() to install an idle callback that traverses the GTK widget
 * tree, finds WebKitWebView instances, and enables the web inspector.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GLib / GTK / WebKitGTK type aliases (avoid pulling in full headers) */
typedef void* gpointer;
typedef int gboolean;
typedef unsigned int guint;
typedef void GtkWidget;
typedef void GtkContainer;
typedef void GList;
typedef void WebKitWebView;
typedef void WebKitSettings;
typedef void WebKitWebInspector;

#define TRUE  1
#define FALSE 0

/* Function pointer types */
typedef void  (*gtk_main_fn)(void);
typedef GList* (*gtk_window_list_toplevels_fn)(void);
typedef gboolean (*gtk_widget_is_toplevel_fn)(GtkWidget*);
typedef GList* (*gtk_container_get_children_fn)(GtkContainer*);
typedef gpointer (*g_list_nth_data_fn)(GList*, guint);
typedef guint (*g_list_length_fn)(GList*);
typedef void  (*g_list_free_fn)(GList*);
typedef guint (*g_idle_add_fn)(gboolean (*func)(gpointer), gpointer);
typedef unsigned long (*g_type_check_instance_is_a_fn)(void*, unsigned long);
typedef unsigned long (*g_type_from_name_fn)(const char*);

typedef WebKitSettings* (*webkit_web_view_get_settings_fn)(WebKitWebView*);
typedef void (*webkit_settings_set_enable_developer_extras_fn)(WebKitSettings*, gboolean);
typedef WebKitWebInspector* (*webkit_web_view_get_inspector_fn)(WebKitWebView*);
typedef void (*webkit_web_inspector_show_fn)(WebKitWebInspector*);

/* Cached function pointers */
static gtk_main_fn                     real_gtk_main = NULL;
static gtk_window_list_toplevels_fn    real_gtk_window_list_toplevels = NULL;
static gtk_container_get_children_fn   real_gtk_container_get_children = NULL;
static g_list_nth_data_fn              real_g_list_nth_data = NULL;
static g_list_length_fn                real_g_list_length = NULL;
static g_list_free_fn                  real_g_list_free = NULL;
static g_idle_add_fn                   real_g_idle_add = NULL;
static g_type_check_instance_is_a_fn   real_g_type_check_instance_is_a = NULL;
static g_type_from_name_fn             real_g_type_from_name = NULL;

static webkit_web_view_get_settings_fn             real_webkit_web_view_get_settings = NULL;
static webkit_settings_set_enable_developer_extras_fn real_webkit_settings_set_enable_developer_extras = NULL;
static webkit_web_view_get_inspector_fn            real_webkit_web_view_get_inspector = NULL;
static webkit_web_inspector_show_fn                real_webkit_web_inspector_show = NULL;

static int spy_enabled = 0;
static int auto_open = 0;

#define RESOLVE(handle, name) do { \
    real_##name = (name##_fn)dlsym(handle, #name); \
} while(0)

static void resolve_symbols(void) {
    void *handle = RTLD_DEFAULT;

    RESOLVE(handle, gtk_window_list_toplevels);
    RESOLVE(handle, gtk_container_get_children);
    RESOLVE(handle, g_list_nth_data);
    RESOLVE(handle, g_list_length);
    RESOLVE(handle, g_list_free);
    RESOLVE(handle, g_idle_add);
    RESOLVE(handle, g_type_check_instance_is_a);
    RESOLVE(handle, g_type_from_name);

    RESOLVE(handle, webkit_web_view_get_settings);
    RESOLVE(handle, webkit_settings_set_enable_developer_extras);
    RESOLVE(handle, webkit_web_view_get_inspector);
    RESOLVE(handle, webkit_web_inspector_show);
}

static void enable_devtools_on_webview(WebKitWebView *view) {
    if (!real_webkit_web_view_get_settings || !real_webkit_settings_set_enable_developer_extras)
        return;

    WebKitSettings *settings = real_webkit_web_view_get_settings(view);
    if (settings) {
        real_webkit_settings_set_enable_developer_extras(settings, TRUE);
        fprintf(stderr, "[tauri-spy] DevTools enabled on WebKitWebView %p\n", view);
    }

    if (auto_open && real_webkit_web_view_get_inspector && real_webkit_web_inspector_show) {
        WebKitWebInspector *inspector = real_webkit_web_view_get_inspector(view);
        if (inspector) {
            real_webkit_web_inspector_show(inspector);
            fprintf(stderr, "[tauri-spy] Inspector auto-opened\n");
        }
    }
}

static void traverse_children(GtkContainer *container, unsigned long webkit_type) {
    if (!real_gtk_container_get_children)
        return;

    GList *children = real_gtk_container_get_children(container);
    if (!children)
        return;

    guint len = real_g_list_length(children);
    for (guint i = 0; i < len; i++) {
        GtkWidget *child = (GtkWidget*)real_g_list_nth_data(children, i);
        if (!child)
            continue;

        /* Check if this widget is a WebKitWebView */
        if (real_g_type_check_instance_is_a && webkit_type != 0) {
            if (real_g_type_check_instance_is_a(child, webkit_type)) {
                enable_devtools_on_webview((WebKitWebView*)child);
            }
        }

        /* Recurse into containers */
        unsigned long container_type = real_g_type_from_name ? real_g_type_from_name("GtkContainer") : 0;
        if (container_type && real_g_type_check_instance_is_a &&
            real_g_type_check_instance_is_a(child, container_type)) {
            traverse_children((GtkContainer*)child, webkit_type);
        }
    }

    real_g_list_free(children);
}

static gboolean idle_callback(gpointer data) {
    (void)data;

    if (spy_enabled)
        return FALSE; /* Already done */

    if (!real_gtk_window_list_toplevels || !real_g_type_from_name)
        return FALSE;

    unsigned long webkit_type = real_g_type_from_name("WebKitWebView");
    if (webkit_type == 0)
        return TRUE; /* WebKitWebView type not registered yet, try again */

    GList *toplevels = real_gtk_window_list_toplevels();
    if (!toplevels)
        return TRUE; /* No windows yet, try again */

    guint len = real_g_list_length(toplevels);
    if (len == 0) {
        real_g_list_free(toplevels);
        return TRUE;
    }

    for (guint i = 0; i < len; i++) {
        GtkWidget *win = (GtkWidget*)real_g_list_nth_data(toplevels, i);
        if (!win)
            continue;

        unsigned long container_type = real_g_type_from_name("GtkContainer");
        if (container_type && real_g_type_check_instance_is_a &&
            real_g_type_check_instance_is_a(win, container_type)) {
            traverse_children((GtkContainer*)win, webkit_type);
        }
    }

    real_g_list_free(toplevels);
    spy_enabled = 1;
    fprintf(stderr, "[tauri-spy] Injection complete\n");
    return FALSE; /* Remove idle callback */
}

/*
 * Hook gtk_main() — called by the Tauri app when entering the GTK event loop.
 * We install our idle callback before handing control to the real gtk_main().
 */
void gtk_main(void) {
    if (!real_gtk_main) {
        real_gtk_main = (gtk_main_fn)dlsym(RTLD_NEXT, "gtk_main");
        if (!real_gtk_main) {
            fprintf(stderr, "[tauri-spy] FATAL: Could not find real gtk_main()\n");
            return;
        }
    }

    resolve_symbols();

    /* Check for auto-open flag */
    const char *env_auto = getenv("TAURI_SPY_AUTO_OPEN");
    if (env_auto && strcmp(env_auto, "1") == 0) {
        auto_open = 1;
    }

    fprintf(stderr, "[tauri-spy] Hooked gtk_main() — installing idle callback\n");

    if (real_g_idle_add) {
        real_g_idle_add(idle_callback, NULL);
    }

    /* Call the real gtk_main() */
    real_gtk_main();
}
