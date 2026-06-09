#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

/// Math type aliases for the Aether file-format library.
///
/// Aether is renderer-agnostic: it depends only on GLM for vector / quaternion
/// math and carries **no** GPU or Vulkan types. These aliases give the parsed
/// data structures a stable vocabulary independent of GLM's own naming.
namespace aether {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using Quat = glm::quat; ///< GLM stores quaternions as (w, x, y, z)

} // namespace aether
