#pragma once

#include <cairo.h>
#include <string>
#include <vector>

namespace review {

struct SignatureInfo {
  std::string id;
  std::string name;
  std::string path;
};

class SignatureStore {
public:
  SignatureStore();
  [[nodiscard]] const std::vector<SignatureInfo>& signatures() const { return signatures_; }
  bool import_image(const std::string& source, const std::string& name,
                    unsigned white_threshold, std::string& error);
  bool save_surface(cairo_surface_t* surface, const std::string& name, std::string& error);
  bool remove(const std::string& id, std::string& error);
  void reload();

private:
  std::string directory_;
  std::vector<SignatureInfo> signatures_;
};

} // namespace review
