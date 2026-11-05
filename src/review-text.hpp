#pragma once

#include "review-types.hpp"

#include <glib.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace review {

inline TextStyle base_text_style(const Annotation& annotation) {
  return {annotation.bold, annotation.italic, annotation.underline,
          annotation.stroke, annotation.font_family, annotation.font_size};
}

inline bool text_style_equal(const TextStyle& left, const TextStyle& right) {
  return left.bold == right.bold && left.italic == right.italic &&
         left.underline == right.underline &&
         left.color.red == right.color.red &&
         left.color.green == right.color.green &&
         left.color.blue == right.color.blue &&
         left.color.alpha == right.color.alpha &&
         left.font_family == right.font_family &&
         left.font_size == right.font_size;
}

inline std::vector<TextSpan> effective_text_runs(const Annotation& annotation) {
  const glong length = g_utf8_strlen(annotation.text.c_str(), -1);
  if (length <= 0) return {};

  const auto text_length = static_cast<std::size_t>(length);
  std::vector<std::size_t> boundaries{0, text_length};
  for (const auto& span : annotation.text_spans) {
    const auto start = std::min(span.start, text_length);
    const auto end = std::min(span.end, text_length);
    if (start < end) {
      boundaries.push_back(start);
      boundaries.push_back(end);
    }
  }
  std::sort(boundaries.begin(), boundaries.end());
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

  const TextStyle base = base_text_style(annotation);
  std::vector<TextSpan> runs;
  for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
    const std::size_t start = boundaries[i];
    const std::size_t end = boundaries[i + 1];
    if (start >= end) continue;
    TextStyle style = base;
    for (const auto& span : annotation.text_spans) {
      const auto span_start = std::min(span.start, text_length);
      const auto span_end = std::min(span.end, text_length);
      if (span_start <= start && span_end >= end) style = span.style;
    }
    if (!runs.empty() && text_style_equal(runs.back().style, style)) {
      runs.back().end = end;
    } else {
      runs.push_back({start, end, style});
    }
  }
  return runs;
}

inline PangoAttrList* text_attributes(const Annotation& annotation,
                                      const std::vector<TextSpan>& runs) {
  PangoAttrList* attributes = pango_attr_list_new();
  const char* text = annotation.text.c_str();
  for (const auto& run : runs) {
    const char* start = g_utf8_offset_to_pointer(text, run.start);
    const char* end = g_utf8_offset_to_pointer(text, run.end);
    const guint start_byte = static_cast<guint>(start - text);
    const guint end_byte = static_cast<guint>(end - text);
    auto add = [&](PangoAttribute* attribute) {
      attribute->start_index = start_byte;
      attribute->end_index = end_byte;
      pango_attr_list_insert(attributes, attribute);
    };
    add(pango_attr_family_new(
        run.style.font_family.empty() ? "Sans" : run.style.font_family.c_str()));
    add(pango_attr_size_new(
        std::max(1, static_cast<int>(run.style.font_size * PANGO_SCALE))));
    add(pango_attr_weight_new(
        run.style.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL));
    add(pango_attr_style_new(
        run.style.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL));
    add(pango_attr_underline_new(
        run.style.underline ? PANGO_UNDERLINE_SINGLE : PANGO_UNDERLINE_NONE));
    add(pango_attr_foreground_new(
        static_cast<guint16>(std::clamp(run.style.color.red, 0.0, 1.0) * 65535),
        static_cast<guint16>(std::clamp(run.style.color.green, 0.0, 1.0) * 65535),
        static_cast<guint16>(std::clamp(run.style.color.blue, 0.0, 1.0) * 65535)));
  }
  return attributes;
}

inline PangoLayout* create_text_layout(cairo_t* cr, const Annotation& annotation,
                                       double width) {
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = pango_font_description_new();
  pango_font_description_set_family(
      font, annotation.font_family.empty() ? "Sans" : annotation.font_family.c_str());
  pango_font_description_set_size(
      font, std::max(1, static_cast<int>(annotation.font_size * PANGO_SCALE)));
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);

  const auto runs = effective_text_runs(annotation);
  PangoAttrList* attributes = text_attributes(annotation, runs);
  pango_layout_set_attributes(layout, attributes);
  pango_attr_list_unref(attributes);

  const int inner_width = std::max(1, static_cast<int>((width - 8.0) * PANGO_SCALE));
  pango_layout_set_width(layout, inner_width);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_alignment(
      layout, annotation.align == TextAlign::Center ? PANGO_ALIGN_CENTER :
              annotation.align == TextAlign::Right ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
  pango_layout_set_text(layout, annotation.text.c_str(), -1);
  return layout;
}

inline double measure_text_height(const Annotation& annotation, double width) {
  auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  auto* cr = cairo_create(surface);
  PangoLayout* layout = create_text_layout(cr, annotation, width);
  int layout_width = 0;
  int layout_height = 0;
  pango_layout_get_size(layout, &layout_width, &layout_height);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  return layout_height / static_cast<double>(PANGO_SCALE);
}

} // namespace review
