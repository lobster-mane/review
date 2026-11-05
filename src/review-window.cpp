#include "review-window.hpp"

#include <cairo.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace review {
namespace {

constexpr double kPageGap = 12.0;
constexpr double kCanvasMargin = 24.0;

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
  load_preferences();
  build_ui();
  install_actions();
  update_title();
  g_signal_connect(window_, "close-request", G_CALLBACK(+[](
      GtkWindow*, gpointer data) -> gboolean {
    auto* w = self(data);
    if (g_object_get_data(G_OBJECT(w->window_), "allow-close")) return FALSE;
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
  if (recent_menu_) g_object_unref(recent_menu_);
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
  annotation_stroke_ = {
    read_component("stroke-red", annotation_stroke_.red),
    read_component("stroke-green", annotation_stroke_.green),
    read_component("stroke-blue", annotation_stroke_.blue),
    read_component("stroke-alpha", annotation_stroke_.alpha),
  };
  annotation_fill_ = {
    read_component("fill-red", annotation_fill_.red),
    read_component("fill-green", annotation_fill_.green),
    read_component("fill-blue", annotation_fill_.blue),
    read_component("fill-alpha", annotation_fill_.alpha),
  };
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
  g_key_file_set_double(key_file, "Annotations", "stroke-red", annotation_stroke_.red);
  g_key_file_set_double(key_file, "Annotations", "stroke-green", annotation_stroke_.green);
  g_key_file_set_double(key_file, "Annotations", "stroke-blue", annotation_stroke_.blue);
  g_key_file_set_double(key_file, "Annotations", "stroke-alpha", annotation_stroke_.alpha);
  g_key_file_set_double(key_file, "Annotations", "fill-red", annotation_fill_.red);
  g_key_file_set_double(key_file, "Annotations", "fill-green", annotation_fill_.green);
  g_key_file_set_double(key_file, "Annotations", "fill-blue", annotation_fill_.blue);
  g_key_file_set_double(key_file, "Annotations", "fill-alpha", annotation_fill_.alpha);
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
    {"review-highlight-symbolic", "Highlight", AnnotationType::Highlight},
    {"review-text-symbolic", "Text box", AnnotationType::TextBox},
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
        if (mode == -2) w->set_hand_tool();
        else if (mode == -1) w->set_select_tool();
        else w->set_tool(static_cast<AnnotationType>(mode));
        if (mode >= 0 && !w->tool_active_) {
          w->active_tool_button_ = nullptr;
          gtk_toggle_button_set_active(button, FALSE);
        }
      } else if (w->active_tool_button_ == button) {
        w->active_tool_button_ = nullptr;
        w->tool_active_ = false;
      }
    }), this);
    append_button(tool_row, GTK_WIDGET(button));
    return button;
  };

  auto* hand_button = add_tool_button("review-hand-symbolic", "Hand tool (pan without annotating)", -2);
  add_tool_button("review-select-symbolic", "Select, move, and resize annotations", -1);
  for (const auto& tool : tools) {
    add_tool_button(tool.icon, tool.tip, static_cast<int>(tool.type));
  }
  annotation_text_ = GTK_ENTRY(gtk_entry_new());
  gtk_editable_set_text(GTK_EDITABLE(annotation_text_), "Text");
  gtk_entry_set_placeholder_text(annotation_text_, "Annotation text");
  gtk_widget_set_size_request(GTK_WIDGET(annotation_text_), 150, -1);
  gtk_widget_set_tooltip_text(GTK_WIDGET(annotation_text_),
                              "Text for new annotation; press Enter to update selected text");
  g_signal_connect_swapped(annotation_text_, "activate", G_CALLBACK(+[](Window* w) {
    w->apply_annotation_controls(true, true);
  }), this);
  g_signal_connect_swapped(annotation_text_, "notify::has-focus", G_CALLBACK(+[](Window* w) {
    if (!w->updating_annotation_controls_ &&
        !gtk_widget_has_focus(GTK_WIDGET(w->annotation_text_)))
      w->apply_annotation_controls(true, true);
  }), this);
  append_button(property_row, GTK_WIDGET(annotation_text_));

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
  auto* stroke_label = gtk_label_new("Outline/Text");
  gtk_widget_add_css_class(stroke_label, "dim-label");
  append_button(property_row, stroke_label);
  append_button(property_row, GTK_WIDGET(stroke_color_));

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
  auto* fill_label = gtk_label_new("Fill/Highlight");
  gtk_widget_add_css_class(fill_label, "dim-label");
  append_button(property_row, fill_label);
  append_button(property_row, GTK_WIDGET(fill_color_));

  auto* font_dialog = gtk_font_dialog_new();
  gtk_font_dialog_set_title(font_dialog, "Choose annotation font");
  font_button_ = GTK_FONT_DIALOG_BUTTON(gtk_font_dialog_button_new(font_dialog));
  auto* initial_font = pango_font_description_from_string("Sans 12");
  gtk_font_dialog_button_set_font_desc(font_button_, initial_font);
  pango_font_description_free(initial_font);
  gtk_widget_set_tooltip_text(GTK_WIDGET(font_button_), "Text annotation font");
  g_signal_connect_swapped(font_button_, "notify::font-desc", G_CALLBACK(+[](Window* w) {
    if (!w->updating_annotation_controls_) w->apply_annotation_controls(true);
  }), this);
  append_button(property_row, GTK_WIDGET(font_button_));
  auto* edit_text = icon_button("document-edit-symbolic", "Edit selected text");
  g_signal_connect_swapped(edit_text, "clicked", G_CALLBACK(+[](Window* w) {
    w->edit_text_annotation();
  }), this);
  append_button(property_row, edit_text);

  auto make_format_toggle = [&](const char* icon, const char* tooltip) {
    auto* button = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
    gtk_button_set_icon_name(GTK_BUTTON(button), icon);
    gtk_widget_set_tooltip_text(GTK_WIDGET(button), tooltip);
    gtk_widget_add_css_class(GTK_WIDGET(button), "flat");
    append_button(property_row, GTK_WIDGET(button));
    g_signal_connect_swapped(button, "toggled", G_CALLBACK(+[](Window* w) {
      if (!w->updating_annotation_controls_) w->apply_annotation_controls(false);
    }), this);
    return button;
  };
  append_button(property_row, gtk_separator_new(GTK_ORIENTATION_VERTICAL));
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

  auto* signature = icon_button("review-signature-symbolic", "Insert saved signature");
  g_signal_connect_swapped(signature, "clicked", G_CALLBACK((+[](Window* w) {
    if (!w->document_ || w->signatures_.signatures().empty()) {
      w->draw_signature_dialog();
      return;
    }
    const auto& page = w->document_->page(w->current_page_);
    Annotation a;
    a.type = AnnotationType::Signature;
    a.signature_path = w->signatures_.signatures().front().path;
    a.bounds = {page.width_points * .25, page.height_points * .4,
                page.width_points * .5, page.height_points * .12};
    w->document_->add_annotation(w->current_page_, std::move(a));
    w->render_current();
  })), this);
  append_button(tool_row, signature);

  auto* crop = icon_button("review-crop-symbolic", "Secure crop");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(crop), "win.secure-crop");
  append_button(tool_row, crop);
  auto* scan = icon_button("review-scan-symbolic", "Convert selected pages to 300 DPI color images");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(scan), "win.scan-pages");
  append_button(tool_row, scan);
  gtk_toggle_button_set_active(hand_button, TRUE);
  gtk_box_append(annotation_bar, GTK_WIDGET(tool_row));
  gtk_box_append(annotation_bar, GTK_WIDGET(property_row));
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
  g_signal_connect_swapped(sidebar_, "row-activated", G_CALLBACK(+[](
      Window* w, GtkListBoxRow* row) {
    if (row) w->select_page(row_index(row));
  }), this);
  g_signal_connect(sidebar_, "row-selected", G_CALLBACK(+[](
      GtkListBox*, GtkListBoxRow* row, gpointer data) {
    auto* w = self(data);
    if (w->syncing_sidebar_ || !row || !w->document_) return;
    GList* rows = gtk_list_box_get_selected_rows(w->sidebar_);
    const bool single = rows && !rows->next;
    const auto page = row_index(row);
    g_list_free(rows);
    if (single) w->select_page(page);
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
  gtk_scrolled_window_set_child(canvas_scroll_, GTK_WIDGET(canvas_));
  g_signal_connect(gtk_scrolled_window_get_vadjustment(canvas_scroll_), "value-changed",
                   G_CALLBACK(+[](GtkAdjustment*, gpointer data) {
                     self(data)->update_current_page_from_scroll();
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
    if (!can_next && !can_previous) {
      w->edge_scroll_accum_ = 0.0;
      w->edge_scroll_events_ = 0;
      w->edge_scroll_direction_ = 0;
      return FALSE;
    }

    const gint64 now = g_get_monotonic_time();
    // Smooth touchpad gestures may keep sending deltas after the page has
    // changed. Limit one edge transition per short gesture burst.
    if (now - w->last_edge_page_change_ < 300000) {
      w->edge_scroll_accum_ = 0.0;
      w->edge_scroll_events_ = 0;
      return FALSE;
    }

    // Only turn the page on a deliberate, fast scroll: accumulate a short
    // burst of deltas instead of switching on every edge event.
    if (direction != w->edge_scroll_direction_ ||
        now - w->edge_scroll_start_ > 350000) {
      w->edge_scroll_direction_ = direction;
      w->edge_scroll_start_ = now;
      w->edge_scroll_accum_ = 0.0;
      w->edge_scroll_events_ = 0;
    }
    w->edge_scroll_accum_ += std::abs(dy);
    ++w->edge_scroll_events_;
    if (w->edge_scroll_accum_ < 3.0 || w->edge_scroll_events_ < 2) return FALSE;

    w->edge_scroll_accum_ = 0.0;
    w->edge_scroll_events_ = 0;
    w->edge_scroll_direction_ = 0;
    w->last_edge_page_change_ = now;
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
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    if (w->continuous_scroll_) w->set_current_page(w->page_at_canvas_y(y));
    w->drag_start_ = w->canvas_to_page(x, y);
    if (w->pointer_mode_ == PointerMode::Hand && !w->crop_mode_) {
      w->pan_horizontal_start_ = gtk_adjustment_get_value(
          gtk_scrolled_window_get_hadjustment(w->canvas_scroll_));
      w->pan_vertical_start_ = gtk_adjustment_get_value(
          gtk_scrolled_window_get_vadjustment(w->canvas_scroll_));
      return;
    }
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
          w->resizing_annotation_ = true;
          w->annotation_edit_started_ = false;
          return;
        }
      }
      w->select_annotation_at(w->drag_start_);
      if (auto* annotation = w->selected_annotation()) {
        w->annotation_edit_origin_ = *annotation;
      }
      w->annotation_edit_started_ = false;
      return;
    }
    if (!w->tool_active_ && !w->crop_mode_) return;
    w->ink_points_.clear();
    w->ink_points_.push_back(w->drag_start_);
  })), this);
  g_signal_connect(drag, "drag-update", G_CALLBACK((+[](
      GtkGestureDrag*, double dx, double dy, gpointer data) {
    auto* w = self(data);
    if (w->pointer_mode_ == PointerMode::Hand && !w->crop_mode_) {
      gtk_adjustment_set_value(gtk_scrolled_window_get_hadjustment(w->canvas_scroll_),
                               w->pan_horizontal_start_ - dx);
      gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(w->canvas_scroll_),
                               w->pan_vertical_start_ - dy);
      return;
    }
    if (w->pointer_mode_ == PointerMode::Select && !w->crop_mode_) {
      auto* annotation = w->selected_annotation();
      if (!annotation || (std::abs(dx) < 2 && std::abs(dy) < 2)) return;
      if (!w->annotation_edit_started_) {
        w->document_->begin_annotation_edit();
        w->annotation_edit_started_ = true;
      }
      *annotation = w->annotation_edit_origin_;
      const double px = dx / w->zoom_;
      const double py = dy / w->zoom_;
      if (w->resizing_annotation_) {
        const Rect old = w->annotation_edit_origin_.bounds;
        annotation->bounds.width = std::max(6.0, old.width + px);
        annotation->bounds.height = std::max(6.0, old.height + py);
        const double sx = old.width > 0 ? annotation->bounds.width / old.width : 1.0;
        const double sy = old.height > 0 ? annotation->bounds.height / old.height : 1.0;
        for (auto& point : annotation->points) {
          point.x = old.x + (point.x - old.x) * sx;
          point.y = old.y + (point.y - old.y) * sy;
        }
      } else {
        annotation->bounds.x += px;
        annotation->bounds.y += py;
        for (auto& point : annotation->points) {
          point.x += px;
          point.y += py;
        }
      }
      gtk_widget_queue_draw(GTK_WIDGET(w->canvas_));
      return;
    }
    if (w->tool_active_ && w->active_tool_ == AnnotationType::Ink)
      w->ink_points_.push_back({w->drag_start_.x + dx / w->zoom_,
                                w->drag_start_.y + dy / w->zoom_});
  })), this);
  g_signal_connect(drag, "drag-end", G_CALLBACK((+[](
      GtkGestureDrag*, double dx, double dy, gpointer data) {
    auto* w = self(data);
    if (w->pointer_mode_ == PointerMode::Hand && !w->crop_mode_) return;
    if (w->pointer_mode_ == PointerMode::Select && !w->crop_mode_) {
      if (w->annotation_edit_started_) {
        w->document_->commit_annotation_edit();
        w->annotation_edit_started_ = false;
        w->update_title();
      }
      w->sync_annotation_controls();
      gtk_widget_queue_draw(GTK_WIDGET(w->canvas_));
      return;
    }
    w->finish_canvas_drag((w->drag_start_.x * w->zoom_ + dx),
                          (w->drag_start_.y * w->zoom_ + dy));
  })), this);
  gtk_widget_add_controller(GTK_WIDGET(canvas_), GTK_EVENT_CONTROLLER(drag));

  auto* double_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(double_click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(double_click),
                                              GTK_PHASE_CAPTURE);
  g_signal_connect(double_click, "pressed", G_CALLBACK(+[](
      GtkGestureClick*, int n_press, double x, double y, gpointer data) {
    auto* w = self(data);
    if (n_press != 2 || !w->document_ || w->pointer_mode_ != PointerMode::Select) return;
    if (w->continuous_scroll_) w->set_current_page(w->page_at_canvas_y(y));
    w->select_annotation_at(w->canvas_to_page(x, y));
    if (auto* annotation = w->selected_annotation();
        annotation && annotation->type == AnnotationType::TextBox)
      w->edit_text_annotation();
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
    {"zoom-in", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d);w->fit_width_=false;w->zoom_=std::min(5.0,w->zoom_*1.2);w->render_current(); }, nullptr, nullptr, nullptr, {}},
    {"zoom-out", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d);w->fit_width_=false;w->zoom_=std::max(.15,w->zoom_/1.2);w->render_current(); }, nullptr, nullptr, nullptr, {}},
    {"fit-width", +[](GSimpleAction*, GVariant*, gpointer d){ auto*w=self(d);w->fit_width_=true;w->render_current(); }, nullptr, nullptr, nullptr, {}},
    {"print", +[](GSimpleAction*, GVariant*, gpointer d){
      auto*w=self(d); if(!w->document_)return;
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
  const std::string status = "Page " + std::to_string(current_page_ + 1) + " of " +
      std::to_string(document_->page_count()) + "  •  " +
      std::to_string(static_cast<int>(zoom_ * 100)) + "%";
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
    zoom_ = std::clamp(available / std::max(1.0, reference_width), .15, 3.0);
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
}

void Window::set_current_page(std::size_t page) {
  if (!document_ || page >= document_->page_count() || page == current_page_) return;
  current_page_ = page;
  selected_annotation_id_ = 0;
  if (sidebar_) {
    syncing_sidebar_ = true;
    gtk_list_box_unselect_all(sidebar_);
    if (auto* row = gtk_list_box_get_row_at_index(sidebar_, static_cast<int>(page)))
      gtk_list_box_select_row(sidebar_, row);
    syncing_sidebar_ = false;
  }
  update_page_status();
  gtk_widget_queue_draw(GTK_WIDGET(canvas_));
}

void Window::update_current_page_from_scroll() {
  if (!continuous_scroll_ || !document_ || !canvas_scroll_) return;
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
  cairo_set_source_rgb(cr, .18, .18, .18);
  cairo_paint(cr);
  if (!w->document_ || w->document_->page_count() == 0) return;

  auto draw_selection = [&](double x, double y) {
    if (w->pointer_mode_ != PointerMode::Select) return;
    const auto* annotation = w->selected_annotation();
    if (!annotation) return;
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

  if (!w->continuous_scroll_) {
    cairo_surface_t* surface = w->document_->render_page(w->current_page_, w->zoom_, true);
    if (!surface) return;
    const int sw = cairo_image_surface_get_width(surface);
    const int sh = cairo_image_surface_get_height(surface);
    const double x = std::max(0.0, (width - sw) / 2.0);
    const double y = std::max(0.0, (height - sh) / 2.0);
    cairo_set_source_surface(cr, surface, x, y);
    cairo_paint(cr);
    cairo_surface_destroy(surface);
    draw_selection(x, y);
    return;
  }

  auto* adjustment = gtk_scrolled_window_get_vadjustment(w->canvas_scroll_);
  const double view_top = std::max(0.0, gtk_adjustment_get_value(adjustment));
  const double view_bottom = view_top + gtk_adjustment_get_page_size(adjustment) + 1.0;
  double top = kCanvasMargin;
  for (std::size_t i = 0; i < w->document_->page_count(); ++i) {
    const auto& page = w->document_->page(i);
    const double page_height = page.height_points * w->zoom_;
    const double bottom = top + page_height;
    if (bottom >= view_top && top <= view_bottom) {
      cairo_surface_t* surface = w->document_->render_page(i, w->zoom_, true);
      if (surface) {
        const int sw = cairo_image_surface_get_width(surface);
        const double x = std::max(0.0, (width - sw) / 2.0);
        cairo_set_source_surface(cr, surface, x, top);
        cairo_paint(cr);
        cairo_surface_destroy(surface);
      }
    }
    top = bottom + kPageGap;
  }

  const auto& current = w->document_->page(w->current_page_);
  const double current_x = std::max(
      0.0, (width - current.width_points * w->zoom_) / 2.0);
  draw_selection(current_x, w->page_top(w->current_page_));
}

void Window::select_page(std::size_t page, bool scroll_to_bottom) {
  if (!document_ || page >= document_->page_count()) return;
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

Annotation* Window::selected_annotation() {
  if (!document_ || !selected_annotation_id_ || current_page_ >= document_->page_count())
    return nullptr;
  auto& annotations = document_->page(current_page_).annotations;
  const auto found = std::find_if(annotations.begin(), annotations.end(), [this](const Annotation& a) {
    return a.id == selected_annotation_id_;
  });
  return found == annotations.end() ? nullptr : &*found;
}

const Annotation* Window::selected_annotation() const {
  if (!document_ || !selected_annotation_id_ || current_page_ >= document_->page_count())
    return nullptr;
  const auto& annotations = document_->page(current_page_).annotations;
  const auto found = std::find_if(annotations.begin(), annotations.end(), [this](const Annotation& a) {
    return a.id == selected_annotation_id_;
  });
  return found == annotations.end() ? nullptr : &*found;
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
  if (!annotation) return;
  updating_annotation_controls_ = true;
  GdkRGBA stroke{annotation->stroke.red, annotation->stroke.green,
                 annotation->stroke.blue, annotation->stroke.alpha};
  GdkRGBA fill{annotation->fill.red, annotation->fill.green,
               annotation->fill.blue, annotation->fill.alpha};
  gtk_color_dialog_button_set_rgba(stroke_color_, &stroke);
  gtk_color_dialog_button_set_rgba(fill_color_, &fill);
  if (annotation->type == AnnotationType::TextBox) {
    gtk_editable_set_text(GTK_EDITABLE(annotation_text_), annotation->text.c_str());
    const std::string font = (annotation->font_family.empty() ? "Sans" : annotation->font_family) +
                             " " + std::to_string(annotation->font_size);
    auto* description = pango_font_description_from_string(font.c_str());
    gtk_font_dialog_button_set_font_desc(font_button_, description);
    pango_font_description_free(description);
    annotation_bold_ = annotation->bold;
    annotation_italic_ = annotation->italic;
    annotation_underline_ = annotation->underline;
    annotation_align_ = annotation->align;
    gtk_toggle_button_set_active(bold_button_, annotation_bold_);
    gtk_toggle_button_set_active(italic_button_, annotation_italic_);
    gtk_toggle_button_set_active(underline_button_, annotation_underline_);
    gtk_toggle_button_set_active(
        annotation_align_ == TextAlign::Center ? align_center_button_ :
        annotation_align_ == TextAlign::Right ? align_right_button_ : align_left_button_, TRUE);
  }
  updating_annotation_controls_ = false;
}

void Window::apply_annotation_controls(bool text_and_font, bool update_text) {
  const GdkRGBA* stroke = gtk_color_dialog_button_get_rgba(stroke_color_);
  const GdkRGBA* fill = gtk_color_dialog_button_get_rgba(fill_color_);
  if (stroke) annotation_stroke_ = {stroke->red, stroke->green, stroke->blue, stroke->alpha};
  if (fill) annotation_fill_ = {fill->red, fill->green, fill->blue, fill->alpha};
  if (text_and_font) {
    const PangoFontDescription* description = gtk_font_dialog_button_get_font_desc(font_button_);
    if (description) {
      if (const char* family = pango_font_description_get_family(description)) {
        annotation_font_ = family;
        const int size = pango_font_description_get_size(description);
        if (size > 0) annotation_font_size_ = size / static_cast<double>(PANGO_SCALE);
      }
    }
  }
  if (bold_button_) annotation_bold_ = gtk_toggle_button_get_active(bold_button_);
  if (italic_button_) annotation_italic_ = gtk_toggle_button_get_active(italic_button_);
  if (underline_button_) annotation_underline_ = gtk_toggle_button_get_active(underline_button_);
  if (align_center_button_ && gtk_toggle_button_get_active(align_center_button_))
    annotation_align_ = TextAlign::Center;
  else if (align_right_button_ && gtk_toggle_button_get_active(align_right_button_))
    annotation_align_ = TextAlign::Right;
  else
    annotation_align_ = TextAlign::Left;
  save_preferences();
  auto* selected = selected_annotation();
  if (!selected || !document_) return;
  Annotation updated = *selected;
  updated.stroke = annotation_stroke_;
  updated.fill = annotation_fill_;
  if (text_and_font && updated.type == AnnotationType::TextBox) {
    if (update_text) updated.text = gtk_editable_get_text(GTK_EDITABLE(annotation_text_));
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

void Window::edit_text_annotation() {
  auto* annotation = selected_annotation();
  if (!document_ || !annotation || annotation->type != AnnotationType::TextBox) {
    toast("Select a text annotation first");
    return;
  }

  struct EditRequest {
    std::size_t page;
    std::uint64_t id;
  };
  auto* request = new EditRequest{current_page_, annotation->id};
  auto* dialog = adw_message_dialog_new(GTK_WINDOW(window_), "Edit text", nullptr);
  auto* scroll = gtk_scrolled_window_new();
  gtk_widget_set_size_request(scroll, 420, 180);
  auto* view = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 8);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 8);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 8);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 8);
  auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  gtk_text_buffer_set_text(buffer, annotation->text.c_str(), -1);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  gtk_text_buffer_place_cursor(buffer, &end);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
  adw_message_dialog_set_extra_child(ADW_MESSAGE_DIALOG(dialog), scroll);
  adw_message_dialog_add_responses(ADW_MESSAGE_DIALOG(dialog), "cancel", "Cancel",
                                   "save", "Save", nullptr);
  adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), "save");
  g_object_set_data_full(G_OBJECT(dialog), "review-text-edit", request,
                         [](gpointer data) { delete static_cast<EditRequest*>(data); });
  g_object_set_data(G_OBJECT(dialog), "review-text-view", view);
  g_signal_connect(dialog, "response", G_CALLBACK(+[](
      AdwMessageDialog* dialog, const char* response, gpointer data) {
    auto* w = self(data);
    if (g_strcmp0(response, "save") == 0) {
      auto* request = static_cast<EditRequest*>(
          g_object_get_data(G_OBJECT(dialog), "review-text-edit"));
      auto* view = GTK_TEXT_VIEW(g_object_get_data(G_OBJECT(dialog), "review-text-view"));
      if (request && view && w->document_ &&
          request->page < w->document_->page_count()) {
        GtkTextBuffer* buffer = gtk_text_view_get_buffer(view);
        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        char* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        auto& annotations = w->document_->page(request->page).annotations;
        const auto found = std::find_if(annotations.begin(), annotations.end(),
            [request](const Annotation& a) { return a.id == request->id; });
        if (found != annotations.end()) {
          Annotation updated = *found;
          updated.text = text ? text : "";
          w->document_->update_annotation(request->page, request->id, updated);
          w->render_current();
          w->update_title();
          w->sync_annotation_controls();
        }
        g_free(text);
      }
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
  }), this);
  gtk_window_present(GTK_WINDOW(dialog));
  gtk_widget_grab_focus(view);
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

void Window::choose_signature_image() {
  auto* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Import signature image");
  gtk_file_dialog_open(dialog, GTK_WINDOW(window_), nullptr,
    +[](GObject* source,GAsyncResult* result,gpointer data){
      auto*w=self(data);GError*e=nullptr;GFile*f=gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source),result,&e);
      if(!f){if(e)g_error_free(e);return;}char*path=g_file_get_path(f);std::string error;
      if(!w->signatures_.import_image(path,"Signature",80,error))w->show_error("Signature import failed",error);
      else w->toast("Signature saved. Select the signature tool again to place it.");
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
      else w->toast("Signature saved. Select the signature tool to place it.");
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
  tool_active_ = true;
  crop_mode_ = false;
  selected_annotation_id_ = 0;
  if (canvas_) gtk_widget_set_cursor_from_name(GTK_WIDGET(canvas_), "crosshair");
  toast("Drag on the page to place the annotation");
}

void Window::set_hand_tool() {
  pointer_mode_ = PointerMode::Hand;
  tool_active_ = false;
  crop_mode_ = false;
  selected_annotation_id_ = 0;
  if (canvas_) {
    gtk_widget_set_cursor_from_name(GTK_WIDGET(canvas_), "grab");
    gtk_widget_queue_draw(GTK_WIDGET(canvas_));
  }
}

void Window::set_select_tool() {
  pointer_mode_ = PointerMode::Select;
  tool_active_ = false;
  crop_mode_ = false;
  if (canvas_) gtk_widget_set_cursor_from_name(GTK_WIDGET(canvas_), "default");
  toast("Click to select; double-click text to edit; drag to move or resize");
  if (canvas_) gtk_widget_queue_draw(GTK_WIDGET(canvas_));
}

void Window::finish_canvas_drag(double x, double y) {
  if (!document_) return;
  const Point end{x / zoom_, y / zoom_};
  Rect bounds{std::min(drag_start_.x,end.x),std::min(drag_start_.y,end.y),
              std::abs(end.x-drag_start_.x),std::abs(end.y-drag_start_.y)};
  if (bounds.width < 3 || bounds.height < 3) return;
  if (crop_mode_) {
    document_->secure_crop(selected_pages(), bounds);
    crop_mode_ = false;
    refresh();
    toast("Secure crop applied; undo is available until closing");
    return;
  }
  if (!tool_active_) return;
  Annotation a; a.type=active_tool_;a.bounds=bounds;
  a.text=gtk_editable_get_text(GTK_EDITABLE(annotation_text_));
  a.stroke=annotation_stroke_;a.fill=annotation_fill_;
  a.font_family=annotation_font_;a.font_size=annotation_font_size_;
  a.bold=annotation_bold_;a.italic=annotation_italic_;a.underline=annotation_underline_;
  a.align=annotation_align_;
  if(active_tool_==AnnotationType::Ink)a.points=ink_points_;
  else if(active_tool_==AnnotationType::Line||active_tool_==AnnotationType::Arrow)
    a.points={drag_start_,end};
  document_->add_annotation(current_page_,std::move(a));
  // Keep the chosen annotation tool active so consecutive drags create
  // consecutive annotations. The toggle button remains visibly pressed.
  render_current();update_title();
}

void Window::add_simple_annotation(AnnotationType type) { set_tool(type); }

void Window::start_destructive(bool crop) {
  if(!document_||document_->read_only()){toast("This document is view-only");return;}
  if(crop){
    crop_mode_=true;
    tool_active_=false;
    if(active_tool_button_) gtk_toggle_button_set_active(active_tool_button_,FALSE);
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
