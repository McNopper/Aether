#include <filesystem>
#include <gtest/gtest.h>

#include "aether/format/ObjImporter.hpp"

namespace {

std::filesystem::path assetsDir() {
    return std::filesystem::path{AETHER_ASSETS_DIR};
}

TEST(ObjImporter, ParsesCornellBox) {
    const auto groups = aether::ObjImporter::parse(assetsDir() / "cornell.obj");
    ASSERT_TRUE(groups.has_value());
    EXPECT_FALSE(groups->empty());
}

TEST(ObjImporter, GroupsHaveValidIndexedGeometry) {
    const auto groups = aether::ObjImporter::parse(assetsDir() / "cornell.obj");
    ASSERT_TRUE(groups.has_value());

    bool anyGeometry = false;
    for (const auto& group : *groups) {
        // Every emitted index must reference a valid deduplicated vertex.
        for (const std::uint32_t idx : group.mesh.indices) {
            ASSERT_LT(idx, group.mesh.vertices.size());
        }
        // Indices describe triangles.
        EXPECT_EQ(group.mesh.indices.size() % 3U, 0U);
        if (!group.mesh.empty()) {
            anyGeometry = true;
        }
    }
    EXPECT_TRUE(anyGeometry);
}

TEST(ObjImporter, NamedGroupsArePreserved) {
    const auto groups = aether::ObjImporter::parse(assetsDir() / "cornell.obj");
    ASSERT_TRUE(groups.has_value());

    // The Cornell box OBJ splits into named groups (Floor, Ceiling, walls, …).
    EXPECT_GT(groups->size(), 1U);
    for (const auto& group : *groups) {
        EXPECT_FALSE(group.name.empty());
    }
}

TEST(ObjImporter, MissingFileReturnsNullopt) {
    const auto groups = aether::ObjImporter::parse(assetsDir() / "no_such_mesh.obj");
    EXPECT_FALSE(groups.has_value());
}

} // namespace
