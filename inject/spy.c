/*
 * libspy.so — WebKitGTK DevTools injection library
 *
 * Loaded via LD_PRELOAD to enable developer extras in Tauri release builds.
 *
 * Hooks:
 *   - gtk_main() and g_application_run() to install an idle callback that
 *     traverses the GTK widget tree, finds WebKitWebView instances, and
 *     enables the web inspector.
 *   - webkit_settings_set_enable_developer_extras() to prevent the target
 *     app from disabling DevTools after we enable them.
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
static int idle_installed = 0;
static int auto_open = 0;
static int retry_count = 0;
#define MAX_RETRIES 200

/* Store discovered webviews so the shortcut handler can toggle them */
#define MAX_WEBVIEWS 16
static WebKitWebView *discovered_webviews[MAX_WEBVIEWS];
static int webview_count = 0;

/* Real function pointers — resolved via dlsym */
typedef void (*gtk_main_fn)(void);
static gtk_main_fn real_gtk_main = NULL;

typedef int (*g_application_run_fn)(GApplication *, int, char **);
static g_application_run_fn real_g_application_run = NULL;

typedef gboolean (*gtk_main_iteration_do_fn)(gboolean);
static gtk_main_iteration_do_fn real_gtk_main_iteration_do = NULL;

typedef void (*set_dev_extras_fn)(WebKitSettings *, gboolean);
static set_dev_extras_fn real_set_dev_extras = NULL;

/* Forward declarations */
static gboolean idle_callback(gpointer data);
static void install_idle_callback(void);

/*
 * Resolve the real webkit_settings_set_enable_developer_extras if needed.
 */
static void ensure_real_set_dev_extras(void) {
  if (!real_set_dev_extras) {
    real_set_dev_extras = (set_dev_extras_fn)dlsym(
        RTLD_NEXT, "webkit_settings_set_enable_developer_extras");
  }
}

/*
 * Hook: webkit_settings_set_enable_developer_extras
 * Always forces TRUE, preventing the target app from disabling DevTools.
 */
void webkit_settings_set_enable_developer_extras(WebKitSettings *settings,
                                                 gboolean enabled) {
  ensure_real_set_dev_extras();
  if (real_set_dev_extras) {
    real_set_dev_extras(settings, TRUE);
    if (!enabled) {
      fprintf(stderr, "[tauri-spy] Blocked attempt to disable DevTools — kept "
                      "enabled\n");
    }
  }
}

static void enable_devtools_on_webview(WebKitWebView *view) {
  WebKitSettings *settings = webkit_web_view_get_settings(view);
  if (settings) {
    /* Use the real function pointer to avoid calling our own hook */
    ensure_real_set_dev_extras();
    if (real_set_dev_extras) {
      real_set_dev_extras(settings, TRUE);
    }
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
  if (webkit_type == 0) {
    retry_count++;
    if (retry_count > MAX_RETRIES) {
      fprintf(stderr,
              "[tauri-spy] WARNING: WebKitWebView type never registered "
              "(not a Tauri/WebKit app?)\n");
      return FALSE;
    }
    return TRUE; /* Not registered yet, try again */
  }

  GList *toplevels = gtk_window_list_toplevels();
  if (!toplevels) {
    retry_count++;
    if (retry_count > MAX_RETRIES) {
      fprintf(stderr, "[tauri-spy] WARNING: No top-level windows found\n");
      return FALSE;
    }
    return TRUE; /* No windows yet, try again */
  }

  int found_webview = 0;

  for (GList *l = toplevels; l != NULL; l = l->next) {
    GtkWidget *win = GTK_WIDGET(l->data);
    if (!win)
      continue;

    if (GTK_IS_CONTAINER(win)) {
      int before = webview_count;
      traverse_children(GTK_CONTAINER(win));
      if (webview_count > before)
        found_webview = 1;
    }
  }

  if (!found_webview) {
    g_list_free(toplevels);
    retry_count++;
    if (retry_count > MAX_RETRIES) {
      fprintf(stderr,
              "[tauri-spy] WARNING: No WebKitWebView found in widget tree\n");
      return FALSE;
    }
    return TRUE; /* No webviews yet, try again */
  }

  /* Connect keyboard shortcut handler to each top-level window (once) */
  for (GList *l = toplevels; l != NULL; l = l->next) {
    GtkWidget *win = GTK_WIDGET(l->data);
    if (!win)
      continue;

    /* Avoid duplicate handlers using GObject data */
    if (!g_object_get_data(G_OBJECT(win), "tauri-spy-key-handler")) {
      g_signal_connect(win, "key-press-event", G_CALLBACK(on_key_press), NULL);
      g_object_set_data(G_OBJECT(win), "tauri-spy-key-handler",
                        GINT_TO_POINTER(1));
    }
  }

  g_list_free(toplevels);
  spy_enabled = 1;
  fprintf(
      stderr,
      "[tauri-spy] Injection complete — Ctrl+Shift+I to toggle inspector\n");
  return FALSE; /* Remove idle callback */
}

/*
 * Shared helper: installs the idle callback once.
 */
static void install_idle_callback(void) {
  if (idle_installed)
    return;
  idle_installed = 1;

  /* Check for auto-open flag */
  const char *env_auto = getenv("TAURI_SPY_AUTO_OPEN");
  if (env_auto && strcmp(env_auto, "1") == 0) {
    auto_open = 1;
  }

  g_idle_add(idle_callback, NULL);
}

/*
 * Hook: gtk_main() — called by some GTK apps when entering the event loop.
 */
void gtk_main(void) {
  if (!real_gtk_main) {
    real_gtk_main = (gtk_main_fn)dlsym(RTLD_NEXT, "gtk_main");
    if (!real_gtk_main) {
      fprintf(stderr, "[tauri-spy] FATAL: Could not find real gtk_main()\n");
      return;
    }
  }

  fprintf(stderr, "[tauri-spy] Hooked gtk_main() — installing idle callback\n");
  install_idle_callback();

  /* Call the real gtk_main() */
  real_gtk_main();
}

/*
 * Hook: g_application_run() — called by Tauri v2 / modern GTK apps.
 * Tauri uses GtkApplication which enters the main loop via this function
 * instead of gtk_main().
 */
int g_application_run(GApplication *application, int argc, char **argv) {
  if (!real_g_application_run) {
    real_g_application_run =
        (g_application_run_fn)dlsym(RTLD_NEXT, "g_application_run");
    if (!real_g_application_run) {
      fprintf(stderr,
              "[tauri-spy] FATAL: Could not find real g_application_run()\n");
      return 1;
    }
  }

  fprintf(stderr, "[tauri-spy] Hooked g_application_run() — installing idle "
                  "callback\n");
  install_idle_callback();

  /* Call the real g_application_run() */
  return real_g_application_run(application, argc, argv);
}

/*
 * Hook: gtk_main_iteration_do() — called by Tauri/tao in a manual event loop.
 * Tao doesn't use gtk_main() or g_application_run(); instead it calls
 * gtk_main_iteration_do() repeatedly. We hook this to install our idle
 * callback on the very first iteration.
 */
gboolean gtk_main_iteration_do(gboolean blocking) {
  if (!real_gtk_main_iteration_do) {
    real_gtk_main_iteration_do =
        (gtk_main_iteration_do_fn)dlsym(RTLD_NEXT, "gtk_main_iteration_do");
    if (!real_gtk_main_iteration_do) {
      fprintf(stderr, "[tauri-spy] FATAL: Could not find real "
                      "gtk_main_iteration_do()\n");
      return FALSE;
    }
  }

  if (!idle_installed) {
    fprintf(stderr, "[tauri-spy] Hooked gtk_main_iteration_do() — installing "
                    "idle callback\n");
    install_idle_callback();
  }

  return real_gtk_main_iteration_do(blocking);
}
