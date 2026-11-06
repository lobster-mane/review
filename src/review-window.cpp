#include "review-window.hpp"
#include "review-text.hpp"

#include <cairo.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace review {
namespace {

constexpr double kPageGap = 12.0;
constexpr double kCanvasMargin = 24.0;
constexpr std::size_t kPageCacheLimit = 6;
constexpr std::size_t kToolColorCount = 8;

bool loading_debug_enabled() {
  static const bool enabled = g_getenv("REVIEW_DEBUG_LOADING") != nullptr;
  return enabled;
}

void loading_log(const char* format, ...) {
  if (!loading_debug_enabled()) return;
  va_list args;
  va_start(args, format);
  gchar* message = g_strdup_vprintf(format, args);
  va_end(args);
  g_printerr("[review-loading %.3f] %s\n", g_get_monotonic_time() / 1000000.0,
             message);
  g_free(message);
}

std::size_t tool_color_index(AnnotationType type) {
  return static_cast<std::size_t>(type);
}

Color default_tool_stroke(AnnotationType) {
  return {0.0, 0.0, 0.0, 1.0};
}

Color default_tool_fill(AnnotationType type) {
  if (type == AnnotationType::Highlight) return {1.0, 0.92, 0.23, 0.35};
  return {0.0, 0.0, 0.0, 0.0};
}

const char* tool_color_key(AnnotationType type) {
  switch (type) {
    case AnnotationType::Highlight: return "highlight";
    case AnnotationType::TextBox: return "text";
    case AnnotationType::Ink: return "ink";
    case AnnotationType::Rectangle: return "rectangle";
    case AnnotationType::Ellipse: return "ellipse";
    case AnnotationType::Line: return "line";
    case AnnotationType::Arrow: return "arrow";
    case AnnotationType::Signature: return "signature";
  }
  return "rectangle";
}

bool tool_uses_stroke(AnnotationType type) {
  return type == AnnotationType::TextBox || type == AnnotationType::Ink ||
         type == AnnotationType::Rectangle || type == AnnotationType::Ellipse ||
         type == AnnotationType::Line || type == AnnotationType::Arrow;
}

bool tool_uses_fill(AnnotationType type) {
  return type == AnnotationType::Highlight || type == AnnotationType::Rectangle ||
         type == AnnotationType::Ellipse;
}

std::vector<PageRef> page_clipboard;
bool clipboard_is_cut = false;
std::vector<PageRef> dragged_pages;
Window* dragged_source = nullptr;
std::vector<std::size_t> dragged_indices;

Window* self(gpointer data) { return static_cast<Window*>(data); }

GtkWidget* icon_button(const char* icon, const char* tooltip) {
  auto* button = gtk_button_new_from_icon_name(icon);
  gtk_widget_set_tooltip_text(button, tooltip);
  gtk_widget_add_css_class(button, "flat");
  return button;
}

void append_button(GtkBox* box, GtkWidget* button) {
  gtk_box_append(box, button);
}

std::size_t row_index(GtkListBoxRow* row) {
  return static_cast<std::size_t>(gtk_list_box_row_get_index(row));
}

bool text_spans_equal(const std::vector<TextSpan>& left,
                      const std::vector<TextSpan>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].start != right[i].start || left[i].end != right[i].end ||
        !text_style_equal(left[i].style, right[i].style))
      return false;
  }
  return true;
}

} // namespace

Window* Window::create(AdwApplication* application, const std::string& path) {
  auto* controller = new Window(application);
  g_object_set_data_full(G_OBJECT(controller->window_), "review-controller", controller,
                         [](gpointer data) { delete static_cast<Window*>(data); });
  if (!path.empty()) controller->open(path);
  return controller;
}

Window::Window(AdwApplication* application) {
  window_ = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(application)));
  gtk_window_set_default_size(GTK_WINDOW(window_), 1100, 760);
  gtk_window_set_icon_name(GTK_WINDOW(window_), "io.github.mika.Review");
  reset_tool_colors();
  load_preferences();
  build_ui();
  install_actions();
  update_title();
  g_signal_connect(window_, "close-request", G_CALLBACK(+[](
      GtkWindow*, gpointer data) -> gboolean {
    auto* w = self(data);
    if (g_object_get_data(G_OBJECT(w->window_), "allow-close")) return FALSE;
    if (w->inline_edit_active_) w->finish_inline_text_edit(true);
    if (!w->document_ || !w->document_->dirty()) return FALSE;
    auto* dialog = adw_message_dialog_new(GTK_WINDOW(w->window_), "Discard unsaved changes?",
      "Closing this window will discard all changes made since the last save.");
    adw_message_dialog_add_responses(ADW_MESSAGE_DIALOG(dialog), "cancel", "Cancel",
                                     "discard", "Discard", nullptr);
    adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(dialog), "discard",
                                               ADW_RESPONSE_DESTRUCTIVE);
    g_signal_connect(dialog, "response", G_CALLBACK(+[](
        AdwMessageDialog* dialog, const char* response, gpointer data) {
      auto* w = self(data);
      if (g_strcmp0(response, "discard") == 0) {
        g_object_set_data(G_OBJECT(w->window_), "allow-close", GINT_TO_POINTER(1));
        gtk_window_destroy(GTK_WINDOW(w->window_));
      }
      gtk_window_destroy(GTK_WINDOW(dialog));
    }), w);
    gtk_window_present(GTK_WINDOW(dialog));
    return TRUE;
  }), this);
}

Window::~Window() {
  if (page_render_source_id_) {
    g_source_remove(page_render_source_id_);
    page_render_source_id_ = 0;
  }
  if (scroll_settle_source_id_) {
    g_source_remove(scroll_settle_source_id_);
    scroll_settle_source_id_ = 0;
  }
  clear_page_cache();
  if (recent_menu_) g_object_unref(recent_menu_);
}

void Window::schedule_preload_next_page() {
  if (continuous_scroll_ || pinch_active_ || !document_ ||
      current_page_ + 1 >= document_->page_count())
    return;
  schedule_page_render(current_page_ + 1, 0);
}

void Window::note_scroll_activity() {
  if (!scrolling_) loading_log("scroll start");
  scrolling_ = true;
  if (canvas_) gtk_widget_queue_draw(GTK_WIDGET(canvas_));
  if (canvas_overlay_) gtk_widget_queue_draw(GTK_WIDGET(canvas_overlay_));
  if (scroll_settle_source_id_) g_source_remove(scroll_settle_source_id_);
  scroll_settle_source_id_ = g_timeout_add(400, Window::finish_scroll, this);
}

gboolean Window::finish_scroll(gpointer data) {
  auto* w = static_cast<Window*>(data);
  w->scroll_settle_source_id_ = 0;
  w->scrolling_ = false;
  loading_log("scroll end pending=%zu", w->pending_page_renders_.size());
  if (w->document_ && w->canvas_scroll_) {
    auto* adjustment = gtk_scrolled_window_get_vadjustment(w->canvas_scroll_);
    const double view_top = gtk_adjustment_get_value(adjustment);
    const double view_bottom = view_top + gtk_adjustment_get_page_size(adjustment);
    const double margin = std::max(0.0, (view_bottom - view_top) * 0.5);
    for (auto it = w->pending_page_renders_.begin();
         it != w->pending_page_renders_.end();) {
      const auto page = it->first;
      bool near_viewport = page < w->document_->page_count();
      if (near_viewport) {
        const double page_top = w->page_top(page);
        const double page_bottom = page_top +
            w->document_->page(page).height_points * w->zoom_;
        near_viewport = page_bottom >= view_top - margin &&
                        page_top <= view_bottom + margin;
      }
      it = near_viewport ? std::next(it) : w->pending_page_renders_.erase(it);
    }
  }
  if (w->canvas_) gtk_widget_queue_draw(GTK_WIDGET(w->canvas_));
  if (w->canvas_overlay_) gtk_widget_queue_draw(GTK_WIDGET(w->canvas_overlay_));
  w->schedule_preload_next_page();
  if (!w->pending_page_renders_.empty() && !w->page_render_source_id_)
    w->page_render_source_id_ = g_idle_add(Window::render_pending_pages, w);
  return G_SOURCE_REMOVE;
}

void Window::schedule_page_render(std::size_t page, std::uint64_t skip) {
  if (!document_ || page >= document_->page_count()) return;
  if (cached_page(page, skip, false)) return;
  const bool added = pending_page_renders_.insert({page, skip}).second;
  if (added) loading_log("queue page %zu scrolling=%d pinch=%d", page,
                         scrolling_, pinch_active_);
  if (!pinch_active_ && !page_render_source_id_)
    page_render_source_id_ = g_idle_add(Window::render_pending_pages, this);
}

gboolean Window::render_pending_pages(gpointer data) {
  auto* w = static_cast<Window*>(data);
  w->page_render_source_id_ = 0;
  if (w->pinch_active_) return G_SOURCE_REMOVE;
  if (!w->document_ || w->pending_page_renders_.empty()) return G_SOURCE_REMOVE;

  auto distance = [](std::size_t left, std::size_t right) {
    return left > right ? left - right : right - left;
  };
  auto best = w->pending_page_renders_.begin();
  for (auto it = std::next(best); it != w->pending_page_renders_.end(); ++it)
    if (distance(it->first, w->current_page_) <
        distance(best->first, w->current_page_))
      best = it;
  const auto page = best->first;
  const auto skip = best->second;
  w->pending_page_renders_.erase(best);

  bool near_viewport = true;
  if (w->canvas_scroll_ && w->document_ && page < w->document_->page_count()) {
    auto* adjustment = gtk_scrolled_window_get_vadjustment(w->canvas_scroll_);
    const double view_top = gtk_adjustment_get_value(adjustment);
    const double view_bottom = view_top + gtk_adjustment_get_page_size(adjustment);
    const double margin = std::max(0.0, (view_bottom - view_top) * 0.5);
    const double page_top = w->page_top(page);
    const double page_bottom = page_top +
        w->document_->page(page).height_points * w->zoom_;
    near_viewport = page_bottom >= view_top - margin &&
                    page_top <= view_bottom + margin;
  }
  if (near_viewport) {
    loading_log("render page %zu start", page);
    const gint64 start = g_get_monotonic_time();
    w->cached_page(page, skip);
    loading_log("render page %zu done %.1f ms", page,
                (g_get_monotonic_time() - start) / 1000.0);
    if (w->canvas_) gtk_widget_queue_draw(GTK_WIDGET(w->canvas_));
    if (w->canvas_overlay_) gtk_widget_queue_draw(GTK_WIDGET(w->canvas_overlay_));
  }
  if (!w->pending_page_renders_.empty())
    w->page_render_source_id_ = g_idle_add(Window::render_pending_pages, w);
  return G_SOURCE_REMOVE;
}

void Window::clear_page_cache() {
  for (auto& entry : page_cache_) cairo_surface_destroy(entry.surface);
  page_cache_.clear();
  page_cache_revision_ = 0;
  page_cache_zoom_ = 0.0;
}

cairo_surface_t* Window::cached_page(std::size_t page, std::uint64_t skip,
                                     bool render_if_missing) {
  if (!document_ || page >= document_->page_count()) return nullptr;
  const bool zoom_changed = page_cache_zoom_ != zoom_;
  if (page_cache_revision_ != document_->revision() ||
      (zoom_changed && !pinch_active_)) {
    loading_log("clear page cache revision=%d zoom=%d->%.3f pinch=%d",
                page_cache_revision_ != document_->revision(), zoom_changed,
                zoom_, pinch_active_);
    clear_page_cache();
  }
  for (auto it = page_cache_.begin(); it != page_cache_.end(); ++it) {
    if (it->page == page && it->skip == skip) {
      CachedPage entry = *it;
      page_cache_.erase(it);
      page_cache_.push_back(entry);
      return entry.surface;
    }
  }
  if (!render_if_missing) return nullptr;
  // While pinching, reuse surfaces at the previous zoom and let the draw code
  // scale them. This avoids a full page render on every gesture event.
  const double render_scale =
      (pinch_active_ && page_cache_zoom_ > 0.0) ? page_cache_zoom_ : zoom_;
  const gint64 render_start = g_get_monotonic_time();
  cairo_surface_t* surface = document_->render_page(page, render_scale, true, skip);
  if (surface)
    loading_log("rendered page %zu at %.3f in %.1f ms", page, render_scale,
                (g_get_monotonic_time() - render_start) / 1000.0);
  else
    loading_log("render failed page %zu", page);
  if (!surface) return nullptr;
  if (page_cache_.size() >= kPageCacheLimit) {
    cairo_surface_destroy(page_cache_.front().surface);
    page_cache_.erase(page_cache_.begin());
  }
  page_cache_.push_back({page, skip, surface});
  page_cache_revision_ = document_->revision();
  if (!pinch_active_ || page_cache_zoom_ <= 0.0)
    page_cache_zoom_ = render_scale;
  return surface;
}

void Window::present() {
  gtk_window_present(GTK_WINDOW(window_));
  // A document opened from the command line is first rendered before the
  // window has its real width; refit once the viewport is laid out.
  if (!document_ || !fit_width_) return;
  struct FitRequest {
    Window* window;
    int attempts{};
  };
  auto* request = new FitRequest{this};
  gtk_widget_add_tick_callback(GTK_WIDGET(canvas_scroll_), +[](
      GtkWidget* widget, GdkFrameClock*, gpointer data) -> gboolean {
    auto* request = static_cast<FitRequest*>(data);
    if (gtk_widget_get_width(widget) > 200) {
      request->window->render_current();
      delete request;
      return G_SOURCE_REMOVE;
    }
    if (++request->attempts >= 60) {
      delete request;
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }, request, nullptr);
}

void Window::reset_tool_colors() {
  for (std::size_t i = 0; i < kToolColorCount; ++i) {
    const auto type = static_cast<AnnotationType>(i);
    tool_colors_[i].stroke = default_tool_stroke(type);
    tool_colors_[i].fill = default_tool_fill(type);
  }
}

void Window::apply_tool_colors(AnnotationType type) {
  const auto& colors = tool_colors_[tool_color_index(type)];
  annotation_stroke_ = colors.stroke;
  annotation_fill_ = colors.fill;
  if (!stroke_color_ || !fill_color_) return;
  const bool was_updating = updating_annotation_controls_;
  updating_annotation_controls_ = true;
  GdkRGBA stroke{colors.stroke.red, colors.stroke.green,
                 colors.stroke.blue, colors.stroke.alpha};
  GdkRGBA fill{colors.fill.red, colors.fill.green,
               colors.fill.blue, colors.fill.alpha};
  gtk_color_dialog_button_set_rgba(stroke_color_, &stroke);
  gtk_color_dialog_button_set_rgba(fill_color_, &fill);
  updating_annotation_controls_ = was_updating;
}

void Window::load_preferences() {
  const auto path = fs::path(g_get_user_config_dir()) / "review" / "preferences.ini";
  auto* key_file = g_key_file_new();
  if (!g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
    g_key_file_unref(key_file);
    return;
  }
  auto read_component = [&](const char* key, double fallback) {
    if (!g_key_file_has_key(key_file, "Annotations", key, nullptr)) return fallback;
    return std::clamp(g_key_file_get_double(key_file, "Annotations", key, nullptr), 0.0, 1.0);
  };
  for (std::size_t i = 0; i < kToolColorCount; ++i) {
    const auto type = static_cast<AnnotationType>(i);
    const std::string suffix = tool_color_key(type);
    auto read_for_tool = [&](const char* component, double fallback) {
      const std::string key = std::string(component) + "-" + suffix;
      return read_component(key.c_str(), fallback);
    };
    auto& colors = tool_colors_[i];
    colors.stroke = {
      read_for_tool("stroke-red", colors.stroke.red),
      read_for_tool("stroke-green", colors.stroke.green),
      read_for_tool("stroke-blue", colors.stroke.blue),
      read_for_tool("stroke-alpha", colors.stroke.alpha),
    };
    colors.fill = {
      read_for_tool("fill-red", colors.fill.red),
      read_for_tool("fill-green", colors.fill.green),
      read_for_tool("fill-blue", colors.fill.blue),
      read_for_tool("fill-alpha", colors.fill.alpha),
    };
  }
  apply_tool_colors(active_tool_);
  if (char* family = g_key_file_get_string(key_file, "Annotations", "font-family", nullptr)) {
    annotation_font_ = family;
    g_free(family);
  }
  if (g_key_file_has_key(key_file, "Annotations", "font-size", nullptr))
    annotation_font_size_ = std::clamp(
        g_key_file_get_double(key_file, "Annotations", "font-size", nullptr), 6.0, 144.0);
  annotation_bold_ = g_key_file_get_boolean(key_file, "Annotations", "bold", nullptr);
  annotation_italic_ = g_key_file_get_boolean(key_file, "Annotations", "italic", nullptr);
  annotation_underline_ = g_key_file_get_boolean(key_file, "Annotations", "underline", nullptr);
  if (char* align = g_key_file_get_string(key_file, "Annotations", "align", nullptr)) {
    if (g_strcmp0(align, "center") == 0) annotation_align_ = TextAlign::Center;
    else if (g_strcmp0(align, "right") == 0) annotation_align_ = TextAlign::Right;
    else annotation_align_ = TextAlign::Left;
    g_free(align);
  }
  if (g_key_file_has_key(key_file, "View", "continuous-scroll", nullptr))
    continuous_scroll_ = g_key_file_get_boolean(key_file, "View", "continuous-scroll", nullptr);
  g_key_file_unref(key_file);
}

void Window::save_preferences() const {
  const auto directory = fs::path(g_get_user_config_dir()) / "review";
  const auto path = directory / "preferences.ini";
  if (g_mkdir_with_parents(directory.c_str(), 0700) != 0) return;
  auto* key_file = g_key_file_new();
  for (std::size_t i = 0; i < kToolColorCount; ++i) {
    const auto type = static_cast<AnnotationType>(i);
    const std::string suffix = tool_color_key(type);
    const auto& colors = tool_colors_[i];
    auto write = [&](const char* component, double value) {
      const std::string key = std::string(component) + "-" + suffix;
      g_key_file_set_double(key_file, "Annotations", key.c_str(), value);
    };
    write("stroke-red", colors.stroke.red);
    write("stroke-green", colors.stroke.green);
    write("stroke-blue", colors.stroke.blue);
    write("stroke-alpha", colors.stroke.alpha);
    write("fill-red", colors.fill.red);
    write("fill-green", colors.fill.green);
    write("fill-blue", colors.fill.blue);
    write("fill-alpha", colors.fill.alpha);
  }
  g_key_file_set_string(key_file, "Annotations", "font-family", annotation_font_.c_str());
  g_key_file_set_double(key_file, "Annotations", "font-size", annotation_font_size_);
  g_key_file_set_boolean(key_file, "Annotations", "bold", annotation_bold_);
  g_key_file_set_boolean(key_file, "Annotations", "italic", annotation_italic_);
  g_key_file_set_boolean(key_file, "Annotations", "underline", annotation_underline_);
  const char* align = annotation_align_ == TextAlign::Center ? "center" :
                      annotation_align_ == TextAlign::Right ? "right" : "left";
  g_key_file_set_string(key_file, "Annotations", "align", align);
  g_key_file_set_boolean(key_file, "View", "continuous-scroll", continuous_scroll_);
  if (g_key_file_save_to_file(key_file, path.c_str(), nullptr)) g_chmod(path.c_str(), 0600);
  g_key_file_unref(key_file);
}

void Window::build_ui() {
  toast_overlay_ = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  auto* toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  adw_toast_overlay_set_child(toast_overlay_, GTK_WIDGET(toolbar_view));
  adw_application_window_set_content(window_, GTK_WIDGET(toast_overlay_));

  auto* header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_toolbar_view_add_top_bar(toolbar_view, GTK_WIDGET(header));

  auto* open_button = icon_button("document-open-symbolic", "Open (Ctrl+O)");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(open_button), "win.open");
  adw_header_bar_pack_start(header, open_button);

  auto* sidebar_button = icon_button("sidebar-show-symbolic", "Show pages");
  g_signal_connect_swapped(sidebar_button, "clicked", G_CALLBACK(+[](Window* w) {
    auto* sidebar = gtk_paned_get_start_child(w->split_);
    if (!sidebar) return;
    if (gtk_widget_get_visible(sidebar)) {
      w->sidebar_position_ = gtk_paned_get_position(w->split_);
      gtk_widget_set_visible(sidebar, FALSE);
    } else {
      gtk_widget_set_visible(sidebar, TRUE);
      gtk_paned_set_position(w->split_, w->sidebar_position_);
    }
  }), this);
  adw_header_bar_pack_start(header, sidebar_button);

  search_ = GTK_SEARCH_ENTRY(gtk_search_entry_new());
  gtk_widget_set_size_request(GTK_WIDGET(search_), 180, -1);
  gtk_widget_set_tooltip_text(GTK_WIDGET(search_), "Search document");
  g_signal_connect_swapped(search_, "activate", G_CALLBACK(+[](Window* w) {
    if (!w->document_) return;
    const char* query = gtk_editable_get_text(GTK_EDITABLE(w->search_));
    if (!query || !*query) return;
    for (std::size_t offset = 1; offset <= w->document_->page_count(); ++offset) {
      const auto index = (w->current_page_ + offset) % w->document_->page_count();
      std::string text = w->document_->page_text(index);
      std::string needle = query;
      std::transform(text.begin(), text.end(), text.begin(), ::tolower);
      std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
      if (text.find(needle) != std::string::npos) {
        w->select_page(index);
        w->toast("Match found on page " + std::to_string(index + 1));
        return;
      }
    }
    w->toast("No more matches");
  }), this);
  adw_header_bar_set_title_widget(header, GTK_WIDGET(search_));

  auto* zoom_out = icon_button("zoom-out-symbolic", "Zoom out");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(zoom_out), "win.zoom-out");
  adw_header_bar_pack_end(header, zoom_out);
  auto* zoom_in = icon_button("zoom-in-symbolic", "Zoom in");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(zoom_in), "win.zoom-in");
  adw_header_bar_pack_end(header, zoom_in);

  auto* menu_button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
  auto* menu_builder = gtk_builder_new_from_resource("/io/github/mika/Review/ui/menu.ui");
  auto* menu = G_MENU_MODEL(gtk_builder_get_object(menu_builder, "main-menu"));
  recent_menu_ = g_menu_new();
  g_menu_prepend_submenu(G_MENU(menu), "Open Recent", G_MENU_MODEL(recent_menu_));
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), menu);
  g_object_unref(menu_builder);
  rebuild_recent_menu();
  adw_header_bar_pack_end(header, menu_button);

  auto* annotation_bar = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
  auto* tool_row = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
  auto* property_row = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
  gtk_widget_add_css_class(GTK_WIDGET(annotation_bar), "toolbar");
  gtk_widget_set_margin_start(GTK_WIDGET(annotation_bar), 8);
  gtk_widget_set_margin_end(GTK_WIDGET(annotation_bar), 8);
  gtk_widget_set_margin_top(GTK_WIDGET(annotation_bar), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(annotation_bar), 6);

  const struct { const char* icon; const char* tip; AnnotationType type; } tools[] = {
    {"review-highlight-symbolic", "Highlight text (marker)", AnnotationType::Highlight},
    {"review-text-symbolic", "Text box (drag, then type)", AnnotationType::TextBox},
    {"review-ink-symbolic", "Freehand ink", AnnotationType::Ink},
    {"review-rectangle-symbolic", "Rectangle", AnnotationType::Rectangle},
    {"review-ellipse-symbolic", "Ellipse", AnnotationType::Ellipse},
    {"review-arrow-symbolic", "Arrow", AnnotationType::Arrow},
  };
  GtkToggleButton* tool_group = nullptr;
  auto add_tool_button = [&](const char* icon, const char* tip, int mode) {
    auto* button = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
    gtk_button_set_icon_name(GTK_BUTTON(button), icon);
    gtk_widget_set_tooltip_text(GTK_WIDGET(button), tip);
    gtk_widget_add_css_class(GTK_WIDGET(button), "flat");
    if (tool_group) gtk_toggle_button_set_group(button, tool_group);
    else tool_group = button;
    g_object_set_data(G_OBJECT(button), "tool-mode", GINT_TO_POINTER(mode));
    g_signal_connect(button, "toggled", G_CALLBACK(+[](
        GtkToggleButton* button, gpointer data) {
      auto* w = self(data);
      if (gtk_toggle_button_get_active(button)) {
        w->active_tool_button_ = button;
        const int mode = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(button), "tool-mode"));
        if (mode == -1) w->set_select_tool();
        else w->set_tool(static_cast<AnnotationType>(mode));
        if (mode >= 0 && !w->tool_active_) {
          w->active_tool_button_ = nullptr;
          gtk_toggle_button_set_active(button, FALSE);
        }
      } else if (w->active_tool_button_ == button) {
        w->active_tool_button_ = nullptr;
        w->tool_active_ = false;
        w->update_text_format_visibility();
      }
    }), this);
    append_button(tool_row, GTK_WIDGET(button));
    return button;
  };

  select_button_ = add_tool_button("review-select-symbolic", "Select, move, and resize annotations", -1);
  for (const auto& tool : tools) {
    add_tool_button(tool.icon, tool.tip, static_cast<int>(tool.type));
  }

  auto* stroke_dialog = gtk_color_dialog_new();
  gtk_color_dialog_set_title(stroke_dialog, "Choose outline and text color");
  gtk_color_dialog_set_with_alpha(stroke_dialog, TRUE);
  stroke_color_ = GTK_COLOR_DIALOG_BUTTON(gtk_color_dialog_button_new(stroke_dialog));
  GdkRGBA initial_stroke{annotation_stroke_.red, annotation_stroke_.green,
                         annotation_stroke_.blue, annotation_stroke_.alpha};
  gtk_color_dialog_button_set_rgba(stroke_color_, &initial_stroke);
  gtk_widget_set_tooltip_text(GTK_WIDGET(stroke_color_), "Outline and text color");
  g_signal_connect_swapped(stroke_color_, "notify::rgba", G_CALLBACK(+[](Window* w) {
    if (!w->updating_annotation_controls_) w->apply_annotation_controls(false);
  }), this);
  auto* stroke_control = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
  stroke_control_ = GTK_WIDGET(stroke_control);
  auto* stroke_label = gtk_label_new("Outline/Text");
  gtk_widget_add_css_class(stroke_label, "dim-label");
  append_button(stroke_control, stroke_label);
  append_button(stroke_control, GTK_WIDGET(stroke_color_));
  append_button(property_row, stroke_control_);

  auto* fill_dialog = gtk_color_dialog_new();
  gtk_color_dialog_set_title(fill_dialog, "Choose fill color");
  gtk_color_dialog_set_with_alpha(fill_dialog, TRUE);
  fill_color_ = GTK_COLOR_DIALOG_BUTTON(gtk_color_dialog_button_new(fill_dialog));
  GdkRGBA initial_fill{annotation_fill_.red, annotation_fill_.green,
                       annotation_fill_.blue, annotation_fill_.alpha};
  gtk_color_dialog_button_set_rgba(fill_color_, &initial_fill);
  gtk_widget_set_tooltip_text(GTK_WIDGET(fill_color_), "Fill and highlight color");
  g_signal_connect_swapped(fill_color_, "notify::rgba", G_CALLBACK(+[](Window* w) {
    if (!w->updating_annotation_controls_) w->apply_annotation_controls(false);
  }), this);
  auto* fill_control = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6));
  fill_control_ = GTK_WIDGET(fill_control);
  auto* fill_label = gtk_label_new("Fill/Highlight");
  gtk_widget_add_css_class(fill_label, "dim-label");
  append_button(fill_control, fill_label);
  append_button(fill_control, GTK_WIDGET(fill_color_));
  append_button(property_row, fill_control_);

  text_format_box_ = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
  append_button(property_row, GTK_WIDGET(text_format_box_));

  font_button_ = GTK_BUTTON(gtk_button_new());
  gtk_widget_add_css_class(GTK_WIDGET(font_button_), "flat");
  update_font_button_label();
  gtk_widget_set_tooltip_text(GTK_WIDGET(font_button_), "Text annotation font");
  g_signal_connect_swapped(font_button_, "clicked", G_CALLBACK(+[](Window* w) {
    w->choose_font();
  }), this);
  append_button(text_format_box_, GTK_WIDGET(font_button_));

  auto make_format_toggle = [&](const char* icon, const char* tooltip) {
    auto* button = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
    gtk_button_set_icon_name(GTK_BUTTON(button), icon);
    gtk_widget_set_tooltip_text(GTK_WIDGET(button), tooltip);
    gtk_widget_add_css_class(GTK_WIDGET(button), "flat");
    append_button(text_format_box_, GTK_WIDGET(button));
    g_signal_connect_swapped(button, "toggled", G_CALLBACK(+[](Window* w) {
      if (!w->updating_annotation_controls_) w->apply_annotation_controls(false);
    }), this);
    return button;
  };
  append_button(text_format_box_, gtk_separator_new(GTK_ORIENTATION_VERTICAL));
  bold_button_ = make_format_toggle("format-text-bold-symbolic", "Bold");
  italic_button_ = make_format_toggle("format-text-italic-symbolic", "Italic");
  underline_button_ = make_format_toggle("format-text-underline-symbolic", "Underline");
  align_left_button_ = make_format_toggle("format-justify-left-symbolic", "Align left");
  align_center_button_ = make_format_toggle("format-justify-center-symbolic", "Align center");
  align_right_button_ = make_format_toggle("format-justify-right-symbolic", "Align right");
  gtk_toggle_button_set_group(align_center_button_, align_left_button_);
  gtk_toggle_button_set_group(align_right_button_, align_left_button_);
  updating_annotation_controls_ = true;
  gtk_toggle_button_set_active(bold_button_, annotation_bold_);
  gtk_toggle_button_set_active(italic_button_, annotation_italic_);
  gtk_toggle_button_set_active(underline_button_, annotation_underline_);
  gtk_toggle_button_set_active(
      annotation_align_ == TextAlign::Center ? align_center_button_ :
      annotation_align_ == TextAlign::Right ? align_right_button_ : align_left_button_, TRUE);
  updating_annotation_controls_ = false;
  update_text_format_visibility();

  signature_button_ = GTK_MENU_BUTTON(gtk_menu_button_new());
  gtk_menu_button_set_icon_name(signature_button_, "review-signature-symbolic");
  gtk_widget_add_css_class(GTK_WIDGET(signature_button_), "flat");
  gtk_widget_set_tooltip_text(GTK_WIDGET(signature_button_),
                              "Insert a saved signature");
  auto* signature_popover = gtk_popover_new();
  gtk_popover_set_has_arrow(GTK_POPOVER(signature_popover), TRUE);
  auto* signature_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(signature_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(
      GTK_SCROLLED_WINDOW(signature_scroll), 300);
  gtk_scrolled_window_set_max_content_width(
      GTK_SCROLLED_WINDOW(signature_scroll), 200);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(signature_scroll), TRUE);
  gtk_scrolled_window_set_propagate_natural_width(
      GTK_SCROLLED_WINDOW(signature_scroll), FALSE);
  gtk_widget_set_size_request(signature_scroll, 200, -1);
  signature_box_ = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
  gtk_widget_set_margin_start(GTK_WIDGET(signature_box_), 6);
  gtk_widget_set_margin_end(GTK_WIDGET(signature_box_), 6);
  gtk_widget_set_margin_top(GTK_WIDGET(signature_box_), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(signature_box_), 6);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(signature_scroll),
                                GTK_WIDGET(signature_box_));
  gtk_popover_set_child(GTK_POPOVER(signature_popover), signature_scroll);
  gtk_menu_button_set_popover(signature_button_, signature_popover);
  append_button(tool_row, GTK_WIDGET(signature_button_));
  rebuild_signature_menu();

  auto* crop = icon_button("review-crop-symbolic", "Secure crop");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(crop), "win.secure-crop");
  append_button(tool_row, crop);
  auto* scan = icon_button("review-scan-symbolic", "Convert selected pages to 300 DPI color images");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(scan), "win.scan-pages");
  append_button(tool_row, scan);
  property_revealer_ = GTK_REVEALER(gtk_revealer_new());
  gtk_revealer_set_transition_type(property_revealer_,
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_transition_duration(property_revealer_, 160);
  gtk_revealer_set_child(property_revealer_, GTK_WIDGET(property_row));
  gtk_revealer_set_reveal_child(property_revealer_, FALSE);
  gtk_box_append(annotation_bar, GTK_WIDGET(tool_row));
  gtk_box_append(annotation_bar, GTK_WIDGET(property_revealer_));
  gtk_toggle_button_set_active(select_button_, TRUE);
  adw_toolbar_view_add_top_bar(toolbar_view, GTK_WIDGET(annotation_bar));

  split_ = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_paned_set_position(split_, sidebar_position_);
  gtk_paned_set_resize_start_child(split_, FALSE);
  gtk_paned_set_resize_end_child(split_, TRUE);
  gtk_paned_set_shrink_start_child(split_, FALSE);
  adw_toolbar_view_set_content(toolbar_view, GTK_WIDGET(split_));

  auto* sidebar_scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
  gtk_widget_set_size_request(GTK_WIDGET(sidebar_scroll), 160, -1);
  sidebar_ = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(sidebar_, GTK_SELECTION_MULTIPLE);
  gtk_widget_add_css_class(GTK_WIDGET(sidebar_), "navigation-sidebar");
  gtk_list_box_set_activate_on_single_click(sidebar_, TRUE);
  gtk_scrolled_window_set_child(sidebar_scroll, GTK_WIDGET(sidebar_));
  gtk_paned_set_start_child(split_, GTK_WIDGET(sidebar_scroll));
  auto* modifier_tracker = gtk_event_controller_legacy_new();
  g_signal_connect(modifier_tracker, "event", G_CALLBACK(+[](
      GtkEventControllerLegacy*, GdkEvent* event, gpointer data) -> gboolean {
    auto* w = self(data);
    if (gdk_event_get_event_type(event) == GDK_BUTTON_PRESS)
      w->sidebar_modifiers_ = gdk_event_get_modifier_state(event);
    return GDK_EVENT_PROPAGATE;
  }), this);
  gtk_widget_add_controller(GTK_WIDGET(sidebar_), modifier_tracker);
  g_signal_connect_swapped(sidebar_, "row-activated", G_CALLBACK(+[](
      Window* w, GtkListBoxRow* row) {
    if (!row) return;
    GList* rows = gtk_list_box_get_selected_rows(w->sidebar_);
    const bool single = rows && !rows->next;
    g_list_free(rows);
    if (single) w->select_page(row_index(row));
  }), this);
  g_signal_connect(sidebar_, "row-selected", G_CALLBACK(+[](
      GtkListBox*, GtkListBoxRow* row, gpointer data) {
    auto* w = self(data);
    if (w->syncing_sidebar_ || !row || !w->document_) return;
    const bool modifier =
        (w->sidebar_modifiers_ & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) != 0;
    if (!modifier) {
      GList* selected = gtk_list_box_get_selected_rows(w->sidebar_);
      if (selected && selected->next) {
        w->syncing_sidebar_ = true;
        gtk_list_box_unselect_all(w->sidebar_);
        gtk_list_box_select_row(w->sidebar_, row);
        w->syncing_sidebar_ = false;
      }
      g_list_free(selected);
    }
    GList* rows = gtk_list_box_get_selected_rows(w->sidebar_);
    const bool single = rows && !rows->next;
    const auto page = row_index(row);
    g_list_free(rows);
    if (single && !modifier) w->select_page(page);
    w->update_page_status();
  }), this);
  auto* file_drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
  g_signal_connect(file_drop, "drop", G_CALLBACK(+[](
      GtkDropTarget*, const GValue* value, double, double, gpointer data) -> gboolean {
    auto* w = self(data);
    if (!w->document_) return FALSE;
    GSList* files = static_cast<GSList*>(g_value_get_boxed(value));
    bool imported_any = false;
    std::size_t destination = w->document_->page_count();
    for (GSList* item = files; item; item = item->next) {
      char* path = g_file_get_path(G_FILE(item->data));
      if (!path) continue;
      Document imported; std::string error;
      if (imported.open(path, error) && imported.kind() == Document::Kind::Pdf) {
        std::vector<std::size_t> all(imported.page_count());
        for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
        auto pages = imported.copy_pages(all);
        w->document_->insert_pages(pages, destination);
        destination += pages.size();
        imported_any = true;
      }
      g_free(path);
    }
    if (imported_any) { w->refresh(); w->toast("PDF pages imported"); }
    return imported_any;
  }), this);
  gtk_widget_add_controller(GTK_WIDGET(sidebar_), GTK_EVENT_CONTROLLER(file_drop));

  auto* content = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  canvas_scroll_ = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
  gtk_scrolled_window_set_policy(canvas_scroll_, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(GTK_WIDGET(canvas_scroll_), TRUE);
  canvas_ = GTK_DRAWING_AREA(gtk_drawing_area_new());
  gtk_widget_add_css_class(GTK_WIDGET(canvas_), "page-canvas");
  gtk_widget_set_cursor_from_name(GTK_WIDGET(canvas_), "grab");
  gtk_drawing_area_set_draw_func(canvas_, draw_page, this, nullptr);
  canvas_overlay_ = GTK_OVERLAY(gtk_overlay_new());
  gtk_overlay_set_child(canvas_overlay_, GTK_WIDGET(canvas_));
  inline_text_view_ = GTK_TEXT_VIEW(gtk_text_view_new());
  inline_editor_ = GTK_WIDGET(inline_text_view_);
  gtk_widget_add_css_class(inline_editor_, "inline-text-editor");
  gtk_widget_add_css_class(inline_editor_, "inline-text-editor-view");
  gtk_text_view_set_wrap_mode(inline_text_view_, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(inline_text_view_, FALSE);
  gtk_text_view_set_left_margin(inline_text_view_, 4);
  gtk_text_view_set_right_margin(inline_text_view_, 4);
  gtk_text_view_set_top_margin(inline_text_view_, 4);
  gtk_text_view_set_bottom_margin(inline_text_view_, 4);
  gtk_overlay_add_overlay(canvas_overlay_, inline_editor_);
  gtk_widget_set_halign(inline_editor_, GTK_ALIGN_START);
  gtk_widget_set_valign(inline_editor_, GTK_ALIGN_START);
  gtk_widget_set_visible(inline_editor_, FALSE);
  GtkTextBuffer* editor_buffer = gtk_text_view_get_buffer(inline_text_view_);
  g_signal_connect(editor_buffer, "changed", G_CALLBACK(+[](GtkTextBuffer*, gpointer data) {
    auto* w = self(data);
    if (w->inline_edit_active_) w->apply_inline_text_style();
  }), this);
  g_signal_connect_after(editor_buffer, "insert-text", G_CALLBACK(+[](
      GtkTextBuffer* buffer, GtkTextIter* location, char* text, int len,
      gpointer data) {
    auto* w = self(data);
    if (!w->inline_edit_active_ || w->loading_inline_text_) return;
    if (!w->inline_typing_tag_)
      w->inline_typing_tag_ = w->create_inline_format_tag(w->inline_typing_style_);
    GtkTextIter start = *location;
    GtkTextIter end = *location;
    const int chars = g_utf8_strlen(text, len < 0 ? -1 : len);
    gtk_text_iter_forward_chars(&end, chars);
    if (!gtk_text_iter_equal(&start, &end))
      gtk_text_buffer_apply_tag(buffer, w->inline_typing_tag_, &start, &end);
    w->apply_inline_text_style();
  }), this);
  g_signal_connect(editor_buffer, "notify::cursor-position",
                   G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                     auto* w = self(data);
                     if (w->inline_edit_active_ && !w->updating_annotation_controls_)
                       w->sync_inline_format_controls();
                   }), this);
  g_signal_connect(editor_buffer, "notify::selection-bound",
                   G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                     auto* w = self(data);
                     if (w->inline_edit_active_ && !w->updating_annotation_controls_)
                       w->sync_inline_format_controls();
                   }), this);
  auto* editor_keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(editor_keys, GTK_PHASE_CAPTURE);
  g_signal_connect(editor_keys, "key-pressed", G_CALLBACK(+[](
      GtkEventControllerKey*, guint keyval, guint, GdkModifierType state,
      gpointer data) -> gboolean {
    auto* w = self(data);
    if (!w->inline_edit_active_) return FALSE;
    if (keyval == GDK_KEY_Escape) {
      w->finish_inline_text_edit(false);
      return TRUE;
    }
    if (keyval == GDK_KEY_Return && (state & GDK_CONTROL_MASK)) {
      w->finish_inline_text_edit(true);
      return TRUE;
    }
    return FALSE;
  }), this);
  gtk_widget_add_controller(GTK_WIDGET(inline_text_view_), editor_keys);
  gtk_scrolled_window_set_child(canvas_scroll_, GTK_WIDGET(canvas_overlay_));
  g_signal_connect(gtk_scrolled_window_get_vadjustment(canvas_scroll_), "value-changed",
                   G_CALLBACK(+[](GtkAdjustment*, gpointer data) {
                     auto* w = self(data);
                     w->note_scroll_activity();
                     w->update_current_page_from_scroll();
                   }), this);
  gtk_box_append(content, GTK_WIDGET(canvas_scroll_));

  auto* page_scroll = gtk_event_controller_scroll_new(
      static_cast<GtkEventControllerScrollFlags>(
          GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_KINETIC));
  gtk_event_controller_set_propagation_phase(page_scroll, GTK_PHASE_CAPTURE);
  g_signal_connect(page_scroll, "scroll", G_CALLBACK(+[](
      GtkEventControllerScroll*, double, double dy, gpointer data) -> gboolean {
    auto* w = self(data);
    if (w->continuous_scroll_) return FALSE;
    if (!w->document_ || dy == 0.0 || w->document_->page_count() < 2) return FALSE;
    auto* adjustment = gtk_scrolled_window_get_vadjustment(w->canvas_scroll_);
    const double lower = gtk_adjustment_get_lower(adjustment);
    const double maximum = std::max(
        lower, gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment));
    const double value = gtk_adjustment_get_value(adjustment);
    const bool at_top = value <= lower + 0.5;
    const bool at_bottom = value >= maximum - 0.5;
    const int direction = dy > 0.0 ? 1 : -1;
    const bool can_next = direction > 0 && at_bottom &&
                          w->current_page_ + 1 < w->document_->page_count();
    const bool can_previous = direction < 0 && at_top && w->current_page_ > 0;
    if (!can_next && !can_previous) return FALSE;
    if (w->page_turn_pending_) return TRUE;
    w->page_turn_pending_ = true;
    if (can_next) w->select_page(w->current_page_ + 1, false);
    else w->select_page(w->current_page_ - 1, true);
    return TRUE;
  }), this);
  gtk_widget_add_controller(GTK_WIDGET(canvas_scroll_), page_scroll);

  auto* status = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
  gtk_widget_set_margin_start(GTK_WIDGET(status), 10);
  gtk_widget_set_margin_end(GTK_WIDGET(status), 10);
  gtk_widget_set_margin_top(GTK_WIDGET(status), 4);
  gtk_widget_set_margin_bottom(GTK_WIDGET(status), 4);
  page_label_ = GTK_LABEL(gtk_label_new("No document"));
  gtk_box_append(status, GTK_WIDGET(page_label_));
  gtk_box_append(content, GTK_WIDGET(status));
  gtk_paned_set_end_child(split_, GTK_WIDGET(content));

  auto* drag = gtk_gesture_drag_new();
  g_signal_connect(drag, "drag-begin", G_CALLBACK((+[](
      GtkGestureDrag* gesture, double x, double y, gpointer data) {
    auto* w = self(data);
    if (!w->document_) return;
    GdkDevice* device = gtk_gesture_get_device(GTK_GESTURE(gesture));
    const bool touch =
        device && gdk_device_get_source(device) == GDK_SOURCE_TOUCHSCREEN;
    if (w->inline_edit_active_ &&
        g_get_monotonic_time() - w->inline_edit_started_at_ > 150000)
      w->finish_inline_text_edit(true);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    if (w->continuous_scroll_) w->set_current_page(w->page_at_canvas_y(y));
    w->drag_origin_canvas_ = {x, y};
    w->drag_start_ = w->canvas_to_page(x, y);
    w->drag_current_ = w->drag_start_;
    w->drag_is_text_ = w->tool_active_ && w->active_tool_ == AnnotationType::TextBox;
    w->drag_active_ = false;
    w->touch_panning_ = false;
    w->annotation_drag_id_ = 0;
    w->annotation_drag_page_ = w->current_page_;
    w->annotation_origin_page_ = w->current_page_;
    if (w->pointer_mode_ == PointerMode::Select && !w->crop_mode_) {
      // The resize handle is centered on the bottom-right edge, so half of it
      // deliberately lies outside the annotation's normal hit rectangle.
      // Give an already-selected handle priority before ordinary hit-testing.
      w->resizing_annotation_ = false;
      if (auto* selected = w->selected_annotation()) {
        const auto& b = selected->bounds;
        const double handle_tolerance = 14.0 / w->zoom_;
        if (std::abs(w->drag_start_.x - (b.x + b.width)) <= handle_tolerance &&
            std::abs(w->drag_start_.y - (b.y + b.height)) <= handle_tolerance) {
          w->annotation_edit_origin_ = *selected;
          w->annotation_drag_page_ = w->current_page_;
          w->annotation_origin_page_ = w->current_page_;
          w->annotation_drag_id_ = selected->id;
          w->drag_active_ = true;
          w->resizing_annotation_ = true;
          w->annotation_edit_started_ = false;
          return;
        }
      }
      w->select_annotation_at(w->drag_start_);
      if (touch && !w->selected_annotation()) {
        w->touch_panning_ = true;
        w->pan_horizontal_start_ = gtk_adjustment_get_value(
            gtk_scrolled_window_get_hadjustment(w->canvas_scroll_));
        w->pan_vertical_start_ = gtk_adjustment_get_value(
            gtk_scrolled_window_get_vadjustment(w->canvas_scroll_));
        w->annotation_edit_started_ = false;
        return;
      }
      if (auto* annotation = w->selected_annotation()) {
        w->annotation_edit_origin_ = *annotation;
        w->annotation_drag_page_ = w->current_page_;
        w->annotation_origin_page_ = w->current_page_;
        w->annotation_drag_id_ = annotation->id;
        w->drag_active_ = true;
      }
      w->annotation_edit_started_ = false;
      return;
    }
    if (!w->tool_active_ && !w->crop_mode_) return;
    w->drag_active_ = true;
    w->ink_points_.clear();
    w->ink_points_.push_back(w->drag_start_);
  })), this);
  g_signal_connect(drag, "drag-update", G_CALLBACK((+[](
      GtkGestureDrag*, double dx, double dy, gpointer data) {
    auto* w = self(data);
    if (w->touch_panning_) {
      gtk_adjustment_set_value(gtk_scrolled_window_get_hadjustment(w->canvas_scroll_),
                               w->pan_horizontal_start_ - dx);
      gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(w->canvas_scroll_),
                               w->pan_vertical_start_ - dy);
      return;
    }
    if (w->pointer_mode_ == PointerMode::Select && !w->crop_mode_) {
      auto* annotation = w->annotation_at(w->annotation_drag_page_, w->annotation_drag_id_);
      if (!annotation || !w->annotation_drag_id_ ||
          (std::abs(dx) < 2 && std::abs(dy) < 2))
        return;
      if (!w->annotation_edit_started_) {
        w->document_->begin_annotation_edit();
        w->annotation_edit_started_ = true;
      }
      std::size_t target_page = w->annotation_origin_page_;
      if (w->continuous_scroll_ && !w->resizing_annotation_)
        target_page = w->page_at_canvas_y(w->drag_origin_canvas_.y + dy);
      double x_shift = 0.0;
      double y_shift = 0.0;
      if (target_page != w->annotation_origin_page_) {
        const double canvas_width = gtk_widget_get_width(GTK_WIDGET(w->canvas_));
        const double source_offset = std::max(0.0,
            (canvas_width -
             w->document_->page(w->annotation_origin_page_).width_points * w->zoom_) / 2.0);
        const double target_offset = std::max(0.0,
            (canvas_width -
             w->document_->page(target_page).width_points * w->zoom_) / 2.0);
        x_shift = (source_offset - target_offset) / w->zoom_;
        y_shift = (w->page_top(w->annotation_origin_page_) -
                   w->page_top(target_page)) / w->zoom_;
      }
      Annotation updated = w->annotation_edit_origin_;
      w->move_dragged_annotation(updated, dx, dy, x_shift, y_shift);
      if (target_page != w->annotation_drag_page_) {
        if (w->document_->move_annotation(w->annotation_drag_page_,
                                          w->annotation_drag_id_, target_page, updated)) {
          w->annotation_drag_page_ = target_page;
          w->show_current_page(target_page);
        }
      } else {
        *annotation = updated;
      }
      w->document_->invalidate_rendering();
      gtk_widget_queue_draw(GTK_WIDGET(w->canvas_));
      return;
    }
    if (!w->tool_active_ && !w->crop_mode_) return;
    w->drag_current_ = {w->drag_start_.x + dx / w->zoom_,
                        w->drag_start_.y + dy / w->zoom_};
    if (!w->crop_mode_ && w->active_tool_ == AnnotationType::Ink)
      w->ink_points_.push_back(w->drag_current_);
    gtk_widget_queue_draw(GTK_WIDGET(w->canvas_));
  })), this);
  g_signal_connect(drag, "drag-end", G_CALLBACK((+[](
      GtkGestureDrag*, double dx, double dy, gpointer data) {
    auto* w = self(data);
    if (w->touch_panning_) {
      w->touch_panning_ = false;
      return;
    }
    if (!w->drag_active_ &&
        (w->pointer_mode_ != PointerMode::Select || w->crop_mode_))
      return;
    if (w->pointer_mode_ == PointerMode::Select && !w->crop_mode_) {
      if (w->annotation_edit_started_) {
        w->document_->commit_annotation_edit();
        w->annotation_edit_started_ = false;
        w->update_title();
      }
      if (w->annotation_drag_page_ != w->current_page_)
        w->show_current_page(w->annotation_drag_page_);
      w->annotation_drag_id_ = 0;
      w->drag_active_ = false;
      w->sync_annotation_controls();
      gtk_widget_queue_draw(GTK_WIDGET(w->canvas_));
      return;
    }
    w->drag_active_ = false;
    w->drag_is_text_ = false;
    gtk_widget_queue_draw(GTK_WIDGET(w->canvas_));
    w->finish_canvas_drag((w->drag_start_.x * w->zoom_ + dx),
                          (w->drag_start_.y * w->zoom_ + dy));
  })), this);
  gtk_widget_add_controller(GTK_WIDGET(canvas_), GTK_EVENT_CONTROLLER(drag));

  auto* zoom_gesture = gtk_gesture_zoom_new();
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(zoom_gesture),
                                              GTK_PHASE_CAPTURE);
  g_signal_connect(zoom_gesture, "begin", G_CALLBACK(+[](
      GtkGesture* gesture, GdkEventSequence*, gpointer data) {
    auto* w = self(data);
    if (!w->document_) return;
    if (w->inline_edit_active_) w->finish_inline_text_edit(true);
    auto* hadjustment = gtk_scrolled_window_get_hadjustment(w->canvas_scroll_);
    auto* vadjustment = gtk_scrolled_window_get_vadjustment(w->canvas_scroll_);
    double x = 0.0;
    double y = 0.0;
    bool have_center = false;
    GdkRectangle box{};
    if (gtk_gesture_get_bounding_box(gesture, &box)) {
      x = box.x + box.width / 2.0;
      y = box.y + box.height / 2.0;
      have_center = true;
    } else {
      x = gtk_adjustment_get_value(hadjustment) +
          gtk_adjustment_get_page_size(hadjustment) / 2.0;
      y = gtk_adjustment_get_value(vadjustment) +
          gtk_adjustment_get_page_size(vadjustment) / 2.0;
    }
    w->pinch_start_zoom_ = w->zoom_;
    w->pinch_anchor_page_ = w->continuous_scroll_ ? w->page_at_canvas_y(y) :
                                                     w->current_page_;
    w->pinch_anchor_ = w->canvas_to_page(x, y);
    graphene_rect_t canvas_bounds;
    if (have_center && gtk_widget_compute_bounds(GTK_WIDGET(w->canvas_),
                                                 GTK_WIDGET(w->canvas_scroll_),
                                                 &canvas_bounds)) {
      w->pinch_view_x_ = canvas_bounds.origin.x + x;
      w->pinch_view_y_ = canvas_bounds.origin.y + y;
    } else {
      w->pinch_view_x_ = gtk_adjustment_get_page_size(hadjustment) / 2.0;
      w->pinch_view_y_ = gtk_adjustment_get_page_size(vadjustment) / 2.0;
    }
    w->pinch_active_ = true;
    w->touch_panning_ = false;
    w->drag_active_ = false;
    w->ink_points_.clear();
  }), this);
  g_signal_connect(zoom_gesture, "scale-changed", G_CALLBACK((+[](
      GtkGestureZoom* gesture, double scale, gpointer data) {
    auto* w = self(data);
    if (!w->pinch_active_ || !w->document_) return;
    const double next_zoom = std::clamp(w->pinch_start_zoom_ * scale, 0.15, 5.0);
    if (std::abs(next_zoom - w->zoom_) < 0.0005) return;
    w->fit_width_ = false;
    w->zoom_ = next_zoom;
    w->render_current();
    struct PinchScroll {
      Window* window;
      std::size_t page;
      Point anchor;
      double view_x{};
      double view_y{};
    };
    auto* request = new PinchScroll{w, w->pinch_anchor_page_, w->pinch_anchor_,
                                    w->pinch_view_x_, w->pinch_view_y_};
    gtk_widget_add_tick_callback(GTK_WIDGET(w->canvas_scroll_), +[](
        GtkWidget* widget, GdkFrameClock*, gpointer data) -> gboolean {
      auto* request = static_cast<PinchScroll*>(data);
      auto* window = request->window;
      if (!window->document_) {
        delete request;
        return G_SOURCE_REMOVE;
      }
      const Point content = window->page_to_canvas(request->page, request->anchor);
      auto* hadjustment = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(widget));
      auto* vadjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(widget));
      auto set_clamped = [](GtkAdjustment* adjustment, double value) {
        const double lower = gtk_adjustment_get_lower(adjustment);
        const double upper = std::max(lower, gtk_adjustment_get_upper(adjustment) -
                                               gtk_adjustment_get_page_size(adjustment));
        gtk_adjustment_set_value(adjustment, std::clamp(value, lower, upper));
      };
      set_clamped(hadjustment, content.x - request->view_x);
      set_clamped(vadjustment, content.y - request->view_y);
      delete request;
      return G_SOURCE_REMOVE;
    }, request, nullptr);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  })), this);
  g_signal_connect(zoom_gesture, "end", G_CALLBACK(+[](
      GtkGesture*, GdkEventSequence*, gpointer data) {
    auto* w = self(data);
    if (!w->pinch_active_) return;
    w->pinch_active_ = false;
    w->clear_page_cache();
    w->render_current();
  }), this);
  gtk_widget_add_controller(GTK_WIDGET(canvas_), GTK_EVENT_CONTROLLER(zoom_gesture));

  auto* double_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(double_click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(double_click),
                                              GTK_PHASE_CAPTURE);
  g_signal_connect(double_click, "pressed", G_CALLBACK(+[](
      GtkGestureClick*, int n_press, double x, double y, gpointer data) {
    auto* w = self(data);
    if (n_press != 2 || !w->document_) return;
    if (w->continuous_scroll_) w->set_current_page(w->page_at_canvas_y(y));
    const Point point = w->canvas_to_page(x, y);
    w->select_annotation_at(point);
    if (auto* annotation = w->selected_annotation();
        annotation && annotation->type == AnnotationType::TextBox)
      w->edit_text_annotation(point);
  }), this);
  gtk_widget_add_controller(GTK_WIDGET(canvas_), GTK_EVENT_CONTROLLER(double_click));

  auto* keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(keys), GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](
      GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) -> gboolean {
    auto* w = self(data);
    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(w->window_));
    if (keyval == GDK_KEY_Delete && !GTK_IS_EDITABLE(focus) &&
        w->document_ && w->selected_annotation_id_) {
      if (w->document_->remove_annotation(w->current_page_, w->selected_annotation_id_)) {
        w->selected_annotation_id_ = 0;
        w->render_current();
        w->update_title();
      }
      return TRUE;
    }
    if (keyval == GDK_KEY_Delete && !GTK_IS_EDITABLE(focus) && w->document_ &&
        focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(w->sidebar_))) {
      w->document_->remove(w->selected_pages());
      w->refresh();
      return TRUE;
    }
    return FALSE;
  }), this);
  gtk_widget_add_controller(GTK_WIDGET(window_), GTK_EVENT_CONTROLLER(keys));
}

void Window::install_actions() {
  const GActionEntry entries[] = {
    {"open", +[](GSimpleAction*, GVariant*, gpointer d){ self(d)->choose_open(); }, nullptr, nullptr, nullptr, {}},
    {"open-recent", +[](GSimpleAction*, GVariant* value, gpointer d){
      if (value) self(d)->open(g_variant_get_string(value, nullptr));
    }, "s", nullptr, nullptr, {}},
    {"save", +[](GSimpleAction*, GVariant*, gpointer d){ self(d)->save(false); }, nullptr, nullptr, nullptr, {}},
    {"save-as", +[](GSimpleAction*, GVariant*, gpointer d){ self(d)->save(true); }, nullptr, nullptr, nullptr, {}},
    {"export-flattened", +[](GSimpleAction*, GVariant*, gpointer d){
      auto* w = self(d); if (!w->document_) return;
      if (w->inline_edit_active_) w->finish_inline_text_edit(true);
      auto* dialog = gtk_file_dialog_new();
      gtk_file_dialog_set_initial_name(dialog, "flattened.pdf");
      gtk_file_dialog_save(dialog, GTK_WINDOW(w->window_), nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
          auto* w = self(data); GError* e = nullptr;
          GFile* f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &e);
          if (!f) { if (e) g_error_free(e); return; }
          char* path = g_file_get_path(f); std::string error;
          if (!w->document_->export_flattened(path, error)) w->show_error("Export failed", error);
          else w->toast("Flattened copy exported");
          g_free(path); g_object_unref(f);
        }, w);
      g_object_unref(dialog);
    }, nullptr, nullptr, nullptr, {}},
    {"undo", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d); if(w->document_&&w->document_->undo())w->refresh(); }, nullptr, nullptr, nullptr, {}},
    {"redo", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d); if(w->document_&&w->document_->redo())w->refresh(); }, nullptr, nullptr, nullptr, {}},
    {"copy-pages", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d); if(!w->document_)return; page_clipboard=w->document_->copy_pages(w->selected_pages()); clipboard_is_cut=false; w->toast("Pages copied"); }, nullptr, nullptr, nullptr, {}},
    {"cut-pages", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d); if(!w->document_)return; auto s=w->selected_pages(); page_clipboard=w->document_->copy_pages(s); clipboard_is_cut=true; w->document_->remove(s); w->refresh(); }, nullptr, nullptr, nullptr, {}},
    {"paste-pages", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d); if(!w->document_||page_clipboard.empty())return; w->document_->insert_pages(page_clipboard,w->current_page_+1); if(clipboard_is_cut){page_clipboard.clear();clipboard_is_cut=false;}w->refresh(); }, nullptr, nullptr, nullptr, {}},
    {"delete-pages", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d);if(!w->document_)return;w->document_->remove(w->selected_pages());w->refresh(); }, nullptr, nullptr, nullptr, {}},
    {"duplicate-pages", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d);if(!w->document_)return;w->document_->duplicate(w->selected_pages(),w->current_page_+1);w->refresh(); }, nullptr, nullptr, nullptr, {}},
    {"rotate-left", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d);if(w->document_){w->document_->rotate(w->selected_pages(),-90);w->refresh();} }, nullptr, nullptr, nullptr, {}},
    {"rotate-right", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d);if(w->document_){w->document_->rotate(w->selected_pages(),90);w->refresh();} }, nullptr, nullptr, nullptr, {}},
    {"import-pages", +[](GSimpleAction*, GVariant*, gpointer d){ self(d)->choose_import_pdf(); }, nullptr, nullptr, nullptr, {}},
    {"scan-pages", +[](GSimpleAction*, GVariant*, gpointer d){ self(d)->start_destructive(false); }, nullptr, nullptr, nullptr, {}},
    {"secure-crop", +[](GSimpleAction*, GVariant*, gpointer d){ self(d)->start_destructive(true); }, nullptr, nullptr, nullptr, {}},
    {"draw-signature", +[](GSimpleAction*, GVariant*, gpointer d){ self(d)->draw_signature_dialog(); }, nullptr, nullptr, nullptr, {}},
    {"import-signature", +[](GSimpleAction*, GVariant*, gpointer d){ self(d)->choose_signature_image(); }, nullptr, nullptr, nullptr, {}},
    {"continuous-scroll", nullptr, nullptr, "false", +[](GSimpleAction*, GVariant*, gpointer d){
      self(d)->set_continuous_scroll(true);
    }, {}},
    {"single-page", nullptr, nullptr, "true", +[](GSimpleAction*, GVariant*, gpointer d){
      self(d)->set_continuous_scroll(false);
    }, {}},
    {"zoom-in", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d);if(w->inline_edit_active_)w->finish_inline_text_edit(true);w->fit_width_=false;w->zoom_=std::min(5.0,w->zoom_*1.2);w->render_current(); }, nullptr, nullptr, nullptr, {}},
    {"zoom-out", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d);if(w->inline_edit_active_)w->finish_inline_text_edit(true);w->fit_width_=false;w->zoom_=std::max(.15,w->zoom_/1.2);w->render_current(); }, nullptr, nullptr, nullptr, {}},
    {"fit-width", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d);if(w->inline_edit_active_)w->finish_inline_text_edit(true);w->fit_width_=true;w->fit_width_dirty_=true;w->render_current(); }, nullptr, nullptr, nullptr, {}},
    {"print", +[](GSimpleAction*, GVariant*, gpointer d){
      auto*w=self(d); if(!w->document_)return;
      if(w->inline_edit_active_)w->finish_inline_text_edit(true);
      gchar* argv[]={const_cast<gchar*>("lp"),const_cast<gchar*>(w->document_->path().c_str()),nullptr};GError*e=nullptr;
      if(!g_spawn_async(nullptr,argv,nullptr,G_SPAWN_SEARCH_PATH,nullptr,nullptr,nullptr,&e)){w->show_error("Printing failed",e?e->message:"Unable to start lp");if(e)g_error_free(e);}else w->toast("Sent to the default printer");
    }, nullptr, nullptr, nullptr, {}},
  };
  g_action_map_add_action_entries(G_ACTION_MAP(window_), entries, G_N_ELEMENTS(entries), this);
  update_view_actions();

  auto* app = gtk_window_get_application(GTK_WINDOW(window_));
  const struct { const char* action; const char* accel; } shortcuts[] = {
    {"win.open", "<primary>o"}, {"win.save", "<primary>s"},
    {"win.save-as", "<primary><shift>s"}, {"win.undo", "<primary>z"},
    {"win.redo", "<primary><shift>z"}, {"win.copy-pages", "<primary>c"},
    {"win.cut-pages", "<primary>x"}, {"win.paste-pages", "<primary>v"},
    {"win.continuous-scroll", "<primary>1"},
    {"win.single-page", "<primary>2"},
    {"win.zoom-in", "<primary>plus"},
    {"win.zoom-out", "<primary>minus"}, {"win.print", "<primary>p"},
  };
  for (const auto& shortcut : shortcuts) {
    const char* values[] = {shortcut.accel, nullptr};
    gtk_application_set_accels_for_action(app, shortcut.action, values);
  }
}

void Window::open(const std::string& path) {
  if (inline_edit_active_) finish_inline_text_edit(true);
  auto candidate = std::make_unique<Document>();
  std::string error;
  if (!candidate->open(path, error)) {
    std::string lower = error;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find("encrypt") != std::string::npos || lower.find("password") != std::string::npos) {
      auto* dialog = adw_message_dialog_new(GTK_WINDOW(window_), "Password required",
                                            "Enter the PDF password to open it in view-only mode.");
      auto* entry = gtk_password_entry_new();
      gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
      adw_message_dialog_set_extra_child(ADW_MESSAGE_DIALOG(dialog), entry);
      adw_message_dialog_add_responses(ADW_MESSAGE_DIALOG(dialog), "cancel", "Cancel",
                                       "open", "Open", nullptr);
      adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), "open");
      g_object_set_data_full(G_OBJECT(dialog), "review-path", g_strdup(path.c_str()), g_free);
      g_object_set_data(G_OBJECT(dialog), "review-password", entry);
      g_signal_connect(dialog, "response", G_CALLBACK(+[](
          AdwMessageDialog* dialog, const char* response, gpointer data) {
        auto* w = self(data);
        if (g_strcmp0(response, "open") == 0) {
          const char* requested_path = static_cast<const char*>(
              g_object_get_data(G_OBJECT(dialog), "review-path"));
          auto* password_entry = GTK_EDITABLE(
              g_object_get_data(G_OBJECT(dialog), "review-password"));
          auto document = std::make_unique<Document>();
          std::string open_error;
          if (!document->open(requested_path, open_error,
                              gtk_editable_get_text(password_entry)))
            w->show_error("Unable to open PDF", open_error);
          else {
            w->document_ = std::move(document); w->current_page_ = 0;
            w->fit_width_ = true; w->zoom_ = 1.0; w->refresh();
            w->scroll_to_page(0, false);
            w->remember_recent(requested_path);
            w->toast("Protected PDF opened in view-only mode");
          }
        }
        gtk_window_destroy(GTK_WINDOW(dialog));
      }), this);
      gtk_window_present(GTK_WINDOW(dialog));
      return;
    }
    show_error("Unable to open file", error);
    return;
  }
  document_ = std::move(candidate);
  current_page_ = 0;
  zoom_ = 1.0;
  fit_width_ = true;
  warned_destructive_ = false;
  remember_recent(path);
  refresh();
  scroll_to_page(0, false);
}

void Window::remember_recent(const std::string& path) {
  const auto directory = fs::path(g_get_user_data_dir()) / "review";
  g_mkdir_with_parents(directory.c_str(), 0700);
  const auto recent_path = directory / "recent-files";
  std::vector<std::string> paths{fs::absolute(path).string()};
  std::ifstream input(recent_path);
  std::string line;
  while (std::getline(input, line) && paths.size() < 10)
    if (!line.empty() && line != paths.front() && fs::exists(line)) paths.push_back(line);
  std::ofstream output(recent_path, std::ios::trunc);
  for (const auto& item : paths) output << item << '\n';
  output.close();
  chmod(recent_path.c_str(), 0600);
  rebuild_recent_menu();
}

void Window::rebuild_recent_menu() {
  if (!recent_menu_) return;
  g_menu_remove_all(recent_menu_);
  const auto recent_path = fs::path(g_get_user_data_dir()) / "review" / "recent-files";
  std::ifstream input(recent_path);
  std::string path;
  while (std::getline(input, path)) {
    if (path.empty() || !fs::exists(path)) continue;
    auto* item = g_menu_item_new(fs::path(path).filename().c_str(), nullptr);
    g_menu_item_set_action_and_target(item, "win.open-recent", "s", path.c_str());
    g_menu_append_item(recent_menu_, item);
    g_object_unref(item);
  }
}

void Window::refresh() {
  if (inline_edit_active_) finish_inline_text_edit(true);
  if (page_render_source_id_) {
    g_source_remove(page_render_source_id_);
    page_render_source_id_ = 0;
  }
  pending_page_renders_.clear();
  rebuild_sidebar();
  if (document_ && current_page_ >= document_->page_count())
    current_page_ = document_->page_count() - 1;
  render_current();
  update_title();
}

void Window::rebuild_sidebar() {
  syncing_sidebar_ = true;
  GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(sidebar_));
  while (child) {
    GtkWidget* next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(sidebar_, child);
    child = next;
  }
  if (!document_) {
    syncing_sidebar_ = false;
    return;
  }
  for (std::size_t i = 0; i < document_->page_count(); ++i) {
    auto* box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
    cairo_surface_t* surface = document_->render_page(i, .16, true);
    if (surface) {
      const int sw = cairo_image_surface_get_width(surface);
      const int sh = cairo_image_surface_get_height(surface);
      const int stride = cairo_image_surface_get_stride(surface);
      const gsize size = static_cast<gsize>(stride) * sh;
      GBytes* bytes = g_bytes_new(cairo_image_surface_get_data(surface), size);
      auto* texture = gdk_memory_texture_new(sw, sh, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                             bytes, stride);
      auto* picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
      gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
      gtk_widget_set_size_request(picture, 130, 150);
      gtk_box_append(box, picture);
      g_object_unref(texture);
      g_bytes_unref(bytes);
      cairo_surface_destroy(surface);
    }
    auto* label = gtk_label_new(("Page " + std::to_string(i + 1)).c_str());
    gtk_box_append(box, label);
    auto* row = GTK_LIST_BOX_ROW(gtk_list_box_row_new());
    gtk_list_box_row_set_child(row, GTK_WIDGET(box));

    auto* drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, static_cast<GdkDragAction>(GDK_ACTION_COPY | GDK_ACTION_MOVE));
    g_signal_connect(drag, "prepare", G_CALLBACK(+[](
        GtkDragSource* source, double, double, gpointer data) -> GdkContentProvider* {
      auto* w = self(data);
      auto* row = GTK_LIST_BOX_ROW(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source)));
      dragged_source = w;
      dragged_indices = w->selected_pages();
      const auto index = row_index(row);
      if (std::find(dragged_indices.begin(), dragged_indices.end(), index) == dragged_indices.end())
        dragged_indices = {index};
      dragged_pages = w->document_->copy_pages(dragged_indices);
      GValue value = G_VALUE_INIT;
      g_value_init(&value, G_TYPE_STRING);
      g_value_set_string(&value, "review-pages");
      auto* provider = gdk_content_provider_new_for_value(&value);
      g_value_unset(&value);
      return provider;
    }), this);
    gtk_widget_add_controller(GTK_WIDGET(row), GTK_EVENT_CONTROLLER(drag));

    auto* drop = gtk_drop_target_new(G_TYPE_STRING,
        static_cast<GdkDragAction>(GDK_ACTION_COPY | GDK_ACTION_MOVE));
    g_signal_connect(drop, "drop", G_CALLBACK(+[](
        GtkDropTarget* target, const GValue*, double, double, gpointer data) -> gboolean {
      auto* w = self(data);
      auto* row = GTK_LIST_BOX_ROW(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)));
      if (!w->document_ || dragged_pages.empty()) return FALSE;
      const auto destination = row_index(row);
      if (dragged_source == w)
        w->document_->reorder(dragged_indices, destination);
      else
        w->document_->insert_pages(dragged_pages, destination);
      w->refresh();
      return TRUE;
    }), this);
    gtk_widget_add_controller(GTK_WIDGET(row), GTK_EVENT_CONTROLLER(drop));

    gtk_list_box_append(sidebar_, GTK_WIDGET(row));
    if (i == current_page_) gtk_list_box_select_row(sidebar_, row);
  }
  syncing_sidebar_ = false;
}

double Window::page_top(std::size_t page) const {
  double top = kCanvasMargin;
  if (!document_) return top;
  for (std::size_t i = 0; i < page && i < document_->page_count(); ++i)
    top += document_->page(i).height_points * zoom_ + kPageGap;
  return top;
}

std::size_t Window::page_at_canvas_y(double y) const {
  if (!document_ || document_->page_count() == 0) return 0;
  double top = kCanvasMargin;
  for (std::size_t i = 0; i < document_->page_count(); ++i) {
    const double height = document_->page(i).height_points * zoom_;
    if (y < top + height) return i;
    top += height + kPageGap;
  }
  return document_->page_count() - 1;
}

void Window::update_page_status() {
  if (!page_label_) return;
  if (!document_ || document_->page_count() == 0) {
    gtk_label_set_text(page_label_, "No document");
    return;
  }
  const std::string zoom = std::to_string(static_cast<int>(zoom_ * 100)) + "%";
  GList* rows = sidebar_ ? gtk_list_box_get_selected_rows(sidebar_) : nullptr;
  const int selected = g_list_length(rows);
  g_list_free(rows);
  const std::string status = selected > 1 ?
      std::to_string(selected) + " pages selected  •  " + zoom :
      "Page " + std::to_string(current_page_ + 1) + " of " +
          std::to_string(document_->page_count()) + "  •  " + zoom;
  gtk_label_set_text(page_label_, status.c_str());
}

void Window::render_current() {
  if (!document_ || document_->page_count() == 0) {
    gtk_drawing_area_set_content_width(canvas_, 1);
    gtk_drawing_area_set_content_height(canvas_, 1);
    update_page_status();
    gtk_widget_queue_draw(GTK_WIDGET(canvas_));
    return;
  }
  if (current_page_ >= document_->page_count())
    current_page_ = document_->page_count() - 1;

  double reference_width = document_->page(current_page_).width_points;
  if (continuous_scroll_) {
    reference_width = 0;
    for (std::size_t i = 0; i < document_->page_count(); ++i)
      reference_width = std::max(reference_width, document_->page(i).width_points);
  }
  if (fit_width_) {
    const int margins = continuous_scroll_ ? static_cast<int>(kCanvasMargin * 2) : 48;
    const int available = std::max(
        200, gtk_widget_get_width(GTK_WIDGET(canvas_scroll_)) - margins);
    if (fit_width_dirty_ || available != last_fit_available_ ||
        reference_width != last_fit_reference_) {
      zoom_ = std::clamp(available / std::max(1.0, reference_width), .15, 3.0);
      fit_width_dirty_ = false;
      last_fit_available_ = available;
      last_fit_reference_ = reference_width;
    }
  }

  if (continuous_scroll_) {
    double max_width = 0;
    double total_height = kCanvasMargin * 2;
    for (std::size_t i = 0; i < document_->page_count(); ++i) {
      const auto& page = document_->page(i);
      max_width = std::max(max_width, page.width_points);
      total_height += page.height_points * zoom_;
      if (i + 1 < document_->page_count()) total_height += kPageGap;
    }
    gtk_drawing_area_set_content_width(
        canvas_, std::max(1, static_cast<int>(std::ceil(max_width * zoom_ + kCanvasMargin * 2))));
    gtk_drawing_area_set_content_height(
        canvas_, std::max(1, static_cast<int>(std::ceil(total_height))));
  } else {
    const auto& page = document_->page(current_page_);
    gtk_drawing_area_set_content_width(
        canvas_, std::max(1, static_cast<int>(std::ceil(page.width_points * zoom_))));
    gtk_drawing_area_set_content_height(
        canvas_, std::max(1, static_cast<int>(std::ceil(page.height_points * zoom_))));
  }
  update_page_status();
  gtk_widget_queue_draw(GTK_WIDGET(canvas_));
  schedule_preload_next_page();
}

void Window::sync_sidebar_selection(std::size_t page) {
  if (!sidebar_) return;
  GList* rows = gtk_list_box_get_selected_rows(sidebar_);
  const bool multi = rows && rows->next;
  g_list_free(rows);
  if (multi) return;
  syncing_sidebar_ = true;
  gtk_list_box_unselect_all(sidebar_);
  if (auto* row = gtk_list_box_get_row_at_index(sidebar_, static_cast<int>(page)))
    gtk_list_box_select_row(sidebar_, row);
  syncing_sidebar_ = false;
}

void Window::show_current_page(std::size_t page) {
  if (!document_ || page >= document_->page_count()) return;
  current_page_ = page;
  sync_sidebar_selection(page);
  update_page_status();
  gtk_widget_queue_draw(GTK_WIDGET(canvas_));
  schedule_preload_next_page();
}

void Window::set_current_page(std::size_t page) {
  if (!document_ || page >= document_->page_count() || page == current_page_) return;
  if (selected_annotation_id_ && !annotation_at(page, selected_annotation_id_))
    selected_annotation_id_ = 0;
  show_current_page(page);
}

void Window::update_current_page_from_scroll() {
  if (!continuous_scroll_ || !document_ || !canvas_scroll_) return;
  if (drag_active_ && pointer_mode_ == PointerMode::Select) return;
  auto* adjustment = gtk_scrolled_window_get_vadjustment(canvas_scroll_);
  const double top = gtk_adjustment_get_value(adjustment);
  const double page_size = gtk_adjustment_get_page_size(adjustment);
  const double lower = gtk_adjustment_get_lower(adjustment);
  const double maximum = gtk_adjustment_get_upper(adjustment) - page_size;
  const bool downward = !has_last_scroll_value_ || top >= last_scroll_value_;
  last_scroll_value_ = top;
  has_last_scroll_value_ = true;

  if (top <= lower + 0.5) {
    set_current_page(0);
  } else if (maximum > 0.0 && top >= maximum - 0.5) {
    set_current_page(document_->page_count() - 1);
  } else {
    // When scrolling down, advance as soon as the next page's top edge enters
    // the viewport. When scrolling up, switch as soon as the previous page's
    // bottom edge does.
    set_current_page(page_at_canvas_y(
        downward ? top + page_size - 1.0 : top + 1.0));
  }
}

void Window::scroll_to_page(std::size_t page, bool scroll_to_bottom) {
  if (!document_ || !canvas_scroll_ || page >= document_->page_count()) return;
  auto* adjustment = gtk_scrolled_window_get_vadjustment(canvas_scroll_);
  double target = gtk_adjustment_get_lower(adjustment);
  if (continuous_scroll_) {
    target = page_top(page);
    if (scroll_to_bottom)
      target += document_->page(page).height_points * zoom_ -
                gtk_adjustment_get_page_size(adjustment);
  } else if (scroll_to_bottom) {
    target = gtk_adjustment_get_upper(adjustment) -
             gtk_adjustment_get_page_size(adjustment);
  }
  struct ScrollRequest {
    Window* window;
    double value;
    std::size_t page;
  };
  auto* request = new ScrollRequest{this, target, page};
  gtk_widget_add_tick_callback(GTK_WIDGET(canvas_scroll_), +[](
      GtkWidget* widget, GdkFrameClock*, gpointer data) -> gboolean {
    auto* request = static_cast<ScrollRequest*>(data);
    auto* adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(widget));
    const double lower = gtk_adjustment_get_lower(adjustment);
    const double upper = gtk_adjustment_get_upper(adjustment) -
                         gtk_adjustment_get_page_size(adjustment);
    gtk_adjustment_set_value(adjustment, std::clamp(request->value, lower,
                                                    std::max(lower, upper)));
    request->window->set_current_page(request->page);
    request->window->page_turn_pending_ = false;
    delete request;
    return G_SOURCE_REMOVE;
  }, request, nullptr);
}

void Window::update_view_actions() {
  auto* map = G_ACTION_MAP(window_);
  if (auto* action = g_action_map_lookup_action(map, "continuous-scroll"))
    g_simple_action_set_state(G_SIMPLE_ACTION(action),
                              g_variant_new_boolean(continuous_scroll_));
  if (auto* action = g_action_map_lookup_action(map, "single-page"))
    g_simple_action_set_state(G_SIMPLE_ACTION(action),
                              g_variant_new_boolean(!continuous_scroll_));
}

void Window::set_continuous_scroll(bool enabled) {
  if (inline_edit_active_) finish_inline_text_edit(true);
  if (enabled == continuous_scroll_) {
    update_view_actions();
    return;
  }
  const auto keep_page = current_page_;
  continuous_scroll_ = enabled;
  selected_annotation_id_ = 0;
  save_preferences();
  update_view_actions();
  render_current();
  if (document_) {
    set_current_page(keep_page);
    scroll_to_page(current_page_, false);
  }
}

void Window::draw_page(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
  auto* w = self(data);
  if (!w->document_ || w->document_->page_count() == 0) {
    cairo_set_source_rgb(cr, .18, .18, .18);
    cairo_paint(cr);
    return;
  }
  double clip_x1 = 0.0;
  double clip_y1 = 0.0;
  double clip_x2 = 0.0;
  double clip_y2 = 0.0;
  cairo_clip_extents(cr, &clip_x1, &clip_y1, &clip_x2, &clip_y2);
  const gint64 draw_start = g_get_monotonic_time();
  double view_top = 0.0;
  double view_bottom = height;
  if (w->continuous_scroll_ && w->canvas_scroll_) {
    auto* adjustment = gtk_scrolled_window_get_vadjustment(w->canvas_scroll_);
    view_top = std::max(0.0, gtk_adjustment_get_value(adjustment));
    view_bottom = view_top + gtk_adjustment_get_page_size(adjustment) + 1.0;
  }
  // Paint only the visible canvas slice. The drawing area can be tens of
  // thousands of pixels tall in continuous mode; filling its whole bounds on
  // every frame is costly and can break partial redraws on some compositors.
  cairo_set_source_rgb(cr, .18, .18, .18);
  cairo_rectangle(cr, 0, view_top, width, std::max(1.0, view_bottom - view_top));
  cairo_fill(cr);
  loading_log("draw continuous=%d current=%zu scrolling=%d pinch=%d canvas=%dx%d clip=%.0f,%.0f..%.0f,%.0f view=%.0f..%.0f",
              w->continuous_scroll_, w->current_page_, w->scrolling_,
              w->pinch_active_, width, height, clip_x1, clip_y1, clip_x2, clip_y2,
              view_top, view_bottom);
  const bool revision_stale =
      w->page_cache_revision_ != w->document_->revision();
  const bool zoom_stale =
      !w->pinch_active_ && w->page_cache_zoom_ > 0.0 &&
      w->page_cache_zoom_ != w->zoom_;

  auto paint_page_surface = [&](cairo_surface_t* surface, double x, double y,
                                double draw_width, double draw_height) {
    const int surface_width = cairo_image_surface_get_width(surface);
    const int surface_height = cairo_image_surface_get_height(surface);
    if (surface_width <= 0 || surface_height <= 0) return;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, draw_width / surface_width, draw_height / surface_height);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
  };

  auto paint_missing_page = [&](std::size_t page, double x, double y,
                                double draw_width, double draw_height) {
    loading_log("placeholder page %zu", page);
    cairo_save(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, x, y, draw_width, draw_height);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x + 0.5, y + 0.5, draw_width - 1.0, draw_height - 1.0);
    cairo_stroke(cr);
    cairo_restore(cr);
  };

  auto draw_selection = [&](double x, double y) {
    if (w->pointer_mode_ != PointerMode::Select) return;
    const auto* annotation = w->selected_annotation();
    if (!annotation) return;
    if (w->inline_edit_active_ && w->inline_edit_id_ == annotation->id) return;
    const auto& b = annotation->bounds;
    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.20, 0.52, 0.95);
    cairo_set_line_width(cr, 2.0);
    const double dash[] = {5.0, 4.0};
    cairo_set_dash(cr, dash, 2, 0);
    cairo_rectangle(cr, x + b.x * w->zoom_, y + b.y * w->zoom_,
                    b.width * w->zoom_, b.height * w->zoom_);
    cairo_stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);
    const double handle = 9.0;
    cairo_rectangle(cr, x + (b.x + b.width) * w->zoom_ - handle / 2.0,
                    y + (b.y + b.height) * w->zoom_ - handle / 2.0,
                    handle, handle);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.20, 0.52, 0.95);
    cairo_stroke(cr);
    cairo_restore(cr);
  };

  auto draw_edit_bounds = [&](double x, double y) {
    if (!w->inline_edit_active_) return;
    const auto* annotation = w->selected_annotation();
    if (!annotation || annotation->id != w->inline_edit_id_) return;
    const auto& b = annotation->bounds;
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.20, 0.52, 0.95, 0.85);
    cairo_set_line_width(cr, 1.0);
    const double dash[] = {4.0, 3.0};
    cairo_set_dash(cr, dash, 2, 0);
    cairo_rectangle(cr, x + b.x * w->zoom_, y + b.y * w->zoom_,
                    b.width * w->zoom_, b.height * w->zoom_);
    cairo_stroke(cr);
    cairo_restore(cr);
  };

  auto draw_text_drag = [&]() {
    if (!w->drag_active_ || !w->drag_is_text_ || !w->tool_active_ ||
        w->current_page_ >= w->document_->page_count()) return;
    const auto& page = w->document_->page(w->current_page_);
    const double offset_x = std::max(
        0.0, (width - page.width_points * w->zoom_) / 2.0);
    const double offset_y = w->continuous_scroll_ ? w->page_top(w->current_page_) :
        std::max(0.0, (height - page.height_points * w->zoom_) / 2.0);
    const double left = offset_x + std::min(w->drag_start_.x, w->drag_current_.x) * w->zoom_;
    const double top = offset_y + std::min(w->drag_start_.y, w->drag_current_.y) * w->zoom_;
    const double box_width =
        std::abs(w->drag_current_.x - w->drag_start_.x) * w->zoom_;
    const double box_height =
        std::abs(w->drag_current_.y - w->drag_start_.y) * w->zoom_;
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.20, 0.52, 0.95, 0.95);
    cairo_set_line_width(cr, 1.5);
    const double dash[] = {5.0, 4.0};
    cairo_set_dash(cr, dash, 2, 0);
    cairo_rectangle(cr, left, top, box_width, box_height);
    cairo_stroke(cr);
    cairo_restore(cr);
  };

  auto draw_tool_preview = [&](double offset_x, double offset_y) {
    if (!w->drag_active_ || w->pointer_mode_ != PointerMode::Annotate ||
        !w->tool_active_ || w->crop_mode_)
      return;
    const auto type = w->active_tool_;
    if (type == AnnotationType::TextBox) return;
    Rect bounds{std::min(w->drag_start_.x, w->drag_current_.x),
                std::min(w->drag_start_.y, w->drag_current_.y),
                std::abs(w->drag_current_.x - w->drag_start_.x),
                std::abs(w->drag_current_.y - w->drag_start_.y)};
    if ((type == AnnotationType::Rectangle || type == AnnotationType::Ellipse) &&
        (bounds.width < 0.5 || bounds.height < 0.5))
      return;
    if (type == AnnotationType::Highlight &&
        std::hypot(w->drag_current_.x - w->drag_start_.x,
                   w->drag_current_.y - w->drag_start_.y) < 0.5)
      return;
    cairo_save(cr);
    cairo_translate(cr, offset_x, offset_y);
    cairo_scale(cr, w->zoom_, w->zoom_);
    cairo_set_line_width(cr, 2.0);
    const auto& stroke = w->annotation_stroke_;
    const auto& fill = w->annotation_fill_;
    switch (type) {
      case AnnotationType::Highlight: {
        cairo_set_source_rgba(cr, fill.red, fill.green, fill.blue, fill.alpha);
        const auto rects = w->document_->highlight_rects(
            w->current_page_, w->drag_start_, w->drag_current_);
        for (const auto& rect : rects) {
          cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
          cairo_fill(cr);
        }
        break;
      }
      case AnnotationType::Rectangle:
      case AnnotationType::Ellipse: {
        if (type == AnnotationType::Rectangle) {
          cairo_rectangle(cr, bounds.x, bounds.y, bounds.width, bounds.height);
        } else {
          cairo_save(cr);
          cairo_translate(cr, bounds.x + bounds.width / 2.0,
                          bounds.y + bounds.height / 2.0);
          cairo_scale(cr, bounds.width / 2.0, bounds.height / 2.0);
          cairo_arc(cr, 0, 0, 1, 0, 2 * G_PI);
          cairo_restore(cr);
        }
        cairo_set_source_rgba(cr, fill.red, fill.green, fill.blue, fill.alpha);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, stroke.red, stroke.green, stroke.blue, stroke.alpha);
        cairo_set_line_width(cr, 2.0);
        cairo_stroke(cr);
        break;
      }
      case AnnotationType::Line:
      case AnnotationType::Arrow: {
        const Point p1 = w->drag_start_;
        const Point p2 = w->drag_current_;
        cairo_set_source_rgba(cr, stroke.red, stroke.green, stroke.blue, stroke.alpha);
        cairo_move_to(cr, p1.x, p1.y);
        cairo_line_to(cr, p2.x, p2.y);
        if (type == AnnotationType::Arrow) {
          const double angle = std::atan2(p2.y - p1.y, p2.x - p1.x);
          const double head = 10.0;
          cairo_move_to(cr, p2.x, p2.y);
          cairo_line_to(cr, p2.x - head * std::cos(angle - 0.45),
                        p2.y - head * std::sin(angle - 0.45));
          cairo_move_to(cr, p2.x, p2.y);
          cairo_line_to(cr, p2.x - head * std::cos(angle + 0.45),
                        p2.y - head * std::sin(angle + 0.45));
        }
        cairo_stroke(cr);
        break;
      }
      case AnnotationType::Ink: {
        if (w->ink_points_.empty()) break;
        cairo_set_source_rgba(cr, stroke.red, stroke.green, stroke.blue, stroke.alpha);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_move_to(cr, w->ink_points_.front().x, w->ink_points_.front().y);
        for (std::size_t i = 1; i < w->ink_points_.size(); ++i)
          cairo_line_to(cr, w->ink_points_[i].x, w->ink_points_[i].y);
        cairo_stroke(cr);
        break;
      }
      default:
        break;
    }
    cairo_restore(cr);
  };

  auto draw_crop_guides = [&](double offset_x, double offset_y) {
    if (!w->crop_mode_ || !w->document_ ||
        w->current_page_ >= w->document_->page_count())
      return;
    const auto& page = w->document_->page(w->current_page_);
    const double page_width = page.width_points;
    const double page_height = page.height_points;
    cairo_save(cr);
    cairo_translate(cr, offset_x, offset_y);
    cairo_scale(cr, w->zoom_, w->zoom_);
    if (!w->drag_active_) {
      cairo_set_source_rgba(cr, 0.20, 0.52, 0.95, 0.85);
      cairo_set_line_width(cr, 1.5 / w->zoom_);
      const double dash[] = {6.0 / w->zoom_, 4.0 / w->zoom_};
      cairo_set_dash(cr, dash, 2, 0);
      cairo_rectangle(cr, 1.0 / w->zoom_, 1.0 / w->zoom_,
                      page_width - 2.0 / w->zoom_, page_height - 2.0 / w->zoom_);
      cairo_stroke(cr);
    } else {
      Rect selection{
        std::min(w->drag_start_.x, w->drag_current_.x),
        std::min(w->drag_start_.y, w->drag_current_.y),
        std::abs(w->drag_current_.x - w->drag_start_.x),
        std::abs(w->drag_current_.y - w->drag_start_.y)};
      selection.x = std::clamp(selection.x, 0.0, page_width);
      selection.y = std::clamp(selection.y, 0.0, page_height);
      selection.width = std::min(selection.width, page_width - selection.x);
      selection.height = std::min(selection.height, page_height - selection.y);
      cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.38);
      cairo_rectangle(cr, 0, 0, page_width, page_height);
      cairo_rectangle(cr, selection.x, selection.y, selection.width, selection.height);
      cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
      cairo_fill(cr);
      cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
      cairo_set_line_width(cr, 1.5 / w->zoom_);
      cairo_rectangle(cr, selection.x, selection.y, selection.width, selection.height);
      cairo_stroke(cr);
    }
    cairo_restore(cr);
  };


  if (!w->continuous_scroll_) {
    const std::uint64_t skip = w->inline_edit_active_ ? w->inline_edit_id_ : 0;
    const bool render_now = skip != 0 || w->drag_active_ || w->crop_mode_ ||
                            revision_stale || zoom_stale;
    cairo_surface_t* surface =
        w->cached_page(w->current_page_, skip, render_now);
    const auto& page = w->document_->page(w->current_page_);
    const double draw_width = page.width_points * w->zoom_;
    const double draw_height = page.height_points * w->zoom_;
    const double x = std::max(0.0, (width - draw_width) / 2.0);
    const double y = std::max(0.0, (height - draw_height) / 2.0);
    if (surface) {
      paint_page_surface(surface, x, y, draw_width, draw_height);
    } else {
      if (!w->pinch_active_) w->schedule_page_render(w->current_page_, skip);
      paint_missing_page(w->current_page_, x, y, draw_width, draw_height);
    }
    draw_selection(x, y);
    draw_edit_bounds(x, y);
    draw_tool_preview(x, y);
    draw_crop_guides(x, y);
    draw_text_drag();
    loading_log("draw end single %.2f ms",
                (g_get_monotonic_time() - draw_start) / 1000.0);
    return;
  }

  auto* adjustment = gtk_scrolled_window_get_vadjustment(w->canvas_scroll_);
  loading_log("view top=%.1f bottom=%.1f page_size=%.1f", view_top, view_bottom,
              gtk_adjustment_get_page_size(adjustment));
  double top = kCanvasMargin;
  for (std::size_t i = 0; i < w->document_->page_count(); ++i) {
    const auto& page = w->document_->page(i);
    const double page_height = page.height_points * w->zoom_;
    const double bottom = top + page_height;
    if (bottom >= view_top && top <= view_bottom) {
      const std::uint64_t skip =
          w->inline_edit_active_ && i == w->inline_edit_page_ ? w->inline_edit_id_ : 0;
      const bool render_now =
          skip != 0 || (i == w->current_page_ &&
                        (w->drag_active_ || w->crop_mode_ || revision_stale ||
                         zoom_stale));
      cairo_surface_t* surface = w->cached_page(i, skip, render_now);
      const double draw_width = page.width_points * w->zoom_;
      const double x = std::max(0.0, (width - draw_width) / 2.0);
      loading_log("visible page %zu top=%.0f cached=%d scrolling=%d",
                  i, top, surface != nullptr, w->scrolling_);
      if (surface) {
        paint_page_surface(surface, x, top, draw_width, page_height);
      } else {
        if (!w->pinch_active_) w->schedule_page_render(i, skip);
        paint_missing_page(i, x, top, draw_width, page_height);
      }
    }
    top = bottom + kPageGap;
  }

  const auto& current = w->document_->page(w->current_page_);
  const double current_x = std::max(
      0.0, (width - current.width_points * w->zoom_) / 2.0);
  draw_selection(current_x, w->page_top(w->current_page_));
  draw_edit_bounds(current_x, w->page_top(w->current_page_));
  draw_tool_preview(current_x, w->page_top(w->current_page_));
  draw_crop_guides(current_x, w->page_top(w->current_page_));
  draw_text_drag();
  loading_log("draw end continuous %.2f ms",
              (g_get_monotonic_time() - draw_start) / 1000.0);
}

void Window::select_page(std::size_t page, bool scroll_to_bottom) {
  if (!document_ || page >= document_->page_count()) return;
  if (inline_edit_active_) finish_inline_text_edit(true);
  if (!continuous_scroll_ && page == current_page_) return;
  set_current_page(page);
  if (!continuous_scroll_) render_current();
  scroll_to_page(page, scroll_to_bottom);
}

Point Window::canvas_to_page(double x, double y) const {
  if (!document_ || document_->page_count() == 0)
    return {x / zoom_, y / zoom_};
  const auto index = continuous_scroll_ ? page_at_canvas_y(y) : current_page_;
  if (index >= document_->page_count()) return {x / zoom_, y / zoom_};
  const auto& page = document_->page(index);
  const double rendered_width = page.width_points * zoom_;
  const double rendered_height = page.height_points * zoom_;
  const double offset_x = std::max(0.0,
      (gtk_widget_get_width(GTK_WIDGET(canvas_)) - rendered_width) / 2.0);
  const double offset_y = continuous_scroll_ ? page_top(index) : std::max(0.0,
      (gtk_widget_get_height(GTK_WIDGET(canvas_)) - rendered_height) / 2.0);
  return {(x - offset_x) / zoom_, (y - offset_y) / zoom_};
}

Point Window::page_to_canvas(std::size_t page, Point local) const {
  if (!document_ || page >= document_->page_count())
    return {local.x * zoom_, local.y * zoom_};
  const auto& ref = document_->page(page);
  const double offset_x = std::max(0.0,
      (gtk_widget_get_width(GTK_WIDGET(canvas_)) - ref.width_points * zoom_) / 2.0);
  const double offset_y = continuous_scroll_ ? page_top(page) : std::max(0.0,
      (gtk_widget_get_height(GTK_WIDGET(canvas_)) - ref.height_points * zoom_) / 2.0);
  return {offset_x + local.x * zoom_, offset_y + local.y * zoom_};
}

Annotation* Window::annotation_at(std::size_t page, std::uint64_t id) {
  if (!document_ || !id || page >= document_->page_count()) return nullptr;
  auto& annotations = document_->page(page).annotations;
  const auto found = std::find_if(annotations.begin(), annotations.end(),
                                  [id](const Annotation& a) { return a.id == id; });
  return found == annotations.end() ? nullptr : &*found;
}

const Annotation* Window::annotation_at(std::size_t page, std::uint64_t id) const {
  if (!document_ || !id || page >= document_->page_count()) return nullptr;
  const auto& annotations = document_->page(page).annotations;
  const auto found = std::find_if(annotations.begin(), annotations.end(),
                                  [id](const Annotation& a) { return a.id == id; });
  return found == annotations.end() ? nullptr : &*found;
}

Annotation* Window::selected_annotation() {
  return annotation_at(current_page_, selected_annotation_id_);
}

const Annotation* Window::selected_annotation() const {
  return annotation_at(current_page_, selected_annotation_id_);
}

void Window::select_annotation_at(Point point) {
  selected_annotation_id_ = 0;
  if (!document_ || current_page_ >= document_->page_count()) return;
  const double tolerance = 5.0 / zoom_;
  const auto& annotations = document_->page(current_page_).annotations;
  for (auto it = annotations.rbegin(); it != annotations.rend(); ++it) {
    const auto& b = it->bounds;
    if (point.x >= b.x - tolerance && point.x <= b.x + b.width + tolerance &&
        point.y >= b.y - tolerance && point.y <= b.y + b.height + tolerance) {
      selected_annotation_id_ = it->id;
      break;
    }
  }
  sync_annotation_controls();
  gtk_widget_queue_draw(GTK_WIDGET(canvas_));
}

void Window::sync_annotation_controls() {
  const auto* annotation = selected_annotation();
  if (!annotation) {
    update_text_format_visibility();
    return;
  }
  auto& tool_colors = tool_colors_[tool_color_index(annotation->type)];
  tool_colors.stroke = annotation->stroke;
  tool_colors.fill = annotation->fill;
  annotation_stroke_ = annotation->stroke;
  annotation_fill_ = annotation->fill;
  updating_annotation_controls_ = true;
  GdkRGBA stroke{annotation->stroke.red, annotation->stroke.green,
                 annotation->stroke.blue, annotation->stroke.alpha};
  GdkRGBA fill{annotation->fill.red, annotation->fill.green,
               annotation->fill.blue, annotation->fill.alpha};
  gtk_color_dialog_button_set_rgba(stroke_color_, &stroke);
  gtk_color_dialog_button_set_rgba(fill_color_, &fill);
  updating_annotation_controls_ = false;
  update_text_format_visibility();
}

void Window::apply_annotation_controls(bool text_and_font) {
  const GdkRGBA* stroke = gtk_color_dialog_button_get_rgba(stroke_color_);
  const GdkRGBA* fill = gtk_color_dialog_button_get_rgba(fill_color_);
  if (stroke) annotation_stroke_ = {stroke->red, stroke->green, stroke->blue, stroke->alpha};
  if (fill) annotation_fill_ = {fill->red, fill->green, fill->blue, fill->alpha};
  if (bold_button_) annotation_bold_ = gtk_toggle_button_get_active(bold_button_);
  if (italic_button_) annotation_italic_ = gtk_toggle_button_get_active(italic_button_);
  if (underline_button_) annotation_underline_ = gtk_toggle_button_get_active(underline_button_);
  if (align_center_button_ && gtk_toggle_button_get_active(align_center_button_))
    annotation_align_ = TextAlign::Center;
  else if (align_right_button_ && gtk_toggle_button_get_active(align_right_button_))
    annotation_align_ = TextAlign::Right;
  else
    annotation_align_ = TextAlign::Left;
  AnnotationType color_type = active_tool_;
  if (inline_edit_active_) color_type = AnnotationType::TextBox;
  else if (const auto* selected = selected_annotation()) color_type = selected->type;
  tool_colors_[tool_color_index(color_type)].stroke = annotation_stroke_;
  tool_colors_[tool_color_index(color_type)].fill = annotation_fill_;
  save_preferences();
  if (inline_edit_active_) {
    apply_inline_annotation_controls(text_and_font);
    return;
  }
  auto* selected = selected_annotation();
  if (!selected || !document_) return;
  Annotation updated = *selected;
  updated.stroke = annotation_stroke_;
  updated.fill = annotation_fill_;
  if (text_and_font && updated.type == AnnotationType::TextBox) {
    updated.font_family = annotation_font_;
    updated.font_size = annotation_font_size_;
  }
  if (updated.type == AnnotationType::TextBox) {
    updated.bold = annotation_bold_;
    updated.italic = annotation_italic_;
    updated.underline = annotation_underline_;
    updated.align = annotation_align_;
  }
  if (document_->update_annotation(current_page_, updated.id, updated)) {
    render_current();
    update_title();
  }
}

void Window::edit_text_annotation(std::optional<Point> click) {
  auto* annotation = selected_annotation();
  if (!document_ || !annotation || annotation->type != AnnotationType::TextBox) {
    toast("Select a text annotation first");
    return;
  }
  start_inline_text_edit(current_page_, annotation->id, false, click);
}

void Window::start_inline_text_edit(std::size_t page, std::uint64_t id, bool is_new,
                                    std::optional<Point> click) {
  if (!document_ || page >= document_->page_count()) return;
  if (inline_edit_active_) finish_inline_text_edit(true);

  auto& annotations = document_->page(page).annotations;
  const auto found = std::find_if(annotations.begin(), annotations.end(),
      [id](const Annotation& a) { return a.id == id; });
  if (found == annotations.end() || found->type != AnnotationType::TextBox) return;

  if (select_button_) gtk_toggle_button_set_active(select_button_, TRUE);
  if (page != current_page_) set_current_page(page);
  selected_annotation_id_ = id;
  inline_edit_annotation_ = *found;
  inline_edit_page_ = page;
  inline_edit_id_ = id;
  inline_edit_is_new_ = is_new;
  inline_edit_active_ = true;
  inline_edit_started_at_ = g_get_monotonic_time();
  inline_typing_style_ = base_text_style(*found);
  inline_typing_tag_ = nullptr;
  update_text_format_visibility();

  GtkTextBuffer* buffer = gtk_text_view_get_buffer(inline_text_view_);
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gtk_text_buffer_remove_all_tags(buffer, &start, &end);
  inline_format_tags_.clear();
  loading_inline_text_ = true;
  gtk_text_buffer_set_text(buffer, found->text.c_str(), -1);
  loading_inline_text_ = false;
  gtk_text_buffer_get_end_iter(buffer, &end);
  gtk_text_buffer_place_cursor(buffer, &end);
  position_inline_editor();
  apply_inline_text_style();
  apply_inline_text_spans();
  apply_inline_text_style();
  gtk_widget_set_visible(inline_editor_, TRUE);
  struct FocusRequest {
    Window* window;
    int attempts{};
  };
  auto* focus_request = new FocusRequest{this};
  gtk_widget_add_tick_callback(GTK_WIDGET(inline_text_view_), +[](
      GtkWidget* widget, GdkFrameClock*, gpointer data) -> gboolean {
    auto* request = static_cast<FocusRequest*>(data);
    graphene_rect_t bounds;
    if (gtk_widget_compute_bounds(widget,
                                  GTK_WIDGET(request->window->canvas_scroll_),
                                  &bounds) &&
        gtk_widget_get_mapped(widget) && bounds.origin.y > 1.0) {
      if (request->window->inline_edit_active_) gtk_widget_grab_focus(widget);
      delete request;
      return G_SOURCE_REMOVE;
    }
    if (++request->attempts > 60) {
      delete request;
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }, focus_request, nullptr);

  if (click) {
    struct WordRequest {
      Window* window;
      Point click;
    };
    auto* request = new WordRequest{this, *click};
    gtk_widget_add_tick_callback(GTK_WIDGET(inline_text_view_), +[](
        GtkWidget* widget, GdkFrameClock*, gpointer data) -> gboolean {
      auto* request = static_cast<WordRequest*>(data);
      auto* w = request->window;
      if (w->inline_edit_active_) {
        const auto& annotation = w->inline_edit_annotation_;
        const int x = static_cast<int>(std::round(
            (request->click.x - annotation.bounds.x) * w->zoom_));
        const int y = static_cast<int>(std::round(
            (request->click.y - annotation.bounds.y) * w->zoom_));
        GtkTextIter iter;
        if (gtk_text_view_get_iter_at_location(
                GTK_TEXT_VIEW(widget), &iter, x, y)) {
          GtkTextIter word_start = iter;
          GtkTextIter word_end = iter;
          if (gtk_text_iter_inside_word(&word_start)) {
            gtk_text_iter_backward_word_start(&word_start);
            gtk_text_iter_forward_word_end(&word_end);
          } else {
            gtk_text_iter_forward_word_end(&word_end);
          }
          if (!gtk_text_iter_equal(&word_start, &word_end)) {
            GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
            gtk_text_buffer_select_range(buffer, &word_start, &word_end);
          }
        }
        w->sync_inline_format_controls();
      }
      delete request;
      return G_SOURCE_REMOVE;
    }, request, nullptr);
  } else {
    sync_inline_format_controls();
  }
}

void Window::position_inline_editor() {
  if (!inline_edit_active_ || !document_ ||
      inline_edit_page_ >= document_->page_count()) return;
  const auto& page = document_->page(inline_edit_page_);
  const auto& annotation = inline_edit_annotation_;
  const double page_width = page.width_points * zoom_;
  const double page_height = page.height_points * zoom_;
  const double offset_x = std::max(
      0.0, (gtk_widget_get_width(GTK_WIDGET(canvas_)) - page_width) / 2.0);
  const double offset_y = continuous_scroll_ ? page_top(inline_edit_page_) :
      std::max(0.0, (gtk_widget_get_height(GTK_WIDGET(canvas_)) - page_height) / 2.0);
  const int x = std::max(0, static_cast<int>(std::floor(
      offset_x + annotation.bounds.x * zoom_)));
  const int y = std::max(0, static_cast<int>(std::floor(
      offset_y + annotation.bounds.y * zoom_)));
  const int width = std::max(24, static_cast<int>(std::ceil(
      annotation.bounds.width * zoom_)));
  gtk_widget_set_margin_start(inline_editor_, x);
  gtk_widget_set_margin_top(inline_editor_, y);
  gtk_widget_set_size_request(inline_editor_, width, -1);
  const int padding = std::max(0, static_cast<int>(std::round(4.0 * zoom_)));
  gtk_text_view_set_left_margin(inline_text_view_, padding);
  gtk_text_view_set_right_margin(inline_text_view_, padding);
  gtk_text_view_set_top_margin(inline_text_view_, padding);
  gtk_text_view_set_bottom_margin(inline_text_view_, padding);
}

void Window::apply_inline_text_style() {
  if (!inline_edit_active_ || !inline_text_view_) return;
  GtkTextBuffer* buffer = gtk_text_view_get_buffer(inline_text_view_);
  if (!inline_text_tag_) {
    inline_text_tag_ = gtk_text_buffer_create_tag(buffer, "review-inline-text", nullptr);
    gtk_text_tag_set_priority(inline_text_tag_, 0);
  }
  const auto& annotation = inline_edit_annotation_;
  char color[8];
  g_snprintf(color, sizeof(color), "#%02x%02x%02x",
             static_cast<int>(std::clamp(annotation.stroke.red, 0.0, 1.0) * 255),
             static_cast<int>(std::clamp(annotation.stroke.green, 0.0, 1.0) * 255),
             static_cast<int>(std::clamp(annotation.stroke.blue, 0.0, 1.0) * 255));
  g_object_set(inline_text_tag_,
               "family", annotation.font_family.empty() ? "Sans" : annotation.font_family.c_str(),
               "size-points", annotation.font_size * zoom_,
               "weight", annotation.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
               "style", annotation.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL,
               "underline", annotation.underline ? PANGO_UNDERLINE_SINGLE : PANGO_UNDERLINE_NONE,
               "foreground", color,
               nullptr);
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gtk_text_buffer_apply_tag(buffer, inline_text_tag_, &start, &end);
  gtk_text_view_set_justification(
      inline_text_view_,
      annotation.align == TextAlign::Center ? GTK_JUSTIFY_CENTER :
      annotation.align == TextAlign::Right ? GTK_JUSTIFY_RIGHT : GTK_JUSTIFY_LEFT);

  const int width = std::max(24, static_cast<int>(std::ceil(
      annotation.bounds.width * zoom_)));
  const int padding = std::max(0, static_cast<int>(std::round(4.0 * zoom_)));
  char* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  Annotation measured = annotation;
  measured.text = text ? text : "";
  measured.text_spans = inline_text_spans();
  g_free(text);
  const double text_height =
      measure_text_height(measured, std::max(24.0, annotation.bounds.width));
  const int minimum_height = std::max(24, static_cast<int>(std::ceil(
      annotation.bounds.height * zoom_)));
  const int height = std::max(
      minimum_height,
      static_cast<int>(std::ceil(text_height * zoom_)) + padding * 2);
  gtk_widget_set_size_request(inline_editor_, width, height);
}

GtkTextTag* Window::create_inline_format_tag(const TextStyle& style) {
  GtkTextBuffer* buffer = gtk_text_view_get_buffer(inline_text_view_);
  char color[8];
  g_snprintf(color, sizeof(color), "#%02x%02x%02x",
             static_cast<int>(std::clamp(style.color.red, 0.0, 1.0) * 255),
             static_cast<int>(std::clamp(style.color.green, 0.0, 1.0) * 255),
             static_cast<int>(std::clamp(style.color.blue, 0.0, 1.0) * 255));
  GtkTextTag* tag = gtk_text_buffer_create_tag(
      buffer, nullptr,
      "family", style.font_family.empty() ? "Sans" : style.font_family.c_str(),
      "size-points", style.font_size * zoom_,
      "weight", style.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
      "style", style.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL,
      "underline", style.underline ? PANGO_UNDERLINE_SINGLE : PANGO_UNDERLINE_NONE,
      "foreground", color, nullptr);
  gtk_text_tag_set_priority(tag, 1 + static_cast<int>(inline_format_tags_.size()));
  inline_format_tags_.push_back({tag, style});
  return tag;
}

void Window::apply_inline_text_spans() {
  if (!inline_text_view_) return;
  GtkTextBuffer* buffer = gtk_text_view_get_buffer(inline_text_view_);
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  char* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  const glong length = g_utf8_strlen(text ? text : "", -1);
  g_free(text);
  if (length <= 0) return;
  for (const auto& span : inline_edit_annotation_.text_spans) {
    const auto span_start = std::min(span.start, static_cast<std::size_t>(length));
    const auto span_end = std::min(span.end, static_cast<std::size_t>(length));
    if (span_start >= span_end) continue;
    GtkTextIter span_begin;
    GtkTextIter span_finish;
    gtk_text_buffer_get_iter_at_offset(buffer, &span_begin, static_cast<int>(span_start));
    gtk_text_buffer_get_iter_at_offset(buffer, &span_finish, static_cast<int>(span_end));
    GtkTextTag* tag = create_inline_format_tag(span.style);
    gtk_text_buffer_apply_tag(buffer, tag, &span_begin, &span_finish);
  }
}

TextStyle Window::inline_style_at_offset(int offset) const {
  TextStyle style = base_text_style(inline_edit_annotation_);
  if (!inline_text_view_) return style;
  GtkTextBuffer* buffer = gtk_text_view_get_buffer(inline_text_view_);
  const int count = gtk_text_buffer_get_char_count(buffer);
  if (count > 0) offset = std::clamp(offset, 0, count - 1);
  else offset = 0;
  GtkTextIter iter;
  gtk_text_buffer_get_iter_at_offset(buffer, &iter, offset);
  for (const auto& [tag, tag_style] : inline_format_tags_)
    if (gtk_text_iter_has_tag(&iter, tag)) style = tag_style;
  return style;
}

std::vector<TextSpan> Window::inline_text_spans() const {
  std::vector<TextSpan> spans;
  if (!inline_text_view_) return spans;
  GtkTextBuffer* buffer = gtk_text_view_get_buffer(inline_text_view_);
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  char* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  const glong length = g_utf8_strlen(text ? text : "", -1);
  g_free(text);
  if (length <= 0) return spans;

  const TextStyle base = base_text_style(inline_edit_annotation_);
  TextStyle previous;
  std::size_t run_start = 0;
  bool have_previous = false;
  for (glong i = 0; i < length; ++i) {
    const TextStyle style = inline_style_at_offset(static_cast<int>(i));
    if (!have_previous) {
      previous = style;
      run_start = static_cast<std::size_t>(i);
      have_previous = true;
    } else if (!text_style_equal(style, previous)) {
      if (!text_style_equal(previous, base))
        spans.push_back({run_start, static_cast<std::size_t>(i), previous});
      previous = style;
      run_start = static_cast<std::size_t>(i);
    }
  }
  if (have_previous && !text_style_equal(previous, base))
    spans.push_back({run_start, static_cast<std::size_t>(length), previous});
  return spans;
}

void Window::sync_inline_format_controls() {
  if (!inline_edit_active_ || !inline_text_view_) return;
  GtkTextBuffer* buffer = gtk_text_view_get_buffer(inline_text_view_);
  GtkTextIter start;
  GtkTextIter end;
  int offset = 0;
  const int count = gtk_text_buffer_get_char_count(buffer);
  if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
    offset = gtk_text_iter_get_offset(&start);
  } else {
    GtkTextMark* insert = gtk_text_buffer_get_insert(buffer);
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(buffer, &cursor, insert);
    offset = gtk_text_iter_get_offset(&cursor);
    if (count > 0 && !gtk_text_iter_inside_word(&cursor) &&
        !gtk_text_iter_starts_word(&cursor))
      return;
  }
  const TextStyle style = count > 0 ? inline_style_at_offset(offset) :
                                      base_text_style(inline_edit_annotation_);
  if (!text_style_equal(style, inline_typing_style_)) {
    inline_typing_style_ = style;
    inline_typing_tag_ = nullptr;
  }
  updating_annotation_controls_ = true;
  annotation_align_ = inline_edit_annotation_.align;
  annotation_bold_ = style.bold;
  annotation_italic_ = style.italic;
  annotation_underline_ = style.underline;
  annotation_stroke_ = style.color;
  annotation_font_ = style.font_family;
  annotation_font_size_ = style.font_size;
  GdkRGBA color{style.color.red, style.color.green, style.color.blue, style.color.alpha};
  gtk_color_dialog_button_set_rgba(stroke_color_, &color);
  gtk_toggle_button_set_active(bold_button_, style.bold);
  gtk_toggle_button_set_active(italic_button_, style.italic);
  gtk_toggle_button_set_active(underline_button_, style.underline);
  gtk_toggle_button_set_active(
      annotation_align_ == TextAlign::Center ? align_center_button_ :
      annotation_align_ == TextAlign::Right ? align_right_button_ : align_left_button_, TRUE);
  update_font_button_label();
  updating_annotation_controls_ = false;
}

void Window::apply_inline_annotation_controls(bool text_and_font) {
  if (!inline_edit_active_ || !inline_text_view_) return;
  GtkTextBuffer* buffer = gtk_text_view_get_buffer(inline_text_view_);
  GtkTextIter start;
  GtkTextIter end;
  bool has_range = gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
  if (!has_range) {
    GtkTextMark* insert = gtk_text_buffer_get_insert(buffer);
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(buffer, &cursor, insert);
    start = cursor;
    end = cursor;
    if (gtk_text_iter_inside_word(&start)) {
      gtk_text_iter_backward_word_start(&start);
      gtk_text_iter_forward_word_end(&end);
      has_range = true;
    } else if (gtk_text_iter_starts_word(&start)) {
      gtk_text_iter_forward_word_end(&end);
      has_range = true;
    }
    if (has_range) gtk_text_buffer_select_range(buffer, &start, &end);
  }
  const int offset = gtk_text_iter_get_offset(&start);
  inline_edit_annotation_.align = annotation_align_;
  TextStyle style = inline_style_at_offset(offset);
  style.bold = bold_button_ && gtk_toggle_button_get_active(bold_button_);
  style.italic = italic_button_ && gtk_toggle_button_get_active(italic_button_);
  style.underline = underline_button_ && gtk_toggle_button_get_active(underline_button_);
  style.color = annotation_stroke_;
  if (text_and_font) {
    style.font_family = annotation_font_;
    style.font_size = annotation_font_size_;
  }
  inline_typing_style_ = style;
  inline_typing_tag_ = nullptr;
  if (!has_range) {
    inline_typing_tag_ = create_inline_format_tag(style);
    return;
  }
  for (const auto& [tag, _style] : inline_format_tags_)
    gtk_text_buffer_remove_tag(buffer, tag, &start, &end);
  GtkTextTag* tag = create_inline_format_tag(style);
  gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
  inline_typing_tag_ = tag;
  apply_inline_text_style();
  struct FormatFocusRequest {
    Window* window;
  };
  auto* focus_request = new FormatFocusRequest{this};
  gtk_widget_add_tick_callback(GTK_WIDGET(inline_text_view_), +[](
      GtkWidget* widget, GdkFrameClock*, gpointer data) -> gboolean {
    auto* request = static_cast<FormatFocusRequest*>(data);
    if (request->window->inline_edit_active_) gtk_widget_grab_focus(widget);
    delete request;
    return G_SOURCE_REMOVE;
  }, focus_request, nullptr);
}

void Window::finish_inline_text_edit(bool commit) {
  if (!inline_edit_active_ || finishing_inline_edit_) return;
  finishing_inline_edit_ = true;
  inline_edit_active_ = false;
  update_text_format_visibility();
  const auto page = inline_edit_page_;
  const auto id = inline_edit_id_;
  const bool is_new = inline_edit_is_new_;
  GtkTextBuffer* buffer = gtk_text_view_get_buffer(inline_text_view_);
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  char* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  const auto spans = inline_text_spans();
  gtk_widget_set_visible(inline_editor_, FALSE);

  bool changed = false;
  if (document_ && page < document_->page_count()) {
    auto& annotations = document_->page(page).annotations;
    const auto found = std::find_if(annotations.begin(), annotations.end(),
        [id](const Annotation& a) { return a.id == id; });
    if (found != annotations.end()) {
      const std::string value = text ? text : "";
      const bool empty = value.find_first_not_of(" \t\r\n") == std::string::npos;
      if (!commit) {
        if (is_new) {
          document_->remove_annotation(page, id);
          selected_annotation_id_ = 0;
          changed = true;
        }
      } else if (empty) {
        document_->remove_annotation(page, id);
        selected_annotation_id_ = 0;
        changed = true;
      } else if (value != found->text ||
                 !text_spans_equal(found->text_spans, spans)) {
        Annotation updated = *found;
        updated.text = value;
        updated.text_spans = spans;
        updated.align = inline_edit_annotation_.align;
        document_->update_annotation(page, id, updated);
        changed = true;
      }
    }
  }
  g_free(text);
  inline_format_tags_.clear();
  inline_typing_tag_ = nullptr;
  inline_typing_style_ = {};
  inline_edit_page_ = 0;
  inline_edit_id_ = 0;
  inline_edit_is_new_ = false;
  inline_edit_annotation_ = {};
  if (changed) {
    render_current();
    update_title();
    if (selected_annotation_id_) sync_annotation_controls();
  }
  finishing_inline_edit_ = false;
}

std::vector<std::size_t> Window::selected_pages() const {
  std::vector<std::size_t> pages;
  for (GList* rows = gtk_list_box_get_selected_rows(sidebar_), *item = rows; item; item = item->next)
    pages.push_back(row_index(GTK_LIST_BOX_ROW(item->data)));
  if (pages.empty() && document_) pages.push_back(current_page_);
  std::sort(pages.begin(), pages.end());
  return pages;
}

void Window::update_title() {
  std::string title = document_ ? fs::path(document_->path()).filename().string() : "Review";
  if (document_ && document_->dirty()) title = "• " + title;
  gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
}

void Window::toast(const std::string& message) {
  adw_toast_overlay_add_toast(toast_overlay_, adw_toast_new(message.c_str()));
}

void Window::show_error(const std::string& heading, const std::string& detail) {
  auto* dialog = adw_message_dialog_new(GTK_WINDOW(window_), heading.c_str(), detail.c_str());
  adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), "close", "Close");
  adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), "close");
  adw_message_dialog_set_close_response(ADW_MESSAGE_DIALOG(dialog), "close");
  gtk_window_present(GTK_WINDOW(dialog));
}

void Window::choose_open() {
  auto* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Open PDF or image");
  gtk_file_dialog_open(dialog, GTK_WINDOW(window_), nullptr,
    +[](GObject* source, GAsyncResult* result, gpointer data) {
      GError* error = nullptr;
      GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
      if (!file) { if (error) g_error_free(error); return; }
      char* path = g_file_get_path(file);
      self(data)->open(path);
      g_free(path); g_object_unref(file);
    }, this);
  g_object_unref(dialog);
}

void Window::save(bool save_as) {
  if (!document_) return;
  if (inline_edit_active_) finish_inline_text_edit(true);
  if (!save_as && document_->destructive_changes() && !warned_destructive_) {
    auto* dialog = adw_message_dialog_new(GTK_WINDOW(window_), "Overwrite the original document?",
      "Secure crop or scanned-page conversion permanently removes PDF content. Save As keeps the original unchanged.");
    adw_message_dialog_add_responses(ADW_MESSAGE_DIALOG(dialog), "cancel", "Cancel",
                                     "save-as", "Save As…", "overwrite", "Overwrite", nullptr);
    adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(dialog), "overwrite",
                                               ADW_RESPONSE_DESTRUCTIVE);
    g_signal_connect(dialog, "response", G_CALLBACK(+[](
        AdwMessageDialog* dialog, const char* response, gpointer data) {
      auto* w = self(data);
      if (g_strcmp0(response, "overwrite") == 0) {
        w->warned_destructive_ = true;
        w->save(false);
      } else if (g_strcmp0(response, "save-as") == 0) {
        w->save(true);
      }
      gtk_window_destroy(GTK_WINDOW(dialog));
    }), this);
    gtk_window_present(GTK_WINDOW(dialog));
    return;
  }
  if (!save_as && !document_->path().empty()) {
    std::string error;
    if (!document_->save(document_->path(), error)) show_error("Save failed", error);
    else { toast("Saved"); update_title(); }
    return;
  }
  auto* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_initial_name(dialog, document_->kind() == Document::Kind::Pdf ?
                                   "document.pdf" : "image.png");
  gtk_file_dialog_save(dialog, GTK_WINDOW(window_), nullptr,
    +[](GObject* source, GAsyncResult* result, gpointer data) {
      auto* w = self(data); GError* e = nullptr;
      GFile* f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &e);
      if (!f) { if (e) g_error_free(e); return; }
      char* path = g_file_get_path(f); std::string error;
      if (!w->document_->save(path, error)) w->show_error("Save failed", error);
      else { w->toast("Saved"); w->update_title(); }
      g_free(path); g_object_unref(f);
    }, this);
  g_object_unref(dialog);
}

void Window::choose_import_pdf() {
  if (!document_) return;
  auto* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Import PDF pages");
  gtk_file_dialog_open(dialog, GTK_WINDOW(window_), nullptr,
    +[](GObject* source, GAsyncResult* result, gpointer data) {
      auto* w = self(data); GError* e = nullptr;
      GFile* f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &e);
      if (!f) { if(e)g_error_free(e); return; }
      char* path = g_file_get_path(f); Document imported; std::string error;
      if (!imported.open(path, error) || imported.kind() != Document::Kind::Pdf)
        w->show_error("Import failed", error.empty()?"Choose a PDF file":error);
      else {
        std::vector<std::size_t> all(imported.page_count());
        for(std::size_t i=0;i<all.size();++i)all[i]=i;
        w->document_->insert_pages(imported.copy_pages(all),w->current_page_+1);w->refresh();
      }
      g_free(path);g_object_unref(f);
    }, this);
  g_object_unref(dialog);
}

void Window::rebuild_signature_menu() {
  if (!signature_box_) return;
  GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(signature_box_));
  while (child) {
    GtkWidget* next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(signature_box_, child);
    child = next;
  }

  const auto& signatures = signatures_.signatures();
  if (signatures.empty()) {
    auto* empty = gtk_label_new("No saved signatures");
    gtk_widget_add_css_class(empty, "dim-label");
    gtk_widget_set_margin_top(empty, 6);
    gtk_widget_set_margin_bottom(empty, 6);
    gtk_box_append(signature_box_, empty);
  }
  for (const auto& info : signatures) {
    auto* button = gtk_button_new();
    gtk_widget_add_css_class(button, "flat");
    auto* content = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 2));
    auto* picture = gtk_picture_new_for_filename(info.path.c_str());
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_size_request(picture, 160, 48);
    gtk_widget_add_css_class(picture, "signature-preview");
    gtk_box_append(content, picture);
    auto* label = gtk_label_new(info.name.c_str());
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(label, 160, -1);
    gtk_box_append(content, label);
    gtk_button_set_child(GTK_BUTTON(button), GTK_WIDGET(content));
    g_object_set_data_full(G_OBJECT(button), "signature-path",
                           g_strdup(info.path.c_str()), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
      auto* w = self(data);
      const char* path = static_cast<const char*>(
          g_object_get_data(G_OBJECT(button), "signature-path"));
      if (path) w->insert_signature(path);
      if (w->signature_button_) gtk_menu_button_popdown(w->signature_button_);
    }), this);
    gtk_box_append(signature_box_, button);
  }

  if (!signatures.empty())
    gtk_box_append(signature_box_, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  auto* add = gtk_button_new_with_label("New signature");
  gtk_button_set_icon_name(GTK_BUTTON(add), "list-add-symbolic");
  gtk_widget_add_css_class(add, "flat");
  g_signal_connect_swapped(add, "clicked", G_CALLBACK(+[](Window* w) {
    if (w->signature_button_) gtk_menu_button_popdown(w->signature_button_);
    w->draw_signature_dialog();
  }), this);
  gtk_box_append(signature_box_, add);
}

void Window::insert_signature(const std::string& path) {
  if (!document_ || current_page_ >= document_->page_count()) return;
  if (document_->read_only()) {
    toast("This document is view-only");
    return;
  }
  if (inline_edit_active_) finish_inline_text_edit(true);
  auto* hadjustment = gtk_scrolled_window_get_hadjustment(canvas_scroll_);
  auto* vadjustment = gtk_scrolled_window_get_vadjustment(canvas_scroll_);
  double view_x = gtk_adjustment_get_value(hadjustment) +
                  gtk_adjustment_get_page_size(hadjustment) / 2.0;
  double view_y = gtk_adjustment_get_value(vadjustment) +
                  gtk_adjustment_get_page_size(vadjustment) / 2.0;
  graphene_rect_t canvas_bounds;
  if (gtk_widget_compute_bounds(GTK_WIDGET(canvas_), GTK_WIDGET(canvas_scroll_),
                                &canvas_bounds)) {
    view_x = gtk_adjustment_get_page_size(hadjustment) / 2.0 -
             canvas_bounds.origin.x;
    view_y = gtk_adjustment_get_page_size(vadjustment) / 2.0 -
             canvas_bounds.origin.y;
  }
  std::size_t page_index = continuous_scroll_ ? page_at_canvas_y(view_y) :
                                                current_page_;
  if (page_index >= document_->page_count()) page_index = current_page_;
  const auto& page = document_->page(page_index);
  const Point page_origin = page_to_canvas(page_index, {0, 0});
  const double center_x = std::clamp((view_x - page_origin.x) / zoom_,
                                     0.0, page.width_points);
  const double center_y = std::clamp((view_y - page_origin.y) / zoom_,
                                     0.0, page.height_points);
  const double width = page.width_points * .5;
  const double height = page.height_points * .12;
  Annotation annotation;
  annotation.type = AnnotationType::Signature;
  annotation.signature_path = path;
  annotation.bounds = {
    std::clamp(center_x - width / 2.0, 0.0, std::max(0.0, page.width_points - width)),
    std::clamp(center_y - height / 2.0, 0.0, std::max(0.0, page.height_points - height)),
    width, height};
  document_->add_annotation(page_index, std::move(annotation));
  if (page_index != current_page_) show_current_page(page_index);
  render_current();
  update_title();
}

void Window::choose_signature_image() {
  auto* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Import signature image");
  gtk_file_dialog_open(dialog, GTK_WINDOW(window_), nullptr,
    +[](GObject* source,GAsyncResult* result,gpointer data){
      auto*w=self(data);GError*e=nullptr;GFile*f=gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source),result,&e);
      if(!f){if(e)g_error_free(e);return;}char*path=g_file_get_path(f);std::string error;
      if(!w->signatures_.import_image(path,"Signature",80,error))w->show_error("Signature import failed",error);
      else { w->rebuild_signature_menu(); w->toast("Signature saved to the signature menu"); }
      g_free(path);g_object_unref(f);
    },this);
  g_object_unref(dialog);
}

void Window::draw_signature_dialog() {
  signature_points_.clear();
  auto* dialog = adw_message_dialog_new(GTK_WINDOW(window_), "Draw a visual signature",
    "Use a mouse, touch screen, or stylus. This is a reusable visual stamp, not a cryptographic signature.");
  auto* area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  gtk_widget_set_size_request(GTK_WIDGET(area), 600, 200);
  gtk_widget_add_css_class(GTK_WIDGET(area), "signature-pad");
  gtk_drawing_area_set_draw_func(area, draw_signature, this, nullptr);
  auto* gesture = gtk_gesture_drag_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_PRIMARY);
  gtk_gesture_single_set_exclusive(GTK_GESTURE_SINGLE(gesture), TRUE);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture), GTK_PHASE_CAPTURE);
  g_signal_connect(gesture, "drag-begin", G_CALLBACK((+[](
      GtkGestureDrag* gesture, double x, double y, gpointer data) {
    auto* w = self(data);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    if (!w->signature_points_.empty()) w->signature_points_.push_back({-1, -1});
    w->signature_points_.push_back({x, y});
    gtk_widget_queue_draw(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)));
  })), this);
  g_signal_connect(gesture, "drag-update", G_CALLBACK((+[](
      GtkGestureDrag* gesture, double dx, double dy, gpointer data) {
    auto* w = self(data); double x = 0, y = 0;
    gtk_gesture_drag_get_start_point(gesture, &x, &y);
    w->signature_points_.push_back({x + dx, y + dy});
    gtk_widget_queue_draw(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)));
  })), this);
  gtk_widget_add_controller(GTK_WIDGET(area), GTK_EVENT_CONTROLLER(gesture));
  adw_message_dialog_set_extra_child(ADW_MESSAGE_DIALOG(dialog), GTK_WIDGET(area));
  adw_message_dialog_add_responses(ADW_MESSAGE_DIALOG(dialog), "cancel", "Cancel",
                                   "save", "Save Signature", nullptr);
  adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), "save");
  g_signal_connect(dialog, "response", G_CALLBACK(+[](
      AdwMessageDialog* dialog, const char* response, gpointer data) {
    auto* w = self(data);
    if (g_strcmp0(response, "save") == 0 && w->signature_points_.size() > 1) {
      cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 600, 200);
      cairo_t* cr = cairo_create(surface);
      cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR); cairo_paint(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
      cairo_set_source_rgb(cr, .05, .05, .05); cairo_set_line_width(cr, 3.0);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      bool beginning = true;
      for (const auto& p : w->signature_points_) {
        if (p.x < 0) { beginning = true; continue; }
        if (beginning) { cairo_move_to(cr, p.x, p.y); beginning = false; }
        else cairo_line_to(cr, p.x, p.y);
      }
      cairo_stroke(cr); cairo_destroy(cr);
      std::string error;
      if (!w->signatures_.save_surface(surface, "Signature", error))
        w->show_error("Unable to save signature", error);
      else {
        w->rebuild_signature_menu();
        w->toast("Signature saved to the signature menu");
      }
      cairo_surface_destroy(surface);
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
  }), this);
  gtk_window_present(GTK_WINDOW(dialog));
}

void Window::set_tool(AnnotationType type) {
  if (!document_ || document_->read_only()) { toast("This document is view-only"); return; }
  pointer_mode_ = PointerMode::Annotate;
  active_tool_ = type;
  apply_tool_colors(type);
  tool_active_ = true;
  crop_mode_ = false;
  selected_annotation_id_ = 0;
  update_text_format_visibility();
  if (canvas_) gtk_widget_set_cursor_from_name(GTK_WIDGET(canvas_), "crosshair");
  toast(type == AnnotationType::TextBox ? "Click or drag to place the text box" :
        type == AnnotationType::Highlight ? "Drag across text to highlight it" :
        "Drag on the page to place the annotation");
}

void Window::update_text_format_visibility() {
  if (!text_format_box_) return;
  const bool inline_text = inline_edit_active_;
  const bool tool_text = tool_active_ && active_tool_ == AnnotationType::TextBox;
  const bool show_text = inline_text || tool_text;
  bool show_stroke = false;
  bool show_fill = false;
  if (inline_text) {
    show_stroke = true;
  } else if (tool_active_ && !crop_mode_) {
    show_stroke = tool_uses_stroke(active_tool_);
    show_fill = tool_uses_fill(active_tool_);
  } else if (const auto* selected = selected_annotation();
             selected && pointer_mode_ == PointerMode::Select) {
    show_stroke = tool_uses_stroke(selected->type);
    show_fill = tool_uses_fill(selected->type);
  }
  if (stroke_control_) gtk_widget_set_visible(stroke_control_, show_stroke);
  if (fill_control_) gtk_widget_set_visible(fill_control_, show_fill);
  gtk_widget_set_visible(GTK_WIDGET(text_format_box_), show_text);
  if (property_revealer_)
    gtk_revealer_set_reveal_child(property_revealer_,
                                  show_stroke || show_fill || show_text);
}

void Window::update_font_button_label() {
  if (!font_button_) return;
  const std::string family = annotation_font_.empty() ? "Sans" : annotation_font_;
  const int size = std::max(1, static_cast<int>(std::round(annotation_font_size_)));
  gtk_button_set_label(font_button_, (family + " " + std::to_string(size)).c_str());
}

void Window::choose_font() {
  auto* dialog = gtk_font_dialog_new();
  gtk_font_dialog_set_title(dialog, "Choose annotation font");
  const std::string font = (annotation_font_.empty() ? "Sans" : annotation_font_) +
                           " " + std::to_string(annotation_font_size_);
  auto* initial = pango_font_description_from_string(font.c_str());
  g_object_set_data_full(G_OBJECT(dialog), "review-initial-font", initial,
                         reinterpret_cast<GDestroyNotify>(pango_font_description_free));
  gtk_font_dialog_choose_font(dialog, GTK_WINDOW(window_), initial, nullptr,
      +[](GObject* source, GAsyncResult* result, gpointer data) {
        auto* w = self(data);
        GError* error = nullptr;
        PangoFontDescription* description = gtk_font_dialog_choose_font_finish(
            GTK_FONT_DIALOG(source), result, &error);
        if (!description) {
          if (error) g_error_free(error);
          g_object_unref(source);
          return;
        }
        if (const char* family = pango_font_description_get_family(description))
          if (*family) w->annotation_font_ = family;
        const int size = pango_font_description_get_size(description);
        if (size > PANGO_SCALE)
          w->annotation_font_size_ = std::clamp(
              size / static_cast<double>(PANGO_SCALE), 6.0, 144.0);
        pango_font_description_free(description);
        w->update_font_button_label();
        w->apply_annotation_controls(true);
        g_object_unref(source);
      }, this);
}

void Window::set_select_tool() {
  pointer_mode_ = PointerMode::Select;
  tool_active_ = false;
  crop_mode_ = false;
  update_text_format_visibility();
  if (canvas_) gtk_widget_set_cursor_from_name(GTK_WIDGET(canvas_), "default");
  if (canvas_) gtk_widget_queue_draw(GTK_WIDGET(canvas_));
}

void Window::move_dragged_annotation(Annotation& annotation, double dx, double dy,
                                     double x_shift, double y_shift) {
  const double px = dx / zoom_ + x_shift;
  const double py = dy / zoom_ + y_shift;
  if (resizing_annotation_) {
    const Rect old = annotation_edit_origin_.bounds;
    if (annotation.type == AnnotationType::Signature &&
        old.width > 0.0 && old.height > 0.0) {
      const double scale_x = (old.width + px) / old.width;
      const double scale_y = (old.height + py) / old.height;
      double scale = std::abs(scale_x - 1.0) >= std::abs(scale_y - 1.0) ?
                     scale_x : scale_y;
      scale = std::max(0.05, scale);
      annotation.bounds.width = std::max(6.0, old.width * scale);
      annotation.bounds.height = annotation.bounds.width * old.height / old.width;
      if (annotation.bounds.height < 6.0) {
        annotation.bounds.height = 6.0;
        annotation.bounds.width = 6.0 * old.width / old.height;
      }
    } else {
      annotation.bounds.width = std::max(6.0, old.width + px);
      annotation.bounds.height = std::max(6.0, old.height + py);
      const double sx = old.width > 0 ? annotation.bounds.width / old.width : 1.0;
      const double sy = old.height > 0 ? annotation.bounds.height / old.height : 1.0;
      for (auto& point : annotation.points) {
        point.x = old.x + (point.x - old.x) * sx;
        point.y = old.y + (point.y - old.y) * sy;
      }
      for (auto& rect : annotation.text_rects) {
        rect.x = old.x + (rect.x - old.x) * sx;
        rect.y = old.y + (rect.y - old.y) * sy;
        rect.width *= sx;
        rect.height *= sy;
      }
    }
  } else {
    annotation.bounds.x += px;
    annotation.bounds.y += py;
    for (auto& point : annotation.points) {
      point.x += px;
      point.y += py;
    }
    for (auto& rect : annotation.text_rects) {
      rect.x += px;
      rect.y += py;
    }
  }
}

void Window::finish_canvas_drag(double x, double y) {
  if (!document_) return;
  const Point end{x / zoom_, y / zoom_};
  Rect bounds{std::min(drag_start_.x,end.x),std::min(drag_start_.y,end.y),
              std::abs(end.x-drag_start_.x),std::abs(end.y-drag_start_.y)};
  if (crop_mode_) {
    if (bounds.width < 3 || bounds.height < 3) return;
    document_->secure_crop(selected_pages(), bounds);
    crop_mode_ = false;
    if (select_button_) gtk_toggle_button_set_active(select_button_, TRUE);
    refresh();
    toast("Secure crop applied; undo is available until closing");
    return;
  }
  if (!tool_active_) return;
  if (active_tool_ == AnnotationType::Highlight) {
    if (std::hypot(end.x - drag_start_.x, end.y - drag_start_.y) < 0.5) return;
    const auto rects = document_->highlight_rects(current_page_, drag_start_, end);
    if (rects.empty()) {
      toast("No text selected");
      return;
    }
    if (document_->erase_highlights(current_page_, rects)) {
      render_current();
      update_title();
      return;
    }
    Rect text_bounds = rects.front();
    for (const auto& rect : rects) {
      const double right = std::max(text_bounds.x + text_bounds.width,
                                    rect.x + rect.width);
      const double bottom = std::max(text_bounds.y + text_bounds.height,
                                     rect.y + rect.height);
      text_bounds.x = std::min(text_bounds.x, rect.x);
      text_bounds.y = std::min(text_bounds.y, rect.y);
      text_bounds.width = right - text_bounds.x;
      text_bounds.height = bottom - text_bounds.y;
    }
    Annotation highlight;
    highlight.type = AnnotationType::Highlight;
    highlight.bounds = text_bounds;
    highlight.text_rects = rects;
    highlight.stroke = annotation_stroke_;
    highlight.fill = annotation_fill_;
    document_->add_annotation(current_page_, std::move(highlight));
    render_current();
    update_title();
    return;
  }
  const bool is_text = active_tool_ == AnnotationType::TextBox;
  if (bounds.width < 3 || bounds.height < 3) {
    if (!is_text) return;
    select_annotation_at(drag_start_);
    if (auto* existing = selected_annotation();
        existing && existing->type == AnnotationType::TextBox) {
      edit_text_annotation(drag_start_);
      return;
    }
    selected_annotation_id_ = 0;
    bounds = {drag_start_.x, drag_start_.y, 120.0, 20.0};
  }
  Annotation a; a.type=active_tool_;a.bounds=bounds;
  a.stroke=annotation_stroke_;a.fill=annotation_fill_;
  a.font_family=annotation_font_;a.font_size=annotation_font_size_;
  a.bold=annotation_bold_;a.italic=annotation_italic_;a.underline=annotation_underline_;
  a.align=annotation_align_;
  if(active_tool_==AnnotationType::Ink)a.points=ink_points_;
  else if(active_tool_==AnnotationType::Line||active_tool_==AnnotationType::Arrow)
    a.points={drag_start_,end};
  document_->add_annotation(current_page_,std::move(a));
  render_current();update_title();
  if (is_text) {
    const auto& annotations = document_->page(current_page_).annotations;
    if (!annotations.empty())
      start_inline_text_edit(current_page_, annotations.back().id, true);
  }
}

void Window::add_simple_annotation(AnnotationType type) { set_tool(type); }

void Window::start_destructive(bool crop) {
  if(!document_||document_->read_only()){toast("This document is view-only");return;}
  if(crop){
    crop_mode_=true;
    tool_active_=false;
    drag_active_=false;
    selected_annotation_id_=0;
    if(active_tool_button_) gtk_toggle_button_set_active(active_tool_button_,FALSE);
    update_text_format_visibility();
    if(canvas_) gtk_widget_set_cursor_from_name(GTK_WIDGET(canvas_),"crosshair");
    gtk_widget_queue_draw(GTK_WIDGET(canvas_));
    toast("Drag the area to keep; everything outside will be removed");
    return;
  }
  auto* dialog=adw_message_dialog_new(GTK_WINDOW(window_),"Convert selected pages to color images?",
    "This removes searchable text, links, forms, and other interactive content. Undo remains available until the document is closed.");
  adw_message_dialog_add_responses(ADW_MESSAGE_DIALOG(dialog),"cancel","Cancel","convert","Convert",nullptr);
  adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(dialog),"convert",ADW_RESPONSE_DESTRUCTIVE);
  g_signal_connect(dialog,"response",G_CALLBACK(+[](AdwMessageDialog* dialog,const char* response,gpointer data){
    auto*w=self(data);
    if(g_strcmp0(response,"convert")==0){std::string error;w->document_->rasterize_color(w->selected_pages(),error);if(!error.empty())w->show_error("Conversion failed",error);else{w->refresh();w->toast("Pages converted to 300 DPI color images");}}
    gtk_window_destroy(GTK_WINDOW(dialog));
  }),this);
  gtk_window_present(GTK_WINDOW(dialog));
}

void Window::draw_signature(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
  auto* w = self(data);
  cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr);
  cairo_set_source_rgb(cr, .75, .75, .75); cairo_set_line_width(cr, 1);
  cairo_move_to(cr, 16, height * .72); cairo_line_to(cr, width - 16, height * .72); cairo_stroke(cr);
  cairo_set_source_rgb(cr, .05, .05, .05); cairo_set_line_width(cr, 3);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  bool beginning = true;
  for (const auto& p : w->signature_points_) {
    if (p.x < 0) { beginning = true; continue; }
    if (beginning) { cairo_move_to(cr, p.x, p.y); beginning = false; }
    else cairo_line_to(cr, p.x, p.y);
  }
  cairo_stroke(cr);
}

} // namespace review
