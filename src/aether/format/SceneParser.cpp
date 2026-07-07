#include "aether/format/SceneParser.hpp"

#include <slang-math/slang-math.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

namespace aether {
namespace {

// ── TOML value readers ─────────────────────────────────────────────────────

[[nodiscard]] std::optional<Vec3> asVec3(const toml::node& n) {
    const toml::array* arr = n.as_array();
    if (arr == nullptr || arr->size() != 3) {
        return std::nullopt;
    }
    const auto x = (*arr)[0].value<double>();
    const auto y = (*arr)[1].value<double>();
    const auto z = (*arr)[2].value<double>();
    if (!x || !y || !z) {
        return std::nullopt;
    }
    return Vec3{static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)};
}

/// Read a quaternion stored on disk as [qx, qy, qz, qw] into a slang-math (x,y,z,w) quat.
[[nodiscard]] std::optional<Quat> asQuat(const toml::node& n) {
    const toml::array* arr = n.as_array();
    if (arr == nullptr || arr->size() != 4) {
        return std::nullopt;
    }
    const auto qx = (*arr)[0].value<double>();
    const auto qy = (*arr)[1].value<double>();
    const auto qz = (*arr)[2].value<double>();
    const auto qw = (*arr)[3].value<double>();
    if (!qx || !qy || !qz || !qw) {
        return std::nullopt;
    }
    return Quat{static_cast<float>(*qx), static_cast<float>(*qy), static_cast<float>(*qz), static_cast<float>(*qw)};
}

/// Resolve a `scale` value: a single number (uniform) or a 3-element array.
[[nodiscard]] std::optional<Vec3> asScale(const toml::node& n) {
    if (const auto s = n.value<double>()) {
        const float scale = static_cast<float>(*s);
        return Vec3{scale, scale, scale};
    }
    return asVec3(n);
}

// ── Section appliers ────────────────────────────────────────────────────────
// Each applies only the keys present in @p tbl, so a referenced base file can be
// applied first and an inline table applied on top to override individual keys.

void applyCamera(const toml::table& cam, SceneDesc& desc) {
    if (const auto* t = cam.get("translate")) {
        if (const auto p = asVec3(*t)) {
            desc.camera.position = *p;
        }
    }

    // Orientation: `look_at` takes precedence; otherwise derive from `rotate` /
    // `rotate_y` (camera default forward is -Z).
    std::optional<Quat> rotation;
    if (const auto* r = cam.get("rotate")) {
        rotation = asQuat(*r);
    }
    if (const auto deg = cam["rotate_y"].value<double>()) {
        rotation = sm::angleAxis(sm::radians(static_cast<float>(*deg)), Vec3{0.0F, 1.0F, 0.0F});
    }

    if (const auto* l = cam.get("look_at")) {
        if (const auto lookAt = asVec3(*l)) {
            desc.camera.lookAt = *lookAt;
            if (const auto* upNode = cam.get("up")) {
                desc.camera.up = asVec3(*upNode).value_or(Vec3{0.0F, 1.0F, 0.0F});
            } else if (!desc.camera.up) {
                desc.camera.up = Vec3{0.0F, 1.0F, 0.0F};
            }
        }
    } else if (rotation) {
        const Vec3 pos = desc.camera.position.value_or(Vec3{0.0F, 0.0F, 0.0F});
        desc.camera.lookAt = pos + (*rotation * Vec3{0.0F, 0.0F, -1.0F});
        desc.camera.up = *rotation * Vec3{0.0F, 1.0F, 0.0F};
    }

    if (const auto vfov = cam["vertical_field_of_view"].value<double>()) {
        desc.camera.vfov = static_cast<float>(*vfov);
    }
    if (const auto ev = cam["ev100"].value<double>()) {
        desc.camera.ev100 = static_cast<float>(*ev);
    }
}

void applyRender(const toml::table& render, SceneDesc& desc) {
    if (const auto v = render["samples_per_pixel"].value<int64_t>()) {
        desc.spp = static_cast<uint32_t>(*v);
    }
    if (const auto v = render["max_depth"].value<int64_t>()) {
        desc.maxDepth = static_cast<uint32_t>(*v);
    }
    if (const auto v = render["environment_unit_nits"].value<double>()) {
        desc.envUnitNits = static_cast<float>(*v);
    }
    if (const auto v = render["environment_map"].value<std::string>()) {
        desc.envMapFile = *v;
    }
    if (const auto v = render["working_color_space"].value<std::string>()) {
        desc.workingColorSpace = *v;
    }
}

void applyTonemap(const toml::table& tonemap, SceneDesc& desc) {
    if (const auto v = tonemap["tonemapper"].value<std::string>()) {
        desc.tonemapper = *v;
    }
}

void applyPostTonemap(const toml::table& postTonemap, SceneDesc& desc) {
    if (const auto v = postTonemap["renderer"].value<std::string>()) {
        desc.postTonemapRenderer = *v;
    }
}

using SectionFn = void (*)(const toml::table&, SceneDesc&);

/// Resolve a `[section]` that may carry an external `reference` (a base preset
/// file, applied first) plus inline keys that override it. References are
/// resolved relative to the scene file's directory.
void resolveSection(const toml::table& root,
                    std::string_view key,
                    const std::filesystem::path& baseDir,
                    SceneDesc& desc,
                    SectionFn apply) {
    const toml::table* tbl = root[key].as_table();
    if (tbl == nullptr) {
        return;
    }
    if (const auto ref = (*tbl)["reference"].value<std::string>()) {
        try {
            const toml::table ext = toml::parse_file((baseDir / *ref).string());
            apply(ext, desc);
        } catch (const toml::parse_error&) {
            // Missing / malformed reference: fall back to inline keys only.
        }
    }
    apply(*tbl, desc);
}

void parseGeometry(const toml::table& g, SceneDesc& desc) {
    const std::string type = g["type"].value_or<std::string>("");

    GeometryBlock blk{};
    if (type == "instance") {
        blk.kind = GeometryBlock::Kind::Object;
        blk.objPath = g["mesh"].value_or<std::string>("");
        if (const toml::table* mats = g["materials"].as_table()) {
            for (auto&& [name, mat] : *mats) {
                blk.groupMaterials.insert_or_assign(std::string{name.str()}, mat.value_or<std::string>(""));
            }
        }
    } else if (type == "sphere") {
        blk.kind = GeometryBlock::Kind::Sphere;
        blk.sphereRadius = static_cast<float>(g["radius"].value_or<double>(0.0));
    } else if (type == "box") {
        blk.kind = GeometryBlock::Kind::Box;
        if (const auto* h = g.get("half_extents")) {
            blk.boxHalf = asVec3(*h).value_or(Vec3{0.0F, 0.0F, 0.0F});
        }
    } else {
        return; // unknown geometry type — skip
    }

    // Shared transform + whole-object material.
    if (const auto* t = g.get("translate")) {
        blk.translation = asVec3(*t).value_or(Vec3{0.0F, 0.0F, 0.0F});
    }
    if (const auto* r = g.get("rotate")) {
        if (const auto q = asQuat(*r)) {
            blk.rotation = *q;
        }
    }
    if (const auto deg = g["rotate_y"].value<double>()) {
        blk.rotation = sm::angleAxis(sm::radians(static_cast<float>(*deg)), Vec3{0.0F, 1.0F, 0.0F});
    }
    if (const auto* s = g.get("scale")) {
        blk.scale = asScale(*s).value_or(Vec3{1.0F, 1.0F, 1.0F});
    }
    blk.materialName = g["material"].value_or<std::string>("");

    desc.geometry.push_back(std::move(blk));
}

} // namespace

std::optional<SceneDesc> SceneParser::parse(const std::filesystem::path& sceneFile) {
    toml::table root;
    try {
        root = toml::parse_file(sceneFile.string());
    } catch (const toml::parse_error&) {
        return std::nullopt;
    }

    SceneDesc desc{};
    const std::filesystem::path baseDir = sceneFile.parent_path();

    // ── Material libraries: `material_libraries` (string or array of strings) ──
    if (const auto* node = root.get("material_libraries")) {
        if (const auto single = node->value<std::string>()) {
            desc.mtllibs.emplace_back(*single);
        } else if (const toml::array* arr = node->as_array()) {
            for (const auto& elem : *arr) {
                if (const auto s = elem.value<std::string>()) {
                    desc.mtllibs.emplace_back(*s);
                }
            }
        }
    }

    // ── Settings sections (each supports `reference` + inline override) ──
    resolveSection(root, "render", baseDir, desc, applyRender);
    resolveSection(root, "camera", baseDir, desc, applyCamera);
    resolveSection(root, "tonemap", baseDir, desc, applyTonemap);
    resolveSection(root, "post_tonemap", baseDir, desc, applyPostTonemap);

    // ── Geometry (ordered) ──
    if (const toml::array* geo = root["geometry"].as_array()) {
        for (const auto& elem : *geo) {
            if (const toml::table* g = elem.as_table()) {
                parseGeometry(*g, desc);
            }
        }
    }

    return desc;
}

} // namespace aether
