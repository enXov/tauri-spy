/*
 * libspy.so — WebKitGTK DevTools injection library
 *
 * Loaded via LD_PRELOAD to enable developer extras in Tauri release builds.
 * Hooks gtk_main() to install an idle callback that traverses the GTK widget
 * tree, finds WebKitWebView instances, and enables the web inspector.
 *
 * Also installs a Ctrl+Shift+I keyboard handler for toggling the inspector.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

static int spy_enabled = 0;
static int auto_open = 0;

/* Store discovered webviews so the shortcut handler can toggle them */
#define MAX_WEBVIEWS 16
static WebKitWebView *discovered_webviews[MAX_WEBVIEWS];
static int webview_count = 0;

/* Real gtk_main — resolved via dlsym */
typedef void (*gtk_main_fn)(void);
static gtk_main_fn real_gtk_main = NULL;

static void enable_devtools_on_webview(WebKitWebView *view) {
  WebKitSettings *settings = webkit_web_view_get_settings(view);
  if (settings) {
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    fprintf(stderr, "[tauri-spy] DevTools enabled on WebKitWebView %p\n",
            (void *)view);
  }

  /* Track this webview for keyboard shortcut toggling */
  if (webview_count < MAX_WEBVIEWS) {
    /* Avoid duplicates */
    for (int i = 0; i < webview_count; i++) {
      if (discovered_webviews[i] == view)
        return;
    }
    discovered_webviews[webview_count++] = view;
  }

  if (auto_open) {
    WebKitWebInspector *inspector = webkit_web_view_get_inspector(view);
    if (inspector) {
      webkit_web_inspector_show(inspector);
      fprintf(stderr, "[tauri-spy] Inspector auto-opened\n");
    }
  }
}

static void traverse_children(GtkContainer *container) {
  GList *children = gtk_container_get_children(container);
  if (!children)
    return;

  for (GList *l = children; l != NULL; l = l->next) {
    GtkWidget *child = GTK_WIDGET(l->data);
    if (!child)
      continue;

    /* Check if this widget is a WebKitWebView */
    if (WEBKIT_IS_WEB_VIEW(child)) {
      enable_devtools_on_webview(WEBKIT_WEB_VIEW(child));
    }

    /* Recurse into containers */
    if (GTK_IS_CONTAINER(child)) {
      traverse_children(GTK_CONTAINER(child));
    }
  }

  g_list_free(children);
}

/*
 * Keyboard handler: Ctrl+Shift+I toggles the web inspector.
 */
static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event,
                             gpointer data) {
  (void)widget;
  (void)data;

  /* Check for Ctrl+Shift+I */
  if ((event->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) ==
          (GDK_CONTROL_MASK | GDK_SHIFT_MASK) &&
      (event->keyval == GDK_KEY_I || event->keyval == GDK_KEY_i)) {

    fprintf(stderr, "[tauri-spy] Ctrl+Shift+I pressed — toggling inspector\n");

    for (int i = 0; i < webview_count; i++) {
      WebKitWebView *view = discovered_webviews[i];
      if (!view)
        continue;

      WebKitWebInspector *inspector = webkit_web_view_get_inspector(view);
      if (!inspector)
        continue;

      if (webkit_web_inspector_is_attached(inspector)) {
        webkit_web_inspector_close(inspector);
      } else {
        webkit_web_inspector_show(inspector);
      }
    }

    return TRUE; /* Event handled */
  }

  return FALSE; /* Pass event through */
}

static gboolean idle_callback(gpointer data) {
  (void)data;

  if (spy_enabled)
    return FALSE; /* Already done */

  /* Check if WebKitWebView type is registered yet */
  GType webkit_type = g_type_from_name("WebKitWebView");
  if (webkit_type == 0)
    return TRUE; /* Not registered yet, try again */

  GList *toplevels = gtk_window_list_toplevels();
  if (!toplevels)
    return TRUE; /* No windows yet, try again */

  guint len = g_list_length(toplevels);
  if (len == 0) {
    g_list_free(toplevels);
    return TRUE;
  }

  for (GList *l = toplevels; l != NULL; l = l->next) {
    GtkWidget *win = GTK_WIDGET(l->data);
    if (!win)
      continue;

    if (GTK_IS_CONTAINER(win)) {
      traverse_children(GTK_CONTAINER(win));
    }

    /* Connect keyboard shortcut handler to each top-level window */
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key_press), NULL);
  }

  g_list_free(toplevels);
  spy_enabled = 1;
  fprintf(
      stderr,
      "[tauri-spy] Injection complete — Ctrl+Shift+I to toggle inspector\n");
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

  /* Check for auto-open flag */
  const char *env_auto = getenv("TAURI_SPY_AUTO_OPEN");
  if (env_auto && strcmp(env_auto, "1") == 0) {
    auto_open = 1;
  }

  fprintf(stderr, "[tauri-spy] Hooked gtk_main() — installing idle callback\n");

  g_idle_add(idle_callback, NULL);

  /* Call the real gtk_main() */
  real_gtk_main();
}
