#include "review-application.hpp"
#include "review-window.hpp"

#include <gtk/gtk.h>

namespace review {
namespace {

void activate(GApplication* application, gpointer) {
  static bool resources_loaded = false;
  if (!resources_loaded) {
    GdkDisplay* display = gdk_display_get_default();
    if (display) {
      gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(display),
                                       "/io/github/mika/Review/icons/scalable/actions");
      auto* provider = gtk_css_provider_new();
      gtk_css_provider_load_from_resource(provider, "/io/github/mika/Review/style.css");
      gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      g_object_unref(provider);
    }
    resources_loaded = true;
  }
  auto* window = Window::create(ADW_APPLICATION(application));
  window->present();
}

void open_files(GApplication* application, GFile** files, gint count,
                const gchar*, gpointer) {
  for (int i = 0; i < count; ++i) {
    char* path = g_file_get_path(files[i]);
    if (!path) continue;
    auto* window = Window::create(ADW_APPLICATION(application), path);
    window->present();
    g_free(path);
  }
}

void new_window(GSimpleAction*, GVariant*, gpointer data) {
  auto* window = Window::create(ADW_APPLICATION(data));
  window->present();
}

void about(GSimpleAction*, GVariant*, gpointer data) {
  auto* dialog = adw_about_window_new();
  adw_about_window_set_application_name(ADW_ABOUT_WINDOW(dialog), "Review");
  adw_about_window_set_application_icon(ADW_ABOUT_WINDOW(dialog), "io.github.mika.Review");
  adw_about_window_set_version(ADW_ABOUT_WINDOW(dialog), "0.1.0");
  adw_about_window_set_developer_name(ADW_ABOUT_WINDOW(dialog), "Review contributors");
  adw_about_window_set_copyright(ADW_ABOUT_WINDOW(dialog),
                                 "Copyright Mika Hämäläinen");
  adw_about_window_set_website(ADW_ABOUT_WINDOW(dialog), "https://mikakalevi.com");
  adw_about_window_set_license_type(ADW_ABOUT_WINDOW(dialog), GTK_LICENSE_GPL_3_0);
  adw_about_window_set_comments(ADW_ABOUT_WINDOW(dialog),
      "A focused PDF and image viewer for Linux.");
  gtk_window_set_transient_for(GTK_WINDOW(dialog),
      gtk_application_get_active_window(GTK_APPLICATION(data)));
  gtk_window_present(GTK_WINDOW(dialog));
}

void quit(GSimpleAction*, GVariant*, gpointer data) {
  g_application_quit(G_APPLICATION(data));
}

} // namespace

AdwApplication* application_new() {
  auto* app = adw_application_new("io.github.mika.Review", G_APPLICATION_HANDLES_OPEN);
  g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
  g_signal_connect(app, "open", G_CALLBACK(open_files), nullptr);

  const GActionEntry entries[] = {
    {"new-window", new_window, nullptr, nullptr, nullptr, {}},
    {"about", about, nullptr, nullptr, nullptr, {}},
    {"quit", quit, nullptr, nullptr, nullptr, {}},
  };
  g_action_map_add_action_entries(G_ACTION_MAP(app), entries, G_N_ELEMENTS(entries), app);
  const char* quit_accels[] = {"<primary>q", nullptr};
  const char* new_accels[] = {"<primary>n", nullptr};
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.new-window", new_accels);
  return app;
}

} // namespace review
