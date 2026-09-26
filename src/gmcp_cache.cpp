#include "mudclient/gmcp_cache.hpp"

namespace mudclient {

namespace {
std::vector<std::string> split_dotted(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t dot = path.find('.', start);
        parts.push_back(path.substr(start, dot - start));
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return parts;
}
} // namespace

void GmcpCache::update(const std::string& package, const nlohmann::json& payload) {
    nlohmann::json* node = &root_;
    for (const auto& part : split_dotted(package)) {
        if (!node->contains(part) || !(*node)[part].is_object()) {
            (*node)[part] = nlohmann::json::object();
        }
        node = &(*node)[part];
    }
    node->merge_patch(payload);
}

std::optional<nlohmann::json> GmcpCache::get(const std::string& dotted_path) const {
    const nlohmann::json* node = &root_;
    for (const auto& part : split_dotted(dotted_path)) {
        if (!node->is_object() || !node->contains(part)) {
            return std::nullopt;
        }
        node = &(*node)[part];
    }
    return *node;
}

} // namespace mudclient
