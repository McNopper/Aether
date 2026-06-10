#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <gtest/gtest.h>

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

TEST(SceneParser, ParsesClassicCornellScene) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(scene.has_value());
}

TEST(SceneParser, ReadsCameraParameters) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(scene.has_value());

    ASSERT_TRUE(scene->camera.position.has_value());
    EXPECT_FLOAT_EQ(scene->camera.position->x, 278.0F);
    EXPECT_FLOAT_EQ(scene->camera.position->y, 273.0F);
    EXPECT_FLOAT_EQ(scene->camera.position->z, -800.0F);

    ASSERT_TRUE(scene->camera.lookAt.has_value());
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

TEST(SceneParser, ParsesGeometryBlocks) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(scene.has_value());

    // One OBJ instance + two procedural boxes.
    ASSERT_EQ(scene->geometry.size(), 3U);

    const auto& instance = scene->geometry[0];
    EXPECT_EQ(instance.kind, aether::GeometryBlock::Kind::Object);
    EXPECT_EQ(instance.objPath, "cornell.obj");
    EXPECT_EQ(instance.groupMaterials.at("LeftWall"), "RedWall");
    EXPECT_EQ(instance.groupMaterials.at("RightWall"), "GreenWall");

    EXPECT_EQ(scene->geometry[1].kind, aether::GeometryBlock::Kind::Box);
    EXPECT_EQ(scene->geometry[2].kind, aether::GeometryBlock::Kind::Box);
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
    EXPECT_EQ(*scene->tonemapper, "agx");
}

TEST(SceneParser, ResolvesEnvironmentMapFromReferencedRenderPreset) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "meadow_scene.scene.toml");
    ASSERT_TRUE(scene.has_value());
    ASSERT_TRUE(scene->envMapFile.has_value());
    EXPECT_EQ(*scene->envMapFile, "meadow_2_4k.exr");
    ASSERT_TRUE(scene->spp.has_value());
    EXPECT_EQ(*scene->spp, 16U);
    ASSERT_TRUE(scene->maxDepth.has_value());
    EXPECT_EQ(*scene->maxDepth, 10U);
}

// An inline key in a [render]/[camera]/[tonemap] table must override the value
// from its referenced preset ("local wins").
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
                    "look_at = [0.0, 0.0, 0.0]\n"
                    "vertical_field_of_view = 45.0\n"
                    "ev100 = 9.0\n"};
    ASSERT_TRUE(f.scene.has_value());
    ASSERT_TRUE(f.scene->camera.position.has_value());
    EXPECT_FLOAT_EQ(f.scene->camera.position->z, 3.0F);
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

} // namespace
