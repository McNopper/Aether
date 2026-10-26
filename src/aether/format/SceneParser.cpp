#include "aether/format/SceneParser.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <slang-math/slang-math.hpp>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

#include "aether/format/NodeGraph.hpp"
#include "aether/format/SceneAnimator.hpp"
#include "aether/format/toml_readers.hpp"

namespace aether {
namespace {

// ── TOML value readers ─────────────────────────────────────────────────────

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
        const auto scale = static_cast<float>(*s);
        return Vec3{scale, scale, scale};
    }
    return asVec3(n);
}

/// Parse Euler rotation (rotate_x, rotate_y, rotate_z in degrees) from a TOML table.
/// Composed in XYZ order: q = qz * qy * qx. Returns nullopt if no Euler angles present.
[[nodiscard]] std::optional<Quat> parseEulerRotation(const toml::table& tbl) {
    const auto rxDeg = tbl["rotate_x"].value<double>();
    const auto ryDeg = tbl["rotate_y"].value<double>();
    const auto rzDeg = tbl["rotate_z"].value<double>();
    if (!rxDeg && !ryDeg && !rzDeg) {
        return std::nullopt;
    }
    const auto qx = sm::angleAxis(sm::radians(static_cast<float>(rxDeg.value_or(0.0))), Vec3{1.0F, 0.0F, 0.0F});
    const auto qy = sm::angleAxis(sm::radians(static_cast<float>(ryDeg.value_or(0.0))), Vec3{0.0F, 1.0F, 0.0F});
    const auto qz = sm::angleAxis(sm::radians(static_cast<float>(rzDeg.value_or(0.0))), Vec3{0.0F, 0.0F, 1.0F});
    return qz * qy * qx;
}

// ── Section appliers ────────────────────────────────────────────────────────
// Each applies only the keys present in @p tbl, so a referenced base file can be
// applied first and an inline table applied on top to override individual keys.

/// Parse the shared TRS keys (translate, rotate/[Euler], scale) from @p tbl.
/// Used identically by geometry blocks and the camera block — a camera's
/// placement is a plain transform, exactly like a mesh instance's; only the
/// keys actually present in @p tbl are written.
void parseTRS(const toml::table& tbl,
              std::optional<Vec3>& translation,
              std::optional<Quat>& rotation,
              std::optional<Vec3>& scale) {
    if (const auto* t = tbl.get("translate")) {
        if (const auto p = asVec3(*t)) {
            translation = *p;
        }
    }
    if (const auto* r = tbl.get("rotate")) {
        if (const auto q = asQuat(*r)) {
            rotation = *q;
        }
    } else if (const auto euler = parseEulerRotation(tbl)) {
        rotation = *euler;
    }
    if (const auto* s = tbl.get("scale")) {
        scale = asScale(*s).value_or(Vec3{1.0F, 1.0F, 1.0F});
    }
}

void applyCamera(const toml::table& cam, SceneDesc& desc) {
    parseTRS(cam, desc.camera.translation, desc.camera.rotation, desc.camera.scale);

    if (const auto vfov = cam["vertical_field_of_view"].value<double>()) {
        desc.camera.vfov = static_cast<float>(*vfov);
    }
    if (const auto ev = cam["ev100"].value<double>()) {
        desc.camera.ev100 = static_cast<float>(*ev);
    }
}

void applyRender(const toml::table& render, SceneDesc& desc) {
    if (const auto v = render["samples_per_pixel"].value<std::int64_t>()) {
        desc.spp = static_cast<std::uint32_t>(*v);
    }
    if (const auto v = render["max_depth"].value<std::int64_t>()) {
        desc.maxDepth = static_cast<std::uint32_t>(*v);
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
        } catch (const toml::parse_error&) { // NOLINT(bugprone-empty-catch)
            // Missing / malformed reference: fall back to inline keys only.
        }
    }
    apply(*tbl, desc);
}

/// Parse the optional [meta] block: AI context for agents, never affects rendering.
/// Keys that are absent or of the wrong TOML type silently keep their defaults -
/// the block is pure context, so it can never cause a parse error. Reads go
/// through node_view (`operator[]`), which is null-safe for absent keys.
void parseMeta(const toml::table& root, SceneDesc& desc) {
    const toml::table* meta = root["meta"].as_table();
    if (meta == nullptr) {
        return;
    }
    SceneMeta m;
    if (const auto v = (*meta)["schema"].value<std::string>()) {
        m.schema = *v;
    }
    if (const auto v = (*meta)["version"].value<std::int64_t>()) {
        m.version = static_cast<std::uint32_t>(*v);
    }
    if (const auto v = (*meta)["model"].value<std::string>()) {
        m.model = *v;
    }
    if (const auto v = (*meta)["title"].value<std::string>()) {
        m.title = *v;
    }
    if (const auto v = (*meta)["description"].value<std::string>()) {
        m.description = *v;
    }
    if (const auto v = (*meta)["units"].value<std::string>()) {
        m.units = *v;
    }
    if (const auto v = (*meta)["generator"].value<std::string>()) {
        m.generator = *v;
    }
    if (const auto v = (*meta)["assembly"].value<std::string>()) {
        m.assembly = *v;
    }
    desc.meta = std::move(m);
}

/// Parse [meta.link."<path>"] tables: per-linked-file AI summaries.
void parseMetaLinks(const toml::table& root, SceneDesc& desc) {
    const toml::table* meta = root["meta"].as_table();
    if (meta == nullptr) {
        return;
    }
    for (auto&& [key, node] : *meta) {
        if (!key.str().starts_with("link.")) {
            continue;
        }
        const toml::table* linkTbl = node.as_table();
        if (linkTbl == nullptr) {
            continue;
        }
        const std::string path{key.str().substr(5)};
        LinkMeta lm;
        // null-safe node_view reads: linkTbl->get(...)->value(...) dereferences
        // nullptr for absent keys (the pre-existing crash the META2 pass fixed in
        // parseMeta; this twin got the same treatment).
        if (const auto v = (*linkTbl)["kind"].value<std::string>()) {
            lm.kind = *v;
        }
        if (const auto v = (*linkTbl)["description"].value<std::string>()) {
            lm.description = *v;
        }
        if (const auto v = (*linkTbl)["triangles"].value<std::int64_t>()) {
            lm.triangles = static_cast<std::uint32_t>(*v);
        }
        if (const toml::array* mats = (*linkTbl)["materials"].as_array()) {
            for (auto&& mat : *mats) {
                if (const auto m = mat.value<std::string>()) {
                    lm.materials.push_back(*m);
                }
            }
        }
        if (const toml::table* bounds = (*linkTbl)["bounds"].as_table()) {
            const auto mn = bounds->get("min");
            const auto mx = bounds->get("max");
            const auto mnVec = mn ? asVec3(*mn) : std::nullopt;
            const auto mxVec = mx ? asVec3(*mx) : std::nullopt;
            if (mnVec && mxVec) {
                lm.bounds = Aabb{*mnVec, *mxVec};
            }
        }
        desc.links[path] = std::move(lm);
    }
}

// ── Geometry-block helpers ──────────────────────────────────────────────────
// [[instance]] and [[node]] blocks share their placement and material keys;
// both read them through the helpers below so the semantics stay identical.

/// A fully defaulted TRS placement (identity where a key is absent) — exactly
/// what a flat instance carries.
struct Trs {
    Vec3 translation{0.0F, 0.0F, 0.0F};
    Quat rotation{0.0F, 0.0F, 0.0F, 1.0F}; ///< slang-math order (x, y, z, w)
    Vec3 scale{1.0F, 1.0F, 1.0F};
};

/// Read the shared TRS keys (translate, rotate/[Euler], scale) from @p tbl via
/// parseTRS, defaulting to identity placement.
[[nodiscard]] Trs readTrs(const toml::table& tbl) {
    std::optional<Vec3> translation;
    std::optional<Quat> rotation;
    std::optional<Vec3> scale;
    parseTRS(tbl, translation, rotation, scale);
    return {translation.value_or(Vec3{0.0F, 0.0F, 0.0F}),
            rotation.value_or(Quat{0.0F, 0.0F, 0.0F, 1.0F}),
            scale.value_or(Vec3{1.0F, 1.0F, 1.0F})};
}

/// Read the shared material bindings from @p tbl: a whole-mesh `material`
/// override plus the per-OBJ-group `materials` map.
void readMaterials(const toml::table& tbl,
                   std::string& materialName,
                   std::unordered_map<std::string, std::string>& groupMaterials) {
    if (const toml::table* mats = tbl["materials"].as_table()) {
        for (auto&& [name, mat] : *mats) {
            groupMaterials.insert_or_assign(std::string{name.str()}, mat.value_or<std::string>(""));
        }
    }
    materialName = tbl["material"].value_or<std::string>("");
}

void parseMesh(const toml::table& m, SceneDesc& desc) {
    MeshDesc mesh{};
    mesh.name = m["name"].value_or<std::string>("");
    if (mesh.name.empty()) {
        return; // a mesh must be named so instances can reference it
    }

    const std::string type = m["type"].value_or<std::string>("");
    if (type == "box") {
        mesh.kind = MeshDesc::Kind::Box;
        if (const auto* h = m.get("half_extents")) {
            mesh.boxHalf = asVec3(*h).value_or(Vec3{0.0F, 0.0F, 0.0F});
        }
    } else if (type == "sphere") {
        mesh.kind = MeshDesc::Kind::Sphere;
        mesh.sphereRadius = static_cast<float>(m["radius"].value_or<double>(0.0));
    } else {
        // Default: a Wavefront OBJ. `type` may be omitted (just `path`) or
        // explicitly "object"/"obj".
        mesh.kind = MeshDesc::Kind::Object;
        mesh.objPath = m["path"].value_or<std::string>("");
    }

    // Optional object-space AABB (inline table `bounds = { min=[...], max=[...] }`
    // or a `[mesh.bounds]` sub-table). When absent the renderer derives it.
    if (const toml::table* b = m["bounds"].as_table()) {
        const auto* mn = b->get("min");
        const auto* mx = b->get("max");
        const auto mnVec = mn ? asVec3(*mn) : std::nullopt;
        const auto mxVec = mx ? asVec3(*mx) : std::nullopt;
        if (mnVec && mxVec) {
            mesh.bounds = Aabb{*mnVec, *mxVec};
        }
    }

    // Optional per-group opacity-micromap references: `opacity_micromaps =
    // { "<group>" = "<file>.micromap.toml", ... }` (group name → path, relative
    // to the scene dir). Mirrors the instance `materials` map.
    if (const toml::table* omm = m["opacity_micromaps"].as_table()) {
        for (auto&& [name, path] : *omm) {
            mesh.opacityMicromaps.insert_or_assign(std::string{name.str()}, path.value_or<std::string>(""));
        }
    }

    desc.meshes.push_back(std::move(mesh));
}

void parseInstance(const toml::table& t, SceneDesc& desc) {
    InstanceDesc inst{};
    inst.meshName = t["mesh"].value_or<std::string>("");
    if (inst.meshName.empty()) {
        return; // an instance must reference a declared mesh
    }

    // Material assignment — either a single whole-mesh override or a per-group map.
    readMaterials(t, inst.materialName, inst.groupMaterials);

    // Placement TRS (same shared keys / semantics as the camera block).
    const Trs trs = readTrs(t);
    inst.translation = trs.translation;
    inst.rotation = trs.rotation;
    inst.scale = trs.scale;

    desc.instances.push_back(std::move(inst));
}

/// Parse one [[node]] entry: a named, optionally parented TRS placement with
/// optional mesh + material bindings (all keys exactly the instance ones).
/// An entry without `id` is skipped, like an unnamed mesh — it could never be
/// addressed by a child `parent` ref.
void parseNode(const toml::table& t, SceneDesc& desc) {
    NodeDesc node{};
    node.id = t["id"].value_or<std::string>("");
    if (node.id.empty()) {
        return; // a node must be named so children and resolution can address it
    }
    node.parent = t["parent"].value_or<std::string>(""); // empty = root

    node.meshName = t["mesh"].value_or<std::string>("");
    readMaterials(t, node.materialName, node.groupMaterials);

    const Trs trs = readTrs(t);
    node.translation = trs.translation;
    node.rotation = trs.rotation;
    node.scale = trs.scale;

    // META2 - agent-legible hierarchy context (what this node/subtree is).
    // Pure AI context: never affects the resolved flat instance list, so an
    // absent or wrong-typed key just leaves the default.
    node.description = t["description"].value_or<std::string>("");
    if (const toml::array* tags = t["tags"].as_array()) {
        for (auto&& tag : *tags) {
            if (const auto s = tag.value<std::string>()) {
                node.tags.emplace_back(*s);
            }
        }
    }

    desc.nodes.push_back(std::move(node));
}

} // namespace

std::optional<SceneDesc> SceneParser::parse(const std::filesystem::path& sceneFile) {
    return parse(sceneFile, nullptr);
}

std::optional<SceneDesc> SceneParser::parse(const std::filesystem::path& sceneFile, std::string* errorMessage) {
    toml::table root;
    try {
        root = toml::parse_file(sceneFile.string());
    } catch (const toml::parse_error& e) {
        // Surface the real syntax error (file + line) to the validation CLI /
        // agent loop instead of a bare "malformed" shrug.
        if (errorMessage) {
            *errorMessage = "TOML syntax error: " + std::string(e.description());
        }
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

    // ── Meshes (unique declarations) + instances (placements) ──
    if (const toml::array* meshes = root["mesh"].as_array()) {
        for (const auto& elem : *meshes) {
            if (const toml::table* m = elem.as_table()) {
                parseMeta(root, desc);
                parseMetaLinks(root, desc);
                parseMesh(*m, desc);
            }
        }
    }
    if (const toml::array* instances = root["instance"].as_array()) {
        for (const auto& elem : *instances) {
            if (const toml::table* t = elem.as_table()) {
                parseInstance(*t, desc);
            }
        }
    }

    // ── Node graph (optional; resolves into the flat instance list) ──
    if (const toml::array* nodes = root["node"].as_array()) {
        for (const auto& elem : *nodes) {
            if (const toml::table* t = elem.as_table()) {
                parseNode(*t, desc);
            }
        }
        if (!resolveNodes(desc, errorMessage)) {
            return std::nullopt;
        }
    }

    // ── Animations (optional; bind against the completed node graph) ──
    if (!parseAnimations(root, desc, errorMessage)) {
        return std::nullopt;
    }

    return desc;
}

} // namespace aether
