// ANI4 (skin schema + LBS CPU evaluation reference) unit receipts:
// parser validation, node-world exposure, the evaluateSkins contract, the
// deformSkinLbs math (rigid / blend / renormalization / zero-sum / normals)
// and the bind-time vertex-count validator. No GPU, no renderers.

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aether/format/NodeGraph.hpp"
#include "aether/format/SceneAnimator.hpp"
#include "aether/format/SceneParser.hpp"
#include "aether/format/Skinning.hpp"
#include "aether/types/MeshData.hpp"
#include "aether/types/SceneDesc.hpp"

namespace {

constexpr float kEps = 1.0e-4F;

// Writes a scene TOML from @p tomlText in a throwaway directory and parses it
// with the error-reporting overload (test_scene_parser's NodeScene pattern).
struct SkinScene {
    std::filesystem::path dir;
    std::optional<aether::SceneDesc> scene;
    std::string error;

    SkinScene(std::string_view name, std::string_view tomlText) {
        namespace fs = std::filesystem;
        dir = fs::temp_directory_path() / ("aether_skin_" + std::string{name});
        fs::create_directories(dir);
        {
            std::ofstream scn(dir / "s.scene.toml");
            scn << tomlText;
        }
        scene = aether::SceneParser::parse(dir / "s.scene.toml", &error);
    }
    ~SkinScene() { std::filesystem::remove_all(dir); }
};

// Two-vertex procedural mesh for the deformer tests (counts drive the
// per-vertex coverage contract, so any vertex count works at parse time).
aether::MeshData twoVertexMesh() {
    aether::MeshData mesh;
    aether::Vertex a{};
    a.position = aether::Vec3{1.0F, 0.0F, 0.0F};
    a.normal = aether::Vec3{1.0F, 0.0F, 0.0F};
    aether::Vertex b{};
    b.position = aether::Vec3{0.0F, 1.0F, 0.0F};
    b.normal = aether::Vec3{0.0F, 1.0F, 0.0F};
    mesh.vertices = {a, b};
    mesh.indices = {0, 1, 1}; // unused by the deformer (vertex-driven)
    return mesh;
}

// A programmatic skin: two joints, identity inverse binds, 2 vertices.
aether::SkinDesc twoJointSkin() {
    aether::SkinDesc skin;
    skin.nodeId = "part";
    skin.joints = {"root", "tip"};
    skin.inverseBind.assign(32, 0.0F);
    for (int m = 0; m < 2; ++m) {
        skin.inverseBind[static_cast<std::size_t>(m) * 16 + 0] = 1.0F;
        skin.inverseBind[static_cast<std::size_t>(m) * 16 + 5] = 1.0F;
        skin.inverseBind[static_cast<std::size_t>(m) * 16 + 10] = 1.0F;
        skin.inverseBind[static_cast<std::size_t>(m) * 16 + 15] = 1.0F;
    }
    skin.boneIndices = {0, 0, 0, 0, 1, 1, 1, 1}; // vertex0 -> joint0, vertex1 -> joint1
    skin.boneWeights = {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F};
    return skin;
}

const char* kHappyPathScene = R"toml(
[animation_clock]
fps = 30

[[mesh]]
name = "m"
path = "m.obj"

[[node]]
id = "root"

[[node]]
id = "tip"
parent = "root"

[[node]]
id = "part"
mesh = "m"

[[skin]]
node = "part"
joints = ["root", "tip"]
inverse_bind = [
  [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
  [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
]
bone_indices = [[0, 0, 0, 0], [1, 1, 1, 1]]
bone_weights = [[1, 0, 0, 0], [1, 0, 0, 0]]
)toml";

} // namespace

// ── parser ───────────────────────────────────────────────────────────────────

TEST(SkinParser, ParsesSkinBindings) {
    SkinScene s("ok", kHappyPathScene);
    ASSERT_TRUE(s.scene.has_value()) << s.error;
    ASSERT_EQ(s.scene->skins.size(), 1U);
    const aether::SkinDesc& skin = s.scene->skins[0];
    EXPECT_EQ(skin.nodeId, "part");
    ASSERT_EQ(skin.joints.size(), 2U);
    EXPECT_EQ(skin.joints[0], "root");
    EXPECT_EQ(skin.joints[1], "tip");
    EXPECT_EQ(skin.inverseBind.size(), 32U);
    EXPECT_EQ(skin.boneIndices.size(), 8U);
    EXPECT_EQ(skin.boneWeights.size(), 8U);
    EXPECT_EQ(skin.boneIndices[4], 1U);
    // resolveNodes recorded a world matrix for EVERY node (meshed + grouping).
    ASSERT_EQ(s.scene->nodeWorlds.size(), 3U);
}

TEST(SkinParser, RejectsUnknownJoint) {
    SkinScene s("badjoint", R"toml(
[[node]]
id = "part"
mesh = "m"
[[skin]]
node = "part"
joints = ["ghost"]
inverse_bind = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]]
bone_indices = [[0, 0, 0, 0]]
bone_weights = [[1, 0, 0, 0]]
)toml");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("unknown joint node 'ghost'"), std::string::npos) << s.error;
}

TEST(SkinParser, RejectsUnknownSkinNode) {
    SkinScene s("badnode", R"toml(
[[skin]]
node = "nowhere"
joints = ["x"]
inverse_bind = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]]
bone_indices = [[0, 0, 0, 0]]
bone_weights = [[1, 0, 0, 0]]
)toml");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("unknown node 'nowhere'"), std::string::npos) << s.error;
}

TEST(SkinParser, RejectsInverseBindCountMismatch) {
    SkinScene s("ibcount", R"toml(
[[node]]
id = "a"
[[node]]
id = "part"
mesh = "m"
[[skin]]
node = "part"
joints = ["a", "part"]
inverse_bind = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]]
bone_indices = [[0, 0, 0, 0]]
bone_weights = [[1, 0, 0, 0]]
)toml");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("one inverse_bind matrix"), std::string::npos) << s.error;
}

TEST(SkinParser, RejectsNonSquareInverseBind) {
    SkinScene s("ibsize", R"toml(
[[node]]
id = "a"
[[node]]
id = "part"
mesh = "m"
[[skin]]
node = "part"
joints = ["a"]
inverse_bind = [[1, 0, 0, 0]]
bone_indices = [[0, 0, 0, 0]]
bone_weights = [[1, 0, 0, 0]]
)toml");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("exactly 16 floats"), std::string::npos) << s.error;
}

TEST(SkinParser, RejectsJointIndexOutOfRange) {
    SkinScene s("idxrange", R"toml(
[[node]]
id = "a"
[[node]]
id = "part"
mesh = "m"
[[skin]]
node = "part"
joints = ["a"]
inverse_bind = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]]
bone_indices = [[0, 3, 0, 0]]
bone_weights = [[1, 0, 0, 0]]
)toml");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("inside [0, joints)"), std::string::npos) << s.error;
}

TEST(SkinParser, RejectsNegativeWeight) {
    SkinScene s("negw", R"toml(
[[node]]
id = "a"
[[node]]
id = "part"
mesh = "m"
[[skin]]
node = "part"
joints = ["a"]
inverse_bind = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]]
bone_indices = [[0, 0, 0, 0]]
bone_weights = [[1, -0.5, 0, 0]]
)toml");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("finite and >= 0"), std::string::npos) << s.error;
}

TEST(SkinParser, RejectsDuplicateSkinNode) {
    SkinScene s("dupskin", R"toml(
[[node]]
id = "part"
mesh = "m"
[[skin]]
node = "part"
joints = ["part"]
inverse_bind = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]]
bone_indices = [[0, 0, 0, 0]]
bone_weights = [[1, 0, 0, 0]]
[[skin]]
node = "part"
joints = ["part"]
inverse_bind = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]]
bone_indices = [[0, 0, 0, 0]]
bone_weights = [[1, 0, 0, 0]]
)toml");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("more than one [[skin]]"), std::string::npos) << s.error;
}

// ── node worlds (the resolveNodes exposure evaluateSkins reads) ──────────────

TEST(SkinNodeWorlds, CompoundsParentChainForAllNodes) {
    aether::SceneDesc desc;
    aether::NodeDesc root{};
    root.id = "root";
    root.translation = aether::Vec3{1.0F, 0.0F, 0.0F};
    aether::NodeDesc child{};
    child.id = "child";
    child.parent = "root";
    child.translation = aether::Vec3{0.0F, 2.0F, 0.0F};
    desc.nodes = {root, child};

    std::string error;
    ASSERT_TRUE(aether::resolveNodes(desc, &error)) << error;
    ASSERT_EQ(desc.nodeWorlds.size(), 2U);
    // world = parent * local: root at (1,0,0), child at (1,2,0) - translation
    // lives in row-major rows' last column (mul(M, v)).
    EXPECT_NEAR(desc.nodeWorlds[0][0][3], 1.0F, kEps);
    EXPECT_NEAR(desc.nodeWorlds[1][0][3], 1.0F, kEps);
    EXPECT_NEAR(desc.nodeWorlds[1][1][3], 2.0F, kEps);
    EXPECT_NEAR(desc.nodeWorlds[1][2][3], 0.0F, kEps);
}

TEST(SkinNodeWorlds, ValidationFailureLeavesWorldsUntouched) {
    aether::SceneDesc desc;
    aether::NodeDesc a{};
    a.id = "a";
    a.parent = "missing";
    desc.nodes = {a};
    std::string error;
    EXPECT_FALSE(aether::resolveNodes(desc, &error));
    EXPECT_TRUE(desc.nodeWorlds.empty()) << "false return must leave desc untouched";
}

// ── evaluateSkins ────────────────────────────────────────────────────────────

TEST(EvaluateSkins, ComposesInvNodeJointIBM) {
    // part at identity; joints root (identity) and tip at x=2 - identity IBMs,
    // so skinMatrices are the joint worlds themselves (inv(I) * W * I).
    aether::SceneDesc desc;
    aether::NodeDesc root{};
    root.id = "root";
    aether::NodeDesc tip{};
    tip.id = "tip";
    tip.parent = "root";
    tip.translation = aether::Vec3{2.0F, 0.0F, 0.0F};
    aether::NodeDesc part{};
    part.id = "part";
    part.meshName = "m";
    desc.nodes = {root, tip, part};
    desc.skins.push_back(twoJointSkin()); // joints root+tip, identity IBMs

    std::string error;
    ASSERT_TRUE(aether::resolveNodes(desc, &error)) << error;
    desc.animationFps = 30.0;

    const auto poses = aether::SceneAnimator::evaluateSkins(desc, 0.0F);
    ASSERT_EQ(poses.size(), 1U);
    EXPECT_EQ(poses[0].nodeId, "part");
    ASSERT_EQ(poses[0].skinMatrices.size(), 2U);
    // joint0 (root, identity world): identity skin matrix.
    EXPECT_NEAR(poses[0].skinMatrices[0][0][0], 1.0F, kEps);
    EXPECT_NEAR(poses[0].skinMatrices[0][0][3], 0.0F, kEps);
    // joint1 (tip at x=2, part at identity): translation (2,0,0).
    EXPECT_NEAR(poses[0].skinMatrices[1][0][3], 2.0F, kEps);
    EXPECT_NEAR(poses[0].skinMatrices[1][1][3], 0.0F, kEps);
}

TEST(EvaluateSkins, TickClockFollowsJointAnimation) {
    SkinScene s("animated", R"toml(
[animation_clock]
fps = 30

[[mesh]]
name = "m"
path = "m.obj"

[[node]]
id = "root"

[[node]]
id = "tip"
parent = "root"

[[node]]
id = "part"
mesh = "m"

[[skin]]
node = "part"
joints = ["root", "tip"]
inverse_bind = [
  [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
  [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
]
bone_indices = [[0, 0, 0, 0], [1, 1, 1, 1]]
bone_weights = [[1, 0, 0, 0], [1, 0, 0, 0]]

[[animation]]
target = "nodes/tip/translation"
interpolation = "linear"
times = [0.0, 1.0]
values = [0, 0, 0, 2, 0, 0]
)toml");
    ASSERT_TRUE(s.scene.has_value()) << s.error;

    const auto pose0 = aether::SceneAnimator::evaluateSkinsAtTick(*s.scene, 0);
    const auto pose30 = aether::SceneAnimator::evaluateSkinsAtTick(*s.scene, 30); // t = 1 s
    ASSERT_EQ(pose0.size(), 1U);
    ASSERT_EQ(pose30.size(), 1U);
    ASSERT_EQ(pose0[0].skinMatrices.size(), 2U);
    EXPECT_NEAR(pose0[0].skinMatrices[1][0][3], 0.0F, kEps) << "tick 0: tip at rest";
    EXPECT_NEAR(pose30[0].skinMatrices[1][0][3], 2.0F, kEps) << "tick 30 (t=1s): tip animated to x=2";
}

TEST(EvaluateSkins, SkinFreeSceneIsEmpty) {
    aether::SceneDesc desc;
    EXPECT_TRUE(aether::SceneAnimator::evaluateSkins(desc, 0.0F).empty());
    EXPECT_TRUE(aether::SceneAnimator::evaluateSkinsAtTick(desc, 7).empty());
}

// ── deformSkinLbs (the CPU reference) ────────────────────────────────────────

TEST(DeformSkinLbs, RigidSingleJointMapsExactly) {
    const aether::MeshData mesh = twoVertexMesh();
    aether::SkinDesc skin = twoJointSkin();
    // vertex0 driven by joint0 = pure translation (2, 0, 0).
    std::vector<aether::Mat4> matrices(2, aether::Mat4{1.0F});
    matrices[0] = sm::trs(aether::Vec3{2.0F, 0.0F, 0.0F}, aether::Quat{0, 0, 0, 1}, aether::Vec3{1.0F});

    const auto out = aether::deformSkinLbs(mesh, skin, matrices);
    ASSERT_TRUE(out.has_value());
    EXPECT_NEAR(out->positions[0].x, 3.0F, kEps); // (1,0,0) + (2,0,0)
    EXPECT_NEAR(out->positions[0].y, 0.0F, kEps);
    EXPECT_NEAR(out->positions[1].y, 1.0F, kEps); // vertex1 driven by joint1 = identity
    EXPECT_NEAR(out->positions[1].x, 0.0F, kEps);
    // Translation must NOT leak into the normal (homogeneous w = 0).
    EXPECT_NEAR(out->normals[0].x, 1.0F, kEps);
    EXPECT_NEAR(out->normals[0].y, 0.0F, kEps);
}

TEST(DeformSkinLbs, TwoJointBlendAverages) {
    const aether::MeshData mesh = twoVertexMesh();
    aether::SkinDesc skin = twoJointSkin();
    // Both vertices blend joint0 (identity) and joint1 (translate x+2) at 0.5/0.5
    // after the glTF renormalization of the authored weights below.
    skin.boneIndices = {0, 1, 0, 0, 0, 1, 0, 0}; // unused slots point at joint0
    skin.boneWeights = {1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F};
    std::vector<aether::Mat4> matrices(2, aether::Mat4{1.0F});
    matrices[1] = sm::trs(aether::Vec3{2.0F, 0.0F, 0.0F}, aether::Quat{0, 0, 0, 1}, aether::Vec3{1.0F});

    const auto out = aether::deformSkinLbs(mesh, skin, matrices);
    ASSERT_TRUE(out.has_value());
    // p' = 0.5*p + 0.5*(p + (2,0,0)) = p + (1,0,0)
    EXPECT_NEAR(out->positions[0].x, 2.0F, kEps);
    EXPECT_NEAR(out->positions[0].y, 0.0F, kEps);
    EXPECT_NEAR(out->positions[1].x, 1.0F, kEps);
    EXPECT_NEAR(out->positions[1].y, 1.0F, kEps);
    // Renormalized blend: authored (2,2,0,0) must behave exactly like (1,1,0,0).
    aether::SkinDesc scaled = skin;
    scaled.boneWeights = {2.0F, 2.0F, 0.0F, 0.0F, 2.0F, 2.0F, 0.0F, 0.0F};
    const auto outScaled = aether::deformSkinLbs(mesh, scaled, matrices);
    ASSERT_TRUE(outScaled.has_value());
    EXPECT_NEAR(outScaled->positions[0].x, out->positions[0].x, 1.0e-6F);
    EXPECT_NEAR(outScaled->positions[1].y, out->positions[1].y, 1.0e-6F);
}

TEST(DeformSkinLbs, ZeroSumWeightsLeaveVertexUnchanged) {
    const aether::MeshData mesh = twoVertexMesh();
    aether::SkinDesc skin = twoJointSkin();
    skin.boneWeights.assign(8, 0.0F);
    std::vector<aether::Mat4> matrices(2, aether::Mat4{1.0F});
    matrices[0] = sm::trs(aether::Vec3{9.0F, 9.0F, 9.0F}, aether::Quat{0, 0, 0, 1}, aether::Vec3{1.0F});

    const auto out = aether::deformSkinLbs(mesh, skin, matrices);
    ASSERT_TRUE(out.has_value());
    EXPECT_NEAR(out->positions[0].x, 1.0F, kEps) << "zero-sum vertex stays authored";
    EXPECT_NEAR(out->positions[0].y, 0.0F, kEps);
    EXPECT_NEAR(out->normals[0].x, 1.0F, kEps);
}

TEST(DeformSkinLbs, RotationBlendRotatesNormals) {
    const aether::MeshData mesh = twoVertexMesh();
    aether::SkinDesc skin = twoJointSkin();
    // 90 degrees about Z: (1,0,0) -> (0,1,0), (0,1,0) -> (-1,0,0).
    const aether::Quat rotZ{0.0F, 0.0F, 0.70710678F, 0.70710678F};
    std::vector<aether::Mat4> matrices(2, sm::trs(aether::Vec3{0.0F, 0.0F, 0.0F}, rotZ, aether::Vec3{1.0F}));

    const auto out = aether::deformSkinLbs(mesh, skin, matrices);
    ASSERT_TRUE(out.has_value());
    EXPECT_NEAR(out->positions[0].x, 0.0F, kEps);
    EXPECT_NEAR(out->positions[0].y, 1.0F, kEps);
    EXPECT_NEAR(out->normals[0].x, 0.0F, kEps);
    EXPECT_NEAR(out->normals[0].y, 1.0F, kEps);
    EXPECT_NEAR(out->normals[1].x, -1.0F, kEps);
    EXPECT_NEAR(out->normals[1].y, 0.0F, kEps);
}

TEST(DeformSkinLbs, ContractViolationsReturnNullopt) {
    const aether::MeshData mesh = twoVertexMesh();
    aether::SkinDesc skin = twoJointSkin();
    std::vector<aether::Mat4> matrices(2, aether::Mat4{1.0F});

    aether::SkinDesc shortCoverage = skin;
    shortCoverage.boneWeights.pop_back();
    EXPECT_FALSE(aether::deformSkinLbs(mesh, shortCoverage, matrices).has_value());

    aether::SkinDesc badIndices = skin;
    badIndices.boneIndices.pop_back();
    EXPECT_FALSE(aether::deformSkinLbs(mesh, badIndices, matrices).has_value());

    std::vector<aether::Mat4> tooFew(1, aether::Mat4{1.0F});
    EXPECT_FALSE(aether::deformSkinLbs(mesh, skin, tooFew).has_value());
}

// ── bind-time vertex-count validation ────────────────────────────────────────

TEST(ValidateSkins, AcceptsMatchingCoverage) {
    aether::SceneDesc desc;
    aether::NodeDesc node{};
    node.id = "part";
    node.meshName = "m";
    desc.nodes = {node};
    desc.skins.push_back(twoJointSkin()); // twoJointSkin covers 2 vertices

    const std::unordered_map<std::string, std::size_t> counts{{"m", 2}};
    std::string error;
    EXPECT_TRUE(aether::validateSkinVertexCounts(desc, counts, &error)) << error;
}

TEST(ValidateSkins, RejectsVertexCountMismatch) {
    aether::SceneDesc desc;
    aether::NodeDesc node{};
    node.id = "part";
    node.meshName = "m";
    desc.nodes = {node};
    desc.skins.push_back(twoJointSkin());

    const std::unordered_map<std::string, std::size_t> counts{{"m", 512}};
    std::string error;
    EXPECT_FALSE(aether::validateSkinVertexCounts(desc, counts, &error));
    EXPECT_NE(error.find("covers 2 vertices but mesh 'm' has 512"), std::string::npos) << error;
}

TEST(ValidateSkins, RejectsGroupingNodeAndUnknownMesh) {
    aether::SceneDesc desc;
    aether::NodeDesc group{};
    group.id = "part"; // no mesh: grouping node
    desc.nodes = {group};
    desc.skins.push_back(twoJointSkin());
    const std::unordered_map<std::string, std::size_t> counts{{"m", 2}};
    std::string error;
    EXPECT_FALSE(aether::validateSkinVertexCounts(desc, counts, &error));
    EXPECT_NE(error.find("no mesh"), std::string::npos) << error;

    aether::NodeDesc meshed{};
    meshed.id = "part";
    meshed.meshName = "not_imported";
    desc.nodes = {meshed};
    EXPECT_FALSE(aether::validateSkinVertexCounts(desc, counts, &error));
    EXPECT_NE(error.find("was not imported"), std::string::npos) << error;
}

// ── ANI4-DQS (dual-quaternion skinning) ──────────────────────────────────────

namespace {

// Rotation of @p degrees about +Z as a quaternion (x, y, z, w).
aether::Quat rotZ(float degrees) {
    const float half = degrees * 3.14159265358979F / 360.0F;
    return aether::Quat{0.0F, 0.0F, std::sin(half), std::cos(half)};
}

// A single-joint skin (all influences on joint 0, weight 1).
aether::SkinDesc singleJointSkin() {
    aether::SkinDesc skin;
    skin.nodeId = "part";
    skin.joints = {"root"};
    skin.inverseBind.assign(16, 0.0F);
    skin.inverseBind[0] = 1.0F;
    skin.inverseBind[5] = 1.0F;
    skin.inverseBind[10] = 1.0F;
    skin.inverseBind[15] = 1.0F;
    skin.boneIndices = {0, 0, 0, 0, 0, 0, 0, 0};
    skin.boneWeights = {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F};
    skin.blend = aether::SkinDesc::Blend::Dqs;
    return skin;
}

// A uniform 0.5/0.5 two-joint skin (both vertices blended equally).
aether::SkinDesc halfHalfSkin() {
    aether::SkinDesc skin;
    skin.nodeId = "part";
    skin.joints = {"root", "tip"};
    skin.inverseBind.assign(32, 0.0F);
    for (int m = 0; m < 2; ++m) {
        skin.inverseBind[static_cast<std::size_t>(m) * 16 + 0] = 1.0F;
        skin.inverseBind[static_cast<std::size_t>(m) * 16 + 5] = 1.0F;
        skin.inverseBind[static_cast<std::size_t>(m) * 16 + 10] = 1.0F;
        skin.inverseBind[static_cast<std::size_t>(m) * 16 + 15] = 1.0F;
    }
    skin.boneIndices = {0, 1, 0, 0, 0, 1, 0, 0};
    skin.boneWeights = {0.5F, 0.5F, 0.0F, 0.0F, 0.5F, 0.5F, 0.0F, 0.0F};
    skin.blend = aether::SkinDesc::Blend::Dqs;
    return skin;
}

} // namespace

TEST(SkinDqs, SingleJointMatchesRigidTransformAndLbs) {
    // One joint carrying rotation + translation: DQS must reproduce the exact
    // affine transform (and therefore agree with LBS for a single influence).
    const aether::MeshData mesh = twoVertexMesh();
    const aether::SkinDesc skin = singleJointSkin();
    const std::vector<aether::Mat4> matrices{sm::trs(aether::Vec3{1.0F, 2.0F, 3.0F}, rotZ(90.0F), aether::Vec3{1.0F})};

    const auto dqs = aether::deformSkinDqs(mesh, skin, matrices);
    ASSERT_TRUE(dqs.has_value());
    const auto lbs = aether::deformSkinLbs(mesh, skin, matrices);
    ASSERT_TRUE(lbs.has_value());

    // Fixture: v0 = (1,0,0), v1 = (0,1,0). M*v0 = col0 + T = (0,1,0)+(1,2,3).
    EXPECT_NEAR(dqs->positions[0].x, 1.0F, 1.0e-5F);
    EXPECT_NEAR(dqs->positions[0].y, 3.0F, 1.0e-5F);
    EXPECT_NEAR(dqs->positions[0].z, 3.0F, 1.0e-5F);
    // M*v1 = col1 + T = (-1,0,0) + (1,2,3).
    EXPECT_NEAR(dqs->positions[1].x, 0.0F, 1.0e-5F);
    EXPECT_NEAR(dqs->positions[1].y, 2.0F, 1.0e-5F);
    EXPECT_NEAR(dqs->positions[1].z, 3.0F, 1.0e-5F);
    // Normal rotated, not translated (rigid rotation of the direction).
    EXPECT_NEAR(dqs->normals[0].x, 0.0F, 1.0e-5F);
    EXPECT_NEAR(dqs->normals[0].y, 1.0F, 1.0e-5F);
    // Parity anchor: single influence DQS == LBS.
    for (std::size_t v = 0; v < 2; ++v) {
        EXPECT_NEAR(dqs->positions[v].x, lbs->positions[v].x, 1.0e-5F) << "v=" << v;
        EXPECT_NEAR(dqs->positions[v].y, lbs->positions[v].y, 1.0e-5F) << "v=" << v;
        EXPECT_NEAR(dqs->positions[v].z, lbs->positions[v].z, 1.0e-5F) << "v=" << v;
    }
}

TEST(SkinDqs, TwoJointRotationBlendPreservesLengthWhileLbsCollapses) {
    // The candy-wrapper receipt: two joints rotating +-80 degrees about Z with
    // 0.5/0.5 weights. LBS averages the MATRICES (diag(cos80, cos80, 1) - the
    // vertex collapses to 0.1737 of its length); DQS averages the ROTATIONS
    // (identity - the vertex keeps its length).
    const aether::MeshData mesh = twoVertexMesh();
    const aether::SkinDesc skin = halfHalfSkin();
    const std::vector<aether::Mat4> matrices{sm::trs(aether::Vec3{0.0F}, rotZ(80.0F), aether::Vec3{1.0F}),
                                             sm::trs(aether::Vec3{0.0F}, rotZ(-80.0F), aether::Vec3{1.0F})};

    const auto dqs = aether::deformSkinDqs(mesh, skin, matrices);
    ASSERT_TRUE(dqs.has_value());
    const auto lbs = aether::deformSkinLbs(mesh, skin, matrices);
    ASSERT_TRUE(lbs.has_value());

    const float dqsLen = sm::length(dqs->positions[1]);
    const float lbsLen = sm::length(lbs->positions[1]);
    EXPECT_NEAR(dqsLen, 1.0F, 1.0e-4F) << "rigid blend preserves length";
    EXPECT_NEAR(lbsLen, std::cos(80.0F * 3.14159265F / 180.0F), 1.0e-4F)
        << "matrix blend collapses (the artifact DQS exists to fix)";
    EXPECT_GT(dqsLen - lbsLen, 0.5F) << "the two blend modes must actually differ";
    EXPECT_NEAR(dqs->positions[0].x, 1.0F, 1.0e-5F) << "identity blend, no translation";
    EXPECT_NEAR(dqs->positions[0].y, 0.0F, 1.0e-5F);
}

TEST(SkinDqs, SignAlignmentRescuesNear360DegreeJoints) {
    // Joints +-179 degrees apart: their quaternions straddle hemispheres
    // (dot < 0). WITHOUT sign alignment the real parts cancel toward identity;
    // WITH it the blend is the aligned ~180-degree rotation.
    const aether::MeshData mesh = twoVertexMesh();
    const aether::SkinDesc skin = halfHalfSkin();
    const std::vector<aether::Mat4> matrices{sm::trs(aether::Vec3{0.0F}, rotZ(179.0F), aether::Vec3{1.0F}),
                                             sm::trs(aether::Vec3{0.0F}, rotZ(-179.0F), aether::Vec3{1.0F})};

    const auto dqs = aether::deformSkinDqs(mesh, skin, matrices);
    ASSERT_TRUE(dqs.has_value());
    EXPECT_NEAR(dqs->positions[1].x, 0.0F, 1.0e-4F);
    EXPECT_LT(dqs->positions[1].y, -0.99F) << "aligned blend must land at ~180 degrees";
    const float len = sm::length(dqs->positions[1]);
    EXPECT_NEAR(len, 1.0F, 1.0e-4F) << "aligned result stays rigid and finite";
}

TEST(SkinDqs, DropsScaleByContract) {
    // DQS is rigid-by-design: a uniform scale in the joint matrix is dropped
    // (column normalization in skinDualQuaternions), LBS applies it.
    const aether::MeshData mesh = twoVertexMesh();
    const aether::SkinDesc skin = singleJointSkin();
    const std::vector<aether::Mat4> matrices{
        sm::trs(aether::Vec3{5.0F, 0.0F, 0.0F}, aether::Quat{0.0F, 0.0F, 0.0F, 1.0F}, aether::Vec3{2.0F})};

    const auto dqs = aether::deformSkinDqs(mesh, skin, matrices);
    ASSERT_TRUE(dqs.has_value());
    const auto lbs = aether::deformSkinLbs(mesh, skin, matrices);
    ASSERT_TRUE(lbs.has_value());
    EXPECT_NEAR(dqs->positions[0].x, 6.0F, 1.0e-4F) << "rigid: (1,0,0) + T, no scale";
    EXPECT_NEAR(lbs->positions[0].x, 7.0F, 1.0e-4F) << "LBS applies the 2x scale";
}

TEST(SkinDqs, ZeroSumAndContractFallbacks) {
    const aether::MeshData mesh = twoVertexMesh();
    aether::SkinDesc skin = halfHalfSkin();

    // Zero-sum weights -> authored vertices (the LBS glTF fallback).
    aether::SkinDesc zeroed = skin;
    zeroed.boneWeights.assign(8, 0.0F);
    const std::vector<aether::Mat4> matrices{sm::trs(aether::Vec3{0.0F}, rotZ(30.0F), aether::Vec3{1.0F}),
                                             sm::trs(aether::Vec3{0.0F}, rotZ(-30.0F), aether::Vec3{1.0F})};
    const auto out = aether::deformSkinDqs(mesh, zeroed, matrices);
    ASSERT_TRUE(out.has_value());
    EXPECT_NEAR(out->positions[0].x, 1.0F, 1.0e-6F) << "authored v0 = (1,0,0)";
    EXPECT_NEAR(out->positions[0].y, 0.0F, 1.0e-6F);

    // Contract breaks -> nullopt (coverage / joint-count), like LBS.
    aether::SkinDesc shortCoverage = skin;
    shortCoverage.boneWeights.pop_back();
    EXPECT_FALSE(aether::deformSkinDqs(mesh, shortCoverage, matrices).has_value());
    const std::vector<aether::Mat4> tooFew{matrices[0]};
    EXPECT_FALSE(aether::deformSkinDqs(mesh, skin, tooFew).has_value());

    // A degenerate joint matrix (zero basis column) fails the extraction.
    aether::Mat4 degenerate = sm::trs(aether::Vec3{0.0F}, aether::Quat{0.0F, 0.0F, 0.0F, 1.0F}, aether::Vec3{1.0F});
    degenerate[0][0] = 0.0F;
    degenerate[1][0] = 0.0F;
    degenerate[2][0] = 0.0F; // column 0 has zero length now
    EXPECT_FALSE(aether::deformSkinDqs(mesh, singleJointSkin(), {degenerate}).has_value());
}

TEST(SkinDqs, DualQuaternionLayoutIsJointMajorAndUnit) {
    const std::vector<aether::Mat4> matrices{sm::trs(aether::Vec3{1.0F, 2.0F, 3.0F}, rotZ(45.0F), aether::Vec3{1.0F}),
                                             sm::trs(aether::Vec3{0.0F}, rotZ(-10.0F), aether::Vec3{1.0F})};
    const auto dq = aether::skinDualQuaternions(matrices);
    ASSERT_TRUE(dq.has_value());
    ASSERT_EQ(dq->size(), 16U) << "8 floats per joint (real xyzw + dual xyzw)";
    // Joint 0's real part is the unit rotation quaternion.
    const float* real = dq->data();
    const float norm = std::sqrt(real[0] * real[0] + real[1] * real[1] + real[2] * real[2] + real[3] * real[3]);
    EXPECT_NEAR(norm, 1.0F, 1.0e-5F);
    // Joint 1's dual encodes half the translation (zero here -> pure rotation).
    EXPECT_NEAR((*dq)[12], 0.0F, 1.0e-6F);
    EXPECT_NEAR((*dq)[13], 0.0F, 1.0e-6F);
}

// ── ANI4-DQS: the blend key ──────────────────────────────────────────────────

namespace {

// The happy-path scene with `blend = "<value>"` injected before the joints list.
std::string sceneWithBlend(std::string_view value) {
    std::string s{kHappyPathScene};
    const auto pos = s.find("joints = [");
    if (pos == std::string::npos) {
        return s;
    }
    return s.substr(0, pos) + "blend = \"" + std::string{value} + "\"\n" + s.substr(pos);
}

} // namespace

TEST(SkinBlendKey, ParsesDqsAndDefaultsToLbs) {
    SkinScene dqs("blend_dqs", sceneWithBlend("dqs"));
    ASSERT_TRUE(dqs.scene.has_value()) << dqs.error;
    ASSERT_EQ(dqs.scene->skins.size(), 1U);
    EXPECT_EQ(dqs.scene->skins[0].blend, aether::SkinDesc::Blend::Dqs);

    SkinScene lbs("blend_default", kHappyPathScene);
    ASSERT_TRUE(lbs.scene.has_value()) << lbs.error;
    EXPECT_EQ(lbs.scene->skins[0].blend, aether::SkinDesc::Blend::Lbs) << "default stays LBS";
}

TEST(SkinBlendKey, RejectsUnknownBlendValue) {
    SkinScene bad("blend_bad", sceneWithBlend("quaternion"));
    EXPECT_FALSE(bad.scene.has_value());
    EXPECT_NE(bad.error.find("unknown blend 'quaternion' (lbs | dqs)"), std::string::npos) << bad.error;
}
