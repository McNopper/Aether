#include <filesystem>
#include <gtest/gtest.h>

#include "aether/format/MaterialLibrary.hpp"

namespace {

std::filesystem::path assetsDir() {
    return std::filesystem::path{AETHER_ASSETS_DIR};
}

TEST(MaterialLibrary, LoadsCornellLibrary) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "cornell.materials.toml"));
    EXPECT_FALSE(lib.empty());
    EXPECT_EQ(lib.size(), 9U);
}

TEST(MaterialLibrary, ResolvesNamedMaterials) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "cornell.materials.toml"));

    EXPECT_TRUE(lib.get("WhiteWall").has_value());
    EXPECT_TRUE(lib.get("RedWall").has_value());
    EXPECT_TRUE(lib.get("Glass").has_value());
    EXPECT_FALSE(lib.get("DoesNotExist").has_value());
}

TEST(MaterialLibrary, GlassIsTransmissive) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "cornell.materials.toml"));

    const auto glass = lib.get("Glass");
    ASSERT_TRUE(glass.has_value());
    EXPECT_GT(glass->transmission_weight, 0.0F);
}

TEST(MaterialLibrary, MetalsAreMetallic) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "cornell.materials.toml"));

    const auto gold = lib.get("RoughGold");
    ASSERT_TRUE(gold.has_value());
    EXPECT_GT(gold->base_metalness, 0.5F);
}

TEST(MaterialLibrary, AreaLightEmits) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "cornell.materials.toml"));

    const auto light = lib.get("AreaLight");
    ASSERT_TRUE(light.has_value());
    EXPECT_GT(light->emission_luminance, 0.0F);
}

TEST(MaterialLibrary, MissingFileFailsToLoad) {
    aether::MaterialLibrary lib;
    EXPECT_FALSE(lib.load(assetsDir() / "this_file_does_not_exist.materials.toml"));
    EXPECT_TRUE(lib.empty());
}

TEST(MaterialLibrary, GetOrDefaultReturnsDefaultForUnknown) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "cornell.materials.toml"));

    const aether::MaterialDesc def = lib.getOrDefault("UnknownMaterialName");
    EXPECT_FLOAT_EQ(def.base_weight, 1.0F);
}

// Smoke test: every shipped .materials.toml must load and contain materials.
TEST(MaterialLibrary, AllShippedLibrariesLoad) {
    namespace fs = std::filesystem;
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(assetsDir())) {
        const auto& p = entry.path();
        if (p.extension() != ".toml" || !p.stem().string().ends_with(".materials")) {
            continue;
        }
        ++count;
        aether::MaterialLibrary lib;
        EXPECT_TRUE(lib.load(p)) << "failed to load " << p.string();
        EXPECT_FALSE(lib.empty()) << "no materials in " << p.string();
    }
    EXPECT_GT(count, 0U);
}

} // namespace
