#include <filesystem>
#include <fstream>
#include <string_view>
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

// ── Material model tag & per-material color space ───────────────────────────

// Writes a throwaway .materials.toml and loads it.
struct MaterialFixture {
    std::filesystem::path file;
    aether::MaterialLibrary lib;
    bool loaded = false;

    explicit MaterialFixture(std::string_view body) {
        namespace fs = std::filesystem;
        file = fs::temp_directory_path() / "aether_test.materials.toml";
        {
            std::ofstream out(file);
            out << body;
        }
        loaded = lib.load(file);
    }
    ~MaterialFixture() { std::filesystem::remove(file); }
};

TEST(MaterialLibrary, ModelDefaultsToOpenPbr) {
    MaterialFixture f{"[Mat]\nbase_color = [0.5, 0.5, 0.5]\n"};
    ASSERT_TRUE(f.loaded);
    const auto mat = f.lib.get("Mat");
    ASSERT_TRUE(mat.has_value());
    EXPECT_EQ(mat->model, "openpbr");
}

TEST(MaterialLibrary, ModelFileLevelAndPerMaterialOverride) {
    MaterialFixture f{"model = \"openpbr\"\n"
                      "[A]\nbase_color = [0.5, 0.5, 0.5]\n"
                      "[B]\nmodel = \"openpbr\"\nbase_color = [0.5, 0.5, 0.5]\n"};
    ASSERT_TRUE(f.loaded);
    EXPECT_EQ(f.lib.size(), 2U);
    EXPECT_EQ(f.lib.get("A")->model, "openpbr");
    EXPECT_EQ(f.lib.get("B")->model, "openpbr");
}

TEST(MaterialLibrary, UnknownModelSkipsMaterial) {
    MaterialFixture f{"[Future]\nmodel = \"disney_v6\"\nbase_color = [0.5, 0.5, 0.5]\n"
                      "[Ok]\nbase_color = [0.5, 0.5, 0.5]\n"};
    ASSERT_TRUE(f.loaded);
    EXPECT_FALSE(f.lib.get("Future").has_value());
    EXPECT_TRUE(f.lib.get("Ok").has_value());
}

TEST(MaterialLibrary, PerMaterialColorSpaceOverride) {
    MaterialFixture f{"colorspace = \"lin_rec709_scene\"\n"
                      "[Wide]\ncolorspace = \"lin_rec2020_scene\"\nbase_color = [0.5, 0.5, 0.5]\n"
                      "[Narrow]\nbase_color = [0.5, 0.5, 0.5]\n"};
    ASSERT_TRUE(f.loaded);
    EXPECT_EQ(f.lib.get("Wide")->inputColorSpace, aether::MaterialColorSpace::LinRec2020);
    EXPECT_EQ(f.lib.get("Narrow")->inputColorSpace, aether::MaterialColorSpace::LinRec709);
}

TEST(MaterialLibrary, MixedPrimariesPerMaterialIsRejected) {
    // Rec.2020 material with a Rec.709-encoded color texture → gamut mixing.
    MaterialFixture f{"[Mixed]\n"
                      "colorspace = \"lin_rec2020_scene\"\n"
                      "map_base_color = \"t.png\"\n"
                      "map_base_color_colorspace = \"srgb_rec709_scene\"\n"};
    ASSERT_TRUE(f.loaded);
    EXPECT_FALSE(f.lib.get("Mixed").has_value());
}

TEST(MaterialLibrary, MatchingPrimariesWithEncodedTextureIsAccepted) {
    // sRGB-encoded Rec.709 texture in a lin_rec709_scene material: same
    // primaries, different encoding — allowed. Data maps are always exempt.
    MaterialFixture f{"[Ok]\n"
                      "colorspace = \"lin_rec709_scene\"\n"
                      "map_base_color = \"t.png\"\n"
                      "map_base_color_colorspace = \"srgb_rec709_scene\"\n"
                      "map_normal = \"n.png\"\n"
                      "map_normal_colorspace = \"data\"\n"};
    ASSERT_TRUE(f.loaded);
    const auto mat = f.lib.get("Ok");
    ASSERT_TRUE(mat.has_value());
    EXPECT_EQ(mat->map_base_color.colorSpace, aether::TextureColorSpace::SrgbRec709Scene);
    EXPECT_EQ(mat->map_normal.colorSpace, aether::TextureColorSpace::Data);
}

} // namespace
