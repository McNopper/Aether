#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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

TEST(SceneParser, ReadsCameraNodeBinding) {
    // ANI8: `[camera] node = "<id>"` binds against the completed node graph;
    // the section's own TRS stays parsed alongside (static fallback/preview).
    NodeScene f("cam_node", R"toml([camera]
node = "orbit"
translate = [0.0, 4.0, 8.0]
vertical_field_of_view = 50.0

[[node]]
id = "orbit"
)toml");
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_TRUE(f.scene->camera.nodeId.has_value());
    EXPECT_EQ(*f.scene->camera.nodeId, "orbit");
    ASSERT_TRUE(f.scene->camera.translation.has_value()) << "TRS parses alongside node";
    EXPECT_FLOAT_EQ(f.scene->camera.translation->y, 4.0F);
}

TEST(SceneParser, RejectsUnknownCameraNode) {
    NodeScene f("cam_badnode", R"toml([camera]
node = "nope"
vertical_field_of_view = 50.0
)toml");
    ASSERT_FALSE(f.scene.has_value());
    EXPECT_NE(f.error.find("camera references unknown node 'nope'"), std::string::npos) << f.error;
}

TEST(SceneParser, ReadsThinLensParameters) {
    NodeScene f("cam_thin_lens", R"toml([camera]
lens_radius = 0.00625
focus_distance = 1.02
vertical_field_of_view = 50.0
)toml");
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_TRUE(f.scene->camera.lensRadius.has_value());
    ASSERT_TRUE(f.scene->camera.focusDistance.has_value());
    EXPECT_FLOAT_EQ(*f.scene->camera.lensRadius, 0.00625F);
    EXPECT_FLOAT_EQ(*f.scene->camera.focusDistance, 1.02F);
}

TEST(SceneParser, RejectsInvalidThinLensParameters) {
    for (const auto* body : {"[camera]\nlens_radius = -0.1\nvertical_field_of_view = 50.0\n",
                             "[camera]\nfocus_distance = 0.0\nvertical_field_of_view = 50.0\n"}) {
        NodeScene f("cam_bad_lens", body);
        ASSERT_FALSE(f.scene.has_value()) << f.error;
        EXPECT_NE(f.error.find("camera"), std::string::npos) << f.error;
    }
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

TEST(SceneParser, HazeGroupResolvedFromReferencePreset) {
    PresetFixture f{"haze", "density = 0.002\nheight = 180.0\n"};
    ASSERT_TRUE(f.scene.has_value());
    ASSERT_TRUE(f.scene->hazeDensity.has_value());
    EXPECT_FLOAT_EQ(*f.scene->hazeDensity, 0.002F);
    ASSERT_TRUE(f.scene->hazeHeight.has_value());
    EXPECT_FLOAT_EQ(*f.scene->hazeHeight, 180.0F);
}

TEST(SceneParser, HazeAbsentByDefault) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(scene.has_value());
    EXPECT_FALSE(scene->hazeDensity.has_value());
    EXPECT_FALSE(scene->hazeHeight.has_value());
}

TEST(SceneParser, RejectsAHazeWithOneKeyOrUnphysicalValues) {
    for (const auto* body : {"[haze]\ndensity = 0.002\n",
                             "[haze]\nheight = 180.0\n",
                             "[haze]\ndensity = -0.001\nheight = 180.0\n",
                             "[haze]\ndensity = 0.002\nheight = 0.0\n",
                             "[haze]\ndensity = 0.002\nheight = -5.0\n",
                             "[haze]\ndensity = inf\nheight = 180.0\n",
                             "[haze]\ndensity = 0.002\nheight = nan\n"}) {
        NodeScene f{"haze_invalid", body};
        EXPECT_FALSE(f.scene.has_value()) << body;
        EXPECT_NE(f.error.find("[haze]"), std::string::npos) << body;
    }
}

TEST(SceneParser, ZeroHazeDensityIsASwitchedOffMedium) {
    NodeScene f{"haze_zero", "[haze]\ndensity = 0.0\nheight = 180.0\n"};
    ASSERT_TRUE(f.scene.has_value());
    ASSERT_TRUE(f.scene->hazeDensity.has_value());
    EXPECT_FLOAT_EQ(*f.scene->hazeDensity, 0.0F);
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

TEST(SceneParser, BlenderExportedHierarchyParsesAndResolves) {
    // Round-trip fixture: the verbatim shape of a blender_to_aether NH4 export
    // (Blender 5.2 headless; external material/preset/camera refs stripped -
    // mesh paths resolve consumer-side, so missing OBJs are fine at parse time).
    NodeScene f{"blender_export",
                R"toml([[mesh]]
name = "LoneCube"
path = "LoneCube.obj"

[[mesh]]
name = "TwinCube"
path = "TwinCube.obj"

[[node]]
id = "RootEmpty"
translate = [1.0, 3.0, -2.0]
rotate = [0.0, 0.2588, -0.0000000113, 0.9659]
scale = [2.0, 2.0, 2.0]

[[node]]
id = "LoneCube"
translate = [5.0, 0.0, 0.0]
mesh = "LoneCube"

[[node]]
id = "MarkerEmpty"

[[node]]
id = "TwinCube"
parent = "RootEmpty"
translate = [1.0, 1.0, -0.5]
scale = [0.5, 0.5, 0.5]
mesh = "TwinCube"

[[node]]
id = "NegGroup"
parent = "RootEmpty"
scale = [-1.0, -1.0, -1.0]

[[node]]
id = "MidEmpty"
parent = "RootEmpty"
translate = [0.5, 2.6e-15, 0.0000000596]
rotate = [0.1305, 0.0, 0.0, 0.9914]
scale = [1.5, 1.25, 1.0]

[[node]]
id = "LeafCube"
parent = "MidEmpty"
mesh = "TwinCube"
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    EXPECT_EQ(f.scene->nodes.size(), 7U);
    // 3 meshed nodes -> 3 flat instances; grouping nodes emit nothing.
    ASSERT_EQ(f.scene->instances.size(), 3U);
    // Depth-first from roots in declaration order.
    EXPECT_EQ(f.scene->instances[0].meshName, "TwinCube");
    EXPECT_EQ(f.scene->instances[1].meshName, "TwinCube");
    EXPECT_EQ(f.scene->instances[2].meshName, "LoneCube");
}

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
    EXPECT_NE(selfLoop.error.find('s'), std::string::npos) << "error: " << selfLoop.error;
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

// ── Node metadata (META2) ───────────────────────────────────────────────────
// description/tags/assembly are pure agent context: they land on the node or
// scene they are authored on and never influence the resolved instance list.

TEST(SceneParser, NodeDescriptionAndTagsLandOnRightNodes) {
    NodeScene f{"node_meta",
                R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "arm"
translate = [0.0, 1.0, 0.0]
description = "The shaderball's arm - parents the grip."
tags = ["rig", "kinematic"]

[[node]]
id = "grip"
parent = "arm"
mesh = "ball"
description = "Grip tip of the arm."
tags = ["rig"]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->nodes.size(), 2U);
    EXPECT_EQ(f.scene->nodes[0].id, "arm");
    EXPECT_EQ(f.scene->nodes[0].description, "The shaderball's arm - parents the grip.");
    ASSERT_EQ(f.scene->nodes[0].tags.size(), 2U);
    EXPECT_EQ(f.scene->nodes[0].tags[0], "rig");
    EXPECT_EQ(f.scene->nodes[0].tags[1], "kinematic");
    EXPECT_EQ(f.scene->nodes[1].id, "grip");
    EXPECT_EQ(f.scene->nodes[1].description, "Grip tip of the arm.");
    ASSERT_EQ(f.scene->nodes[1].tags.size(), 1U);
    EXPECT_EQ(f.scene->nodes[1].tags[0], "rig");
}

TEST(SceneParser, NodeMetaDefaultsAreEmpty) {
    // Absent description/tags leave the defaults: pure context, not structure.
    NodeScene f{"node_meta_default",
                R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "solo"
mesh = "ball"
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->nodes.size(), 1U);
    EXPECT_TRUE(f.scene->nodes[0].description.empty());
    EXPECT_TRUE(f.scene->nodes[0].tags.empty());
}

TEST(SceneParser, WrongTypedNodeMetaIsLenientlyIgnored) {
    // description = 42 (not a string) and tags = "rig" (not an array) parse as
    // absent: the metadata is pure AI context, never a parse error.
    NodeScene f{"node_meta_wrong_type",
                R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "solo"
mesh = "ball"
description = 42
tags = "rig"
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->nodes.size(), 1U);
    EXPECT_TRUE(f.scene->nodes[0].description.empty());
    EXPECT_TRUE(f.scene->nodes[0].tags.empty());
}

TEST(SceneParser, MetaAssemblyParses) {
    // A minimal [meta] block (only the META2 assembly hint) parses; the other
    // keys keep their defaults.
    NodeScene f{"meta_assembly",
                R"toml([meta]
assembly = "The arm node parents the grip; the grip places the ball mesh."

[[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "arm"
translate = [0.0, 1.0, 0.0]

[[node]]
id = "grip"
parent = "arm"
mesh = "ball"
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_TRUE(f.scene->meta.has_value());
    EXPECT_EQ(f.scene->meta->assembly, "The arm node parents the grip; the grip places the ball mesh.");
}

TEST(SceneParser, MetaGeneratorParses) {
    // A minimal [meta] block with only the generator field parses (the shape
    // blender_to_aether emits): every other key keeps its default.
    NodeScene f{"meta_generator",
                R"toml([meta]
generator = "blender_to_aether/1.2.0"

[[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "solo"
mesh = "ball"
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_TRUE(f.scene->meta.has_value());
    EXPECT_EQ(f.scene->meta->generator, "blender_to_aether/1.2.0");
    EXPECT_EQ(f.scene->meta->schema, "aether-scene"); // untouched defaults
    EXPECT_EQ(f.scene->meta->version, 1U);
}

TEST(SceneParser, NodeMetaDoesNotChangeResolvedInstances) {
    // The same node scene with and without META2 annotations must resolve to
    // identical instance lists: the metadata is authoring-layer only.
    NodeScene plain{"meta_plain",
                    R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "parent"
translate = [1.0, 2.0, 3.0]
rotate_x = 90.0

[[node]]
id = "child"
parent = "parent"
mesh = "ball"
translate = [10.0, 20.0, 30.0]
scale = [2.0, 1.0, 0.5]
)toml"};
    NodeScene annotated{"meta_annotated",
                        R"toml([meta]
assembly = "Parent rig rotates the child grip into place."

[[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "parent"
translate = [1.0, 2.0, 3.0]
rotate_x = 90.0
description = "Arm rig - the whole subtree rotates with it."
tags = ["rig"]

[[node]]
id = "child"
parent = "parent"
mesh = "ball"
translate = [10.0, 20.0, 30.0]
scale = [2.0, 1.0, 0.5]
description = "Grip tip - end effector of the arm rig."
tags = ["rig", "end-effector"]
)toml"};
    ASSERT_TRUE(plain.scene.has_value()) << plain.error;
    ASSERT_TRUE(annotated.scene.has_value()) << annotated.error;
    ASSERT_EQ(annotated.scene->nodes.size(), plain.scene->nodes.size());
    // Annotations landed on the annotated copy (sanity: the scenes differ only in meta).
    EXPECT_FALSE(annotated.scene->nodes[1].description.empty());
    EXPECT_TRUE(plain.scene->nodes[1].description.empty());
    ASSERT_EQ(plain.scene->instances.size(), annotated.scene->instances.size());
    for (std::size_t i = 0; i < plain.scene->instances.size(); ++i) {
        EXPECT_EQ(plain.scene->instances[i].meshName, annotated.scene->instances[i].meshName);
        expectVec3Near(plain.scene->instances[i].translation,
                       annotated.scene->instances[i].translation.x,
                       annotated.scene->instances[i].translation.y,
                       annotated.scene->instances[i].translation.z);
        expectVec3Near(plain.scene->instances[i].scale,
                       annotated.scene->instances[i].scale.x,
                       annotated.scene->instances[i].scale.y,
                       annotated.scene->instances[i].scale.z);
        expectQuatNear(plain.scene->instances[i].rotation,
                       annotated.scene->instances[i].rotation.x,
                       annotated.scene->instances[i].rotation.y,
                       annotated.scene->instances[i].rotation.z,
                       annotated.scene->instances[i].rotation.w);
    }
}

// ── Link metadata (META1 remainder) ─────────────────────────────────────────
// [meta.link."<path>"] summaries are pure agent context too: they land on
// SceneDesc::links and never influence the resolved instance list. Lenient
// like every meta key — absent or wrong-typed values keep their defaults.

TEST(SceneParser, LinkMetaFullBlockLandsAllKeys) {
    // Every documented link key lands: kind/description/triangles/groups/
    // materials and the bounds incl. its frame + orientation quaternion.
    NodeScene f{"link_meta_full",
                R"toml([meta]
title = "Link meta landing"

[meta.link."x.obj"]
kind = "mesh"
description = "Test hero mesh - glossy ceramic, the scene's centerpiece."
triangles = 2904
groups = 3
materials = ["ceramic_white", "snow"]
bounds = { min = [-0.5, -0.31, -0.42], max = [0.5, 0.48, 0.31], frame = "object", orientation = [0.0, 0.7071, 0.0, 0.7071] }

[[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[instance]]
mesh = "ball"
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_TRUE(f.scene->meta.has_value());
    ASSERT_EQ(f.scene->links.size(), 1U);
    const auto& link = f.scene->links.at("x.obj");
    EXPECT_EQ(link.kind, "mesh");
    EXPECT_EQ(link.description, "Test hero mesh - glossy ceramic, the scene's centerpiece.");
    EXPECT_EQ(link.triangles, 2904U);
    EXPECT_EQ(link.groups, 3U);
    ASSERT_EQ(link.materials.size(), 2U);
    EXPECT_EQ(link.materials[0], "ceramic_white");
    EXPECT_EQ(link.materials[1], "snow");
    ASSERT_TRUE(link.bounds.has_value());
    EXPECT_FLOAT_EQ(link.bounds->min.x, -0.5F);
    EXPECT_FLOAT_EQ(link.bounds->min.z, -0.42F);
    EXPECT_FLOAT_EQ(link.bounds->max.x, 0.5F);
    EXPECT_FLOAT_EQ(link.bounds->max.y, 0.48F);
    EXPECT_EQ(link.bounds->frame, "object");
    expectQuatNear(link.bounds->orientation, 0.0F, 0.7071F, 0.0F, 0.7071F);
}

TEST(SceneParser, WrongTypedLinkMetaIsLenientlyIgnored) {
    // bounds = "wide" (not a table), groups = "three" (not an integer), a
    // wrong-typed frame and a 3-float orientation: every one keeps its
    // default — meta is pure context and can never fail the parse.
    NodeScene f{"link_meta_wrong_type",
                R"toml([meta.link."x.obj"]
kind = "mesh"
bounds = "wide"
groups = "three"

[meta.link."y.obj"]
kind = "mesh"
bounds = { min = [-1.0, 0.0, 0.0], max = [1.0, 0.0, 0.0], frame = 42, orientation = [0.0, 0.0, 0.7071] }
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->links.size(), 2U);
    const auto& x = f.scene->links.at("x.obj");
    EXPECT_FALSE(x.bounds.has_value()); // non-table bounds parse as absent
    EXPECT_EQ(x.groups, 0U);
    const auto& y = f.scene->links.at("y.obj");
    ASSERT_TRUE(y.bounds.has_value());    // valid min/max land…
    EXPECT_EQ(y.bounds->frame, "object"); // …while wrong-typed keys default
    expectQuatNear(y.bounds->orientation, 0.0F, 0.0F, 0.0F, 1.0F);
}

TEST(SceneParser, LinkMetaDoesNotChangeResolvedInstances) {
    // The same flat scene with and without [meta]/[meta.link] annotations must
    // resolve to identical instance lists: the summaries are authoring-layer.
    NodeScene plain{"link_meta_plain",
                    R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[instance]]
mesh = "ball"
translate = [1.0, 2.0, 3.0]
)toml"};
    NodeScene annotated{"link_meta_annotated",
                        R"toml([meta]
title = "Annotated twin"

[meta.link."ball.obj"]
kind = "mesh"
description = "The one mesh."
triangles = 12
groups = 1
materials = ["rubber"]
bounds = { min = [-0.5, -0.5, -0.5], max = [0.5, 0.5, 0.5] }

[[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[instance]]
mesh = "ball"
translate = [1.0, 2.0, 3.0]
)toml"};
    ASSERT_TRUE(plain.scene.has_value()) << plain.error;
    ASSERT_TRUE(annotated.scene.has_value()) << annotated.error;
    // The summaries landed on the annotated copy (sanity: the scenes differ only in meta).
    ASSERT_TRUE(annotated.scene->meta.has_value());
    EXPECT_EQ(annotated.scene->links.at("ball.obj").kind, "mesh");
    EXPECT_TRUE(plain.scene->links.empty());
    EXPECT_FALSE(plain.scene->meta.has_value()); // no [meta] authored → no meta
    ASSERT_EQ(plain.scene->instances.size(), annotated.scene->instances.size());
    for (std::size_t i = 0; i < plain.scene->instances.size(); ++i) {
        EXPECT_EQ(plain.scene->instances[i].meshName, annotated.scene->instances[i].meshName);
        expectVec3Near(plain.scene->instances[i].translation,
                       annotated.scene->instances[i].translation.x,
                       annotated.scene->instances[i].translation.y,
                       annotated.scene->instances[i].translation.z);
        expectVec3Near(plain.scene->instances[i].scale,
                       annotated.scene->instances[i].scale.x,
                       annotated.scene->instances[i].scale.y,
                       annotated.scene->instances[i].scale.z);
        expectQuatNear(plain.scene->instances[i].rotation,
                       annotated.scene->instances[i].rotation.x,
                       annotated.scene->instances[i].rotation.y,
                       annotated.scene->instances[i].rotation.z,
                       annotated.scene->instances[i].rotation.w);
    }
}

TEST(SceneParser, MeshlessSceneStillCarriesMeta) {
    // A scene with no [[mesh]] at all (only [meta], a camera and a link
    // summary) still parses its meta: the [meta] reads sit outside the mesh
    // loop, exactly once per scene — a mesh-less scene dropped them entirely.
    NodeScene f{"meta_meshless",
                R"toml([meta]
title = "Camera-only scene"
description = "No geometry - the meta block must still land."

[camera]
translate = [0.0, 1.0, -5.0]
vertical_field_of_view = 45.0

[meta.link."env.exr"]
kind = "env"
description = "Studio IBL placeholder."
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_TRUE(f.scene->meta.has_value());
    EXPECT_EQ(f.scene->meta->title, "Camera-only scene");
    EXPECT_EQ(f.scene->meta->description, "No geometry - the meta block must still land.");
    EXPECT_EQ(f.scene->links.at("env.exr").kind, "env");
    EXPECT_TRUE(f.scene->meshes.empty());
    EXPECT_TRUE(f.scene->instances.empty());
}

// ── [[light]] ───────────────────────────────────────────────────────────────

// A light-only scene body with the given [[light]] entries.
std::string lightScene(std::string_view lights) {
    return std::string{lights};
}

// The scene must be rejected and the message must contain @p needle.
void expectLightRejected(std::string_view name, std::string_view toml, std::string_view needle) {
    NodeScene f{name, toml};
    EXPECT_FALSE(f.scene.has_value()) << "accepted a scene it must reject:\n" << toml;
    EXPECT_NE(f.error.find(needle), std::string::npos) << "error: " << f.error;
}

using LightDesc = aether::LightDesc;

TEST(SceneParser, LightsEveryTypeRoundTrips) {
    NodeScene f{"lights_types", lightScene(R"toml([[light]]
name = "Bulb"
type = "point"
translate = [1.0, 2.0, 3.0]
color = [1.0, 0.5, 0.25]
intensity = 800.0
unit = "candela"
range = 5.0

[[light]]
name = "Cone"
type = "spot"
translate = [-1.0, 4.0, 0.0]
rotate_x = 90.0
color_space = "lin_rec2020_scene"
intensity = 1000.0
unit = "lumen"
inner_angle = 12.0
outer_angle = 30.0

[[light]]
name = "Panel"
type = "rect"
translate = [0.0, 3.0, 0.0]
rotate = [0.0, 0.0, 0.0, 2.0]
intensity = 1500.0
unit = "nit"
half_extents = [0.25, 0.5]

[[light]]
name = "Sun"
type = "directional"
rotate = [0.3826834, 0.0, 0.0, 0.9238795]
intensity = 100000.0
unit = "lux"
)toml")};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->lights.size(), 4U);

    const LightDesc& bulb = f.scene->lights[0];
    EXPECT_EQ(bulb.type, LightDesc::Type::Point);
    EXPECT_EQ(bulb.name, "Bulb");
    expectVec3Near(bulb.translation, 1.0F, 2.0F, 3.0F);
    expectQuatNear(bulb.rotation, 0.0F, 0.0F, 0.0F, 1.0F);
    expectVec3Near(bulb.color, 1.0F, 0.5F, 0.25F);
    EXPECT_FLOAT_EQ(bulb.intensity, 800.0F);
    EXPECT_EQ(bulb.unit, LightDesc::Unit::Candela);
    EXPECT_EQ(bulb.colorSpace, "lin_rec709_scene") << "default color space";
    EXPECT_FLOAT_EQ(bulb.range, 5.0F);

    const LightDesc& cone = f.scene->lights[1];
    EXPECT_EQ(cone.type, LightDesc::Type::Spot);
    EXPECT_EQ(cone.unit, LightDesc::Unit::Lumen);
    EXPECT_EQ(cone.colorSpace, "lin_rec2020_scene");
    EXPECT_FLOAT_EQ(cone.intensity, 1000.0F);
    EXPECT_FLOAT_EQ(cone.innerAngle, 12.0F);
    EXPECT_FLOAT_EQ(cone.outerAngle, 30.0F);
    EXPECT_FLOAT_EQ(cone.range, 0.0F) << "no range key = no cutoff";
    expectVec3Near(cone.color, 1.0F, 1.0F, 1.0F); // no color key = white
    // Placement: rotate_x = 90 turns the local -Z emission axis onto +Y.
    const aether::Vec3 axis = cone.rotation * aether::Vec3{0.0F, 0.0F, -1.0F};
    expectVec3Near(axis, 0.0F, 1.0F, 0.0F);

    const LightDesc& panel = f.scene->lights[2];
    EXPECT_EQ(panel.type, LightDesc::Type::Rect);
    EXPECT_EQ(panel.unit, LightDesc::Unit::Nit);
    EXPECT_FLOAT_EQ(panel.intensity, 1500.0F);
    EXPECT_FLOAT_EQ(panel.halfWidth, 0.25F);
    EXPECT_FLOAT_EQ(panel.halfHeight, 0.5F);
    expectQuatNear(panel.rotation, 0.0F, 0.0F, 0.0F, 1.0F); // rotate is normalized
    // Local axes: X and Y span the emitting rectangle, -Z is its emission normal.
    const aether::Vec3 localX = panel.rotation * aether::Vec3{1.0F, 0.0F, 0.0F};
    const aether::Vec3 localY = panel.rotation * aether::Vec3{0.0F, 1.0F, 0.0F};
    const aether::Vec3 normal = panel.rotation * aether::Vec3{0.0F, 0.0F, -1.0F};
    expectVec3Near(localX, 1.0F, 0.0F, 0.0F);
    expectVec3Near(localY, 0.0F, 1.0F, 0.0F);
    expectVec3Near(normal, 0.0F, 0.0F, -1.0F);

    const LightDesc& sun = f.scene->lights[3];
    EXPECT_EQ(sun.type, LightDesc::Type::Directional);
    EXPECT_EQ(sun.unit, LightDesc::Unit::Lux);
    EXPECT_FLOAT_EQ(sun.intensity, 100000.0F);
    // Travel direction: rotation * (0, 0, -1) for a 45 degree turn about X.
    const aether::Vec3 travel = sun.rotation * aether::Vec3{0.0F, 0.0F, -1.0F};
    expectVec3Near(travel, 0.0F, 0.7071068F, -0.7071068F);
}

TEST(SceneParser, LightsSpotEdgeCasesAccepted) {
    // Equal angles are a spot without penumbra; 90 degrees is a hemisphere.
    NodeScene f{"lights_spot_edges", lightScene(R"toml([[light]]
name = "Hard"
type = "spot"
intensity = 10.0
unit = "candela"
inner_angle = 30.0
outer_angle = 30.0

[[light]]
name = "Half"
type = "spot"
intensity = 10.0
unit = "lumen"
inner_angle = 0.0
outer_angle = 90.0
)toml")};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->lights.size(), 2U);
    EXPECT_FLOAT_EQ(f.scene->lights[0].innerAngle, f.scene->lights[0].outerAngle);
    EXPECT_FLOAT_EQ(f.scene->lights[1].outerAngle, 90.0F);
}

TEST(SceneParser, LightsAreOptional) {
    // No [[light]]: nothing changes for an existing scene.
    NodeScene f{"lights_none", lightScene(R"toml([[mesh]]
name = "Box"
type = "box"
half_extents = [1.0, 1.0, 1.0]

[[instance]]
mesh = "Box"
)toml")};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    EXPECT_TRUE(f.scene->lights.empty());
    EXPECT_EQ(f.scene->instances.size(), 1U);
    const auto cornell = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(cornell.has_value());
    EXPECT_TRUE(cornell->lights.empty());
}

TEST(SceneParser, LightsRejectUnknownTypeUnitAndColorSpace) {
    expectLightRejected("light_type",
                        lightScene(R"([[light]]
name = "A"
type = "sphere"
intensity = 1.0
unit = "candela"
)"),
                        "unknown type 'sphere'");
    expectLightRejected("light_unit",
                        lightScene(R"([[light]]
name = "A"
type = "point"
intensity = 1.0
unit = "watt"
)"),
                        "unknown unit 'watt'");
    expectLightRejected("light_unit_missing",
                        lightScene(R"([[light]]
name = "A"
type = "point"
intensity = 1.0
)"),
                        "missing 'unit'");
    expectLightRejected("light_unit_fit",
                        lightScene(R"([[light]]
name = "A"
type = "rect"
intensity = 1.0
unit = "candela"
half_extents = [1.0, 1.0]
)"),
                        "does not fit type 'rect' (nit)");
    expectLightRejected("light_unit_fit2",
                        lightScene(R"([[light]]
name = "A"
type = "directional"
intensity = 1.0
unit = "nit"
)"),
                        "does not fit type 'directional' (lux)");
    expectLightRejected("light_unit_fit3",
                        lightScene(R"([[light]]
name = "A"
type = "point"
intensity = 1.0
unit = "lux"
)"),
                        "does not fit type 'point' (candela | lumen)");
    expectLightRejected("light_color_space",
                        lightScene(R"([[light]]
name = "A"
type = "point"
intensity = 1.0
unit = "candela"
color_space = "srgb_rec709_scene"
)"),
                        "unknown color_space");
}

TEST(SceneParser, LightsRejectBadNamesAndShapes) {
    expectLightRejected("light_name_missing",
                        lightScene(R"([[light]]
type = "point"
intensity = 1.0
unit = "candela"
)"),
                        "missing 'name'");
    expectLightRejected("light_name_twice",
                        lightScene(R"([[light]]
name = "A"
type = "point"
intensity = 1.0
unit = "candela"

[[light]]
name = "A"
type = "point"
intensity = 1.0
unit = "candela"
)"),
                        "more than one light");
    expectLightRejected("light_not_table", lightScene("light = [1]\n"), "must be a table");
    expectLightRejected("light_no_type",
                        lightScene(R"([[light]]
name = "A"
intensity = 1.0
unit = "candela"
)"),
                        "unknown type ''");
}

TEST(SceneParser, LightsRejectMissingNonFiniteAndNegativeIntensity) {
    const char* body = R"([[light]]
name = "A"
type = "point"
unit = "candela"
intensity = %s
)";
    for (const char* bad : {"0.0", "-5.0", "nan", "inf", "-inf", "\"bright\"", "true"}) {
        char toml[256];
        std::snprintf(toml, sizeof(toml), body, bad);
        expectLightRejected("light_intensity", toml, "intensity must be a finite number above 0");
    }
    expectLightRejected("light_intensity_missing",
                        lightScene(R"([[light]]
name = "A"
type = "point"
unit = "candela"
)"),
                        "missing 'intensity'");
}

TEST(SceneParser, LightsRejectBadColorAndPlacement) {
    expectLightRejected("light_color_negative",
                        lightScene(R"([[light]]
name = "A"
type = "point"
intensity = 1.0
unit = "candela"
color = [1.0, -0.1, 0.5]
)"),
                        "color must hold 3 finite numbers, each at least 0");
    expectLightRejected("light_color_nan",
                        lightScene(R"([[light]]
name = "A"
type = "point"
intensity = 1.0
unit = "candela"
color = [1.0, nan, 0.5]
)"),
                        "color must hold 3 finite numbers, each at least 0");
    expectLightRejected("light_color_arity",
                        lightScene(R"([[light]]
name = "A"
type = "point"
intensity = 1.0
unit = "candela"
color = [1.0, 0.5]
)"),
                        "color must hold 3 finite numbers, each at least 0");
    expectLightRejected("light_translate",
                        lightScene(R"([[light]]
name = "A"
type = "point"
translate = [1.0, 2.0]
intensity = 1.0
unit = "candela"
)"),
                        "translate must hold 3 finite numbers");
    expectLightRejected("light_translate_inf",
                        lightScene(R"([[light]]
name = "A"
type = "point"
translate = [1.0, inf, 0.0]
intensity = 1.0
unit = "candela"
)"),
                        "translate must hold 3 finite numbers");
    expectLightRejected("light_scale",
                        lightScene(R"([[light]]
name = "A"
type = "point"
scale = [2.0, 2.0, 2.0]
intensity = 1.0
unit = "candela"
)"),
                        "a light has no scale");
    expectLightRejected("light_rotate_zero",
                        lightScene(R"([[light]]
name = "A"
type = "spot"
rotate = [0.0, 0.0, 0.0, 0.0]
intensity = 1.0
unit = "candela"
inner_angle = 10.0
outer_angle = 20.0
)"),
                        "forming a rotation");
    expectLightRejected("light_rotate_nan",
                        lightScene(R"([[light]]
name = "A"
type = "spot"
rotate = [nan, 0.0, 0.0, 1.0]
intensity = 1.0
unit = "candela"
inner_angle = 10.0
outer_angle = 20.0
)"),
                        "forming a rotation");
    expectLightRejected("light_euler_type",
                        lightScene(R"([[light]]
name = "A"
type = "spot"
rotate_x = "up"
intensity = 1.0
unit = "candela"
inner_angle = 10.0
outer_angle = 20.0
)"),
                        "rotate_x must be a finite number");
}

TEST(SceneParser, LightsRejectBadTypeSpecificValues) {
    // rect
    expectLightRejected("light_rect_missing",
                        lightScene(R"([[light]]
name = "A"
type = "rect"
intensity = 1.0
unit = "nit"
)"),
                        "missing 'half_extents'");
    expectLightRejected("light_rect_zero",
                        lightScene(R"([[light]]
name = "A"
type = "rect"
intensity = 1.0
unit = "nit"
half_extents = [0.0, 1.0]
)"),
                        "half_extents must hold 2 finite numbers, each above 0");
    expectLightRejected("light_rect_negative",
                        lightScene(R"([[light]]
name = "A"
type = "rect"
intensity = 1.0
unit = "nit"
half_extents = [-1.0, 1.0]
)"),
                        "half_extents must hold 2 finite numbers, each above 0");
    expectLightRejected("light_rect_nan",
                        lightScene(R"([[light]]
name = "A"
type = "rect"
intensity = 1.0
unit = "nit"
half_extents = [nan, 1.0]
)"),
                        "half_extents must hold 2 finite numbers, each above 0");
    expectLightRejected("light_rect_arity",
                        lightScene(R"([[light]]
name = "A"
type = "rect"
intensity = 1.0
unit = "nit"
half_extents = [1.0, 1.0, 1.0]
)"),
                        "half_extents must hold 2 finite numbers, each above 0");
    expectLightRejected("light_rect_on_point",
                        lightScene(R"([[light]]
name = "A"
type = "point"
intensity = 1.0
unit = "candela"
half_extents = [1.0, 1.0]
)"),
                        "half_extents is a rect key");
    // spot
    expectLightRejected("light_spot_missing",
                        lightScene(R"([[light]]
name = "A"
type = "spot"
intensity = 1.0
unit = "candela"
inner_angle = 10.0
)"),
                        "missing 'outer_angle'");
    expectLightRejected("light_spot_order",
                        lightScene(R"([[light]]
name = "A"
type = "spot"
intensity = 1.0
unit = "candela"
inner_angle = 40.0
outer_angle = 30.0
)"),
                        "inner_angle must not exceed outer_angle");
    expectLightRejected("light_spot_outer_zero",
                        lightScene(R"([[light]]
name = "A"
type = "spot"
intensity = 1.0
unit = "candela"
inner_angle = 0.0
outer_angle = 0.0
)"),
                        "outer_angle must be above 0");
    expectLightRejected("light_spot_too_wide",
                        lightScene(R"([[light]]
name = "A"
type = "spot"
intensity = 1.0
unit = "candela"
inner_angle = 10.0
outer_angle = 95.0
)"),
                        "inside [0, 90]");
    expectLightRejected("light_spot_negative",
                        lightScene(R"([[light]]
name = "A"
type = "spot"
intensity = 1.0
unit = "candela"
inner_angle = -1.0
outer_angle = 30.0
)"),
                        "inside [0, 90]");
    expectLightRejected("light_spot_inf",
                        lightScene(R"([[light]]
name = "A"
type = "spot"
intensity = 1.0
unit = "candela"
inner_angle = 0.0
outer_angle = inf
)"),
                        "inside [0, 90]");
    expectLightRejected("light_angles_on_rect",
                        lightScene(R"([[light]]
name = "A"
type = "rect"
intensity = 1.0
unit = "nit"
half_extents = [1.0, 1.0]
outer_angle = 30.0
)"),
                        "outer_angle is a spot key");
    // range
    expectLightRejected("light_range_negative",
                        lightScene(R"([[light]]
name = "A"
type = "point"
intensity = 1.0
unit = "candela"
range = -1.0
)"),
                        "range must be a finite number, at least 0");
    expectLightRejected("light_range_nan",
                        lightScene(R"([[light]]
name = "A"
type = "point"
intensity = 1.0
unit = "candela"
range = nan
)"),
                        "range must be a finite number, at least 0");
    expectLightRejected("light_range_on_rect",
                        lightScene(R"([[light]]
name = "A"
type = "rect"
intensity = 1.0
unit = "nit"
range = 3.0
half_extents = [1.0, 1.0]
)"),
                        "range is a point or spot key");
}

TEST(SceneParser, LightsExampleSceneAndValidatorFixturesParse) {
    // The committed example: the Cornell box with an analytic ceiling panel of
    // the same 10000 cd/m2 the classic scene's emissive panel carries, a warm
    // point light and a spot aimed at the short box.
    const auto example = aether::SceneParser::parse(assetsDir() / "analytic_lights.scene.toml");
    ASSERT_TRUE(example.has_value());
    ASSERT_EQ(example->lights.size(), 3U);
    EXPECT_EQ(example->lights[0].name, "CeilingPanel");
    EXPECT_EQ(example->lights[0].type, LightDesc::Type::Rect);
    EXPECT_FLOAT_EQ(example->lights[0].intensity, 10000.0F);
    EXPECT_FLOAT_EQ(example->lights[0].halfWidth, 0.65F);
    EXPECT_FLOAT_EQ(example->lights[0].halfHeight, 0.525F);
    // Emits straight down (rotate_x = -90 turns local -Z onto -Y).
    const aether::Vec3 down = example->lights[0].rotation * aether::Vec3{0.0F, 0.0F, -1.0F};
    expectVec3Near(down, 0.0F, -1.0F, 0.0F);
    // Its local X is the world X: the 1.3 m width spans x, the 1.05 m depth z.
    const aether::Vec3 across = example->lights[0].rotation * aether::Vec3{1.0F, 0.0F, 0.0F};
    expectVec3Near(across, 1.0F, 0.0F, 0.0F);
    EXPECT_EQ(example->lights[1].type, LightDesc::Type::Point);
    EXPECT_EQ(example->lights[2].type, LightDesc::Type::Spot);
    // The spot aims from (1.9, 5.2, 0.9) at the top of the short box (-0.93, 1.6, 1.69).
    const aether::Vec3 aim = example->lights[2].rotation * aether::Vec3{0.0F, 0.0F, -1.0F};
    const aether::Vec3 toBox{-0.93F - 1.9F, 1.6F - 5.2F, 1.69F - 0.9F};
    const float length = std::sqrt(toBox.x * toBox.x + toBox.y * toBox.y + toBox.z * toBox.z);
    expectVec3Near(aim, toBox.x / length, toBox.y / length, toBox.z / length);
    // The classic scene it is compared with has no analytic lights.
    const auto classic = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(classic.has_value());
    EXPECT_TRUE(classic->lights.empty());

    // The validator fixtures: one light per type, and a rejected twin.
    const std::filesystem::path fixtures = assetsDir().parent_path() / "tests" / "fixtures";
    const auto four = aether::SceneParser::parse(fixtures / "lights.scene.toml");
    ASSERT_TRUE(four.has_value());
    ASSERT_EQ(four->lights.size(), 4U);
    EXPECT_EQ(four->lights[0].type, LightDesc::Type::Point);
    EXPECT_EQ(four->lights[1].type, LightDesc::Type::Spot);
    EXPECT_EQ(four->lights[2].type, LightDesc::Type::Rect);
    EXPECT_EQ(four->lights[3].type, LightDesc::Type::Directional);
    std::string error;
    EXPECT_FALSE(aether::SceneParser::parse(fixtures / "lights_rejected.scene.toml", &error).has_value());
    EXPECT_NE(error.find("does not fit type 'rect'"), std::string::npos) << error;
}

} // namespace
