#include "anvil/config/paths.h"

#include <array>
#include <string_view>

namespace anvil::config {
namespace {

// Directories a reverse proxy commonly serves as static roots.
constexpr std::array<std::string_view, 6> kWebServedRoots{
    "/var/www", "/srv/www", "/srv/http", "/usr/share/nginx", "/usr/local/www", "/var/lib/nginx",
};

}  // namespace

bool path_is_within(std::string_view root, std::string_view path) noexcept {
    while (root.size() > 1 && root.back() == '/') { root.remove_suffix(1); }
    if (path.size() < root.size()) { return false; }
    if (path.compare(0, root.size(), root) != 0) { return false; }
    if (path.size() == root.size()) { return true; }
    // The component boundary. Without this, "/srv/storage-evil" passes against
    // "/srv/storage".
    return path[root.size()] == '/';
}

bool is_web_served_root(std::string_view path) noexcept {
    for (const std::string_view served : kWebServedRoots) {
        if (path_is_within(served, path)) { return true; }
    }
    return false;
}

}  // namespace anvil::config
