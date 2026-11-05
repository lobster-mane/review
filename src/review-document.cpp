#include "review-document.hpp"

#include <cairo-pdf.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include <sstream>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace review {
namespace {

constexpr double kScanScale = 300.0 / 72.0;

std::string file_uri(const std::string& path) {
  GError* error = nullptr;
  char* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
  if (!uri) {
    if (error) g_error_free(error);
    return {};
  }
  std::string result(uri);
  g_free(uri);
  return result;
}

void set_error(std::string& destination, GError* error, const char* fallback) {
  destination = error ? error->message : fallback;
  if (error) g_error_free(error);
}

bool run_process(const std::vector<std::string>& args, std::string& error) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  gchar* stderr_text = nullptr;
  gint status = 0;
  GError* spawn_error = nullptr;
  const gboolean ok = g_spawn_sync(nullptr, argv.data(), nullptr,
                                   static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH),
                                   nullptr, nullptr, nullptr, &stderr_text, &status,
                                   &spawn_error);
  if (!ok || !g_spawn_check_wait_status(status, &spawn_error)) {
    error = stderr_text && *stderr_text ? stderr_text :
            (spawn_error ? spawn_error->message : "The PDF helper failed");
    if (spawn_error) g_error_free(spawn_error);
    g_free(stderr_text);
    return false;
  }
  g_free(stderr_text);
  return true;
}

PangoLayout* create_text_layout(cairo_t* cr, const Annotation& annotation, double width) {
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = pango_font_description_new();
  pango_font_description_set_family(
      font, annotation.font_family.empty() ? "Sans" : annotation.font_family.c_str());
  pango_font_description_set_size(
      font, std::max(1, static_cast<int>(annotation.font_size * PANGO_SCALE)));
  pango_font_description_set_weight(
      font, annotation.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_style(
      font, annotation.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);

  const int inner_width = std::max(1, static_cast<int>((width - 8.0) * PANGO_SCALE));
  pango_layout_set_width(layout, inner_width);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_alignment(
      layout, annotation.align == TextAlign::Center ? PANGO_ALIGN_CENTER :
              annotation.align == TextAlign::Right ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
  pango_layout_set_text(layout, annotation.text.c_str(), -1);
  if (annotation.underline) {
    PangoAttrList* attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
    pango_layout_set_attributes(layout, attributes);
    pango_attr_list_unref(attributes);
  }
  return layout;
}

Rect fit_text_bounds(const Annotation& annotation) {
  Rect fitted = annotation.bounds;
  auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  auto* cr = cairo_create(surface);
  PangoLayout* layout = create_text_layout(
      cr, annotation, std::max(24.0, fitted.width));
  int width = 0;
  int height = 0;
  pango_layout_get_size(layout, &width, &height);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  fitted.width = std::max(fitted.width, 24.0);
  fitted.height = std::max(
      fitted.height, std::ceil(height / static_cast<double>(PANGO_SCALE) + 8.0));
  return fitted;
}

std::string pdf_utf16_string(const std::string& text) {
  glong length = 0;
  GError* error = nullptr;
  gunichar2* utf16 = g_utf8_to_utf16(text.c_str(), -1, nullptr, &length, &error);
  if (!utf16) {
    if (error) g_error_free(error);
    return "<FEFF>";
  }
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string result = "<FEFF";
  result.reserve(6 + static_cast<std::size_t>(length) * 4);
  for (glong i = 0; i < length; ++i) {
    const auto value = static_cast<unsigned>(utf16[i]);
    result.push_back(hex[(value >> 12) & 0xf]);
    result.push_back(hex[(value >> 8) & 0xf]);
    result.push_back(hex[(value >> 4) & 0xf]);
    result.push_back(hex[value & 0xf]);
  }
  result.push_back('>');
  g_free(utf16);
  return result;
}

std::string pdf_number(double value) {
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];
  return g_ascii_dtostr(buffer, sizeof(buffer), value);
}

std::string pdf_font_name(const Annotation& annotation) {
  const bool mono = annotation.font_family.find("Mono") != std::string::npos;
  const bool serif = annotation.font_family.find("Serif") != std::string::npos;
  std::string font;
  if (mono) {
    font = "Courier";
    if (annotation.bold && annotation.italic) font += "-BoldOblique";
    else if (annotation.bold) font += "-Bold";
    else if (annotation.italic) font += "-Oblique";
  } else if (serif) {
    font = "Times";
    if (annotation.bold && annotation.italic) font += "-BoldItalic";
    else if (annotation.bold) font += "-Bold";
    else if (annotation.italic) font += "-Italic";
    else font += "-Roman";
  } else {
    font = "Helvetica";
    if (annotation.bold && annotation.italic) font += "-BoldOblique";
    else if (annotation.bold) font += "-Bold";
    else if (annotation.italic) font += "-Oblique";
  }
  return "/" + font;
}

bool convert_saved_free_text(const std::string& path,
                             const std::vector<PageRef>& pages,
                             std::string& error) {
  bool has_text = false;
  for (const auto& page : pages)
    for (const auto& annotation : page.annotations)
      has_text = has_text || annotation.type == AnnotationType::TextBox;
  if (!has_text) return true;

  const auto qdf_path = path + ".freetext.qdf.pdf";
  const auto repaired_path = path + ".freetext.pdf";
  if (!run_process({"qpdf", "--qdf", "--object-streams=disable", path, qdf_path}, error))
    return false;

  gchar* raw = nullptr;
  gsize raw_size = 0;
  GError* file_error = nullptr;
  if (!g_file_get_contents(qdf_path.c_str(), &raw, &raw_size, &file_error)) {
    set_error(error, file_error, "Unable to prepare editable PDF text");
    g_unlink(qdf_path.c_str());
    return false;
  }
  std::string qdf(raw, raw_size);
  g_free(raw);

  for (const auto& page : pages) {
    for (const auto& annotation : page.annotations) {
      if (annotation.type != AnnotationType::TextBox) continue;
      const auto marker = "review-freetext-" + std::to_string(annotation.id);
      auto encoded_marker = pdf_utf16_string(marker);
      std::transform(encoded_marker.begin(), encoded_marker.end(), encoded_marker.begin(),
                     [](unsigned char value) { return static_cast<char>(g_ascii_tolower(value)); });
      const auto contents_token = "/Contents " + encoded_marker;
      const auto contents = qdf.find(contents_token);
      if (contents == std::string::npos) {
        error = "Unable to locate the editable text annotation in the saved PDF";
        g_unlink(qdf_path.c_str());
        return false;
      }
      const auto object_end = qdf.find("endobj", contents);
      auto subtype = qdf.find("/Subtype /Stamp", contents);
      if (object_end == std::string::npos || subtype == std::string::npos || subtype > object_end) {
        error = "Unable to convert the saved text annotation to FreeText";
        g_unlink(qdf_path.c_str());
        return false;
      }
      qdf.replace(contents, contents_token.size(),
                  "/Contents " + pdf_utf16_string(annotation.text));
      subtype = qdf.find("/Subtype /Stamp", contents);
      qdf.replace(subtype, std::strlen("/Subtype /Stamp"), "/Subtype /FreeText");
      const int alignment = annotation.align == TextAlign::Center ? 1 :
                            annotation.align == TextAlign::Right ? 2 : 0;
      const auto appearance = std::string("\n  /DA (") + pdf_font_name(annotation) + " " +
                              pdf_number(annotation.font_size) +
                              " Tf " + pdf_number(annotation.stroke.red) + " " +
                              pdf_number(annotation.stroke.green) + " " +
                              pdf_number(annotation.stroke.blue) + " rg)\n  /Q " +
                              std::to_string(alignment);
      qdf.insert(subtype + std::strlen("/Subtype /FreeText"), appearance);
    }
  }

  if (!g_file_set_contents(qdf_path.c_str(), qdf.data(), qdf.size(), &file_error)) {
    set_error(error, file_error, "Unable to write editable PDF text");
    g_unlink(qdf_path.c_str());
    return false;
  }
  if (!run_process({"qpdf", "--warning-exit-0", qdf_path, repaired_path}, error)) {
    g_unlink(qdf_path.c_str());
    return false;
  }
  g_unlink(qdf_path.c_str());
  std::error_code rename_error;
  fs::rename(repaired_path, path, rename_error);
  if (rename_error) {
    error = "Unable to finish editable PDF text: " + rename_error.message();
    g_unlink(repaired_path.c_str());
    return false;
  }
  return true;
}

void paint_pixbuf(cairo_t* cr, GdkPixbuf* pixbuf, double x, double y,
                  double width, double height) {
  const int pw = gdk_pixbuf_get_width(pixbuf);
  const int ph = gdk_pixbuf_get_height(pixbuf);
  if (pw <= 0 || ph <= 0) return;
  auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
  auto* destination = cairo_image_surface_get_data(surface);
  const int destination_stride = cairo_image_surface_get_stride(surface);
  const auto* source = gdk_pixbuf_get_pixels(pixbuf);
  const int source_stride = gdk_pixbuf_get_rowstride(pixbuf);
  const int channels = gdk_pixbuf_get_n_channels(pixbuf);
  for (int py = 0; py < ph; ++py) {
    auto* out = reinterpret_cast<std::uint32_t*>(destination + py * destination_stride);
    const auto* in = source + py * source_stride;
    for (int px = 0; px < pw; ++px) {
      const unsigned alpha = channels == 4 ? in[px * channels + 3] : 255;
      const unsigned red = in[px * channels] * alpha / 255;
      const unsigned green = in[px * channels + 1] * alpha / 255;
      const unsigned blue = in[px * channels + 2] * alpha / 255;
      out[px] = (alpha << 24) | (red << 16) | (green << 8) | blue;
    }
  }
  cairo_surface_mark_dirty(surface);
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, width / pw, height / ph);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);
  cairo_restore(cr);
  cairo_surface_destroy(surface);
}

void arrow_head(cairo_t* cr, Point from, Point to, double width) {
  const double angle = std::atan2(to.y - from.y, to.x - from.x);
  const double length = std::max(8.0, width * 4.0);
  cairo_move_to(cr, to.x, to.y);
  cairo_line_to(cr, to.x - length * std::cos(angle - 0.45),
                to.y - length * std::sin(angle - 0.45));
  cairo_move_to(cr, to.x, to.y);
  cairo_line_to(cr, to.x - length * std::cos(angle + 0.45),
                to.y - length * std::sin(angle + 0.45));
}

GdkPixbuf* pixbuf_from_surface(cairo_surface_t* surface) {
  cairo_surface_flush(surface);
  const int width = cairo_image_surface_get_width(surface);
  const int height = cairo_image_surface_get_height(surface);
  const int source_stride = cairo_image_surface_get_stride(surface);
  const auto* source = cairo_image_surface_get_data(surface);
  GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
  auto* destination = gdk_pixbuf_get_pixels(pixbuf);
  const int destination_stride = gdk_pixbuf_get_rowstride(pixbuf);
  for (int y = 0; y < height; ++y) {
    const auto* in = reinterpret_cast<const std::uint32_t*>(source + y * source_stride);
    auto* out = destination + y * destination_stride;
    for (int x = 0; x < width; ++x) {
      const unsigned pixel = in[x];
      const unsigned alpha = pixel >> 24;
      const unsigned red = (pixel >> 16) & 0xff;
      const unsigned green = (pixel >> 8) & 0xff;
      const unsigned blue = pixel & 0xff;
      out[x * 4] = alpha ? std::min(255u, red * 255u / alpha) : 0;
      out[x * 4 + 1] = alpha ? std::min(255u, green * 255u / alpha) : 0;
      out[x * 4 + 2] = alpha ? std::min(255u, blue * 255u / alpha) : 0;
      out[x * 4 + 3] = alpha;
    }
  }
  return pixbuf;
}

} // namespace

Document::Document() {
  GError* error = nullptr;
  char* dir = g_dir_make_tmp("review-XXXXXX", &error);
  if (dir) {
    session_dir_ = dir;
    g_free(dir);
  } else if (error) {
    g_warning("Unable to create Review session directory: %s", error->message);
    g_error_free(error);
  }
}

Document::~Document() {
  if (pdf_) g_object_unref(pdf_);
  if (image_) g_object_unref(image_);
  if (!session_dir_.empty()) {
    std::error_code ignored;
    fs::remove_all(session_dir_, ignored);
  }
}

bool Document::open(const std::string& path, std::string& error,
                    const std::string& password) {
  const auto content_type = g_content_type_guess(path.c_str(), nullptr, 0, nullptr);
  const bool pdf = content_type && g_content_type_is_a(content_type, "application/pdf");
  g_free(const_cast<char*>(content_type));
  return pdf ? open_pdf(path, password, error) : open_image(path, error);
}

PopplerDocument* Document::load_pdf(const std::string& path, std::string& error,
                                    const std::string& password) const {
  GError* local_error = nullptr;
  const std::string uri = file_uri(path);
  PopplerDocument* doc = uri.empty() ? nullptr :
      poppler_document_new_from_file(uri.c_str(), password.empty() ? nullptr : password.c_str(),
                                     &local_error);
  if (!doc) set_error(error, local_error, "Unable to open PDF");
  return doc;
}

bool Document::open_pdf(const std::string& path, const std::string& password,
                        std::string& error) {
  PopplerDocument* doc = load_pdf(path, error, password);
  if (!doc) return false;
  if (pdf_) g_object_unref(pdf_);
  if (image_) { g_object_unref(image_); image_ = nullptr; }
  pdf_ = doc;
  kind_ = Kind::Pdf;
  path_ = fs::absolute(path).string();
  pages_.clear();

  const int count = poppler_document_get_n_pages(pdf_);
  for (int i = 0; i < count; ++i) {
    PopplerPage* page = poppler_document_get_page(pdf_, i);
    if (!page) continue;
    PageRef ref;
    ref.source_path = path_;
    ref.source_index = i;
    poppler_page_get_size(page, &ref.width_points, &ref.height_points);
    pages_.push_back(std::move(ref));
    g_object_unref(page);
  }

  PopplerPermissions permissions{};
  g_object_get(pdf_, "permissions", &permissions, nullptr);
  read_only_ = !password.empty() || !(permissions & POPPLER_PERMISSIONS_OK_TO_MODIFY) ||
               poppler_document_get_n_signatures(pdf_) > 0;
  dirty_ = destructive_changes_ = false;
  undo_.clear();
  redo_.clear();
  return true;
}

bool Document::open_image(const std::string& path, std::string& error) {
  GError* local_error = nullptr;
  GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &local_error);
  if (!pixbuf) {
    set_error(error, local_error, "Unsupported image file");
    return false;
  }
  if (pdf_) { g_object_unref(pdf_); pdf_ = nullptr; }
  if (image_) g_object_unref(image_);
  image_ = pixbuf;
  kind_ = Kind::Image;
  path_ = fs::absolute(path).string();
  PageRef ref;
  ref.source_path = path_;
  ref.source_index = 0;
  ref.width_points = gdk_pixbuf_get_width(image_);
  ref.height_points = gdk_pixbuf_get_height(image_);
  pages_ = {ref};
  read_only_ = false;
  dirty_ = destructive_changes_ = false;
  undo_.clear();
  redo_.clear();
  return true;
}

cairo_surface_t* Document::render_source(const PageRef& ref, double scale) const {
  if (!ref.generated_path.empty()) {
    cairo_surface_t* generated = cairo_image_surface_create_from_png(ref.generated_path.c_str());
    if (cairo_surface_status(generated) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(generated);
      return nullptr;
    }
    const int width = std::max(1, static_cast<int>(std::ceil(ref.width_points * scale)));
    const int height = std::max(1, static_cast<int>(std::ceil(ref.height_points * scale)));
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    const int source_width = cairo_image_surface_get_width(generated);
    const int source_height = cairo_image_surface_get_height(generated);
    cairo_scale(cr, static_cast<double>(width) / source_width,
                static_cast<double>(height) / source_height);
    cairo_set_source_surface(cr, generated, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(generated);
    return surface;
  }

  if (kind_ == Kind::Image && ref.source_path == path_ && image_) {
    const int width = std::max(1, static_cast<int>(std::ceil(ref.width_points * scale)));
    const int height = std::max(1, static_cast<int>(std::ceil(ref.height_points * scale)));
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    paint_pixbuf(cr, image_, 0, 0, width, height);
    cairo_destroy(cr);
    return surface;
  }

  std::string ignored;
  PopplerDocument* source = (pdf_ && ref.source_path == path_) ?
      static_cast<PopplerDocument*>(g_object_ref(pdf_)) : load_pdf(ref.source_path, ignored);
  if (!source) return nullptr;
  PopplerPage* page = poppler_document_get_page(source, ref.source_index);
  if (!page) { g_object_unref(source); return nullptr; }
  double width = 0, height = 0;
  poppler_page_get_size(page, &width, &height);
  auto* surface = cairo_image_surface_create(
      CAIRO_FORMAT_ARGB32,
      std::max(1, static_cast<int>(std::ceil(width * scale))),
      std::max(1, static_cast<int>(std::ceil(height * scale))));
  cairo_t* cr = cairo_create(surface);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_paint(cr);
  cairo_scale(cr, scale, scale);
  poppler_page_render(page, cr);
  cairo_destroy(cr);
  g_object_unref(page);
  g_object_unref(source);
  return surface;
}

cairo_surface_t* Document::render_page(std::size_t index, double scale,
                                       bool annotations) const {
  if (index >= pages_.size()) return nullptr;
  const PageRef& ref = pages_[index];
  cairo_surface_t* base = render_source(ref, scale);
  if (!base) return nullptr;

  if (ref.secure_crop.active) {
    const auto& crop = ref.secure_crop.rect;
    const int width = std::max(1, static_cast<int>(std::ceil(crop.width * scale)));
    const int height = std::max(1, static_cast<int>(std::ceil(crop.height * scale)));
    auto* cropped = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t* cr = cairo_create(cropped);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_set_source_surface(cr, base, -crop.x * scale, -crop.y * scale);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(base);
    base = cropped;
  }

  if (annotations && !ref.annotations.empty()) {
    cairo_t* cr = cairo_create(base);
    if (ref.secure_crop.active)
      cairo_translate(cr, -ref.secure_crop.rect.x * scale,
                      -ref.secure_crop.rect.y * scale);
    draw_annotations(cr, ref, scale);
    cairo_destroy(cr);
  }
  if (ref.rotation != 0) {
    const int source_width = cairo_image_surface_get_width(base);
    const int source_height = cairo_image_surface_get_height(base);
    const bool quarter_turn = ref.rotation == 90 || ref.rotation == 270;
    auto* rotated = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
        quarter_turn ? source_height : source_width,
        quarter_turn ? source_width : source_height);
    cairo_t* cr = cairo_create(rotated);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    if (ref.rotation == 90) {
      cairo_translate(cr, source_height, 0);
      cairo_rotate(cr, G_PI / 2);
    } else if (ref.rotation == 180) {
      cairo_translate(cr, source_width, source_height);
      cairo_rotate(cr, G_PI);
    } else if (ref.rotation == 270) {
      cairo_translate(cr, 0, source_width);
      cairo_rotate(cr, -G_PI / 2);
    }
    cairo_set_source_surface(cr, base, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(base);
    base = rotated;
  }
  return base;
}

void Document::draw_annotations(cairo_t* cr, const PageRef& ref, double scale) const {
  cairo_save(cr);
  cairo_scale(cr, scale, scale);
  for (const auto& a : ref.annotations) {
    cairo_save(cr);
    cairo_set_line_width(cr, a.stroke_width);
    cairo_set_source_rgba(cr, a.stroke.red, a.stroke.green, a.stroke.blue, a.stroke.alpha);
    const auto& b = a.bounds;
    switch (a.type) {
      case AnnotationType::Highlight:
        cairo_set_source_rgba(cr, a.fill.red, a.fill.green, a.fill.blue, a.fill.alpha);
        cairo_rectangle(cr, b.x, b.y, b.width, b.height);
        cairo_fill(cr);
        break;
      case AnnotationType::Rectangle:
        cairo_set_source_rgba(cr, a.fill.red, a.fill.green, a.fill.blue, a.fill.alpha);
        cairo_rectangle(cr, b.x, b.y, b.width, b.height);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, a.stroke.red, a.stroke.green, a.stroke.blue, a.stroke.alpha);
        cairo_stroke(cr);
        break;
      case AnnotationType::Ellipse:
        cairo_translate(cr, b.x + b.width / 2.0, b.y + b.height / 2.0);
        cairo_scale(cr, b.width / 2.0, b.height / 2.0);
        cairo_arc(cr, 0, 0, 1, 0, 2 * G_PI);
        cairo_restore(cr);
        cairo_save(cr);
        cairo_set_source_rgba(cr, a.fill.red, a.fill.green, a.fill.blue, a.fill.alpha);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, a.stroke.red, a.stroke.green, a.stroke.blue, a.stroke.alpha);
        cairo_set_line_width(cr, a.stroke_width);
        cairo_stroke(cr);
        break;
      case AnnotationType::Line:
      case AnnotationType::Arrow: {
        Point p1 = a.points.size() > 0 ? a.points[0] : Point{b.x, b.y};
        Point p2 = a.points.size() > 1 ? a.points[1] : Point{b.x + b.width, b.y + b.height};
        cairo_move_to(cr, p1.x, p1.y);
        cairo_line_to(cr, p2.x, p2.y);
        if (a.type == AnnotationType::Arrow) arrow_head(cr, p1, p2, a.stroke_width);
        cairo_stroke(cr);
        break;
      }
      case AnnotationType::Ink:
        if (!a.points.empty()) {
          cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
          cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
          cairo_move_to(cr, a.points.front().x, a.points.front().y);
          for (std::size_t i = 1; i < a.points.size(); ++i)
            cairo_line_to(cr, a.points[i].x, a.points[i].y);
          cairo_stroke(cr);
        }
        break;
      case AnnotationType::TextBox: {
        PangoLayout* layout = create_text_layout(cr, a, b.width);
        cairo_save(cr);
        cairo_translate(cr, b.x + 4.0, b.y + 4.0);
        cairo_set_source_rgba(cr, a.stroke.red, a.stroke.green, a.stroke.blue, a.stroke.alpha);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
        g_object_unref(layout);
        break;
      }
      case AnnotationType::Signature: {
        GError* error = nullptr;
        GdkPixbuf* signature = gdk_pixbuf_new_from_file(a.signature_path.c_str(), &error);
        if (signature) {
          paint_pixbuf(cr, signature, b.x, b.y, b.width, b.height);
          g_object_unref(signature);
        }
        if (error) g_error_free(error);
        break;
      }
    }
    cairo_restore(cr);
  }
  cairo_restore(cr);
}

std::string Document::page_text(std::size_t index) const {
  if (index >= pages_.size() || kind_ != Kind::Pdf) return {};
  const auto& ref = pages_[index];
  if (ref.rasterized || ref.secure_crop.active) return {};
  std::string ignored;
  PopplerDocument* source = (pdf_ && ref.source_path == path_) ?
      static_cast<PopplerDocument*>(g_object_ref(pdf_)) : load_pdf(ref.source_path, ignored);
  if (!source) return {};
  PopplerPage* page = poppler_document_get_page(source, ref.source_index);
  char* text = page ? poppler_page_get_text(page) : nullptr;
  std::string result = text ? text : "";
  g_free(text);
  if (page) g_object_unref(page);
  g_object_unref(source);
  return result;
}

void Document::checkpoint() {
  undo_.push_back({pages_, destructive_changes_});
  if (undo_.size() > 100) undo_.erase(undo_.begin());
  redo_.clear();
}

void Document::changed(bool destructive) {
  dirty_ = true;
  destructive_changes_ = destructive_changes_ || destructive;
}

bool Document::undo() {
  if (undo_.empty()) return false;
  redo_.push_back({pages_, destructive_changes_});
  pages_ = std::move(undo_.back().pages);
  destructive_changes_ = undo_.back().destructive;
  undo_.pop_back();
  dirty_ = true;
  return true;
}

bool Document::redo() {
  if (redo_.empty()) return false;
  undo_.push_back({pages_, destructive_changes_});
  pages_ = std::move(redo_.back().pages);
  destructive_changes_ = redo_.back().destructive;
  redo_.pop_back();
  dirty_ = true;
  return true;
}

void Document::reorder(const std::vector<std::size_t>& indices, std::size_t destination) {
  if (indices.empty()) return;
  checkpoint();
  auto selected = copy_pages(indices);
  std::set<std::size_t> wanted(indices.begin(), indices.end());
  std::vector<PageRef> remaining;
  for (std::size_t i = 0; i < pages_.size(); ++i)
    if (!wanted.contains(i)) remaining.push_back(pages_[i]);
  const auto removed_before = std::count_if(indices.begin(), indices.end(),
      [destination](std::size_t i) { return i < destination; });
  destination = std::min(destination - std::min(destination, static_cast<std::size_t>(removed_before)),
                         remaining.size());
  remaining.insert(remaining.begin() + destination, selected.begin(), selected.end());
  pages_ = std::move(remaining);
  changed();
}

void Document::remove(const std::vector<std::size_t>& indices) {
  if (indices.empty() || indices.size() >= pages_.size()) return;
  checkpoint();
  std::set<std::size_t> wanted(indices.begin(), indices.end());
  std::erase_if(pages_, [&, i = std::size_t{0}](const PageRef&) mutable {
    return wanted.contains(i++);
  });
  changed();
}

void Document::duplicate(const std::vector<std::size_t>& indices, std::size_t destination) {
  insert_pages(copy_pages(indices), destination);
}

void Document::insert_pages(const std::vector<PageRef>& pages, std::size_t destination) {
  if (pages.empty()) return;
  checkpoint();
  destination = std::min(destination, pages_.size());
  pages_.insert(pages_.begin() + destination, pages.begin(), pages.end());
  changed();
}

void Document::rotate(const std::vector<std::size_t>& indices, int degrees) {
  if (indices.empty()) return;
  checkpoint();
  for (auto index : indices) if (index < pages_.size()) {
    pages_[index].rotation = ((pages_[index].rotation + degrees) % 360 + 360) % 360;
    if (std::abs(degrees) % 180 == 90)
      std::swap(pages_[index].width_points, pages_[index].height_points);
  }
  changed();
}

void Document::add_annotation(std::size_t page_index, Annotation annotation) {
  if (page_index >= pages_.size()) return;
  checkpoint();
  annotation.id = next_annotation_id_++;
  if (annotation.type == AnnotationType::TextBox)
    annotation.bounds = fit_text_bounds(annotation);
  pages_[page_index].annotations.push_back(std::move(annotation));
  changed();
}

bool Document::update_annotation(std::size_t page_index, std::uint64_t id,
                                 const Annotation& annotation) {
  if (page_index >= pages_.size()) return false;
  auto& list = pages_[page_index].annotations;
  const auto found = std::find_if(list.begin(), list.end(),
                                  [id](const Annotation& a) { return a.id == id; });
  if (found == list.end()) return false;
  checkpoint();
  *found = annotation;
  found->id = id;
  if (found->type == AnnotationType::TextBox)
    found->bounds = fit_text_bounds(*found);
  changed();
  return true;
}

void Document::begin_annotation_edit() {
  if (annotation_edit_open_) return;
  checkpoint();
  annotation_edit_open_ = true;
}

void Document::commit_annotation_edit() {
  if (!annotation_edit_open_) return;
  annotation_edit_open_ = false;
  changed();
}

bool Document::remove_annotation(std::size_t page_index, std::uint64_t id) {
  if (page_index >= pages_.size()) return false;
  auto& list = pages_[page_index].annotations;
  const auto found = std::find_if(list.begin(), list.end(),
                                  [id](const Annotation& a) { return a.id == id; });
  if (found == list.end()) return false;
  checkpoint();
  list.erase(found);
  changed();
  return true;
}

void Document::secure_crop(const std::vector<std::size_t>& indices, Rect rect) {
  if (indices.empty() || rect.empty()) return;
  checkpoint();
  for (auto index : indices) if (index < pages_.size()) {
    auto& page = pages_[index];
    Rect bounded{
      std::clamp(rect.x, 0.0, page.width_points),
      std::clamp(rect.y, 0.0, page.height_points),
      std::min(rect.width, page.width_points - std::clamp(rect.x, 0.0, page.width_points)),
      std::min(rect.height, page.height_points - std::clamp(rect.y, 0.0, page.height_points)),
    };
    if (!bounded.empty()) {
      page.secure_crop = {true, bounded};
      page.width_points = bounded.width;
      page.height_points = bounded.height;
    }
  }
  changed(true);
}

void Document::rasterize_color(const std::vector<std::size_t>& indices, std::string& error) {
  if (indices.empty()) return;
  checkpoint();
  for (auto index : indices) if (index < pages_.size()) {
    auto& ref = pages_[index];
    cairo_surface_t* surface = render_page(index, kScanScale, true);
    if (!surface) { error = "Unable to render a selected page"; undo(); return; }
    const auto png = (session_dir_ / ("scan-" + std::to_string(index) + "-" +
                     std::to_string(g_get_monotonic_time()) + ".png")).string();
    const auto status = cairo_surface_write_to_png(surface, png.c_str());
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
      error = cairo_status_to_string(status);
      undo();
      return;
    }
    ref.generated_path = png;
    ref.rasterized = true;
    ref.secure_crop = {};
    ref.annotations.clear();
  }
  changed(true);
}

std::vector<PageRef> Document::copy_pages(const std::vector<std::size_t>& indices) const {
  std::vector<PageRef> result;
  for (auto index : indices) if (index < pages_.size()) result.push_back(pages_[index]);
  return result;
}

std::string Document::temporary_path(const std::string& near_path, const char* suffix) {
  const auto target = fs::absolute(near_path);
  return (target.parent_path() / ("." + target.filename().string() + "." +
      std::to_string(getpid()) + "." + std::to_string(g_get_monotonic_time()) + suffix)).string();
}

bool Document::create_image_pdf(const PageRef& ref, const std::string& destination,
                                bool include_annotations, std::string& error) const {
  const auto index = static_cast<std::size_t>(&ref - pages_.data());
  cairo_surface_t* image = render_page(index, kScanScale, include_annotations);
  if (!image) { error = "Unable to render page for secure output"; return false; }
  const double width = ref.secure_crop.active ? ref.secure_crop.rect.width : ref.width_points;
  const double height = ref.secure_crop.active ? ref.secure_crop.rect.height : ref.height_points;
  cairo_surface_t* pdf = cairo_pdf_surface_create(destination.c_str(), width, height);
  cairo_t* cr = cairo_create(pdf);
  cairo_scale(cr, 1.0 / kScanScale, 1.0 / kScanScale);
  cairo_set_source_surface(cr, image, 0, 0);
  cairo_paint(cr);
  cairo_show_page(cr);
  cairo_destroy(cr);
  cairo_surface_finish(pdf);
  const auto status = cairo_surface_status(pdf);
  cairo_surface_destroy(pdf);
  cairo_surface_destroy(image);
  if (status != CAIRO_STATUS_SUCCESS) {
    error = cairo_status_to_string(status);
    return false;
  }
  return true;
}

bool Document::assemble_pdf(const std::string& destination, bool flatten,
                            std::string& error) const {
  std::vector<std::string> generated;
  std::vector<std::pair<std::string, int>> sources;
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    const auto& page = pages_[i];
    if (flatten || page.rasterized || page.secure_crop.active || kind_ == Kind::Image) {
      const auto one_page = (session_dir_ / ("page-" + std::to_string(i) + "-" +
          std::to_string(g_get_monotonic_time()) + ".pdf")).string();
      if (!create_image_pdf(page, one_page, true, error)) return false;
      generated.push_back(one_page);
      sources.emplace_back(one_page, 1);
    } else {
      sources.emplace_back(page.source_path, page.source_index + 1);
    }
  }
  std::vector<std::string> args{"qpdf", "--empty", "--pages"};
  for (const auto& [source, page] : sources) {
    args.push_back(source);
    args.push_back(std::to_string(page));
  }
  args.push_back("--");
  args.push_back(destination);
  const bool ok = run_process(args, error);
  for (const auto& file : generated) g_unlink(file.c_str());
  return ok;
}

bool Document::write_pdf_annotations(const std::string& path, std::string& error) const {
  PopplerDocument* document = load_pdf(path, error);
  if (!document) return false;
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    const auto& ref = pages_[i];
    if (ref.rasterized || ref.secure_crop.active) continue;
    PopplerPage* page = poppler_document_get_page(document, static_cast<int>(i));
    if (!page) continue;
    for (const auto& original : ref.annotations) {
      Annotation prepared = original;
      if (prepared.type == AnnotationType::TextBox)
        prepared.bounds = fit_text_bounds(prepared);
      const auto& a = prepared;
      // Review's canvas is top-left based. PDF annotation rectangles and path
      // coordinates are bottom-left based.
      PopplerRectangle rect{a.bounds.x,
                            ref.height_points - (a.bounds.y + a.bounds.height),
                            a.bounds.x + a.bounds.width,
                            ref.height_points - a.bounds.y};
      PopplerAnnot* annotation = nullptr;
      bool custom_appearance = false;
      bool added_to_page = false;
      PopplerColor stroke_color{
        static_cast<guint16>(std::clamp(a.stroke.red, 0.0, 1.0) * 65535),
        static_cast<guint16>(std::clamp(a.stroke.green, 0.0, 1.0) * 65535),
        static_cast<guint16>(std::clamp(a.stroke.blue, 0.0, 1.0) * 65535)};
      PopplerColor fill_color{
        static_cast<guint16>(std::clamp(a.fill.red, 0.0, 1.0) * 65535),
        static_cast<guint16>(std::clamp(a.fill.green, 0.0, 1.0) * 65535),
        static_cast<guint16>(std::clamp(a.fill.blue, 0.0, 1.0) * 65535)};
      GArray* quads = nullptr;
      PopplerQuadrilateral quad{{rect.x1, rect.y1}, {rect.x2, rect.y1},
                                {rect.x1, rect.y2}, {rect.x2, rect.y2}};
      switch (a.type) {
        case AnnotationType::Highlight:
          quads = g_array_new(FALSE, FALSE, sizeof(PopplerQuadrilateral));
          g_array_append_val(quads, quad);
          annotation = poppler_annot_text_markup_new_highlight(document, &rect, quads);
          g_array_unref(quads);
          break;
        case AnnotationType::TextBox:
          // A temporary stamp lets Poppler generate a portable appearance
          // stream on Ubuntu 24.04. Before the transaction is committed it is
          // converted below to a genuine editable /FreeText annotation.
          annotation = poppler_annot_stamp_new(document, &rect);
          poppler_annot_set_contents(
              annotation, ("review-freetext-" + std::to_string(a.id)).c_str());
          poppler_page_add_annot(page, annotation);
          added_to_page = true;
          {
            const int width = std::max(1, static_cast<int>(std::ceil(a.bounds.width * 2)));
            const int height = std::max(1, static_cast<int>(std::ceil(a.bounds.height * 2)));
            cairo_surface_t* appearance =
                cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
            cairo_t* cr = cairo_create(appearance);
            cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR); cairo_paint(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            cairo_translate(cr, -a.bounds.x * 2, -a.bounds.y * 2);
            PageRef only; only.annotations = {a};
            draw_annotations(cr, only, 2.0);
            cairo_destroy(cr);
            GError* appearance_error = nullptr;
            if (!poppler_annot_stamp_set_custom_image(POPPLER_ANNOT_STAMP(annotation),
                                                       appearance, &appearance_error)) {
              cairo_surface_destroy(appearance);
              set_error(error, appearance_error, "Unable to create the text appearance stream");
              g_object_unref(annotation);
              g_object_unref(page);
              g_object_unref(document);
              return false;
            }
            cairo_surface_destroy(appearance);
          }
          custom_appearance = true;
          break;
        case AnnotationType::Rectangle:
          annotation = poppler_annot_square_new(document, &rect);
          poppler_annot_square_set_interior_color(POPPLER_ANNOT_SQUARE(annotation),
                                                  &fill_color);
          break;
        case AnnotationType::Ellipse:
          annotation = poppler_annot_circle_new(document, &rect);
          poppler_annot_circle_set_interior_color(POPPLER_ANNOT_CIRCLE(annotation),
                                                  &fill_color);
          break;
        case AnnotationType::Line:
        case AnnotationType::Arrow: {
          PopplerPoint start{a.points.size() > 0 ? a.points[0].x : rect.x1,
                             a.points.size() > 0 ? ref.height_points - a.points[0].y : rect.y1};
          PopplerPoint end{a.points.size() > 1 ? a.points[1].x : rect.x2,
                           a.points.size() > 1 ? ref.height_points - a.points[1].y : rect.y2};
          annotation = poppler_annot_line_new(document, &rect, &start, &end);
          break;
        }
        case AnnotationType::Ink: {
#ifdef HAVE_POPPLER_INK_API
          annotation = poppler_annot_ink_new(document, &rect);
          // Poppler needs the page association before converting path
          // coordinates into the annotation's local coordinate system.
          poppler_page_add_annot(page, annotation);
          added_to_page = true;
          std::vector<PopplerPoint> points;
          points.reserve(a.points.size());
          for (const auto& point : a.points)
            points.push_back({point.x, ref.height_points - point.y});
          if (points.size() < 2) {
            points = {{rect.x1, rect.y1}, {rect.x2, rect.y2}};
          }
          PopplerPath* path = poppler_path_new_from_array(points.data(), points.size());
          PopplerPath* paths[] = {path};
          poppler_annot_ink_set_ink_list(POPPLER_ANNOT_INK(annotation), paths, 1);
          break;
#else
          [[fallthrough]];
#endif
        }
        case AnnotationType::Signature: {
          annotation = poppler_annot_stamp_new(document, &rect);
          // Set geometry before installing the appearance. Mutating a stamp
          // after set_custom_image() invalidates /AP and makes PDF viewers
          // fall back to the default "Draft" stamp.
#ifdef HAVE_POPPLER_BORDER_WIDTH
          poppler_annot_set_border_width(annotation, 0.0);
#endif
          poppler_page_add_annot(page, annotation);
          added_to_page = true;
          const int width = std::max(1, static_cast<int>(std::ceil(a.bounds.width * 2)));
          const int height = std::max(1, static_cast<int>(std::ceil(a.bounds.height * 2)));
          cairo_surface_t* stamp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
          cairo_t* cr = cairo_create(stamp);
          cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR); cairo_paint(cr);
          cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
          cairo_translate(cr, -a.bounds.x * 2, -a.bounds.y * 2);
          PageRef only; only.annotations = {a};
          draw_annotations(cr, only, 2.0);
          cairo_destroy(cr);
          GError* stamp_error = nullptr;
          if (!poppler_annot_stamp_set_custom_image(POPPLER_ANNOT_STAMP(annotation), stamp,
                                                     &stamp_error)) {
            cairo_surface_destroy(stamp);
            set_error(error, stamp_error, "Unable to create the signature appearance stream");
            g_object_unref(annotation);
            g_object_unref(page);
            g_object_unref(document);
            return false;
          }
          custom_appearance = true;
          cairo_surface_destroy(stamp);
          break;
        }
      }
      if (annotation) {
        if (!custom_appearance) {
          const bool uses_fill_color = a.type == AnnotationType::Highlight;
          poppler_annot_set_color(annotation, uses_fill_color ? &fill_color : &stroke_color);
#ifdef HAVE_POPPLER_BORDER_WIDTH
          poppler_annot_set_border_width(annotation, a.stroke_width);
#endif
          if (POPPLER_IS_ANNOT_MARKUP(annotation)) {
            const bool geometry = a.type == AnnotationType::Rectangle ||
                                  a.type == AnnotationType::Ellipse ||
                                  a.type == AnnotationType::Highlight;
            poppler_annot_markup_set_opacity(POPPLER_ANNOT_MARKUP(annotation),
                                              geometry ? a.fill.alpha : a.stroke.alpha);
          }
        }
        if (!added_to_page) poppler_page_add_annot(page, annotation);
        g_object_unref(annotation);
      }
    }
    g_object_unref(page);
  }
  const auto annotated_path = temporary_path(path, ".annotations.pdf");
  const auto uri = file_uri(annotated_path);
  GError* save_error = nullptr;
  const gboolean ok = poppler_document_save(document, uri.c_str(), &save_error);
  g_object_unref(document);
  if (!ok) {
    set_error(error, save_error, "Unable to write PDF annotations");
    g_unlink(annotated_path.c_str());
    return false;
  }
  if (!convert_saved_free_text(annotated_path, pages_, error)) {
    g_unlink(annotated_path.c_str());
    return false;
  }
  return atomic_replace(annotated_path, path, error);
}

bool Document::atomic_replace(const std::string& temporary, const std::string& destination,
                              std::string& error) {
  std::error_code ec;
  fs::rename(temporary, destination, ec);
  if (ec) {
    error = "Unable to replace the destination: " + ec.message();
    g_unlink(temporary.c_str());
    return false;
  }
  return true;
}

bool Document::save(const std::string& destination, std::string& error) {
  if (read_only_) { error = "Signed or protected PDFs are view-only"; return false; }
  if (kind_ == Kind::Image) {
    cairo_surface_t* surface = render_page(0, 1.0, true);
    if (!surface) { error = "Unable to render image"; return false; }
    std::string extension = fs::path(destination).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    const char* format = (extension == ".jpg" || extension == ".jpeg") ? "jpeg" :
                         extension == ".webp" ? "webp" : "png";
    const auto temp = temporary_path(destination, ("." + std::string(format)).c_str());
    GdkPixbuf* pixbuf = pixbuf_from_surface(surface);
    GError* image_error = nullptr;
    const gboolean encoded = (std::strcmp(format, "png") == 0) ?
        gdk_pixbuf_save(pixbuf, temp.c_str(), format, &image_error, nullptr) :
        gdk_pixbuf_save(pixbuf, temp.c_str(), format, &image_error,
                        "quality", "95", nullptr);
    g_object_unref(pixbuf);
    cairo_surface_destroy(surface);
    if (!encoded) {
      set_error(error, image_error, "Unable to encode image");
      g_unlink(temp.c_str());
      return false;
    }
    if (!atomic_replace(temp, destination, error)) return false;
  } else {
    const auto temp = temporary_path(destination, ".pdf");
    if (!assemble_pdf(temp, false, error)) { g_unlink(temp.c_str()); return false; }
    if (!write_pdf_annotations(temp, error)) { g_unlink(temp.c_str()); return false; }
    std::string check_error;
    if (!run_process({"qpdf", "--check", temp}, check_error)) {
      error = "Generated PDF failed validation: " + check_error;
      g_unlink(temp.c_str());
      return false;
    }
    if (!atomic_replace(temp, destination, error)) return false;
  }
  path_ = fs::absolute(destination).string();
  dirty_ = false;
  destructive_changes_ = false;
  undo_.clear();
  redo_.clear();
  return true;
}

bool Document::export_flattened(const std::string& destination, std::string& error) const {
  const auto temp = temporary_path(destination, ".pdf");
  if (!assemble_pdf(temp, true, error)) { g_unlink(temp.c_str()); return false; }
  std::string check_error;
  if (!run_process({"qpdf", "--check", temp}, check_error)) {
    error = "Generated PDF failed validation: " + check_error;
    g_unlink(temp.c_str());
    return false;
  }
  return atomic_replace(temp, destination, error);
}

} // namespace review
