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
/// The camera's placement is a plain TRS transform — exactly the same
/// representation an instance uses (translate, rotate quaternion,
/// rotate_x/y/z Euler as shorthand, scale). SceneParser only records the raw
/// transform; deriving a forward/look-at direction or view matrix from
/// `rotation` is the consumer's job (mirrors how an instance's `rotation` isn't
/// interpreted here either). `scale` is parsed for structural parity with
/// instances but has no defined meaning for a camera — consumers should ignore
/// it.
struct CameraDesc {
    std::optional<Vec3> translation;
    std::optional<Quat> rotation; ///< slang-math order (x, y, z, w)
    std::optional<Vec3> scale;    ///< parsed for parity with instances; unused by cameras
    std::optional<float> vfov;    ///< vertical field of view in degrees
    std::optional<float> ev100;   ///< physical camera EV100 override
};

/// A declared mesh — pure geometry, carrying **no transform and no material**.
///
/// A mesh is described in object/local space. World placement is the job of one
/// or more `InstanceDesc` blocks that reference the mesh by name. Listing each
/// unique mesh once (like `material_libraries`) lets the consumer import / upload
/// / accelerate it a single time and instance it many times.
///
/// Material assignment lives on the instance, not the mesh: the same mesh can be
/// placed twice with different materials (e.g. a parameter sweep).
struct MeshDesc {
    enum class Kind : uint8_t {
        Object, ///< Wavefront OBJ (`objPath`)
        Sphere, ///< analytic / procedural sphere (`sphereRadius`)
        Box,    ///< procedural box (`boxHalf` half-extents)
    };

    Kind kind = Kind::Object;
    std::string name;        ///< unique name referenced by [[instance]] blocks
    std::string objPath;     ///< Object: OBJ path (relative to scene dir)
    float sphereRadius = 0.0F; ///< Sphere: radius
    Vec3 boxHalf{0.0F};      ///< Box: half-extents
};

/// A placed instance of a declared mesh.
///
/// `meshName` resolves against the `SceneDesc::meshes` list. The TRS places the
/// mesh (glTF T × R × S convention). `materialName` overrides the whole mesh's
/// material; `groupMaterials` overrides per OBJ group/object name (a multi-group
/// OBJ splits into sub-meshes, each assigned via its group name).
struct InstanceDesc {
    std::string meshName; ///< references a MeshDesc::name

    Vec3 translation{0.0F};
    Quat rotation{0.0F, 0.0F, 0.0F, 1.0F}; ///< identity, slang-math order (x, y, z, w)
    Vec3 scale{1.0F};

    std::string materialName; ///< whole-mesh material override (usemtl name)
    /// Per-group material overrides: OBJ group/object name → material name.
    std::unordered_map<std::string, std::string> groupMaterials;
};

/// Fully parsed `.scene.toml` file — pure CPU data, no GPU/renderer types.
///
/// References (mtllibs, OBJ paths, env map) are recorded as relative paths; the
/// consumer resolves and loads them. The tonemapper and post-tonemap renderer
/// are carried as raw keyword tokens — mapping them to renderer modes is a
/// Harmonia / renderer concern, not file-format data.
struct SceneDesc {
    CameraDesc camera;

    std::optional<uint32_t> spp;           ///< samples per pixel
    std::optional<uint32_t> maxDepth;      ///< maximum ray bounce depth
    std::optional<float> envUnitNits;      ///< cd/m² per unit EXR value
    std::optional<std::string> envMapFile; ///< equirect EXR IBL path (relative)
    std::optional<std::string> tonemapper; ///< raw `tonemapper` token (e.g. "agx")
    std::optional<std::string> postTonemapRenderer; ///< raw `renderer` token (e.g. "green_screen")
    /// Raw `working_color_space` token from [render]
    /// ("lin_rec2020_scene" | "lin_rec709_scene" — always linear).
    /// The scene-referred working color space all assets are converted into;
    /// absent means the consumer's default (rec2020).
    std::optional<std::string> workingColorSpace;

    std::vector<std::string> mtllibs;       ///< referenced .materials.toml libraries (relative paths)
    std::vector<MeshDesc> meshes;           ///< declared meshes, deduplicated by name
    std::vector<InstanceDesc> instances;    ///< placed instances, in declaration order
};

} // namespace aether
