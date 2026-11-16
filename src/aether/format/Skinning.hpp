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

/// ANI4-DQS: per-joint DUAL QUATERNIONS - 8 floats per joint (real x,y,z,w,
/// then dual x,y,z,w), joint order, over the same `skinMatrices` the LBS path
/// consumes. SINGLE SOURCE OF TRUTH: the `deformSkinDqs` reference and the
/// renderer's per-tick staging both call this, and the GPU blends exactly
/// these values - the extraction NEVER runs on the GPU, so CPU/GPU parity is
/// by shared code, not by reimplementation.
///
/// Rigid contract (Kavan et al. 2008): each joint matrix is projected to its
/// orthonormal rotation (COLUMN NORMALIZATION - **scale/shear is dropped**,
/// which is what DQS means by rigid) + translation, then
/// `real = rotationQuat`, `dual = 0.5 * (T_quat * rotationQuat)` (Hamilton).
/// Returns std::nullopt on a degenerate matrix (zero-length basis column).
[[nodiscard]] std::optional<std::vector<float>> skinDualQuaternions(const std::vector<Mat4>& skinMatrices);

/// ANI4-DQS CPU reference - dual-quaternion skinning (the spec for the GPU
/// kernel's `useDqs` branch):
///
///   per vertex: renormalized weights (glTF, same as LBS); the first
///     NON-ZERO-weight influence anchors, later influences are SIGN-ALIGNED to
///     it (`dot(real_k, real_anchor) < 0` -> negate real AND dual; dq and -dq
///     are the same transform - without alignment near-360-degree joints blend
///     through zero);
///   sum over influences, then:
///     rLen = |sum real|;  r' = sumReal / rLen;
///     dual' = (sumDual - dot4(sumDual, r') * r') / rLen;   // dual unit constraint
///     t = 2 * vec(dual' * conj(r'));                        // translation
///   p' = rotate(r', p) + t;  normals/tangents rotate by r' (unit rotation).
///
/// Contract fallbacks mirror `deformSkinLbs`: mesh/skin/matrix violations ->
/// std::nullopt; wsum == 0 -> the authored vertex; a degenerate blend
/// (|r| <= 1e-8 or no usable influence) -> the authored vertex (never NaN).
/// Scale is DROPPED (see `skinDualQuaternions`).
[[nodiscard]] std::optional<DeformedVertices>
deformSkinDqs(const MeshData& mesh, const SkinDesc& skin, const std::vector<Mat4>& skinMatrices);

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
