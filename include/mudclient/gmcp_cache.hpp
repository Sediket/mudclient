#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

// Engine-thread-only cache of GMCP data, keyed by package name (dotted,
// e.g. "Char.Vitals"). Incoming messages merge into the cache tree with
// RFC 7396 merge_patch semantics (a JSON array in the patch replaces the
// existing array wholesale rather than merging element-wise, matching
// docs/SPEC.md section 4: "merge_patch semantics (arrays replace)").
namespace mudclient {

class GmcpCache {
public:
    // Merges `payload` into the cache at the dotted `package` path,
    // creating intermediate objects as needed.
    void update(const std::string& package, const nlohmann::json& payload);

    // Looks up a dotted path (e.g. "Char.Vitals.hp"). Returns a deep copy
    // of the value found, or std::nullopt if any segment of the path is
    // missing.
    std::optional<nlohmann::json> get(const std::string& dotted_path) const;

private:
    nlohmann::json root_ = nlohmann::json::object();
};

} // namespace mudclient
