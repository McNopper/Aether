#include "aether/format/ObjImporter.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace aether {
namespace {

struct ObjIndex {
    int m_position = 0;
    int m_texcoord = 0;
    int m_normal = 0;
};

struct PendingMesh {
    std::string m_name;
    std::vector<ObjIndex> m_indices;
};

// ── Vertex deduplication ──────────────────────────────────────────────────
// aether::Vertex is 8 tightly-packed floats (position, normal, uv) with no
// padding, so byte-wise hashing/comparison is well-defined.

struct VertexHash {
    [[nodiscard]] std::size_t operator()(const Vertex& v) const noexcept {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&v);
        std::uint64_t hash = 1469598103934665603ULL; // FNV-1a offset basis
        for (std::size_t i = 0; i < sizeof(Vertex); ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL; // FNV-1a prime
        }
        return static_cast<std::size_t>(hash);
    }
};

struct VertexEqual {
    [[nodiscard]] bool operator()(const Vertex& a, const Vertex& b) const noexcept {
        return std::memcmp(&a, &b, sizeof(Vertex)) == 0;
    }
};

// ── Text helpers ─────────────────────────────────────────────────────────

[[nodiscard]] std::string trim(std::string_view sv) {
    const auto first = sv.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    return std::string(sv.substr(first, sv.find_last_not_of(" \t\r\n") - first + 1));
}

[[nodiscard]] bool parseVec3(std::istringstream& s, Vec3& v) {
    return static_cast<bool>(s >> v.x >> v.y >> v.z);
}

[[nodiscard]] bool parseVec2(std::istringstream& s, Vec2& v) {
    return static_cast<bool>(s >> v.x >> v.y);
}

[[nodiscard]] int resolveIndex(int idx, std::size_t count) noexcept {
    if (idx > 0) {
        return idx - 1;
    }
    if (idx < 0) {
        return static_cast<int>(count) + idx;
    }
    return -1;
}

[[nodiscard]] ObjIndex parseFaceVertex(std::string_view token) {
    ObjIndex idx{};
    const std::size_t s0 = token.find('/');
    if (s0 == std::string_view::npos) {
        idx.m_position = std::stoi(std::string(token));
        return idx;
    }
    const std::size_t s1 = token.find('/', s0 + 1);
    idx.m_position = std::stoi(std::string(token.substr(0, s0)));
    if (s1 == std::string_view::npos) {
        if (s0 + 1 < token.size()) {
            idx.m_texcoord = std::stoi(std::string(token.substr(s0 + 1)));
        }
        return idx;
    }
    if (s1 > s0 + 1) {
        idx.m_texcoord = std::stoi(std::string(token.substr(s0 + 1, s1 - s0 - 1)));
    }
    if (s1 + 1 < token.size()) {
        idx.m_normal = std::stoi(std::string(token.substr(s1 + 1)));
    }
    return idx;
}

// ── Mesh flush ────────────────────────────────────────────────────────────

[[nodiscard]] bool flushMesh(PendingMesh& pending,
                             std::vector<MeshGroup>& out,
                             const std::vector<Vec3>& positions,
                             const std::vector<Vec3>& normals,
                             const std::vector<Vec2>& texcoords) {
    if (pending.m_indices.empty()) {
        return true;
    }

    std::unordered_map<Vertex, std::uint32_t, VertexHash, VertexEqual> unique;
    MeshData mesh;
    mesh.vertices.reserve(pending.m_indices.size());
    mesh.indices.reserve(pending.m_indices.size());

    for (const ObjIndex& oi : pending.m_indices) {
        const int p = resolveIndex(oi.m_position, positions.size());
        const int n = resolveIndex(oi.m_normal, normals.size());
        const int t = resolveIndex(oi.m_texcoord, texcoords.size());

        if (p < 0 || std::cmp_greater_equal(p, positions.size())) {
            return false;
        }

        const Vec3 pos = positions[static_cast<std::size_t>(p)];
        const Vec3 nrm = (n >= 0 && std::cmp_less(n, normals.size())) ? normals[static_cast<std::size_t>(n)]
                                                                      : Vec3(0.0F, 1.0F, 0.0F);
        const Vec2 uv =
            (t >= 0 && std::cmp_less(t, texcoords.size())) ? texcoords[static_cast<std::size_t>(t)] : Vec2(0.0F);

        const Vertex v{.position = pos, .normal = nrm, .uv = uv};

        const auto [it, inserted] = unique.emplace(v, static_cast<std::uint32_t>(mesh.vertices.size()));
        if (inserted) {
            mesh.vertices.push_back(v);
        }
        mesh.indices.push_back(it->second);
    }

    out.push_back(MeshGroup{.name = pending.m_name, .mesh = std::move(mesh)});
    pending.m_indices.clear();
    return true;
}

} // namespace

std::optional<std::vector<MeshGroup>> ObjImporter::parse(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return std::nullopt;
    }

    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> texcoords;
    std::vector<MeshGroup> groups;
    PendingMesh pending{.m_name = path.stem().string(), .m_indices = {}};

    std::string line;
    while (std::getline(file, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        std::istringstream ss(trimmed);
        std::string kw;
        ss >> kw;

        if (kw == "v") {
            Vec3 p{};
            if (parseVec3(ss, p)) {
                positions.push_back(p);
            }
        } else if (kw == "vn") {
            Vec3 n{};
            if (parseVec3(ss, n)) {
                normals.push_back(glm::normalize(n));
            }
        } else if (kw == "vt") {
            Vec2 uv{};
            if (parseVec2(ss, uv)) {
                texcoords.push_back(uv);
            }
        } else if (kw == "usemtl") {
            // OBJ material directives are ignored by design — materials are
            // assigned exclusively by the scene file. usemtl still acts as a
            // mesh boundary so multi-material OBJs split into sub-meshes.
            if (!flushMesh(pending, groups, positions, normals, texcoords)) {
                return std::nullopt;
            }
        } else if (kw == "o" || kw == "g") {
            if (!flushMesh(pending, groups, positions, normals, texcoords)) {
                return std::nullopt;
            }
            std::string name;
            std::getline(ss, name);
            pending.m_name = trim(name);
            if (pending.m_name.empty()) {
                pending.m_name = path.stem().string();
            }
        } else if (kw == "f") {
            // Only triangles are supported; non-triangle faces are skipped.
            std::string t0;
            std::string t1;
            std::string t2;
            std::string extra;
            if (!(ss >> t0 >> t1 >> t2)) {
                continue;
            }
            if (ss >> extra) {
                continue;
            }
            pending.m_indices.push_back(parseFaceVertex(t0));
            pending.m_indices.push_back(parseFaceVertex(t1));
            pending.m_indices.push_back(parseFaceVertex(t2));
        }
    }

    if (!flushMesh(pending, groups, positions, normals, texcoords)) {
        return std::nullopt;
    }

    return groups;
}

} // namespace aether
