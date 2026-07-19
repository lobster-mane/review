#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace review {

struct Point {
  double x{};
  double y{};
};

struct Rect {
  double x{};
  double y{};
  double width{};
  double height{};

  [[nodiscard]] bool empty() const { return width <= 0.0 || height <= 0.0; }
};

struct Color {
  double red{0.94};
  double green{0.18};
  double blue{0.18};
  double alpha{1.0};
};

enum class AnnotationType {
  Highlight,
  TextBox,
  Ink,
  Rectangle,
  Ellipse,
  Line,
  Arrow,
  Signature,
};

struct Annotation {
  std::uint64_t id{};
  AnnotationType type{AnnotationType::Rectangle};
  Rect bounds{};                  // PDF points in unrotated page coordinates
  std::vector<Point> points{};    // ink path or line endpoints
  std::string text{};
  std::string signature_path{};
  Color stroke{};
  Color fill{1.0, 0.92, 0.23, 0.35};
  double stroke_width{2.0};
  std::string font_family{"Sans"};
  double font_size{12.0};
};

struct Crop {
  bool active{false};
  Rect rect{};
};

struct PageRef {
  std::string source_path{};
  int source_index{};
  int rotation{};
  bool rasterized{false};
  std::string generated_path{};
  Crop secure_crop{};
  double width_points{};
  double height_points{};
  std::vector<Annotation> annotations{};
};

} // namespace review
