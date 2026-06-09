#include <filesystem>
#include <gtest/gtest.h>

#include "aether/format/MaterialLibrary.hpp"

namespace {

std::filesystem::path assetsDir() {
    return std::filesystem::path{AETHER_ASSETS_DIR};
}

TEST(MaterialLibrary, LoadsCornellLibrary) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "cornell.mtlx"));
    EXPECT_FALSE(lib.empty());
    EXPECT_EQ(lib.size(), 9U);
}

TEST(MaterialLibrary, ResolvesNamedMaterials) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "cornell.mtlx"));

    EXPECT_TRUE(lib.get("WhiteWall").has_value());
    EXPECT_TRUE(lib.get("RedWall").has_value());
    EXPECT_TRUE(lib.get("Glass").has_value());
    EXPECT_FALSE(lib.get("DoesNotExist").has_value());
}

TEST(MaterialLibrary, GlassIsTransmissive) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "cornell.mtlx"));

    const auto glass = lib.get("Glass");
    ASSERT_TRUE(glass.has_value());
    EXPECT_GT(glass->transmission_weight, 0.0F);
}

TEST(MaterialLibrary, MetalsAreMetallic) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "cornell.mtlx"));

    const auto gold = lib.get("RoughGold");
    ASSERT_TRUE(gold.has_value());
    EXPECT_GT(gold->base_metalness, 0.5F);
}

TEST(MaterialLibrary, AreaLightEmits) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "cornell.mtlx"));

    const auto light = lib.get("AreaLight");
    ASSERT_TRUE(light.has_value());
    EXPECT_GT(light->emission_luminance, 0.0F);
}

TEST(MaterialLibrary, MissingFileFailsToLoad) {
    aether::MaterialLibrary lib;
    EXPECT_FALSE(lib.load(assetsDir() / "this_file_does_not_exist.mtlx"));
    EXPECT_TRUE(lib.empty());
}

TEST(MaterialLibrary, GetOrDefaultReturnsDefaultForUnknown) {
    aether::MaterialLibrary lib;
    ASSERT_TRUE(lib.load(assetsDir() / "cornell.mtlx"));

    const aether::MaterialDesc def = lib.getOrDefault("UnknownMaterialName");
    EXPECT_FLOAT_EQ(def.base_weight, 1.0F);
}

} // namespace
