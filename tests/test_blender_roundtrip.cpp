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

//  ANI8 (camera-from-node convention)  ------------------------------------------------

TEST(BlenderRoundTrip, AnimatedCameraNodeAimsAtTheOrigin) {
    // The exporter PRE-multiplies camera-node locals (CONV @ R - the same form
    // the static [camera] block writes) instead of conjugating them like every
    // other node, because a camera's payload is its OPTICAL AXIS: plain
    // conjugation swizzles local -Z onto -Y and the aim ends up 90 degrees off
    // (the rendered frames were sky-only). This pins both the position and the
    // aim - forward = rotation * (0, 0, -1) must point at the origin from
    // Aether (0, 2, 6) (= Blender (0, -6, 2) under CONV).
    const auto scene = aether::SceneParser::parse(assetsDir() / "camera_orbit.scene.toml");
    ASSERT_TRUE(scene.has_value());
    ASSERT_TRUE(scene->camera.nodeId.has_value());
    EXPECT_EQ(*scene->camera.nodeId, "OrbitCam");

    const auto p0 = aether::SceneAnimator::evaluateCameraAtTick(*scene, 0);
    ASSERT_TRUE(p0.has_value());
    EXPECT_NEAR(p0->position.x, 0.0F, kEps);
    EXPECT_NEAR(p0->position.y, 2.0F, kEps);
    EXPECT_NEAR(p0->position.z, 6.0F, kEps) << "tick 0: orbit pivot at its identity key";

    const auto aimsAtOrigin = [](const aether::CameraPose& pose) {
        const sm::float3 fwd = pose.rotation * sm::float3{0.0F, 0.0F, -1.0F};
        const float len = sm::length(pose.position);
        const sm::float3 toOrigin = pose.position * (-1.0F / len);
        EXPECT_NEAR(fwd.x, toOrigin.x, 1.0e-3F) << "forward.x vs to-origin";
        EXPECT_NEAR(fwd.y, toOrigin.y, 1.0e-3F) << "forward.y vs to-origin";
        EXPECT_NEAR(fwd.z, toOrigin.z, 1.0e-3F) << "forward.z vs to-origin";
    };
    aimsAtOrigin(*p0);

    // Deep into the orbit: the pivot rig keeps the aim on the origin while the
    // camera swings (~90 degrees about Aether +Y) - position height and radius
    // stay exact, and the forward direction still hits the target.
    const auto p58 = aether::SceneAnimator::evaluateCameraAtTick(*scene, 58);
    ASSERT_TRUE(p58.has_value());
    EXPECT_NEAR(p58->position.y, 2.0F, kEps) << "orbit keeps the camera height";
    EXPECT_NEAR(sm::length(p58->position), std::sqrt(40.0F), 1.0e-3F) << "orbit keeps the radius";
    aimsAtOrigin(*p58);
}

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

TEST(BlenderRoundTrip, FeatureMatrixExportsEveryFormatKey) {
    // blender_to_aether 1.6.0 feature matrix: every format key beyond the
    // core node/animation path, asserted key by key through the real parser
    // against the committed fixture (assets/feature_matrix.scene.toml).
    const auto scene = aether::SceneParser::parse(assetsDir() / "feature_matrix.scene.toml");
    ASSERT_TRUE(scene.has_value());

    // [render] inline keys override the preset reference ("local wins").
    ASSERT_TRUE(scene->spp.has_value());
    EXPECT_EQ(*scene->spp, 128U);
    ASSERT_TRUE(scene->maxDepth.has_value());
    EXPECT_EQ(*scene->maxDepth, 5U);
    ASSERT_TRUE(scene->workingColorSpace.has_value());
    EXPECT_EQ(*scene->workingColorSpace, "lin_rec709_scene");
    ASSERT_TRUE(scene->envMapFile.has_value());
    EXPECT_EQ(*scene->envMapFile, "meadow_2_4k.exr");
    ASSERT_TRUE(scene->envUnitNits.has_value());
    EXPECT_FLOAT_EQ(*scene->envUnitNits, 10000.0F);

    // [tonemap] / [post_tonemap]
    ASSERT_TRUE(scene->tonemapper.has_value());
    EXPECT_EQ(*scene->tonemapper, "agx");
    ASSERT_TRUE(scene->postTonemapRenderer.has_value());
    EXPECT_EQ(*scene->postTonemapRenderer, "green_screen");

    // [meta] provenance
    ASSERT_TRUE(scene->meta.has_value());
    EXPECT_EQ(scene->meta->title, "Feature Matrix");
    EXPECT_EQ(scene->meta->description, "Exporter feature-matrix fixture");
    EXPECT_NE(scene->meta->generator.find("blender_to_aether"), std::string::npos);

    // [[mesh]] BV1 bounds - object space of the exported OBJ (the cube is +-1)
    ASSERT_EQ(scene->meshes.size(), 1U);
    const aether::MeshDesc& mesh = scene->meshes[0];
    ASSERT_TRUE(mesh.bounds.has_value());
    EXPECT_FLOAT_EQ(mesh.bounds->min.x, -1.0F);
    EXPECT_FLOAT_EQ(mesh.bounds->min.y, -1.0F);
    EXPECT_FLOAT_EQ(mesh.bounds->min.z, -1.0F);
    EXPECT_FLOAT_EQ(mesh.bounds->max.x, 1.0F);
    EXPECT_FLOAT_EQ(mesh.bounds->max.y, 1.0F);
    EXPECT_FLOAT_EQ(mesh.bounds->max.z, 1.0F);

    // [[node]] META2 context + NAME-only material bindings (Blender material
    // parameters are never converted - the map keys are the usemtl groups).
    ASSERT_EQ(scene->nodes.size(), 1U);
    const aether::NodeDesc& node = scene->nodes[0];
    EXPECT_EQ(node.id, "FeatureCube");
    EXPECT_EQ(node.description, "Feature-matrix cube (META2 node description)");
    ASSERT_EQ(node.tags.size(), 2U);
    EXPECT_EQ(node.tags[0], "showcase");
    EXPECT_EQ(node.tags[1], "feature-matrix");
    EXPECT_EQ(node.materialName, "MatAlpha");
    ASSERT_EQ(node.groupMaterials.size(), 2U);
    EXPECT_EQ(node.groupMaterials.at("MatAlpha"), "MatAlpha");
    EXPECT_EQ(node.groupMaterials.at("MatBeta"), "MatBeta");
}

TEST(BlenderRoundTrip, MetaLinksDescribeLinkedFilesForAgents) {
    // [meta.link."..."] tables: agent summaries built ONLY from what the
    // exporter knows - stats read back from the written OBJ, the explicitly
    // provided environment map, and the referenced material library.
    const auto scene = aether::SceneParser::parse(assetsDir() / "feature_matrix.scene.toml");
    ASSERT_TRUE(scene.has_value());
    ASSERT_TRUE(scene->meta.has_value());
    const auto& links = scene->links; // parseLinkTable stores per-path LinkMeta here

    // Mesh link: counts + object-space bounds + the node's description.
    const auto meshIt = links.find("FeatureCube.obj");
    ASSERT_NE(meshIt, links.end()) << "mesh link missing";
    EXPECT_EQ(meshIt->second.kind, "mesh");
    EXPECT_EQ(meshIt->second.triangles, 12U) << "triangulated Blender cube = 12 faces";
    EXPECT_EQ(meshIt->second.groups, 2U) << "two usemtl group splits";
    EXPECT_EQ(meshIt->second.description, "Feature-matrix cube (META2 node description)");
    ASSERT_TRUE(meshIt->second.bounds.has_value());
    EXPECT_FLOAT_EQ(meshIt->second.bounds->min.x, -1.0F);
    EXPECT_FLOAT_EQ(meshIt->second.bounds->max.z, 1.0F);
    EXPECT_EQ(meshIt->second.bounds->frame, "object");

    // Env link (explicit operator prop) + material-library link.
    const auto envIt = links.find("meadow_2_4k.exr");
    ASSERT_NE(envIt, links.end()) << "env link missing";
    EXPECT_EQ(envIt->second.kind, "env");
    const auto libIt = links.find("openpbr_mtlx.materials.toml");
    ASSERT_NE(libIt, links.end()) << "material-library link missing";
    EXPECT_EQ(libIt->second.kind, "material-library");

    // The library reference itself lands in material_libraries.
    ASSERT_EQ(scene->mtllibs.size(), 1U);
    EXPECT_EQ(scene->mtllibs[0], "openpbr_mtlx.materials.toml");
}

TEST(BlenderRoundTrip, PoseBoneAnimationMatchesBlenderGroundTruth) {
    // Skeletal pose-bone export (blender_to_aether 1.6.0): the fixture keys
    // pose.bones channels (root location, child euler rotation) and the
    // exporter composes them onto each bone node's rest local. The expected
    // vertices are BLENDER'S OWN depsgraph evaluation at frames 0 and 30
    // (CONV'd into the Aether frame) as dumped by tools/test_scenes.py for
    // the skinned_pose builder - the pose formula (Blender-verified:
    // pose(child) = pose(parent) @ (rest(parent)^-1 @ rest(child)) @ basis)
    // must reproduce them through parse -> evaluateSkinsAtTick ->
    // deformSkinLbs.
    const auto scene = aether::SceneParser::parse(assetsDir() / "skinned_pose.scene.toml");
    ASSERT_TRUE(scene.has_value());
    ASSERT_EQ(scene->skins.size(), 1U);

    // Both composed whole-parameter pose tracks must exist.
    bool hasTranslation = false;
    bool hasRotation = false;
    for (const aether::AnimationTrack& track : scene->animations) {
        if (track.target == "nodes/Rig::Bone.Root/translation") {
            hasTranslation = true;
        }
        if (track.target == "nodes/Rig::Bone.Child/rotation") {
            hasRotation = true;
        }
    }
    EXPECT_TRUE(hasTranslation) << "root location pose track missing";
    EXPECT_TRUE(hasRotation) << "child rotation pose track missing";

    const auto groups = aether::ObjImporter::parse(assetsDir() / "SkinnedQuad.obj");
    ASSERT_TRUE(groups.has_value());
    const aether::MeshData& mesh = groups->at(0).mesh;
    ASSERT_EQ(mesh.vertices.size(), 4U);

    std::string error;
    const std::unordered_map<std::string, std::size_t> counts{{"SkinnedQuad", 4}};
    ASSERT_TRUE(aether::validateSkinVertexCounts(*scene, counts, &error)) << error;

    struct Truth {
        float x, y, z;
    };
    // frame 0 (= rest) and frame 30 (= t 1 s: root +0.5 along the bone,
    // child rotated 90 degrees about its local X) from Blender's evaluation.
    const Truth rest[4] = {
        {0.0F, 0.0F, 0.0F},
        {0.0F, -4.37113883e-08F, -1.0F},
        {1.0F, -4.37113883e-08F, -1.0F},
        {1.0F, 0.0F, 0.0F},
    };
    const Truth posed[4] = {
        {0.0F, 0.5F, -2.18556941e-08F},
        {0.0F, 0.49999997F, -1.0F},
        {1.0F, 2.5F, -1.0F},
        {1.0F, 1.5F, -1.00000012F},
    };

    const auto checkTick = [&](std::uint64_t tick, const Truth* expected) {
        const auto poses = aether::SceneAnimator::evaluateSkinsAtTick(*scene, tick);
        ASSERT_EQ(poses.size(), 1U);
        ASSERT_EQ(poses[0].skinMatrices.size(), 2U);
        if (tick == 30) {
            // The animation must actually move a joint (not stay at rest).
            EXPECT_GT(std::abs(poses[0].skinMatrices[1][1][1] - 1.0F), 0.5F) << "tick 30: child joint did not rotate";
        }
        const auto out = aether::deformSkinLbs(mesh, scene->skins[0], poses[0].skinMatrices);
        ASSERT_TRUE(out.has_value());
        for (std::size_t v = 0; v < 4; ++v) {
            EXPECT_NEAR(out->positions[v].x, expected[v].x, 1.0e-3F) << "tick " << tick << " v" << v << " x";
            EXPECT_NEAR(out->positions[v].y, expected[v].y, 1.0e-3F) << "tick " << tick << " v" << v << " y";
            EXPECT_NEAR(out->positions[v].z, expected[v].z, 1.0e-3F) << "tick " << tick << " v" << v << " z";
        }
    };
    checkTick(0, rest);
    checkTick(30, posed);
}
