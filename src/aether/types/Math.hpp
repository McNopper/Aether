#pragma once
#include <slang-math/slang-math.hpp>

/// Math type aliases for the Aether file-format library.
///
/// Aether is renderer-agnostic: it depends only on slang-math for vector / quaternion
/// math and carries **no** GPU or Vulkan types. These aliases give the parsed
/// data structures a stable vocabulary independent of slang-math's own naming.
namespace aether {

using Vec2 = sm::float2;
using Vec3 = sm::float3;
using Vec4 = sm::float4;
using Quat = sm::quaternion; ///< stored as (x, y, z, w)

} // namespace aether
