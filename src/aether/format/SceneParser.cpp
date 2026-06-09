#include "aether/format/SceneParser.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace aether {
namespace {

// ── String helpers ────────────────────────────────────────────────────────

[[nodiscard]] std::string_view trimSv(std::string_view sv) noexcept {
    const auto first = sv.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    return sv.substr(first, sv.find_last_not_of(" \t\r\n") - first + 1);
}

[[nodiscard]] std::string_view stripComment(std::string_view line) noexcept {
    const auto pos = line.find('#');
    return trimSv(pos == std::string_view::npos ? line : line.substr(0, pos));
}

// ── Value parsers ─────────────────────────────────────────────────────────

bool parseVec3(std::string_view text, Vec3& out) {
    std::istringstream ss{std::string(text)};
    return static_cast<bool>(ss >> out.x >> out.y >> out.z);
}

bool parseFloat(std::string_view text, float& out) {
    std::istringstream ss{std::string(text)};
    return static_cast<bool>(ss >> out);
}

bool parseUint(std::string_view text, uint32_t& out) {
    std::istringstream ss{std::string(text)};
    return static_cast<bool>(ss >> out);
}

// ── Pending camera block ──────────────────────────────────────────────────

struct CameraBlock {
    Vec3 m_position{0.0F};
    bool m_hasTranslate = false;

    // Orientation: rotate and look_at are mutually exclusive; last one wins.
    std::optional<Quat> m_rotation; // from rotate / rotate_y
    std::optional<Vec3> m_lookAt;   // from look_at

    Vec3 m_up{0.0F, 1.0F, 0.0F};
    std::optional<float> m_vfov;
};

} // namespace

std::optional<SceneDesc> SceneParser::parse(const std::filesystem::path& sceneFile) {
    std::ifstream file(sceneFile);
    if (!file) {
        return std::nullopt;
    }

    SceneDesc desc{};

    // Active block state.
    enum class ActiveBlock { eNone, eGeometry, eCamera };
    ActiveBlock activeBlock = ActiveBlock::eNone;
    GeometryBlock blk{};
    bool hasGeometry = false;
    CameraBlock camBlk{};

    // Commit the pending geometry block into the SceneDesc.
    auto flushGeometry = [&] {
        if (hasGeometry) {
            desc.geometry.push_back(blk);
        }
        hasGeometry = false;
    };

    // Resolve the pending camera block into desc.camera.
    auto flushCamera = [&] {
        if (camBlk.m_hasTranslate) {
            desc.camera.position = camBlk.m_position;
        }

        if (camBlk.m_rotation) {
            // Derive look-at target and up from the quaternion.
            // Camera default forward is -Z; target = position + forward.
            const Vec3 pos = camBlk.m_hasTranslate ? camBlk.m_position : Vec3(278.0F, 273.0F, -800.0F);
            const Vec3 forward = *camBlk.m_rotation * Vec3(0.0F, 0.0F, -1.0F);
            desc.camera.lookAt = pos + forward;
            desc.camera.up = *camBlk.m_rotation * Vec3(0.0F, 1.0F, 0.0F);
        } else if (camBlk.m_lookAt) {
            desc.camera.lookAt = *camBlk.m_lookAt;
            desc.camera.up = camBlk.m_up;
        }

        if (camBlk.m_vfov) {
            desc.camera.vfov = *camBlk.m_vfov;
        }
    };

    auto flushActive = [&] {
        if (activeBlock == ActiveBlock::eCamera) {
            flushCamera();
        } else {
            flushGeometry();
        }
    };

    std::string line;
    while (std::getline(file, line)) {
        const std::string_view sv = stripComment(line);
        if (sv.empty()) {
            continue;
        }

        std::istringstream ss{std::string(sv)};
        std::string kw;
        ss >> kw;
        std::string rest;
        std::getline(ss, rest);
        const std::string_view rv = trimSv(rest);

        // ── Block starters ────────────────────────────────────────────────
        if (kw == "camera") {
            flushActive();
            camBlk = {};
            activeBlock = ActiveBlock::eCamera;

        } else if (kw == "instance") {
            flushActive();
            blk = {};
            blk.kind = GeometryBlock::Kind::Object;
            blk.objPath = std::string(rv);
            hasGeometry = true;
            activeBlock = ActiveBlock::eGeometry;

        } else if (kw == "sphere") {
            flushActive();
            blk = {};
            blk.kind = GeometryBlock::Kind::Sphere;
            parseFloat(rv, blk.sphereRadius);
            hasGeometry = true;
            activeBlock = ActiveBlock::eGeometry;

        } else if (kw == "box") {
            flushActive();
            blk = {};
            blk.kind = GeometryBlock::Kind::Box;
            std::istringstream vs{std::string(rv)};
            vs >> blk.boxHalf.x >> blk.boxHalf.y >> blk.boxHalf.z;
            hasGeometry = true;
            activeBlock = ActiveBlock::eGeometry;

        }
        // ── Shared modifiers: translate / rotate / rotate_y ───────────────
        else if (kw == "translate") {
            if (activeBlock == ActiveBlock::eCamera) {
                parseVec3(rv, camBlk.m_position);
                camBlk.m_hasTranslate = true;
            } else if (hasGeometry) {
                parseVec3(rv, blk.translation);
            }

        } else if (kw == "rotate") {
            // glTF convention on disk: qx qy qz qw; GLM quat is (w, x, y, z).
            float qx = 0.0F;
            float qy = 0.0F;
            float qz = 0.0F;
            float qw = 1.0F;
            std::istringstream vs{std::string(rv)};
            vs >> qx >> qy >> qz >> qw;
            const Quat q(qw, qx, qy, qz);
            if (activeBlock == ActiveBlock::eCamera) {
                camBlk.m_rotation = q;
                camBlk.m_lookAt = std::nullopt; // rotate wins over look_at
            } else if (hasGeometry) {
                blk.rotation = q;
            }

        } else if (kw == "rotate_y") {
            float deg = 0.0F;
            parseFloat(rv, deg);
            const Quat q = glm::angleAxis(glm::radians(deg), Vec3(0.0F, 1.0F, 0.0F));
            if (activeBlock == ActiveBlock::eCamera) {
                camBlk.m_rotation = q;
                camBlk.m_lookAt = std::nullopt;
            } else if (hasGeometry) {
                blk.rotation = q;
            }

        }
        // ── Geometry-only modifiers ───────────────────────────────────────
        else if (kw == "usemtl") {
            if (hasGeometry) {
                blk.materialName = std::string(rv);
            }

        } else if (kw == "material" && activeBlock == ActiveBlock::eGeometry) {
            // "material GroupName MatName" — per-group assignment for Object blocks.
            std::istringstream ms{std::string(rv)};
            std::string groupName;
            std::string matName;
            if (ms >> groupName >> matName) {
                blk.groupMaterials[groupName] = matName;
            }

        } else if (kw == "scale") {
            if (hasGeometry) {
                std::istringstream vs{std::string(rv)};
                float sx = 1.0F;
                float sy = 1.0F;
                float sz = 1.0F;
                vs >> sx;
                if (!(vs >> sy >> sz)) {
                    sy = sx;
                    sz = sx; // single value → uniform scale
                }
                blk.scale = {sx, sy, sz};
            }

        }
        // ── Camera-only modifiers ─────────────────────────────────────────
        else if (kw == "look_at") {
            Vec3 v;
            if (parseVec3(rv, v)) {
                camBlk.m_lookAt = v;
                camBlk.m_rotation = std::nullopt; // look_at wins over rotate
            }

        } else if (kw == "up") {
            parseVec3(rv, camBlk.m_up);

        } else if (kw == "vfov") {
            float v = 0.0F;
            if (parseFloat(rv, v)) {
                camBlk.m_vfov = v;
            }

        }
        // ── Material libraries ────────────────────────────────────────────
        else if (kw == "mtllib") {
            if (!rv.empty()) {
                desc.mtllibs.emplace_back(rv);
            }

        }
        // ── Render settings ───────────────────────────────────────────────
        else if (kw == "spp") {
            uint32_t v = 0;
            if (parseUint(rv, v)) {
                desc.spp = v;
            }
        } else if (kw == "max_depth") {
            uint32_t v = 0;
            if (parseUint(rv, v)) {
                desc.maxDepth = v;
            }
        } else if (kw == "env_unit_nits") {
            float v = 0.0F;
            if (parseFloat(rv, v)) {
                desc.envUnitNits = v;
            }
        } else if (kw == "ev100") {
            float v = 0.0F;
            if (parseFloat(rv, v)) {
                desc.camera.ev100 = v;
            }
        } else if (kw == "env_map") {
            if (!rv.empty()) {
                desc.envMapFile = std::string(rv);
            }
        } else if (kw == "tonemapper") {
            if (!rv.empty()) {
                desc.tonemapper = std::string(rv);
            }
        }
        // Unknown keywords are silently ignored, consistent with OBJ/MTL.
    }

    flushActive();
    return desc;
}

} // namespace aether
