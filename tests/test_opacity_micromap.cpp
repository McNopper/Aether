// Opacity-micromap asset tests: the `.micromap.toml` + `.omm` pair parsed by
// OmmImporter, the material's `map_opacity` input, and the mesh-level
// `opacity_micromaps` reference that binds a baked micromap to an OBJ group.
//
// The micromap is only ever an ACCELERATOR for OpenPBR `geometry_opacity`, so
// these tests pin the structural contract a consumer relies on: one index per
// base triangle, record ordinals resolving into `triangles`, specials preserved
// as negative values, and the usage histogram agreeing with the record count.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#include "aether/format/MaterialLibrary.hpp"
#include "aether/format/OmmImporter.hpp"
#include "aether/format/SceneParser.hpp"
#include "aether/types/OpacityMicromap.hpp"

namespace {

std::filesystem::path assetsDir() {
    return std::filesystem::path{AETHER_ASSETS_DIR};
}

/// Write @p text to a temp file and return its path (removed by the caller).
std::filesystem::path writeTemp(const std::string& name, const std::string& text) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary);
    out << text;
    return path;
}

} // namespace

TEST(OmmImporter, ParsesShippedShaderballCheckerAsset) {
    const auto data = aether::OmmImporter::parse(assetsDir() / "shaderball_checker_omm.micromap.toml");
    ASSERT_TRUE(data.has_value());
    ASSERT_FALSE(data->groups.empty());

    for (const aether::OpacityMicromapGroup& g : data->groups) {
        // One micromap index per base triangle — this is what the BLAS build
        // asserts against its primitive count.
        EXPECT_EQ(g.micromapIndices.size(), g.triangleCount) << "group " << g.name;

        std::uint32_t records = 0;
        for (const std::int32_t idx : g.micromapIndices) {
            if (idx < 0) {
                // Specials are the four Vulkan reserved values.
                EXPECT_GE(idx, -4) << "group " << g.name;
                continue;
            }
            EXPECT_LT(static_cast<std::size_t>(idx), g.triangles.size()) << "group " << g.name;
            ++records;
        }
        EXPECT_EQ(records, g.triangles.size()) << "group " << g.name;

        // Every record's data offset must address the packed-state buffer.
        for (const aether::OpacityMicromapTriangle& t : g.triangles) {
            EXPECT_LT(t.dataOffset, g.dataBits.size()) << "group " << g.name;
            EXPECT_TRUE(t.format == 1 || t.format == 2) << "group " << g.name;
        }

        // The usage histogram is build input for vkGetMicromapBuildSizesEXT; its
        // total must equal the number of records it describes.
        std::uint32_t declared = 0;
        for (const aether::OpacityMicromapUsage& u : g.usage) {
            declared += u.count;
        }
        EXPECT_EQ(declared, g.triangles.size()) << "group " << g.name;
    }
}

TEST(OmmImporter, RejectsMissingAndMalformedAssets) {
    EXPECT_FALSE(aether::OmmImporter::parse(assetsDir() / "does_not_exist.micromap.toml").has_value());

    // Descriptor naming a group the sidecar does not contain.
    const auto omm = writeTemp("aether_omm_test.omm", "G Present\nS -2\n");
    const auto toml = writeTemp("aether_omm_test.micromap.toml",
                                "[data]\nfile = \"aether_omm_test.omm\"\n\n"
                                "[[group]]\nname = \"Absent\"\ntriangle_count = 1\nusage = []\n");
    EXPECT_FALSE(aether::OmmImporter::parse(toml).has_value());

    // Record count drifting from the declared triangle_count.
    const auto toml2 = writeTemp("aether_omm_test2.micromap.toml",
                                 "[data]\nfile = \"aether_omm_test.omm\"\n\n"
                                 "[[group]]\nname = \"Present\"\ntriangle_count = 7\nusage = []\n");
    EXPECT_FALSE(aether::OmmImporter::parse(toml2).has_value());

    std::filesystem::remove(omm);
    std::filesystem::remove(toml);
    std::filesystem::remove(toml2);
}

TEST(MaterialLibrary, ParsesMapOpacity) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "shaderball_checker.materials.toml"));

    const auto hero = lib.get("CheckerBall");
    ASSERT_TRUE(hero.has_value());
    EXPECT_EQ(hero->map_opacity.path, "checker_opacity.png");
    EXPECT_EQ(hero->map_opacity.colorSpace, aether::TextureColorSpace::Data);
    EXPECT_FLOAT_EQ(hero->geometry_opacity, 1.0F);

    // A material without the map leaves the slot empty — that is the signal the
    // loader uses to decide a mesh needs no cutout handling at all.
    const auto neutral = lib.get("NeutralBall");
    ASSERT_TRUE(neutral.has_value());
    EXPECT_TRUE(neutral->map_opacity.empty());
}

TEST(SceneParser, ParsesOpacityMicromapReferences) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "shaderball_checker.scene.toml");
    ASSERT_TRUE(scene.has_value());

    const aether::MeshDesc* plain = nullptr;
    const aether::MeshDesc* accelerated = nullptr;
    for (const aether::MeshDesc& m : scene->meshes) {
        if (m.name == "shader_ball_plain") {
            plain = &m;
        } else if (m.name == "shader_ball_omm") {
            accelerated = &m;
        }
    }
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(accelerated, nullptr);

    // The two meshes are the same OBJ; only one declares the acceleration.
    EXPECT_EQ(plain->objPath, accelerated->objPath);
    EXPECT_TRUE(plain->opacityMicromaps.empty());
    EXPECT_EQ(accelerated->opacityMicromaps.size(), 2U);
    for (const auto& [group, file] : accelerated->opacityMicromaps) {
        EXPECT_TRUE(group == "BallSurface" || group == "BaseFoot");
        EXPECT_EQ(file, "shaderball_checker_omm.micromap.toml");
    }
}
