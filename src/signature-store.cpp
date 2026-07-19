#include "signature-store.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace review {

SignatureStore::SignatureStore() {
  directory_ = (fs::path(g_get_user_data_dir()) / "review" / "signatures").string();
  g_mkdir_with_parents(directory_.c_str(), 0700);
  chmod(directory_.c_str(), 0700);
  reload();
}

void SignatureStore::reload() {
  signatures_.clear();
  std::error_code ec;
  if (!fs::exists(directory_, ec)) return;
  for (const auto& entry : fs::directory_iterator(directory_, ec)) {
    if (entry.path().extension() != ".png") continue;
    SignatureInfo info;
    info.id = entry.path().stem().string();
    info.name = info.id;
    info.path = entry.path().string();
    auto label = entry.path();
    label.replace_extension(".name");
    std::ifstream input(label);
    if (input) std::getline(input, info.name);
    signatures_.push_back(std::move(info));
  }
  std::sort(signatures_.begin(), signatures_.end(),
            [](const auto& a, const auto& b) { return a.name < b.name; });
}

bool SignatureStore::import_image(const std::string& source, const std::string& name,
                                  unsigned threshold, std::string& error) {
  GError* local_error = nullptr;
  GdkPixbuf* original = gdk_pixbuf_new_from_file(source.c_str(), &local_error);
  if (!original) {
    error = local_error ? local_error->message : "Unable to load signature image";
    if (local_error) g_error_free(local_error);
    return false;
  }
  GdkPixbuf* rgba = gdk_pixbuf_add_alpha(original, FALSE, 0, 0, 0);
  g_object_unref(original);
  auto* pixels = gdk_pixbuf_get_pixels(rgba);
  const int width = gdk_pixbuf_get_width(rgba);
  const int height = gdk_pixbuf_get_height(rgba);
  const int stride = gdk_pixbuf_get_rowstride(rgba);
  const int channels = gdk_pixbuf_get_n_channels(rgba);
  threshold = std::clamp(threshold, 1u, 255u);
  for (int y = 0; y < height; ++y) {
    auto* row = pixels + y * stride;
    for (int x = 0; x < width; ++x) {
      auto* p = row + x * channels;
      const auto minimum = std::min({p[0], p[1], p[2]});
      const auto darkness = static_cast<unsigned>(255 - minimum);
      p[3] = static_cast<guchar>(std::clamp(darkness * 255u / threshold, 0u, 255u));
      p[0] = p[1] = p[2] = 20;
    }
  }
  const std::string id = std::to_string(g_get_real_time());
  const auto target = (fs::path(directory_) / (id + ".png")).string();
  const gboolean ok = gdk_pixbuf_save(rgba, target.c_str(), "png", &local_error, nullptr);
  g_object_unref(rgba);
  if (!ok) {
    error = local_error ? local_error->message : "Unable to store signature";
    if (local_error) g_error_free(local_error);
    return false;
  }
  chmod(target.c_str(), 0600);
  std::ofstream label(fs::path(directory_) / (id + ".name"));
  label << (name.empty() ? "Signature" : name) << '\n';
  label.close();
  chmod((fs::path(directory_) / (id + ".name")).c_str(), 0600);
  reload();
  return true;
}

bool SignatureStore::save_surface(cairo_surface_t* surface, const std::string& name,
                                  std::string& error) {
  const std::string id = std::to_string(g_get_real_time());
  const auto target = (fs::path(directory_) / (id + ".png")).string();
  const auto status = cairo_surface_write_to_png(surface, target.c_str());
  if (status != CAIRO_STATUS_SUCCESS) {
    error = cairo_status_to_string(status);
    return false;
  }
  chmod(target.c_str(), 0600);
  std::ofstream label(fs::path(directory_) / (id + ".name"));
  label << (name.empty() ? "Signature" : name) << '\n';
  label.close();
  chmod((fs::path(directory_) / (id + ".name")).c_str(), 0600);
  reload();
  return true;
}

bool SignatureStore::remove(const std::string& id, std::string& error) {
  std::error_code ec;
  fs::remove(fs::path(directory_) / (id + ".png"), ec);
  fs::remove(fs::path(directory_) / (id + ".name"), ec);
  if (ec) {
    error = ec.message();
    return false;
  }
  reload();
  return true;
}

} // namespace review
