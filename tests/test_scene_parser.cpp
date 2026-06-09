#include <filesystem>
#include <gtest/gtest.h>

#include "aether/format/SceneParser.hpp"

namespace {

std::filesystem::path assetsDir() {
    return std::filesystem::path{AETHER_ASSETS_DIR};
}

TEST(SceneParser, ParsesClassicCornellScene) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene");
    ASSERT_TRUE(scene.has_value());
}

TEST(SceneParser, ReadsCameraParameters) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene");
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
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene");
    ASSERT_TRUE(scene.has_value());

    ASSERT_TRUE(scene->spp.has_value());
    EXPECT_EQ(*scene->spp, 64U);
    ASSERT_TRUE(scene->maxDepth.has_value());
    EXPECT_EQ(*scene->maxDepth, 8U);
    ASSERT_TRUE(scene->camera.ev100.has_value());
    EXPECT_FLOAT_EQ(*scene->camera.ev100, 7.0F);
}

TEST(SceneParser, RecordsMaterialLibrary) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene");
    ASSERT_TRUE(scene.has_value());

    ASSERT_FALSE(scene->mtllibs.empty());
    EXPECT_EQ(scene->mtllibs.front(), "cornell.mtlx");
}

TEST(SceneParser, ParsesGeometryBlocks) {
    const auto scene = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene");
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
    const auto scene = aether::SceneParser::parse(assetsDir() / "no_such_scene.scene");
    EXPECT_FALSE(scene.has_value());
}

} // namespace
