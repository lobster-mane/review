#pragma once

#include "review-types.hpp"

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <poppler.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace review {

class Document {
public:
  enum class Kind { Pdf, Image };

  Document();
  ~Document();
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;

  bool open(const std::string& path, std::string& error,
            const std::string& password = {});
  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] Kind kind() const { return kind_; }
  [[nodiscard]] std::size_t page_count() const { return pages_.size(); }
  [[nodiscard]] const PageRef& page(std::size_t index) const { return pages_.at(index); }
  [[nodiscard]] PageRef& page(std::size_t index) { return pages_.at(index); }
  [[nodiscard]] bool dirty() const { return dirty_; }
  [[nodiscard]] bool destructive_changes() const { return destructive_changes_; }
  [[nodiscard]] bool read_only() const { return read_only_; }
  [[nodiscard]] std::uint64_t revision() const { return revision_; }
  void invalidate_rendering();

  cairo_surface_t* render_page(std::size_t index, double scale, bool annotations = true,
                               std::uint64_t skip_annotation_id = 0) const;
  std::string page_text(std::size_t index) const;
  [[nodiscard]] std::vector<Rect> highlight_rects(std::size_t index, Point start,
                                                 Point end) const;

  void checkpoint();
  bool undo();
  bool redo();
  void reorder(const std::vector<std::size_t>& indices, std::size_t destination);
  void remove(const std::vector<std::size_t>& indices);
  void duplicate(const std::vector<std::size_t>& indices, std::size_t destination);
  void insert_pages(const std::vector<PageRef>& pages, std::size_t destination);
  void rotate(const std::vector<std::size_t>& indices, int degrees);
  void add_annotation(std::size_t page_index, Annotation annotation);
  bool update_annotation(std::size_t page_index, std::uint64_t id,
                         const Annotation& annotation);
  bool move_annotation(std::size_t from_page, std::uint64_t id, std::size_t to_page,
                       const Annotation& annotation);
  bool remove_annotation(std::size_t page_index, std::uint64_t id);
  void begin_annotation_edit();
  void commit_annotation_edit();
  void secure_crop(const std::vector<std::size_t>& indices, Rect rect);
  void rasterize_color(const std::vector<std::size_t>& indices, std::string& error);

  bool save(const std::string& destination, std::string& error);
  bool export_flattened(const std::string& destination, std::string& error) const;

  [[nodiscard]] std::vector<PageRef> copy_pages(const std::vector<std::size_t>& indices) const;

private:
  struct State {
    std::vector<PageRef> pages;
    bool destructive{};
  };

  bool open_pdf(const std::string& path, const std::string& password, std::string& error);
  bool open_image(const std::string& path, std::string& error);
  PopplerDocument* load_pdf(const std::string& path, std::string& error,
                            const std::string& password = {}) const;
  cairo_surface_t* render_source(const PageRef& ref, double scale) const;
  void draw_annotations(cairo_t* cr, const PageRef& ref, double scale,
                        std::uint64_t skip_annotation_id = 0) const;
  bool create_image_pdf(const PageRef& ref, const std::string& destination,
                        bool include_annotations, std::string& error) const;
  bool assemble_pdf(const std::string& destination, bool flatten,
                    std::string& error) const;
  bool write_pdf_annotations(const std::string& path, std::string& error) const;
  static bool atomic_replace(const std::string& temporary, const std::string& destination,
                             std::string& error);
  static std::string temporary_path(const std::string& near_path, const char* suffix);
  void changed(bool destructive = false);

  std::string path_{};
  Kind kind_{Kind::Pdf};
  PopplerDocument* pdf_{};
  GdkPixbuf* image_{};
  std::vector<PageRef> pages_{};
  std::vector<State> undo_{};
  std::vector<State> redo_{};
  bool dirty_{};
  bool destructive_changes_{};
  bool read_only_{};
  std::uint64_t revision_{1};
  bool annotation_edit_open_{};
  std::uint64_t next_annotation_id_{1};
  std::filesystem::path session_dir_{};
};

} // namespace review
