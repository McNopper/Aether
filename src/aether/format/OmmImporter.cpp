#include "aether/format/OmmImporter.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <slang-math/slang-math.hpp> // IWYU: keeps the translation unit's TU set consistent
#include <sstream>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <unordered_map>
#include <vector>

namespace aether {

namespace {

/// Parse a single hex character to its nibble value, or -1 if not hex.
[[nodiscard]] int hexNibble(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// Decode a hex string (byte-dump, byte 0 first) into bytes. Returns false on
/// an odd length or a non-hex character.
[[nodiscard]] bool hexToBytes(std::string_view hex, std::vector<std::byte>& out) {
    if ((hex.size() & 1U) != 0U) {
        return false;
    }
    out.reserve(out.size() + hex.size() / 2U);
    for (std::size_t i = 0; i < hex.size(); i += 2U) {
        const int hi = hexNibble(hex[i]);
        const int lo = hexNibble(hex[i + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<std::byte>((hi << 4) | lo));
    }
    return true;
}

/// Trim ASCII whitespace from both ends.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/// Parse a signed integer token; returns false on failure.
[[nodiscard]] bool parseInt(std::string_view tok, long& out) noexcept {
    const auto* first = tok.data();
    const auto* last = tok.data() + tok.size();
    return std::from_chars(first, last, out, 10).ec == std::errc{};
}

/// One parsed `.omm` group: ordered records, each either a special or a
/// (level, format, hex) record. Populated by `parseOmmText`, consumed by the
/// descriptor-driven packing pass.
struct OmmRecord {
    bool isSpecial = false;
    long special = 0;        // valid when isSpecial
    std::uint16_t level = 0; // valid when !isSpecial
    std::uint16_t format = 0;
    std::string hex;
};

struct OmmText {
    std::unordered_map<std::string, std::vector<OmmRecord>> groups;
};

/// Read and parse the `.omm` text sidecar into per-group ordered records.
/// Grammar: `# comment` / blank (ignored), `G <group>` (section start),
/// `T <level> <format> <hex>` (a record), `S <special>` (a uniform triangle).
[[nodiscard]] std::optional<OmmText> parseOmmText(const std::filesystem::path& ommPath) {
    std::ifstream in(ommPath);
    if (!in) {
        return std::nullopt;
    }
    OmmText out;
    std::string line;
    std::string current;
    bool haveCurrent = false;
    while (std::getline(in, line)) {
        const std::string_view sv = trim(line);
        if (sv.empty() || sv.front() == '#') {
            continue;
        }
        if (sv.starts_with("G ")) {
            current = std::string(trim(sv.substr(2)));
            out.groups[current];
            haveCurrent = true;
            continue;
        }
        if (!haveCurrent) {
            return std::nullopt; // record before any group
        }
        if (sv.starts_with("S ")) {
            long special = 0;
            if (!parseInt(trim(sv.substr(2)), special) || special > 0) {
                return std::nullopt;
            }
            out.groups[current].push_back(OmmRecord{true, special, 0, 0, ""});
        } else if (sv.starts_with("T ")) {
            std::istringstream ss{std::string(sv.substr(2))};
            long level = 0;
            long format = 0;
            std::string hex;
            if (!(ss >> level >> format >> hex)) {
                return std::nullopt;
            }
            if (level < 0 || level > 12 || (format != 1 && format != 2)) {
                return std::nullopt;
            }
            out.groups[current].push_back(
                OmmRecord{false, 0, static_cast<std::uint16_t>(level), static_cast<std::uint16_t>(format), hex});
        } else {
            return std::nullopt; // unknown record
        }
    }
    return out;
}

} // namespace

std::optional<OpacityMicromapData> OmmImporter::parse(const std::filesystem::path& micromapToml) {
    toml::table root;
    try {
        root = toml::parse_file(micromapToml.string());
    } catch (const toml::parse_error&) {
        return std::nullopt;
    }

    const auto dataFile = root["data"]["file"].value<std::string>();
    if (!dataFile) {
        return std::nullopt;
    }
    const std::filesystem::path ommPath = micromapToml.parent_path() / *dataFile;
    auto ommText = parseOmmText(ommPath);
    if (!ommText) {
        return std::nullopt;
    }

    OpacityMicromapData data{};
    data.defaultSubdivisionLevel = static_cast<std::uint16_t>(root["default_subdivision_level"].value_or(2));
    data.defaultFormat = static_cast<std::uint16_t>(root["default_format"].value_or(2));

    // Each [[group]] table selects a `G` section by name and packs its records.
    if (const toml::array* groups = root["group"].as_array()) {
        for (const auto& elem : *groups) {
            const toml::table* g = elem.as_table();
            if (g == nullptr) {
                continue;
            }
            OpacityMicromapGroup group{};
            group.name = (*g)["name"].value_or<std::string>("");
            group.sourceMaterial = (*g)["source_material"].value_or<std::string>("");
            group.triangleCount = static_cast<std::uint32_t>((*g)["triangle_count"].value_or<std::int64_t>(0));

            auto found = ommText->groups.find(group.name);
            if (found == ommText->groups.end()) {
                return std::nullopt; // descriptor names a group the .omm lacks
            }
            const auto& records = found->second;
            if (static_cast<std::uint32_t>(records.size()) != group.triangleCount) {
                return std::nullopt; // record count drifts from declared triangle_count
            }

            // Build a local histogram (level, format) -> count to cross-check
            // the descriptor's usage table.
            std::uint32_t recordCount = 0;
            for (const auto& rec : records) {
                if (rec.isSpecial) {
                    group.micromapIndices.push_back(static_cast<std::int32_t>(rec.special));
                    continue;
                }
                const std::uint32_t offset = static_cast<std::uint32_t>(group.dataBits.size());
                if (!hexToBytes(rec.hex, group.dataBits)) {
                    return std::nullopt; // malformed hex
                }
                group.triangles.push_back(OpacityMicromapTriangle{
                    .dataOffset = offset,
                    .subdivisionLevel = rec.level,
                    .format = rec.format,
                });
                group.micromapIndices.push_back(static_cast<std::int32_t>(group.triangles.size() - 1U));
                ++recordCount;
            }

            // Carry the descriptor's usage histogram verbatim (the consumer
            // needs it for vkGetMicromapBuildSizesEXT), and sanity-check its
            // total against the records parsed.
            std::uint32_t declaredRecords = 0;
            if (const toml::array* usage = (*g)["usage"].as_array()) {
                for (const auto& u : *usage) {
                    const toml::table* ut = u.as_table();
                    if (ut == nullptr) {
                        continue;
                    }
                    OpacityMicromapUsage us{};
                    us.count = static_cast<std::uint32_t>((*ut)["count"].value_or<std::int64_t>(0));
                    us.subdivisionLevel =
                        static_cast<std::uint16_t>((*ut)["subdivision_level"].value_or<std::int64_t>(0));
                    us.format = static_cast<std::uint16_t>((*ut)["format"].value_or<std::int64_t>(0));
                    declaredRecords += us.count;
                    group.usage.push_back(us);
                }
            }
            if (declaredRecords != recordCount) {
                return std::nullopt; // histogram total != parsed record count
            }

            data.groups.push_back(std::move(group));
        }
    }

    if (data.groups.empty()) {
        return std::nullopt;
    }
    return data;
}

} // namespace aether
