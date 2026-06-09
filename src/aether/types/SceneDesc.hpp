#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aether/types/Math.hpp"

namespace aether {

/// Camera parameters parsed from a `camera` block.
///
/// `rotate` / `rotate_y` and `look_at` are resolved by the parser into a
/// look-at target + up vector (last-one-wins), so consumers see a single
/// representation regardless of how the file expressed orientation.
struct CameraDesc {
    std::optional<Vec3> position;
    std::optional<Vec3> lookAt;
    std::optional<Vec3> up;
    std::optional<float> vfov;  ///< vertical field of view in degrees
    std::optional<float> ev100; ///< physical camera EV100 override
};

/// A single geometry block from a `.scene.toml` file.
///
/// Geometry is described in object/local space; world placement is the TRS
/// (glTF T × R × S convention). Material assignment is by *name* only —
/// resolution against a material library is the consumer's job.
struct GeometryBlock {
    enum class Kind : uint8_t {
        Object, ///< instantiate a Wavefront OBJ (geometry only)
        Sphere, ///< analytic sphere (radius; centre via translation)
        Box,    ///< procedural box (half-extents; placed via TRS)
    };

    Kind kind = Kind::Object;

    std::string objPath;       ///< Object: path to the OBJ (relative to scene dir)
    float sphereRadius = 0.0F; ///< Sphere: radius
    Vec3 boxHalf{0.0F};        ///< Box: half-extents

    std::string materialName; ///< usemtl override (whole-object material name)
    Vec3 translation{0.0F};
    Quat rotation{1.0F, 0.0F, 0.0F, 0.0F}; ///< GLM (w, x, y, z)
    Vec3 scale{1.0F};

    /// Per-group material overrides: OBJ group/object name → material name.
    std::unordered_map<std::string, std::string> groupMaterials;
};

/// Fully parsed `.scene.toml` file — pure CPU data, no GPU/renderer types.
///
/// References (mtllibs, OBJ paths, env map) are recorded as relative paths; the
/// consumer resolves and loads them. The tonemapper is carried as the raw
/// keyword token (e.g. "agx") — mapping it to a renderer tonemapping mode is a
/// Harmonia / renderer concern, not file-format data.
struct SceneDesc {
    CameraDesc camera;

    std::optional<uint32_t> spp;           ///< samples per pixel
    std::optional<uint32_t> maxDepth;      ///< maximum ray bounce depth
    std::optional<float> envUnitNits;      ///< cd/m² per unit EXR value
    std::optional<std::string> envMapFile; ///< equirect EXR IBL path (relative)
    std::optional<std::string> tonemapper; ///< raw `tonemapper` token (e.g. "agx")

    std::vector<std::string> mtllibs;    ///< referenced .materials.toml libraries (relative paths)
    std::vector<GeometryBlock> geometry; ///< geometry blocks in declaration order
};

} // namespace aether
