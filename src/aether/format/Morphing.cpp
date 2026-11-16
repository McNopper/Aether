#include "aether/format/Morphing.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether {
namespace {

/// Mesh/morph/weight contract shared by the entry points: one weight per
/// target and one delta row per vertex for every target (both mismatches are
/// bind errors, not deformations).
[[nodiscard]] bool
contractOk(std::size_t vertexCount, std::span<const MorphTarget> morphs, std::span<const float> weights) {
    if (morphs.size() != weights.size()) {
        return false;
    }
    for (const MorphTarget& morph : morphs) {
        if (morph.deltas.size() != vertexCount) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<MorphedVertices>
deformMorphs(const MeshData& mesh, std::span<const MorphTarget> morphs, std::span<const float> weights) {
    const std::size_t vertexCount = mesh.vertices.size();
    if (!contractOk(vertexCount, morphs, weights)) {
        return std::nullopt;
    }

    MorphedVertices out;
    out.positions.resize(vertexCount);
    for (std::size_t v = 0; v < vertexCount; ++v) {
        Vec3 position = mesh.vertices[v].position;
        for (std::size_t t = 0; t < morphs.size(); ++t) {
            if (weights[t] != 0.0F) {
                position = position + weights[t] * morphs[t].deltas[v];
            }
            // weight 0 contributes exactly nothing (the authored rest pose)
        }
        out.positions[v] = position;
    }
    return out;
}

bool validateMorphVertexCounts(const SceneDesc& desc,
                               const std::unordered_map<std::string, std::size_t>& vertexCounts,
                               std::string* error) {
    for (const MorphTarget& morph : desc.morphs) {
        const auto meshIt = vertexCounts.find(morph.meshName);
        if (meshIt == vertexCounts.end()) {
            if (error != nullptr) {
                *error = "morph '" + morph.meshName + "/" + morph.name + "' references mesh '" + morph.meshName +
                         "' which was not imported";
            }
            return false;
        }
        if (morph.deltas.size() != meshIt->second) {
            if (error != nullptr) {
                *error = "morph '" + morph.meshName + "/" + morph.name + "' covers " +
                         std::to_string(morph.deltas.size()) + " vertices but mesh '" + morph.meshName + "' has " +
                         std::to_string(meshIt->second);
            }
            return false;
        }
    }
    return true;
}

} // namespace aether
