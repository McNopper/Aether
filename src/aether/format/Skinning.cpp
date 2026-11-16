#include "aether/format/Skinning.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether {
namespace {

/// Per-vertex blend weights, glTF normalization: renormalize when the authored
/// sum is positive; a zero sum leaves the weights as authored (the caller's
/// wsum == 0 fallback skips the vertex entirely).
struct BlendWeights {
    float w[4]{};
    float sum = 0.0F;
};

BlendWeights blendWeights(const SkinDesc& skin, std::size_t vertex) {
    BlendWeights out{};
    for (std::size_t k = 0; k < 4; ++k) {
        out.w[k] = skin.boneWeights[vertex * 4 + k];
        out.sum += out.w[k];
    }
    if (out.sum > 0.0F) {
        const float inv = 1.0F / out.sum;
        for (float& w : out.w) {
            w *= inv;
        }
    }
    return out;
}

/// Mesh/skin/matrix contract: 4 entries per vertex for both weight arrays and
/// one matrix per joint (both mismatches are load/bind errors, not deformations).
bool contractOk(std::size_t vertexCount, const SkinDesc& skin, const std::vector<Mat4>& skinMatrices) {
    return skin.boneIndices.size() == 4 * vertexCount && skin.boneWeights.size() == 4 * vertexCount &&
           skinMatrices.size() == skin.joints.size();
}

} // namespace

std::optional<DeformedVertices>
deformSkinLbs(const MeshData& mesh, const SkinDesc& skin, const std::vector<Mat4>& skinMatrices) {
    const std::size_t vertexCount = mesh.vertices.size();
    if (!contractOk(vertexCount, skin, skinMatrices)) {
        return std::nullopt;
    }

    DeformedVertices out;
    out.positions.resize(vertexCount);
    out.normals.resize(vertexCount);
    for (std::size_t v = 0; v < vertexCount; ++v) {
        const BlendWeights bw = blendWeights(skin, v);
        const Vec3 p = mesh.vertices[v].position;
        const Vec3 n = mesh.vertices[v].normal;

        if (bw.sum <= 0.0F) {
            // glTF invalid-weights fallback: the vertex stays as authored.
            out.positions[v] = p;
            out.normals[v] = n;
            continue;
        }

        // Position: affine blend of the mapped points (homogeneous w = 1).
        Vec3 blendedP{0.0F, 0.0F, 0.0F};
        // Normal: direction blend through the SAME matrices (w = 0 - translation
        // must not leak into directions), renormalized (the rigid-blend
        // approximation; exact for rotation + uniform-scale blends).
        Vec3 blendedN{0.0F, 0.0F, 0.0F};
        for (std::size_t k = 0; k < 4; ++k) {
            const float w = bw.w[k];
            if (w == 0.0F) {
                continue;
            }
            const Mat4& m = skinMatrices[skin.boneIndices[v * 4 + k]];
            const Vec4 hp = m * Vec4{p.x, p.y, p.z, 1.0F};
            const Vec4 hn = m * Vec4{n.x, n.y, n.z, 0.0F};
            blendedP = blendedP + w * Vec3{hp.x, hp.y, hp.z};
            blendedN = blendedN + w * Vec3{hn.x, hn.y, hn.z};
        }
        out.positions[v] = blendedP;

        const float nLen = sm::length(blendedN);
        out.normals[v] = (nLen > 1.0e-8F) ? blendedN * (1.0F / nLen) : n;
    }
    return out;
}

bool validateSkinVertexCounts(const SceneDesc& desc,
                              const std::unordered_map<std::string, std::size_t>& vertexCounts,
                              std::string* error) {
    // Node id -> mesh name (the parser validated node ids; hand-built scenes
    // are checked defensively here).
    std::unordered_map<std::string, std::string> nodeMesh;
    nodeMesh.reserve(desc.nodes.size());
    for (const NodeDesc& node : desc.nodes) {
        nodeMesh.emplace(node.id, node.meshName);
    }

    for (const SkinDesc& skin : desc.skins) {
        const auto nodeIt = nodeMesh.find(skin.nodeId);
        if (nodeIt == nodeMesh.end()) {
            if (error != nullptr) {
                *error = "skin references unknown node '" + skin.nodeId + "'";
            }
            return false;
        }
        if (nodeIt->second.empty()) {
            if (error != nullptr) {
                *error = "skin node '" + skin.nodeId + "' has no mesh (a grouping node cannot be skinned)";
            }
            return false;
        }
        const auto meshIt = vertexCounts.find(nodeIt->second);
        if (meshIt == vertexCounts.end()) {
            if (error != nullptr) {
                *error =
                    "skin node '" + skin.nodeId + "' references mesh '" + nodeIt->second + "' which was not imported";
            }
            return false;
        }
        if (skin.boneIndices.size() != 4 * meshIt->second || skin.boneWeights.size() != 4 * meshIt->second) {
            if (error != nullptr) {
                *error = "skin '" + skin.nodeId + "' covers " + std::to_string(skin.boneIndices.size() / 4) +
                         " vertices but mesh '" + nodeIt->second + "' has " + std::to_string(meshIt->second);
            }
            return false;
        }
        const std::size_t jointCount = skin.joints.size();
        for (std::size_t i = 0; i < skin.boneIndices.size(); ++i) {
            if (skin.boneIndices[i] >= jointCount) {
                if (error != nullptr) {
                    *error = "skin '" + skin.nodeId + "' vertex " + std::to_string(i / 4) + " references joint " +
                             std::to_string(skin.boneIndices[i]) + " but only " + std::to_string(jointCount) +
                             " joints are declared";
                }
                return false;
            }
        }
    }
    return true;
}

} // namespace aether
