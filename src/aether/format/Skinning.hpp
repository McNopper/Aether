#ifndef AETHER_FORMAT_SKINNING_HPP
#define AETHER_FORMAT_SKINNING_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aether/types/Math.hpp"
#include "aether/types/MeshData.hpp"
#include "aether/types/SceneDesc.hpp"

namespace aether {

/// ANI4 - deformed vertex data (the CPU evaluation reference's output): the
/// skinned positions/normals in the skinned mesh's OBJECT space. The renderers
/// refit their per-frame BLAS with these vertices; the instance transform and
/// the whole instance/TLAS pipeline stay exactly as authored.
struct DeformedVertices {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
};

/// ANI4 CPU reference - linear-blend skinning (LBS), the spec for the
/// renderers' GPU deformation:
///
///   wsum = Σ boneWeights[4v..4v+3];  ŵ = wsum > 0 ? w / wsum : w   (glTF rule)
///   p'   = Σ ŵ_i · (skinMatrices[i] · p)          (homogeneous, translation applied)
///   n'   = normalize(Σ ŵ_i · (skinMatrices[i] · n))  (homogeneous w = 0: directions)
///
/// A vertex with wsum == 0 is left at its authored position/normal (the glTF
/// "invalid weights" fallback - no contribution, no NaNs).
/// `skinMatrices` comes from `SceneAnimator::evaluateSkins` (LOCAL-space:
/// inverse(nodeWorld) * jointWorld * inverseBind, one per joint, joint order).
///
/// Returns std::nullopt when the mesh/skin/matrix contract is violated (per
/// vertex coverage != 4 * vertexCount, joint-count mismatch) - callers treat
/// that as a load/bind error, never as an empty deformation.
[[nodiscard]] std::optional<DeformedVertices>
deformSkinLbs(const MeshData& mesh, const SkinDesc& skin, const std::vector<Mat4>& skinMatrices);

/// ANI4 bind-time validation: every skin's boneIndices/boneWeights must cover
/// exactly the skinned node's mesh vertex count (4 entries per vertex, indices
/// inside the joint range). @p vertexCounts maps mesh name -> imported vertex
/// count (the scene loader supplies its imports; tests supply synthetic maps).
/// Returns false and names the offending skin/node in @p error.
[[nodiscard]] bool validateSkinVertexCounts(const SceneDesc& desc,
                                            const std::unordered_map<std::string, std::size_t>& vertexCounts,
                                            std::string* error);

} // namespace aether
#endif // AETHER_FORMAT_SKINNING_HPP
