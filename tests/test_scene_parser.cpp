#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>

#include "aether/format/SceneParser.hpp"

namespace {

std::filesystem::path assetsDir() {
    return std::filesystem::path{AETHER_ASSETS_DIR};
}

// Writes a standalone preset file plus a scene that references it (no inline
// keys), in a throwaway directory, and parses the scene. Lets each settings
// group be unit-tested in isolation, independent of shipped assets.
struct PresetFixture {
    std::filesystem::path dir;
    std::optional<aether::SceneDesc> scene;

    PresetFixture(std::string_view section, std::string_view presetBody) {
        namespace fs = std::filesystem;
        dir = fs::temp_directory_path() / fs::path{"aether_preset_" + std::string{section}};
        fs::create_directories(dir / "presets");
        {
            std::ofstream preset(dir / "presets" / "p.toml");
            preset << presetBody;
        }
        {
            std::ofstream scn(dir / "s.scene.toml");
            scn << '[' << section << "]\nreference = \"presets/p.toml\"\n";
        }
        scene = aether::SceneParser::parse(dir / "s.scene.toml");
    }
    ~PresetFixture() { std::filesystem::remove_all(dir); }
};

// Writes a [[node]] scene from @p tomlText in a throwaway directory (one per
// test, like PresetFixture) and parses it with the error-reporting overload,
// so node-graph rejections can be checked against their message.
struct NodeScene {
    std::filesystem::path dir;
    std::optional<aether::SceneDesc> scene;
    std::string error;

    NodeScene(std::string_view name, std::string_view tomlText) {
        namespace fs = std::filesystem;
        dir = fs::temp_directory_path() / ("aether_node_" + std::string{name});
        fs::create_directories(dir);
        {
            std::ofstream scn(dir / "s.scene.toml");
            scn << tomlText;
        }
        scene = aether::SceneParser::parse(dir / "s.scene.toml", &error);
    }
    ~NodeScene() { std::filesystem::remove_all(dir); }
};

// Component-wise near-equality for compounded TRS values (rotations go through
// sin/cos of Euler degrees, so exact float equality is not meaningful).
constexpr float kTrsEpsilon = 1.0e-4F;

void expectVec3Near(const aether::Vec3& v, float x, float y, float z) {
    EXPECT_NEAR(v.x, x, kTrsEpsilon);
    EXPECT_NEAR(v.y, y, kTrsEpsilon);
    EXPECT_NEAR(v.z, z, kTrsEpsilon);
}

void expectQuatNear(const aether::Quat& q, float x, float y, float z, float w) {
    EXPECT_NEAR(q.x, x, kTrsEpsilon);
    EXPECT_NEAR(q.y, y, kTrsEpsilon);
    EXPECT_NEAR(q.z, z, kTrsEpsilon);
    EXPECT_NEAR(q.w, w, kTrsEpsilon);
}

TEST(SceneParser, ParsesClassicCornellScene) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(scene.has_value());
}

TEST(SceneParser, ReadsCameraParameters) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(scene.has_value());

    ASSERT_TRUE(scene->camera.translation.has_value());
    EXPECT_FLOAT_EQ(scene->camera.translation->x, 0.0F);
    EXPECT_FLOAT_EQ(scene->camera.translation->y, 2.73F);
    EXPECT_FLOAT_EQ(scene->camera.translation->z, -8.0F);

    ASSERT_TRUE(scene->camera.rotation.has_value());
    ASSERT_TRUE(scene->camera.vfov.has_value());
    EXPECT_FLOAT_EQ(*scene->camera.vfov, 39.1F);
}

TEST(SceneParser, ReadsGlobalRenderSettings) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(scene.has_value());

    ASSERT_TRUE(scene->spp.has_value());
    EXPECT_EQ(*scene->spp, 64U);
    ASSERT_TRUE(scene->maxDepth.has_value());
    EXPECT_EQ(*scene->maxDepth, 8U);
    ASSERT_TRUE(scene->camera.ev100.has_value());
    EXPECT_FLOAT_EQ(*scene->camera.ev100, 7.0F);
}

TEST(SceneParser, RecordsMaterialLibrary) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(scene.has_value());

    ASSERT_FALSE(scene->mtllibs.empty());
    EXPECT_EQ(scene->mtllibs.front(), "cornell.materials.toml");
}

TEST(SceneParser, ParsesMeshesAndInstances) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(scene.has_value());

    // One OBJ mesh + two distinct procedural boxes (different half-extents).
    ASSERT_EQ(scene->meshes.size(), 3U);
    EXPECT_EQ(scene->meshes[0].kind, aether::MeshDesc::Kind::Object);
    EXPECT_EQ(scene->meshes[0].name, "cornell");
    EXPECT_EQ(scene->meshes[0].objPath, "cornell.obj");
    EXPECT_EQ(scene->meshes[1].kind, aether::MeshDesc::Kind::Box);
    EXPECT_EQ(scene->meshes[2].kind, aether::MeshDesc::Kind::Box);

    // One instance per mesh, in declaration order.
    ASSERT_EQ(scene->instances.size(), 3U);
    const auto& cornell = scene->instances[0];
    EXPECT_EQ(cornell.meshName, "cornell");
    EXPECT_EQ(cornell.groupMaterials.at("LeftWall"), "RedWall");
    EXPECT_EQ(cornell.groupMaterials.at("RightWall"), "GreenWall");
    // The two boxes reference their distinct meshes.
    EXPECT_EQ(scene->instances[1].meshName, "box");
    EXPECT_EQ(scene->instances[2].meshName, "box_2");
}

TEST(SceneParser, ParsesAuthoredMeshBounds) {
    // shader_ball.scene.toml ships an authored object-space AABB on its OBJ mesh.
    const auto scene = aether::SceneParser::parse(assetsDir() / "shader_ball.scene.toml");
    ASSERT_TRUE(scene.has_value());
    const auto ball = std::find_if(scene->meshes.cbegin(), scene->meshes.cend(), [](const aether::MeshDesc& m) {
        return m.name == "shader_ball";
    });
    ASSERT_NE(ball, scene->meshes.cend());
    ASSERT_TRUE(ball->bounds.has_value());
    EXPECT_FLOAT_EQ(ball->bounds->min.x, -1.3456983F);
    EXPECT_FLOAT_EQ(ball->bounds->max.y, 2.6776464F);
    EXPECT_FLOAT_EQ(ball->bounds->max.z, 1.1554182F);
}

TEST(SceneParser, ParsesInlineBoundsTable) {
    // An inline `bounds = { min=[...], max=[...] }` on an OBJ mesh must round-trip,
    // and meshes that don't declare bounds keep std::nullopt (renderer derives them).
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "aether_bounds_test";
    fs::create_directories(dir);
    {
        std::ofstream scn(dir / "b.scene.toml");
        scn << R"toml([[mesh]]
name = "a"
path = "a.obj"
bounds = { min = [-1.0, -2.0, -3.0], max = [4.0, 5.0, 6.0] }

[[mesh]]
name = "b"
type = "sphere"
radius = 0.7

[[instance]]
mesh = "a"
[[instance]]
mesh = "b"
)toml";
    }
    const auto scene = aether::SceneParser::parse(dir / "b.scene.toml");
    ASSERT_TRUE(scene.has_value());
    ASSERT_EQ(scene->meshes.size(), 2U);
    ASSERT_TRUE(scene->meshes[0].bounds.has_value());
    EXPECT_FLOAT_EQ(scene->meshes[0].bounds->min.z, -3.0F);
    EXPECT_FLOAT_EQ(scene->meshes[0].bounds->max.x, 4.0F);
    // The sphere has no authored bounds → renderer derives from radius.
    EXPECT_FALSE(scene->meshes[1].bounds.has_value());
    fs::remove_all(dir);
}

TEST(SceneParser, DeduplicatesSharedMeshInstances) {
    // shaderball_metal places one shader_ball OBJ 10 times + one box.
    const auto scene = aether::SceneParser::parse(assetsDir() / "shaderball_metal.scene.toml");
    ASSERT_TRUE(scene.has_value());

    // The OBJ is declared exactly once despite 10 placements.
    ASSERT_EQ(scene->meshes.size(), 2U);
    EXPECT_EQ(scene->meshes[0].name, "box");
    EXPECT_EQ(scene->meshes[1].name, "shader_ball");
    EXPECT_EQ(scene->meshes[1].objPath, "shader_ball.obj");

    // 10 shader_ball instances + 1 box instance.
    EXPECT_EQ(scene->instances.size(), 11U);
    std::size_t ballCount = 0;
    for (const auto& inst : scene->instances) {
        if (inst.meshName == "shader_ball") {
            ++ballCount;
        }
    }
    EXPECT_EQ(ballCount, 10U);
}

TEST(SceneParser, MissingFileReturnsNullopt) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "no_such_scene.scene.toml");
    EXPECT_FALSE(scene.has_value());
}

// The cornell camera/render values above are themselves resolved from shared
// preset files (presets/cornell.camera.toml, presets/preview.render.toml), so
// those tests already exercise basic reference resolution. The cases below
// cover the tonemap section, an environment-map render preset, and the
// inline-override-wins rule.

TEST(SceneParser, ResolvesTonemapFromReferencedPreset) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "dragon_teapot.scene.toml");
    ASSERT_TRUE(scene.has_value());
    ASSERT_TRUE(scene->tonemapper.has_value());
    EXPECT_EQ(*scene->tonemapper, "aces");
}

TEST(SceneParser, ResolvesEnvironmentMapFromReferencedRenderPreset) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "shaderball_metal.scene.toml");
    ASSERT_TRUE(scene.has_value());
    ASSERT_TRUE(scene->envMapFile.has_value());
    EXPECT_EQ(*scene->envMapFile, "meadow_2_4k.exr");
    ASSERT_TRUE(scene->spp.has_value());
    EXPECT_EQ(*scene->spp, 64U);
    ASSERT_TRUE(scene->maxDepth.has_value());
    EXPECT_EQ(*scene->maxDepth, 6U);
}

TEST(SceneParser, InlineKeyOverridesReferencedPreset) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "aether_override_test";
    fs::create_directories(dir / "presets");

    {
        std::ofstream preset(dir / "presets" / "base.render.toml");
        preset << "samples_per_pixel = 64\nmax_depth = 8\n";
    }
    {
        std::ofstream scene(dir / "x.scene.toml");
        scene << "[render]\n"
              << "reference = \"presets/base.render.toml\"\n"
              << "samples_per_pixel = 256\n"; // overrides the referenced 64
    }

    const auto scene = aether::SceneParser::parse(dir / "x.scene.toml");
    ASSERT_TRUE(scene.has_value());
    ASSERT_TRUE(scene->spp.has_value());
    EXPECT_EQ(*scene->spp, 256U); // inline override wins
    ASSERT_TRUE(scene->maxDepth.has_value());
    EXPECT_EQ(*scene->maxDepth, 8U); // inherited from the referenced preset

    fs::remove_all(dir);
}

// ── Per-group preset resolution in isolation ────────────────────────────────
// Each settings group ([camera], [render], [tonemap]) must be populated purely
// from a referenced preset file, independently of any shipped asset.

TEST(SceneParser, CameraGroupResolvedFromReferencePreset) {
    PresetFixture f{"camera",
                    "translate = [1.0, 2.0, 3.0]\n"
                    "rotate_y = 30.0\n"
                    "vertical_field_of_view = 45.0\n"
                    "ev100 = 9.0\n"};
    ASSERT_TRUE(f.scene.has_value());
    ASSERT_TRUE(f.scene->camera.translation.has_value());
    EXPECT_FLOAT_EQ(f.scene->camera.translation->z, 3.0F);
    ASSERT_TRUE(f.scene->camera.rotation.has_value());
    ASSERT_TRUE(f.scene->camera.vfov.has_value());
    EXPECT_FLOAT_EQ(*f.scene->camera.vfov, 45.0F);
    ASSERT_TRUE(f.scene->camera.ev100.has_value());
    EXPECT_FLOAT_EQ(*f.scene->camera.ev100, 9.0F);
}

TEST(SceneParser, RenderGroupResolvedFromReferencePreset) {
    PresetFixture f{"render",
                    "samples_per_pixel = 128\n"
                    "max_depth = 12\n"
                    "environment_map = \"studio.exr\"\n"
                    "environment_unit_nits = 5000\n"};
    ASSERT_TRUE(f.scene.has_value());
    ASSERT_TRUE(f.scene->spp.has_value());
    EXPECT_EQ(*f.scene->spp, 128U);
    ASSERT_TRUE(f.scene->maxDepth.has_value());
    EXPECT_EQ(*f.scene->maxDepth, 12U);
    ASSERT_TRUE(f.scene->envMapFile.has_value());
    EXPECT_EQ(*f.scene->envMapFile, "studio.exr");
    ASSERT_TRUE(f.scene->envUnitNits.has_value());
    EXPECT_FLOAT_EQ(*f.scene->envUnitNits, 5000.0F);
}

TEST(SceneParser, WorkingColorSpaceAbsentByDefault) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(scene.has_value());
    EXPECT_FALSE(scene->workingColorSpace.has_value());
}

TEST(SceneParser, WorkingColorSpaceResolvedFromReferencePreset) {
    PresetFixture f{"render", "working_color_space = \"lin_rec709_scene\"\n"};
    ASSERT_TRUE(f.scene.has_value());
    ASSERT_TRUE(f.scene->workingColorSpace.has_value());
    EXPECT_EQ(*f.scene->workingColorSpace, "lin_rec709_scene");
}

TEST(SceneParser, TonemapGroupResolvedFromReferencePreset) {
    PresetFixture f{"tonemap", "tonemapper = \"agx\"\n"};
    ASSERT_TRUE(f.scene.has_value());
    ASSERT_TRUE(f.scene->tonemapper.has_value());
    EXPECT_EQ(*f.scene->tonemapper, "agx");
}

TEST(SceneParser, PostTonemapGroupResolvedFromReferencePreset) {
    PresetFixture f{"post_tonemap", "renderer = \"green_screen\"\n"};
    ASSERT_TRUE(f.scene.has_value());
    ASSERT_TRUE(f.scene->postTonemapRenderer.has_value());
    EXPECT_EQ(*f.scene->postTonemapRenderer, "green_screen");
}

TEST(SceneParser, ReadmeGalleryScenesParse) {
    static constexpr std::array<std::string_view, 15> kGalleryScenes{
        "cornell_classic.scene.toml",
        "cornell_empty.scene.toml",
        "cornell_spheres.scene.toml",
        "cornell_suzanne.scene.toml",
        "openpbr_metals.scene.toml",
        "openpbr_dielectrics.scene.toml",
        "openpbr_coat.scene.toml",
        "openpbr_fuzz.scene.toml",
        "openpbr_specular.scene.toml",
        "openpbr_organics.scene.toml",
        "openpbr_thinfilm.scene.toml",
        "cornell_textured_cube.scene.toml",
        "dragon_teapot.scene.toml",
        "openpbr_advanced.scene.toml",
        "ABeautifulGame.scene.toml",
    };
    for (std::string_view name : kGalleryScenes) {
        const auto parsed = aether::SceneParser::parse(assetsDir() / std::string{name});
        EXPECT_TRUE(parsed.has_value()) << "failed to parse " << name;
    }
}

// Smoke test: every shipped .scene.toml must parse without error or crash.
TEST(SceneParser, AllShippedScenesParse) {
    namespace fs = std::filesystem;
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(assetsDir())) {
        const auto& p = entry.path();
        if (p.extension() != ".toml" || !p.stem().string().ends_with(".scene")) {
            continue;
        }
        ++count;
        EXPECT_TRUE(aether::SceneParser::parse(p).has_value()) << "failed to parse " << p.string();
    }
    EXPECT_GT(count, 0U);
}

// ── Node graph (NH1) ────────────────────────────────────────────────────────
// Hand-computed values, composed with the slang-math Hamilton convention
// (world = parent_world * local, T × R × S: the child translation is scaled
// then rotated into the parent frame, rotations compose parent-after-child).

TEST(SceneParser, NodesCompoundParentChildTrs) {
    // Parent: rotate_x 90°, translate [1,2,3], scale [1,2,4].
    // Child:  rotate_y 90°, translate [10,20,30], scale [2,1,0.5], meshed.
    // world_t = [1,2,3] + Rx(90)·([1,2,4]∘[10,20,30]) = [1,2,3] + [10,−120,40]
    // world_s = [2,2,2]; world_q = Rx(90)·Ry(90) = (0.5, 0.5, 0.5, 0.5).
    NodeScene f{"compound",
                R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "parent"
translate = [1.0, 2.0, 3.0]
rotate_x = 90.0
scale = [1.0, 2.0, 4.0]

[[node]]
id = "child"
parent = "parent"
mesh = "ball"
translate = [10.0, 20.0, 30.0]
rotate_y = 90.0
scale = [2.0, 1.0, 0.5]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->nodes.size(), 2U);
    // The authored node keeps its LOCAL transform; the instance is world-space.
    EXPECT_EQ(f.scene->nodes[1].id, "child");
    EXPECT_EQ(f.scene->nodes[1].parent, "parent");
    expectVec3Near(f.scene->nodes[1].translation, 10.0F, 20.0F, 30.0F);

    ASSERT_EQ(f.scene->instances.size(), 1U);
    EXPECT_EQ(f.scene->instances[0].meshName, "ball");
    expectVec3Near(f.scene->instances[0].translation, 11.0F, -118.0F, 43.0F);
    expectVec3Near(f.scene->instances[0].scale, 2.0F, 2.0F, 2.0F);
    expectQuatNear(f.scene->instances[0].rotation, 0.5F, 0.5F, 0.5F, 0.5F);
}

TEST(SceneParser, InstancedSubtreeUnderDifferentParents) {
    // Two nodes reference the SAME mesh under two differently-placed parents —
    // an instanced subtree: each meshed node becomes its own instance.
    NodeScene f{"subtree",
                R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "arm_a"
translate = [1.0, 0.0, 0.0]

[[node]]
id = "ball_a"
parent = "arm_a"
mesh = "ball"
translate = [10.0, 0.0, 0.0]

[[node]]
id = "arm_b"
translate = [0.0, 5.0, 0.0]

[[node]]
id = "ball_b"
parent = "arm_b"
mesh = "ball"
translate = [20.0, 0.0, 0.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->nodes.size(), 4U);
    ASSERT_EQ(f.scene->instances.size(), 2U);
    EXPECT_EQ(f.scene->instances[0].meshName, "ball");
    expectVec3Near(f.scene->instances[0].translation, 11.0F, 0.0F, 0.0F);
    EXPECT_EQ(f.scene->instances[1].meshName, "ball");
    expectVec3Near(f.scene->instances[1].translation, 20.0F, 5.0F, 0.0F);
}

TEST(SceneParser, DeepChainCompoundsFourLevels) {
    // root -> n1 -> n2 -> n3, each translating [1,0,0] and rotating 90° about
    // +Z. World translations walk the corners of a unit square back to the
    // origin; the final rotation is a full turn (identity action on vectors).
    NodeScene f{"chain",
                R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "n0"
mesh = "ball"
translate = [1.0, 0.0, 0.0]
rotate_z = 90.0

[[node]]
id = "n1"
parent = "n0"
mesh = "ball"
translate = [1.0, 0.0, 0.0]
rotate_z = 90.0

[[node]]
id = "n2"
parent = "n1"
mesh = "ball"
translate = [1.0, 0.0, 0.0]
rotate_z = 90.0

[[node]]
id = "n3"
parent = "n2"
mesh = "ball"
translate = [1.0, 0.0, 0.0]
rotate_z = 90.0
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->nodes.size(), 4U);
    ASSERT_EQ(f.scene->instances.size(), 4U);
    expectVec3Near(f.scene->instances[0].translation, 1.0F, 0.0F, 0.0F);
    expectVec3Near(f.scene->instances[1].translation, 1.0F, 1.0F, 0.0F);
    expectVec3Near(f.scene->instances[2].translation, 0.0F, 1.0F, 0.0F);
    expectVec3Near(f.scene->instances[3].translation, 0.0F, 0.0F, 0.0F);
    // Rz(360°) ≡ identity: basis vectors come back unchanged (any quaternion
    // double-cover sign must pass).
    expectVec3Near(f.scene->instances[3].rotation * aether::Vec3{1.0F, 0.0F, 0.0F}, 1.0F, 0.0F, 0.0F);
    expectVec3Near(f.scene->instances[3].rotation * aether::Vec3{0.0F, 1.0F, 0.0F}, 0.0F, 1.0F, 0.0F);
}

TEST(SceneParser, DuplicateNodeIdFailsParse) {
    NodeScene f{"duplicate",
                R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "grip"
mesh = "ball"

[[node]]
id = "grip"
mesh = "ball"
)toml"};
    EXPECT_FALSE(f.scene.has_value());
    EXPECT_NE(f.error.find("grip"), std::string::npos) << "error: " << f.error;
}

TEST(SceneParser, UnknownParentFailsParse) {
    NodeScene f{"unknown_parent",
                R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "hand"
parent = "arm"
mesh = "ball"
)toml"};
    EXPECT_FALSE(f.scene.has_value());
    EXPECT_NE(f.error.find("hand"), std::string::npos) << "error: " << f.error;
    EXPECT_NE(f.error.find("arm"), std::string::npos) << "error: " << f.error;
}

TEST(SceneParser, NodeCycleFailsParse) {
    // Two-node cycle ...
    NodeScene cycle{"cycle",
                    R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "a"
parent = "b"

[[node]]
id = "b"
parent = "a"
)toml"};
    EXPECT_FALSE(cycle.scene.has_value());
    EXPECT_NE(cycle.error.find("cycle"), std::string::npos) << "error: " << cycle.error;

    // ... and a self-loop.
    NodeScene selfLoop{"self_cycle",
                       R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "s"
parent = "s"
)toml"};
    EXPECT_FALSE(selfLoop.scene.has_value());
    EXPECT_NE(selfLoop.error.find("s"), std::string::npos) << "error: " << selfLoop.error;
}

TEST(SceneParser, GroupingNodeEmitsNoInstance) {
    // A mesh-less node is a pure transform grouping node: it contributes zero
    // instances but compounds into its children.
    NodeScene f{"grouping",
                R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "pivot"
translate = [5.0, 0.0, 0.0]

[[node]]
id = "leaf"
parent = "pivot"
mesh = "ball"
translate = [1.0, 2.0, 3.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->nodes.size(), 2U);
    ASSERT_EQ(f.scene->instances.size(), 1U);
    expectVec3Near(f.scene->instances[0].translation, 6.0F, 2.0F, 3.0F);
}

TEST(SceneParser, GroupingOnlySceneEmitsNothing) {
    // A scene whose nodes are ALL grouping nodes resolves to zero instances.
    NodeScene f{"grouping_only",
                R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "root"
translate = [5.0, 0.0, 0.0]

[[node]]
id = "child"
parent = "root"
translate = [1.0, 2.0, 3.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    EXPECT_EQ(f.scene->instances.size(), 0U);
}

TEST(SceneParser, NodeFreeSceneKeepsFlatInstances) {
    // Backward compat (structural): a scene without [[node]] entries keeps the
    // exact flat instance list, and the node list defaults to empty.
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(scene.has_value());
    EXPECT_TRUE(scene->nodes.empty());
    ASSERT_EQ(scene->instances.size(), 3U);
    EXPECT_EQ(scene->instances[0].meshName, "cornell");
    EXPECT_EQ(scene->instances[1].meshName, "box");
    EXPECT_EQ(scene->instances[2].meshName, "box_2");
}

TEST(SceneParser, FlatInstancesAndNodesCoexist) {
    // Coexistence contract: flat [[instance]] blocks always parse as before and
    // stay first; meshed [[node]] entries are appended after them. A node with
    // no TRS keys resolves to the identity placement.
    NodeScene f{"coexist",
                R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[instance]]
mesh = "ball"
translate = [100.0, 0.0, 0.0]

[[node]]
id = "solo"
mesh = "ball"
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->instances.size(), 2U);
    // Flat instance, unchanged, first.
    expectVec3Near(f.scene->instances[0].translation, 100.0F, 0.0F, 0.0F);
    // Node-derived instance appended with its identity world TRS.
    EXPECT_EQ(f.scene->instances[1].meshName, "ball");
    expectVec3Near(f.scene->instances[1].translation, 0.0F, 0.0F, 0.0F);
    expectVec3Near(f.scene->instances[1].scale, 1.0F, 1.0F, 1.0F);
    expectQuatNear(f.scene->instances[1].rotation, 0.0F, 0.0F, 0.0F, 1.0F);
}

TEST(SceneParser, NodeMaterialBindingsMatchInstanceSemantics) {
    // A meshed node binds materials exactly like an [[instance]] block.
    NodeScene f{"materials",
                R"toml([[mesh]]
name = "cornell"
path = "cornell.obj"

[[node]]
id = "room"
mesh = "cornell"
material = "Floor"
materials = { LeftWall = "RedWall", RightWall = "GreenWall" }
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->instances.size(), 1U);
    EXPECT_EQ(f.scene->instances[0].materialName, "Floor");
    EXPECT_EQ(f.scene->instances[0].groupMaterials.at("LeftWall"), "RedWall");
    EXPECT_EQ(f.scene->instances[0].groupMaterials.at("RightWall"), "GreenWall");
}

} // namespace
