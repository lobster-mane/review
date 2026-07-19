#pragma once

#include "review-document.hpp"
#include "signature-store.hpp"

#include <adwaita.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace review {

class Window {
public:
  static Window* create(AdwApplication* application, const std::string& path = {});
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  void present();
  void open(const std::string& path);

private:
  explicit Window(AdwApplication* application);
  void build_ui();
  void load_preferences();
  void save_preferences() const;
  void install_actions();
  void refresh();
  void rebuild_sidebar();
  void render_current();
  void update_title();
  void remember_recent(const std::string& path);
  void rebuild_recent_menu();
  void select_page(std::size_t page, bool scroll_to_bottom = false);
  std::vector<std::size_t> selected_pages() const;
  void toast(const std::string& message);
  void show_error(const std::string& heading, const std::string& detail);
  void save(bool save_as);
  void choose_open();
  void choose_import_pdf();
  void choose_signature_image();
  void draw_signature_dialog();
  void add_simple_annotation(AnnotationType type);
  void start_destructive(bool crop);
  void set_tool(AnnotationType type);
  void set_hand_tool();
  void set_select_tool();
  Annotation* selected_annotation();
  const Annotation* selected_annotation() const;
  void select_annotation_at(Point point);
  void sync_annotation_controls();
  void apply_annotation_controls(bool text_and_font);
  Point canvas_to_page(double x, double y) const;
  void finish_canvas_drag(double x, double y);

  static void draw_page(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                        gpointer data);
  static void draw_signature(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                             gpointer data);

  AdwApplicationWindow* window_{};
  AdwToastOverlay* toast_overlay_{};
  AdwOverlaySplitView* split_{};
  GtkListBox* sidebar_{};
  GtkDrawingArea* canvas_{};
  GtkScrolledWindow* canvas_scroll_{};
  GtkSearchEntry* search_{};
  GtkEntry* annotation_text_{};
  GtkToggleButton* active_tool_button_{};
  GtkColorDialogButton* stroke_color_{};
  GtkColorDialogButton* fill_color_{};
  GtkFontDialogButton* font_button_{};
  GtkLabel* page_label_{};
  GMenu* recent_menu_{};
  std::unique_ptr<Document> document_{};
  SignatureStore signatures_{};
  std::size_t current_page_{};
  double zoom_{1.0};
  bool fit_width_{true};
  bool warned_destructive_{};
  bool tool_active_{};
  bool crop_mode_{};
  enum class PointerMode { Hand, Select, Annotate } pointer_mode_{PointerMode::Hand};
  AnnotationType active_tool_{AnnotationType::Rectangle};
  Color annotation_stroke_{};
  Color annotation_fill_{1.0, 0.92, 0.23, 0.35};
  std::string annotation_font_{"Sans"};
  double annotation_font_size_{12.0};
  std::uint64_t selected_annotation_id_{};
  bool resizing_annotation_{};
  bool annotation_edit_started_{};
  bool updating_annotation_controls_{};
  Annotation annotation_edit_origin_{};
  Point drag_start_{};
  double pan_horizontal_start_{};
  double pan_vertical_start_{};
  gint64 last_edge_page_change_{};
  std::vector<Point> ink_points_{};
  std::vector<Point> signature_points_{};
};

} // namespace review
