#pragma once

#include <mutex>
#include <string>
#include <string_view>

namespace printsphere::github {

struct SemVer {
  int major = 0;
  int minor = 0;
  int patch = 0;
  bool valid = false;
};

inline SemVer parse_semver(std::string_view text) {
  SemVer value;
  if (text.empty()) return value;
  if (text.front() == 'v' || text.front() == 'V') text.remove_prefix(1);
  int parts[3] = {};
  int count = 0;
  size_t i = 0;
  while (i < text.size() && count < 3) {
    if (text[i] < '0' || text[i] > '9') break;
    int n = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      n = n * 10 + (text[i] - '0');
      ++i;
    }
    parts[count++] = n;
    if (i < text.size() && text[i] == '.') {
      ++i;
      continue;
    }
    break;
  }
  if (count == 0) return value;
  value.major = parts[0];
  value.minor = parts[1];
  value.patch = parts[2];
  value.valid = true;
  return value;
}

inline int compare_semver(const SemVer& a, const SemVer& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return 0;
}

// Knomi/2.8C images carry the board token in the asset name. AMOLED uses the
// unsuffixed printsphere_ota*.bin from the Waveshare releases.
inline bool ota_asset_matches(std::string_view name, std::string_view board_token) {
  if (name.find("printsphere_ota") == std::string_view::npos) return false;
  if (board_token.empty()) {
    return name.find("knomi") == std::string_view::npos &&
           name.find("2.8c") == std::string_view::npos &&
           name.find("2_8c") == std::string_view::npos;
  }
  return name.find(board_token) != std::string_view::npos;
}

struct UpdateSnapshot {
  bool checked = false;
  bool checking = false;
  bool available = false;
  bool ok = false;
  std::string current;
  std::string latest;
  std::string url;
  std::string error;

  std::string to_json() const;
};

class Checker {
 public:
  static Checker& instance();
  void kick(bool force = false);
  UpdateSnapshot snapshot() const;

 private:
  Checker() = default;
  static void task(void* arg);
  void run_check();

  mutable std::mutex mutex_;
  UpdateSnapshot snapshot_{};
};

}  // namespace printsphere::github
