// NH4 Blender-export round-trip receipts: the COMMITTED output of
// tools/blender_to_aether (Blender 5.3.0 headless - regenerate with
// `blender --background --python tools/test_scenes.py -- --scene skinned
// --out <dir>` and refresh the three assets) parsed by the REAL
// aether::ObjImporter + SceneParser. The failure this pins:
// ObjImporter::flushMesh deduplicates face corners on the RESOLVED
// (position, normal, uv) values, so the fixture quad's two triangles hand it
// 6 corners but only 4 unique vertices - a [[skin]]/[[morph]] row expansion
// that counts corners instead of imported vertices fails
// validateSkinVertexCounts/validateMorphVertexCounts at load (the 6-vs-4
// controls below prove the validator bites). Also exercises the exported
// blend key, the rest-pose identity deform (both blend modes) and the shape
// key -> morph weight track -> deformMorphs chain. No GPU, no renderers.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aether/format/Morphing.hpp"
#include "aether/format/NodeGraph.hpp"
#include "aether/format/ObjImporter.hpp"
#include "aether/format/SceneAnimator.hpp"
#include "aether/format/SceneParser.hpp"
#include "aether/format/Skinning.hpp"
#include "aether/types/MeshData.hpp"
#include "aether/types/SceneDesc.hpp"

namespace {

constexpr float kEps = 1.0e-4F;

std::filesystem::path assetsDir() {
    return std::filesystem::path{AETHER_ASSETS_DIR};
}

// One committed fixture: a scene TOML plus the OBJ it references (both scenes
// share SkinnedQuad.obj - the exporter writes byte-identical geometry for the
// same builder, verified at fixture generation).
struct Fixture {
    std::optional<aether::SceneDesc> scene;
    std::string error;
    std::optional<std::vector<aether::MeshGroup>> groups;

    explicit Fixture(const char* sceneName) {
        scene = aether::SceneParser::parse(assetsDir() / sceneName, &error);
        if (scene) {
            groups = aether::ObjImporter::parse(assetsDir() / "SkinnedQuad.obj");
        }
    }
};

// Imported vertex total summed over groups, exactly like Harmonia's
// SceneLoader builds meshVertexCounts (sum of submesh vertex counts).
std::size_t importedTotal(const std::vector<aether::MeshGroup>& groups) {
    std::size_t total = 0;
    for (const auto& group : groups) {
        total += group.mesh.vertices.size();
    }
    return total;
}

} // namespace

// � ObjImporter parity (the dedup contract) �������������������������������

TEST(BlenderRoundTrip, ObjImportDedupsSixCornersToFourVertices) {
    const Fixture f("skinned.scene.toml");
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_TRUE(f.groups.has_value());
    ASSERT_EQ(f.groups->size(), 1U) << "one `o` line, no usemtl: a single group";
    EXPECT_EQ(importedTotal(*f.groups), 4U) << "flushMesh must collapse the quad's 6 face corners to 4 unique vertices";
}

TEST(BlenderRoundTrip, DeformationRowsMatchImportedVertexCount) {
    for (const char* name : {"skinned.scene.toml", "skinned_dqs.scene.toml"}) {
        const Fixture f(name);
        ASSERT_TRUE(f.scene.has_value()) << name << ": " << f.error;
        ASSERT_TRUE(f.groups.has_value()) << name;

        const std::unordered_map<std::string, std::size_t> counts{{"SkinnedQuad", importedTotal(*f.groups)}};
        std::string error;
        EXPECT_TRUE(aether::validateSkinVertexCounts(*f.scene, counts, &error)) << name << ": " << error;
        EXPECT_TRUE(aether::validateMorphVertexCounts(*f.scene, counts, &error)) << name << ": " << error;

        // Negative control: a row expansion that counts face corners (the
        // pre-fix exporter emitted 6 rows) must be rejected by both validators.
        const std::unordered_map<std::string, std::size_t> corners{{"SkinnedQuad", 6}};
        EXPECT_FALSE(aether::validateSkinVertexCounts(*f.scene, corners, &error)) << name;
        EXPECT_FALSE(aether::validateMorphVertexCounts(*f.scene, corners, &error)) << name;
    }
}

// � exported binding shape �����������������������������������������������

TEST(BlenderRoundTrip, SkinBindingRowsFollowVertexGroupLayout) {
    const Fixture f("skinned.scene.toml");
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->skins.size(), 1U);

    const aether::SkinDesc& skin = f.scene->skins[0];
    EXPECT_EQ(skin.nodeId, "SkinnedQuad");
    EXPECT_EQ(skin.joints, (std::vector<std::string>{"Rig::Bone.Root", "Rig::Bone.Child"}));
    EXPECT_EQ(skin.blend, aether::SkinDesc::Blend::Lbs);
    ASSERT_EQ(skin.inverseBind.size(), 32U) << "2 joints x 16 floats";

    // Blocky fixture weights: verts 0,1 -> Bone.Root (joint 0);
    // verts 2,3 -> Bone.Child (joint 1), one influence each, weight 1.
    const std::vector<std::uint32_t> expectedIndices{0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    ASSERT_EQ(skin.boneIndices.size(), 16U) << "4 imported vertices x 4 slots";
    EXPECT_EQ(skin.boneIndices, expectedIndices);
    for (std::size_t v = 0; v < 4; ++v) {
        EXPECT_FLOAT_EQ(skin.boneWeights[v * 4], 1.0F) << "vertex " << v;
        EXPECT_FLOAT_EQ(skin.boneWeights[v * 4 + 1], 0.0F) << "vertex " << v;
        EXPECT_FLOAT_EQ(skin.boneWeights[v * 4 + 2], 0.0F) << "vertex " << v;
        EXPECT_FLOAT_EQ(skin.boneWeights[v * 4 + 3], 0.0F) << "vertex " << v;
    }
}

TEST(BlenderRoundTrip, BlendModeRoundTrips) {
    const Fixture lbs("skinned.scene.toml");
    ASSERT_TRUE(lbs.scene.has_value()) << lbs.error;
    ASSERT_EQ(lbs.scene->skins.size(), 1U);
    EXPECT_EQ(lbs.scene->skins[0].blend, aether::SkinDesc::Blend::Lbs);

    const Fixture dqs("skinned_dqs.scene.toml");
    ASSERT_TRUE(dqs.scene.has_value()) << dqs.error;
    ASSERT_EQ(dqs.scene->skins.size(), 1U);
    EXPECT_EQ(dqs.scene->skins[0].blend, aether::SkinDesc::Blend::Dqs)
        << "the operator's blend = dqs must survive the export -> parse round trip";
}

// � rest-pose identity (IBM correctness) �����������������������������������

TEST(BlenderRoundTrip, RestPoseSkinMatricesAreIdentity) {
    // inverse_bind = inverse(CONV arm.world bone.local CONV^-1) and the node
    // worlds compound to exactly that value, so at rest every skin matrix is
    // the identity and both deformers leave the imported geometry untouched.
    for (const char* name : {"skinned.scene.toml", "skinned_dqs.scene.toml"}) {
        Fixture f(name); // non-const: resolveNodes records nodeWorlds in place
        ASSERT_TRUE(f.scene.has_value()) << name << ": " << f.error;
        ASSERT_TRUE(f.groups.has_value()) << name;

        std::string error;
        ASSERT_TRUE(aether::resolveNodes(*f.scene, &error)) << error;
        const auto poses = aether::SceneAnimator::evaluateSkinsAtTick(*f.scene, 0);
        ASSERT_EQ(poses.size(), 1U) << name;
        ASSERT_EQ(poses[0].skinMatrices.size(), 2U) << name;
        for (const aether::Mat4& m : poses[0].skinMatrices) {
            for (int row = 0; row < 4; ++row) {
                for (int col = 0; col < 4; ++col) {
                    EXPECT_NEAR(m[row][col], row == col ? 1.0F : 0.0F, kEps) << name;
                }
            }
        }

        const aether::MeshData& mesh = f.groups->at(0).mesh;
        const bool useDqs = f.scene->skins[0].blend == aether::SkinDesc::Blend::Dqs;
        const auto out = useDqs ? aether::deformSkinDqs(mesh, f.scene->skins[0], poses[0].skinMatrices)
                                : aether::deformSkinLbs(mesh, f.scene->skins[0], poses[0].skinMatrices);
        ASSERT_TRUE(out.has_value()) << name;
        ASSERT_EQ(out->positions.size(), mesh.vertices.size()) << name;
        for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
            EXPECT_NEAR(out->positions[v].x, mesh.vertices[v].position.x, kEps) << name << " v" << v;
            EXPECT_NEAR(out->positions[v].y, mesh.vertices[v].position.y, kEps) << name << " v" << v;
            EXPECT_NEAR(out->positions[v].z, mesh.vertices[v].position.z, kEps) << name << " v" << v;
        }
    }
}

// � shape key -> weight track -> deformMorphs �����������������������������

TEST(BlenderRoundTrip, MorphWeightTrackDrivesDeform) {
    const Fixture f("skinned.scene.toml");
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_TRUE(f.groups.has_value());

    // Grammar: the shape-key value F-curve exports as a MorphWeight track on
    // morphs/<mesh>/<name>, cubic_spline, frame 1 -> frame 30 at 30 fps.
    ASSERT_EQ(f.scene->animations.size(), 1U);
    const aether::AnimationTrack& track = f.scene->animations[0];
    EXPECT_EQ(track.target, "morphs/SkinnedQuad/Smile");
    EXPECT_EQ(track.parameter, aether::AnimationTrack::Parameter::MorphWeight);
    EXPECT_EQ(track.interpolation, aether::AnimationTrack::Interpolation::CubicSpline);
    ASSERT_EQ(track.times.size(), 2U);
    EXPECT_NEAR(track.times[0], 1.0F / 30.0F, 1.0e-3F) << "frame 1 at 30 fps";
    EXPECT_NEAR(track.times[1], 1.0F, kEps) << "frame 30";

    ASSERT_EQ(f.scene->morphs.size(), 1U);
    const aether::MorphTarget& target = f.scene->morphs[0];
    EXPECT_EQ(target.meshName, "SkinnedQuad");
    EXPECT_EQ(target.name, "Smile");
    ASSERT_EQ(target.deltas.size(), 4U);
    EXPECT_NEAR(target.deltas[0].y, 0.0F, kEps) << "lower row untouched";
    EXPECT_NEAR(target.deltas[1].y, 0.0F, kEps) << "lower row untouched";
    EXPECT_NEAR(target.deltas[2].y, 0.25F, kEps) << "Blender +z becomes +y under CONV";
    EXPECT_NEAR(target.deltas[3].y, 0.25F, kEps) << "Blender +z becomes +y under CONV";

    // Tick 0 (t = 0, before the first key): weight 0 -> rest pose.
    const auto w0 = aether::SceneAnimator::evaluateMorphWeightsAtTick(*f.scene, 0);
    ASSERT_EQ(w0.size(), 1U);
    EXPECT_NEAR(w0[0], 0.0F, kEps);
    // Tick 30 (t = 1 s = the last key): weight 1 -> full smile.
    const auto w30 = aether::SceneAnimator::evaluateMorphWeightsAtTick(*f.scene, 30);
    ASSERT_EQ(w30.size(), 1U);
    EXPECT_NEAR(w30[0], 1.0F, kEps);

    const aether::MeshData& mesh = f.groups->at(0).mesh;
    const auto out = aether::deformMorphs(mesh, {&target, 1}, w30);
    ASSERT_TRUE(out.has_value());
    // Rows 2,3 are the smile's vertices: +0.25 in the exported Y-up frame;
    // rows 0,1 (the lower row) stay put. Normals are structurally untouched -
    // MorphedVertices carries positions only (the POSITION-only v1 contract).
    EXPECT_NEAR(out->positions[2].y, mesh.vertices[2].position.y + 0.25F, kEps);
    EXPECT_NEAR(out->positions[3].y, mesh.vertices[3].position.y + 0.25F, kEps);
    EXPECT_NEAR(out->positions[0].y, mesh.vertices[0].position.y, kEps);
    EXPECT_NEAR(out->positions[1].y, mesh.vertices[1].position.y, kEps);
}
