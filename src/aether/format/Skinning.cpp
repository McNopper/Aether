#include "aether/format/Skinning.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
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

/// Rotate @p v by the (unit) quaternion @p q - the q * (0, v) * conj(q) form:
/// v + 2w(c x v) + 2c x (c x v) with c = q.xyz. MIRRORED EXACTLY by the
/// deform.comp.slang DQS branch (same op order on both sides); @p q is the
/// float[4] = {x, y, z, w} layout the DQ buffers use.
Vec3 rotateByQuaternion(const float* q, const Vec3& v) {
    const Vec3 c{q[0], q[1], q[2]};
    const Vec3 t = 2.0F * sm::cross(c, v);
    return v + q[3] * t + sm::cross(c, t);
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

std::optional<std::vector<float>> skinDualQuaternions(const std::vector<Mat4>& skinMatrices) {
    std::vector<float> out;
    out.reserve(skinMatrices.size() * 8);
    for (const Mat4& m : skinMatrices) {
        // Rigid projection: orthonormalize the 3x3 by COLUMN NORMALIZATION -
        // this is where DQS drops scale/shear (documented rigid contract; it is
        // also sm::toQuaternion's required input contract: orthonormal only).
        Vec3 col[3];
        for (std::size_t j = 0; j < 3; ++j) {
            const auto ji = static_cast<std::int32_t>(j);
            col[j] = Vec3{m[0][ji], m[1][ji], m[2][ji]};
            const float len = sm::length(col[j]);
            if (len <= 1.0e-20F) {
                return std::nullopt; // degenerate basis: bind error, not a pose
            }
            col[j] = col[j] * (1.0F / len);
        }
        const sm::float3x3 rotation{
            {col[0].x, col[1].x, col[2].x}, {col[0].y, col[1].y, col[2].y}, {col[0].z, col[1].z, col[2].z}};
        const sm::quaternion r = sm::toQuaternion(rotation);
        const Vec3 t{m[0][3], m[1][3], m[2][3]};

        // dual = 0.5 * T_quat * r (Hamilton, T_quat = (x, y, z, w=0)) -
        // componentwise write mirrors the shader's buffer layout.
        const sm::quaternion tQuat{t.x, t.y, t.z, 0.0F};
        const sm::quaternion dual = tQuat * r;
        out.push_back(r.x);
        out.push_back(r.y);
        out.push_back(r.z);
        out.push_back(r.w);
        out.push_back(0.5F * dual.x);
        out.push_back(0.5F * dual.y);
        out.push_back(0.5F * dual.z);
        out.push_back(0.5F * dual.w);
    }
    return out;
}

std::optional<DeformedVertices>
deformSkinDqs(const MeshData& mesh, const SkinDesc& skin, const std::vector<Mat4>& skinMatrices) {
    const std::size_t vertexCount = mesh.vertices.size();
    if (!contractOk(vertexCount, skin, skinMatrices)) {
        return std::nullopt;
    }
    const auto dq = skinDualQuaternions(skinMatrices);
    if (!dq) {
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
            out.positions[v] = p; // glTF invalid-weights fallback, as LBS
            out.normals[v] = n;
            continue;
        }

        // Gather: the first non-zero-weight influence anchors; later
        // influences are sign-aligned to it (dq and -dq encode the same
        // transform - alignment picks the nearest hemisphere, so joints
        // separated by more than 180 degrees blend into a valid rotation
        // instead of cancelling toward zero).
        float r[4]{};
        float d[4]{};
        float anchor[4]{};
        bool haveAnchor = false;
        for (std::size_t k = 0; k < 4; ++k) {
            const float w = bw.w[k];
            if (w == 0.0F) {
                continue;
            }
            const float* joint = dq->data() + static_cast<std::size_t>(skin.boneIndices[v * 4 + k]) * 8;
            float sign = 1.0F;
            if (!haveAnchor) {
                for (std::size_t c = 0; c < 4; ++c) {
                    anchor[c] = joint[c];
                }
                haveAnchor = true;
            } else {
                float dotReal = 0.0F;
                for (std::size_t c = 0; c < 4; ++c) {
                    dotReal += joint[c] * anchor[c];
                }
                if (dotReal < 0.0F) {
                    sign = -1.0F;
                }
            }
            for (std::size_t c = 0; c < 4; ++c) {
                r[c] += w * sign * joint[c];
                d[c] += w * sign * joint[4 + c];
            }
        }

        float rLen = 0.0F;
        for (float component : r) {
            rLen += component * component;
        }
        rLen = std::sqrt(rLen);
        if (!haveAnchor || rLen <= 1.0e-8F) {
            out.positions[v] = p; // degenerate blend: authored vertex, never NaN
            out.normals[v] = n;
            continue;
        }

        // Normalized dual quaternion: real unit, dual projected (dual unit
        // constraint r . d = 0), both scaled by 1/|r| (Kavan's constraints).
        float real[4];
        float dual[4];
        for (std::size_t c = 0; c < 4; ++c) {
            real[c] = r[c] / rLen;
        }
        float projection = 0.0F;
        for (std::size_t c = 0; c < 4; ++c) {
            projection += d[c] * real[c];
        }
        for (std::size_t c = 0; c < 4; ++c) {
            dual[c] = (d[c] - projection * real[c]) / rLen;
        }

        // t = 2 * vec(dual * conj(real)) (Hamilton via the shared operator).
        const sm::quaternion realQ{real[0], real[1], real[2], real[3]};
        const sm::quaternion dualQ{dual[0], dual[1], dual[2], dual[3]};
        const sm::quaternion translation = dualQ * sm::conjugate(realQ);
        const Vec3 t{2.0F * translation.x, 2.0F * translation.y, 2.0F * translation.z};

        const Vec3 blendedP = rotateByQuaternion(real, p) + t;
        const Vec3 blendedN = rotateByQuaternion(real, n);
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
