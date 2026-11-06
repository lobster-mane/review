#pragma once

#include "review-document.hpp"
#include "signature-store.hpp"

#include <adwaita.h>
#include <array>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
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
  cairo_surface_t* cached_page(std::size_t page, std::uint64_t skip,
                               bool render_if_missing = true);
  void schedule_page_render(std::size_t page, std::uint64_t skip);
  static gboolean render_pending_pages(gpointer data);
  void note_scroll_activity();
  static gboolean finish_scroll(gpointer data);
  void clear_page_cache();
  void schedule_preload_next_page();
  void update_title();
  void set_continuous_scroll(bool enabled);
  void update_view_actions();
  void set_current_page(std::size_t page);
  void update_current_page_from_scroll();
  void update_page_status();
  void scroll_to_page(std::size_t page, bool scroll_to_bottom);
  [[nodiscard]] std::size_t page_at_canvas_y(double y) const;
  [[nodiscard]] double page_top(std::size_t page) const;
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
  void rebuild_signature_menu();
  void insert_signature(const std::string& path);
  void add_simple_annotation(AnnotationType type);
  void start_destructive(bool crop);
  void set_tool(AnnotationType type);
  void set_select_tool();
  Annotation* selected_annotation();
  const Annotation* selected_annotation() const;
  Annotation* annotation_at(std::size_t page, std::uint64_t id);
  const Annotation* annotation_at(std::size_t page, std::uint64_t id) const;
  void select_annotation_at(Point point);
  void sync_annotation_controls();
  void apply_annotation_controls(bool text_and_font);
  void edit_text_annotation(std::optional<Point> click = std::nullopt);
  void start_inline_text_edit(std::size_t page, std::uint64_t id, bool is_new = false,
                              std::optional<Point> click = std::nullopt);
  void finish_inline_text_edit(bool commit);
  void position_inline_editor();
  void apply_inline_text_style();
  void apply_inline_text_spans();
  void apply_inline_annotation_controls(bool text_and_font);
  void sync_inline_format_controls();
  void update_text_format_visibility();
  void reset_tool_colors();
  void apply_tool_colors(AnnotationType type);
  void show_current_page(std::size_t page);
  void sync_sidebar_selection(std::size_t page);
  void move_dragged_annotation(Annotation& annotation, double dx, double dy,
                               double x_shift, double y_shift);
  void choose_font();
  void update_font_button_label();
  [[nodiscard]] TextStyle inline_style_at_offset(int offset) const;
  [[nodiscard]] std::vector<TextSpan> inline_text_spans() const;
  GtkTextTag* create_inline_format_tag(const TextStyle& style);
  Point canvas_to_page(double x, double y) const;
  Point page_to_canvas(std::size_t page, Point local) const;
  void finish_canvas_drag(double x, double y);

  static void draw_page(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                        gpointer data);
  static void draw_signature(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                             gpointer data);

  AdwApplicationWindow* window_{};
  AdwToastOverlay* toast_overlay_{};
  GtkPaned* split_{};
  GtkListBox* sidebar_{};
  GtkDrawingArea* canvas_{};
  GtkScrolledWindow* canvas_scroll_{};
  GtkOverlay* canvas_overlay_{};
  GtkWidget* inline_editor_{};
  GtkTextView* inline_text_view_{};
  GtkTextTag* inline_text_tag_{};
  GtkTextTag* inline_typing_tag_{};
  TextStyle inline_typing_style_{};
  bool loading_inline_text_{};
  std::vector<std::pair<GtkTextTag*, TextStyle>> inline_format_tags_{};
  GtkSearchEntry* search_{};
  GtkToggleButton* active_tool_button_{};
  GtkToggleButton* select_button_{};
  GtkMenuButton* signature_button_{};
  GtkBox* signature_box_{};
  GtkColorDialogButton* stroke_color_{};
  GtkColorDialogButton* fill_color_{};
  GtkRevealer* property_revealer_{};
  GtkWidget* stroke_control_{};
  GtkWidget* fill_control_{};
  GtkBox* text_format_box_{};
  GtkButton* font_button_{};
  GtkToggleButton* bold_button_{};
  GtkToggleButton* italic_button_{};
  GtkToggleButton* underline_button_{};
  GtkToggleButton* align_left_button_{};
  GtkToggleButton* align_center_button_{};
  GtkToggleButton* align_right_button_{};
  GtkLabel* page_label_{};
  GMenu* recent_menu_{};
  std::unique_ptr<Document> document_{};
  struct CachedPage {
    std::size_t page;
    std::uint64_t skip;
    cairo_surface_t* surface;
  };
  std::vector<CachedPage> page_cache_;
  std::uint64_t page_cache_revision_{};
  double page_cache_zoom_{};
  std::vector<cairo_surface_t*> page_previews_{};
  std::set<std::pair<std::size_t, std::uint64_t>> pending_page_renders_{};
  guint page_render_source_id_{};
  guint scroll_settle_source_id_{};
  bool scrolling_{};
  SignatureStore signatures_{};
  std::size_t current_page_{};
  double zoom_{1.0};
  bool fit_width_{true};
  bool fit_width_dirty_{true};
  int last_fit_available_{};
  double last_fit_reference_{};
  bool continuous_scroll_{false};
  bool syncing_sidebar_{false};
  GdkModifierType sidebar_modifiers_{};
  int sidebar_position_{180};
  double last_scroll_value_{};
  bool has_last_scroll_value_{};
  bool warned_destructive_{};
  bool tool_active_{};
  bool crop_mode_{};
  enum class PointerMode { Select, Annotate } pointer_mode_{PointerMode::Select};
  AnnotationType active_tool_{AnnotationType::Rectangle};
  struct ToolColors {
    Color stroke{0.0, 0.0, 0.0, 1.0};
    Color fill{0.0, 0.0, 0.0, 0.0};
  };
  std::array<ToolColors, 8> tool_colors_{};
  Color annotation_stroke_{0.0, 0.0, 0.0, 1.0};
  Color annotation_fill_{0.0, 0.0, 0.0, 0.0};
  std::string annotation_font_{"Sans"};
  double annotation_font_size_{12.0};
  bool annotation_bold_{};
  bool annotation_italic_{};
  bool annotation_underline_{};
  TextAlign annotation_align_{TextAlign::Left};
  std::uint64_t selected_annotation_id_{};
  bool resizing_annotation_{};
  bool annotation_edit_started_{};
  bool updating_annotation_controls_{};
  Annotation annotation_edit_origin_{};
  Point drag_start_{};
  Point drag_current_{};
  Point drag_origin_canvas_{};
  bool drag_active_{};
  bool drag_is_text_{};
  bool touch_panning_{};
  double pan_horizontal_start_{};
  double pan_vertical_start_{};
  double pinch_start_zoom_{1.0};
  double pinch_view_x_{};
  double pinch_view_y_{};
  Point pinch_anchor_{};
  std::size_t pinch_anchor_page_{};
  bool pinch_active_{};
  std::size_t annotation_drag_page_{};
  std::size_t annotation_origin_page_{};
  std::uint64_t annotation_drag_id_{};
  bool page_turn_pending_{};
  std::size_t inline_edit_page_{};
  std::uint64_t inline_edit_id_{};
  Annotation inline_edit_annotation_{};
  bool inline_edit_active_{};
  bool inline_edit_is_new_{};
  bool finishing_inline_edit_{};
  gint64 inline_edit_started_at_{};
  std::vector<Point> ink_points_{};
  std::vector<Point> signature_points_{};
};

} // namespace review
