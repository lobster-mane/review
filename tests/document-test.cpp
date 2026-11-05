#include "review-document.hpp"

#include <cairo.h>
#include <cairo-pdf.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <filesystem>
#include <fstream>
#include <numeric>

namespace {

std::string fixture_pdf() {
  const auto path = (std::filesystem::temp_directory_path() /
                     ("review-test-" + std::to_string(g_get_monotonic_time()) + ".pdf")).string();
  cairo_surface_t* surface = cairo_pdf_surface_create(path.c_str(), 320, 200);
  cairo_t* cr = cairo_create(surface);
  cairo_set_source_rgb(cr, .1, .45, .9); cairo_paint(cr);
  cairo_set_source_rgb(cr, 1, .4, .1); cairo_rectangle(cr, 40, 50, 180, 80); cairo_fill(cr);
  cairo_show_page(cr);
  cairo_destroy(cr); cairo_surface_destroy(surface);
  return path;
}

std::string fixture_text_pdf() {
  const auto path = (std::filesystem::temp_directory_path() /
                     ("review-text-test-" + std::to_string(g_get_monotonic_time()) + ".pdf")).string();
  cairo_surface_t* surface = cairo_pdf_surface_create(path.c_str(), 320, 200);
  cairo_t* cr = cairo_create(surface);
  cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr);
  cairo_set_source_rgb(cr, .05, .05, .05);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 24);
  cairo_move_to(cr, 40, 100);
  cairo_show_text(cr, "Hello");
  cairo_show_page(cr);
  cairo_destroy(cr); cairo_surface_destroy(surface);
  return path;
}

void assert_annotation_appearances(const std::string& pdf_path) {
  const auto qdf_path = pdf_path + ".qdf.pdf";
  gchar* argv[] = {
      const_cast<gchar*>("qpdf"), const_cast<gchar*>("--qdf"),
      const_cast<gchar*>("--object-streams=disable"),
      const_cast<gchar*>(pdf_path.c_str()), const_cast<gchar*>(qdf_path.c_str()), nullptr};
  gint exit_status = 0;
  GError* spawn_error = nullptr;
  const auto flags = static_cast<GSpawnFlags>(
      G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL);
  g_assert_true(g_spawn_sync(nullptr, argv, nullptr, flags,
                             nullptr, nullptr, nullptr, nullptr, &exit_status, &spawn_error));
  g_assert_no_error(spawn_error);
  g_assert_true(g_spawn_check_wait_status(exit_status, &spawn_error));
  g_assert_no_error(spawn_error);

  std::ifstream stream(qdf_path, std::ios::binary);
  g_assert_true(stream.good());
  const std::string qdf((std::istreambuf_iterator<char>(stream)),
                        std::istreambuf_iterator<char>());
  std::size_t stamp = 0;
  int stamp_count = 0;
  while ((stamp = qdf.find("/Subtype /Stamp", stamp)) != std::string::npos) {
    const auto object_start = qdf.rfind(" obj\n", stamp);
    const auto object_end = qdf.find("endobj", stamp);
    g_assert_cmpuint(object_start, !=, std::string::npos);
    g_assert_cmpuint(object_end, !=, std::string::npos);
    const auto object = qdf.substr(object_start, object_end - object_start);
    g_assert_nonnull(g_strstr_len(object.data(), object.size(), "/AP"));
    ++stamp_count;
    stamp = object_end;
  }
  g_assert_cmpint(stamp_count, >=, 1);

  const auto free_text = qdf.find("/Subtype /FreeText");
  g_assert_cmpuint(free_text, !=, std::string::npos);
  const auto free_text_start = qdf.rfind(" obj\n", free_text);
  const auto free_text_end = qdf.find("endobj", free_text);
  g_assert_cmpuint(free_text_start, !=, std::string::npos);
  g_assert_cmpuint(free_text_end, !=, std::string::npos);
  const auto free_text_object = qdf.substr(free_text_start, free_text_end - free_text_start);
  g_assert_nonnull(g_strstr_len(free_text_object.data(), free_text_object.size(), "/AP"));
  g_assert_nonnull(g_strstr_len(free_text_object.data(), free_text_object.size(),
                               "/Helvetica-Bold"));
  g_assert_nonnull(g_strstr_len(free_text_object.data(), free_text_object.size(), "/Q 1"));
  g_unlink(qdf_path.c_str());
}

void open_and_render() {
  const auto path = fixture_pdf();
  review::Document doc; std::string error;
  const bool opened = doc.open(path, error);
  g_test_message("open error: %s", error.c_str());
  g_assert_true(opened);
  g_assert_cmpuint(doc.page_count(), ==, 1);
  auto* rendered = doc.render_page(0, 1.0);
  g_assert_nonnull(rendered);
  g_assert_cmpint(cairo_image_surface_get_width(rendered), ==, 320);
  cairo_surface_destroy(rendered);
  g_unlink(path.c_str());
}

void annotation_undo_redo() {
  const auto path = fixture_pdf();
  review::Document doc; std::string error;
  g_assert_true(doc.open(path, error));
  review::Annotation annotation;
  annotation.type = review::AnnotationType::Rectangle;
  annotation.bounds = {20, 20, 100, 60};
  doc.add_annotation(0, annotation);
  g_assert_true(doc.dirty());
  g_assert_cmpuint(doc.page(0).annotations.size(), ==, 1);
  g_assert_true(doc.undo());
  g_assert_cmpuint(doc.page(0).annotations.size(), ==, 0);
  g_assert_true(doc.redo());
  g_assert_cmpuint(doc.page(0).annotations.size(), ==, 1);
  g_unlink(path.c_str());
}

void annotation_edit_properties() {
  auto fixture = fixture_pdf();
  review::Document doc;
  std::string error;
  g_assert_true(doc.open(fixture, error));
  review::Annotation annotation;
  annotation.type = review::AnnotationType::TextBox;
  annotation.bounds = {20, 30, 120, 40};
  annotation.text = "Original";
  doc.add_annotation(0, annotation);
  const auto id = doc.page(0).annotations.front().id;

  auto edited = doc.page(0).annotations.front();
  edited.text = "Edited";
  edited.font_family = "Serif";
  edited.font_size = 18;
  edited.stroke = {0.1, 0.2, 0.8, 1.0};
  edited.bold = true;
  edited.italic = true;
  edited.underline = true;
  edited.align = review::TextAlign::Center;
  review::TextStyle span_style;
  span_style.bold = true;
  span_style.color = {0.2, 0.4, 0.6, 1.0};
  span_style.font_family = "Serif";
  span_style.font_size = 18;
  edited.text_spans = {{0, 4, span_style}};
  g_assert_true(doc.update_annotation(0, id, edited));
  g_assert_cmpstr(doc.page(0).annotations.front().text.c_str(), ==, "Edited");
  g_assert_cmpstr(doc.page(0).annotations.front().font_family.c_str(), ==, "Serif");
  g_assert_true(doc.page(0).annotations.front().bold);
  g_assert_true(doc.page(0).annotations.front().italic);
  g_assert_true(doc.page(0).annotations.front().underline);
  g_assert_true(doc.page(0).annotations.front().align == review::TextAlign::Center);
  g_assert_cmpuint(doc.page(0).annotations.front().text_spans.size(), ==, 1);
  g_assert_cmpuint(doc.page(0).annotations.front().text_spans.front().end, ==, 4);
  g_assert_true(doc.page(0).annotations.front().text_spans.front().style.bold);
  g_assert_true(doc.undo());
  g_assert_cmpstr(doc.page(0).annotations.front().text.c_str(), ==, "Original");

  doc.begin_annotation_edit();
  doc.page(0).annotations.front().bounds.x += 25;
  doc.commit_annotation_edit();
  g_assert_cmpfloat(doc.page(0).annotations.front().bounds.x, ==, 45);
  g_assert_true(doc.undo());
  g_assert_cmpfloat(doc.page(0).annotations.front().bounds.x, ==, 20);
  g_unlink(fixture.c_str());
}

void move_annotation_between_pages() {
  const auto path = fixture_pdf();
  review::Document doc;
  std::string error;
  g_assert_true(doc.open(path, error));
  auto pages = doc.copy_pages({0});
  doc.insert_pages(pages, 1);
  g_assert_cmpuint(doc.page_count(), ==, 2);

  review::Annotation annotation;
  annotation.type = review::AnnotationType::Rectangle;
  annotation.bounds = {20, 30, 80, 50};
  doc.add_annotation(0, annotation);
  const auto id = doc.page(0).annotations.front().id;

  auto moved = doc.page(0).annotations.front();
  moved.bounds.x += 40;
  g_assert_true(doc.move_annotation(0, id, 1, moved));
  g_assert_cmpuint(doc.page(0).annotations.size(), ==, 0);
  g_assert_cmpuint(doc.page(1).annotations.size(), ==, 1);
  g_assert_cmpuint(doc.page(1).annotations.front().id, ==, id);
  g_assert_cmpfloat(doc.page(1).annotations.front().bounds.x, ==, 60);
  g_unlink(path.c_str());
}

void highlight_uses_text_layout() {
  const auto path = fixture_text_pdf();
  review::Document doc;
  std::string error;
  g_assert_true(doc.open(path, error));
  const auto rects = doc.highlight_rects(0, {20, 70, 240, 60});
  g_assert_cmpuint(rects.size(), >=, 1);
  g_assert_cmpfloat(rects.front().width, >, 0);
  g_assert_cmpfloat(rects.front().height, >, 0);
  g_assert_true(doc.highlight_rects(0, {0, 160, 300, 30}).empty());
  g_unlink(path.c_str());
}

void crop_is_destructive() {
  const auto path = fixture_pdf();
  review::Document doc; std::string error;
  g_assert_true(doc.open(path, error));
  doc.secure_crop({0}, {10, 10, 100, 80});
  g_assert_true(doc.destructive_changes());
  g_assert_cmpfloat(doc.page(0).width_points, ==, 100.0);
  auto* rendered = doc.render_page(0, 1.0);
  g_assert_cmpint(cairo_image_surface_get_width(rendered), ==, 100);
  cairo_surface_destroy(rendered);
  g_assert_true(doc.undo());
  g_assert_false(doc.destructive_changes());
  g_unlink(path.c_str());
}

void page_operations() {
  const auto path = fixture_pdf();
  review::Document doc; std::string error;
  g_assert_true(doc.open(path, error));
  doc.duplicate({0}, 1);
  doc.duplicate({0}, 2);
  g_assert_cmpuint(doc.page_count(), ==, 3);
  doc.rotate({1}, 90);
  g_assert_cmpint(doc.page(1).rotation, ==, 90);
  doc.reorder({2}, 0);
  g_assert_cmpuint(doc.page_count(), ==, 3);
  doc.remove({1});
  g_assert_cmpuint(doc.page_count(), ==, 2);
  g_unlink(path.c_str());
}

void text_wraps_to_bounds() {
  const auto path = fixture_pdf();
  const auto output = path + ".wrap.pdf";
  review::Document doc;
  std::string error;
  g_assert_true(doc.open(path, error));
  review::Annotation text;
  text.type = review::AnnotationType::TextBox;
  text.bounds = {20, 20, 80, 20};
  text.text = "A long line of text that must wrap inside a narrow text box.";
  text.font_size = 14;
  doc.add_annotation(0, text);
  const auto& saved = doc.page(0).annotations.back();
  g_assert_cmpfloat(saved.bounds.width, ==, 80);
  g_assert_cmpfloat(saved.bounds.height, >, 20);
  g_assert_true(doc.save(output, error));
  g_unlink(path.c_str());
  g_unlink(output.c_str());
}

void save_annotations() {
  const auto path = fixture_pdf();
  const auto output = path + ".saved.pdf";
  const auto signature_path = path + ".signature.png";
  review::Document doc; std::string error;
  g_assert_true(doc.open(path, error));
  review::Annotation text;
  text.type = review::AnnotationType::TextBox;
  text.bounds = {25, 60, 40, 8};
  text.text = "Editable PDF text";
  text.font_family = "Sans";
  text.font_size = 32;
  text.stroke = {0.1, 0.2, 0.75, 1.0};
  text.bold = true;
  text.underline = true;
  text.align = review::TextAlign::Center;
  review::TextStyle span;
  span.bold = true;
  span.italic = true;
  span.color = {0.8, 0.1, 0.1, 1.0};
  span.font_family = "Serif";
  span.font_size = 24;
  text.text_spans = {{0, 8, span}};
  doc.add_annotation(0, text);
  const auto saved_text = doc.page(0).annotations.back();
  g_assert_cmpfloat(saved_text.bounds.width, ==, text.bounds.width);
  g_assert_cmpfloat(saved_text.bounds.height, >, text.bounds.height);

  review::Annotation square;
  square.type = review::AnnotationType::Rectangle;
  square.bounds = {70, 20, 50, 35};
  square.stroke = {0.9, 0.1, 0.1, 1.0};
  square.fill = {0.1, 0.8, 0.2, 0.65};
  doc.add_annotation(0, square);

  review::Annotation circle = square;
  circle.type = review::AnnotationType::Ellipse;
  circle.bounds = {130, 20, 50, 35};
  doc.add_annotation(0, circle);

  review::Annotation marker;
  marker.type = review::AnnotationType::Highlight;
  marker.bounds = {60, 70, 120, 28};
  marker.fill = {1.0, 0.92, 0.23, 0.35};
  marker.text_rects = {{60, 70, 100, 12}, {60, 86, 80, 12}};
  doc.add_annotation(0, marker);

  review::Annotation ink;
  ink.type = review::AnnotationType::Ink;
  ink.bounds = {30, 100, 90, 35};
  ink.stroke = {0.8, 0.1, 0.7, 1.0};
  ink.points = {{30, 120}, {50, 102}, {75, 132}, {120, 108}};
  doc.add_annotation(0, ink);

  cairo_surface_t* signature_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 160, 50);
  cairo_t* signature_cr = cairo_create(signature_surface);
  cairo_set_operator(signature_cr, CAIRO_OPERATOR_CLEAR); cairo_paint(signature_cr);
  cairo_set_operator(signature_cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgb(signature_cr, .05, .05, .05); cairo_set_line_width(signature_cr, 5);
  cairo_move_to(signature_cr, 8, 38); cairo_curve_to(signature_cr, 40, 2, 80, 48, 150, 12);
  cairo_stroke(signature_cr); cairo_destroy(signature_cr);
  g_assert_cmpint(cairo_surface_write_to_png(signature_surface, signature_path.c_str()), ==,
                  CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy(signature_surface);
  review::Annotation signature;
  signature.type = review::AnnotationType::Signature;
  signature.bounds = {190, 100, 100, 35};
  signature.signature_path = signature_path;
  doc.add_annotation(0, signature);
  const bool saved = doc.save(output, error);
  g_test_message("save error: %s", error.c_str());
  g_assert_true(saved);

  review::Document reopened;
  g_assert_true(reopened.open(output, error));
  PopplerDocument* pdf = poppler_document_new_from_file(
      g_filename_to_uri(output.c_str(), nullptr, nullptr), nullptr, nullptr);
  g_assert_nonnull(pdf);
  PopplerPage* page = poppler_document_get_page(pdf, 0);
  GList* annotations = poppler_page_get_annot_mapping(page);
  g_assert_nonnull(annotations);
  int squares = 0, circles = 0, free_texts = 0, text_notes = 0, inks = 0, stamps = 0;
  int highlights = 0;
  for (GList* item = annotations; item; item = item->next) {
    auto* mapping = static_cast<PopplerAnnotMapping*>(item->data);
    switch (poppler_annot_get_annot_type(mapping->annot)) {
      case POPPLER_ANNOT_HIGHLIGHT: ++highlights; break;
      case POPPLER_ANNOT_SQUARE: {
        ++squares;
        auto* interior = poppler_annot_square_get_interior_color(
            POPPLER_ANNOT_SQUARE(mapping->annot));
        g_assert_nonnull(interior);
        g_assert_cmpuint(interior->green, >, interior->red);
        g_free(interior);
        break;
      }
      case POPPLER_ANNOT_CIRCLE: {
        ++circles;
        auto* interior = poppler_annot_circle_get_interior_color(
            POPPLER_ANNOT_CIRCLE(mapping->annot));
        g_assert_nonnull(interior);
        g_assert_cmpuint(interior->green, >, interior->red);
        g_free(interior);
        break;
      }
      case POPPLER_ANNOT_INK: ++inks; break;
      case POPPLER_ANNOT_STAMP: ++stamps; break;
      case POPPLER_ANNOT_FREE_TEXT: {
        ++free_texts;
        char* contents = poppler_annot_get_contents(mapping->annot);
        g_assert_cmpstr(contents, ==, "Editable PDF text");
        g_free(contents);
        g_assert_cmpfloat_with_epsilon(mapping->area.x1, saved_text.bounds.x, 0.01);
        g_assert_cmpfloat_with_epsilon(mapping->area.x2,
                                       saved_text.bounds.x + saved_text.bounds.width, 0.01);
        g_assert_cmpfloat_with_epsilon(mapping->area.y1,
                                       200.0 - saved_text.bounds.y - saved_text.bounds.height,
                                       0.01);
        g_assert_cmpfloat_with_epsilon(mapping->area.y2,
                                       200.0 - saved_text.bounds.y, 0.01);
        break;
      }
      case POPPLER_ANNOT_TEXT: ++text_notes; break;
      default: break;
    }
  }
  g_assert_cmpint(squares, ==, 1);
  g_assert_cmpint(circles, ==, 1);
  g_assert_cmpint(highlights, ==, 1);
  g_assert_cmpint(free_texts, ==, 1);
  g_assert_cmpint(text_notes, ==, 0);
#ifdef HAVE_POPPLER_INK_API
  g_assert_cmpint(inks, ==, 1);
#else
  g_assert_cmpint(stamps, >=, 2);
#endif
  g_assert_cmpint(stamps, >=, 1);
  poppler_page_free_annot_mapping(annotations);
  g_object_unref(page); g_object_unref(pdf);
  assert_annotation_appearances(output);
  if (g_getenv("REVIEW_KEEP_TEST_OUTPUT"))
    g_test_message("saved annotation fixture: %s", output.c_str());
  else {
    g_unlink(path.c_str()); g_unlink(output.c_str()); g_unlink(signature_path.c_str());
  }
}

void rasterized_page_has_no_text() {
  const auto path = fixture_pdf();
  const auto output = path + ".scan.pdf";
  review::Document doc; std::string error;
  g_assert_true(doc.open(path, error));
  doc.rasterize_color({0}, error);
  g_assert_true(error.empty());
  const bool saved = doc.save(output, error);
  g_test_message("scan save error: %s", error.c_str());
  g_assert_true(saved);
  review::Document reopened;
  g_assert_true(reopened.open(output, error));
  g_assert_true(reopened.page_text(0).empty());
  g_unlink(path.c_str()); g_unlink(output.c_str());
}

} // namespace

int main(int argc, char** argv) {
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/document/open-render", open_and_render);
  g_test_add_func("/document/annotation-undo-redo", annotation_undo_redo);
  g_test_add_func("/document/annotation-edit-properties", annotation_edit_properties);
  g_test_add_func("/document/move-annotation-between-pages", move_annotation_between_pages);
  g_test_add_func("/document/highlight-text-layout", highlight_uses_text_layout);
  g_test_add_func("/document/secure-crop", crop_is_destructive);
  g_test_add_func("/document/page-operations", page_operations);
  g_test_add_func("/document/text-wrap", text_wraps_to_bounds);
  g_test_add_func("/document/save-annotations", save_annotations);
  g_test_add_func("/document/rasterize-color", rasterized_page_has_no_text);
  return g_test_run();
}
