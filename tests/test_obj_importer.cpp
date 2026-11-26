#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <vector>

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

TEST(ObjImporter, UsemtlSplitsAreAddressableByTheirMaterialName) {
    // The committed Blender multi-material export: one `o` line, then one
    // `usemtl` per material slot. The split is only assignable if each sub-mesh
    // carries the label that distinguishes it from its siblings.
    const auto groups = aether::ObjImporter::parse(assetsDir() / "FeatureCube.obj");
    ASSERT_TRUE(groups.has_value());
    ASSERT_EQ(groups->size(), 2U);

    std::vector<std::string> materialNames;
    for (const auto& group : *groups) {
        // The object name is shared by both sub-meshes - it cannot tell them apart.
        EXPECT_EQ(group.name, "FeatureCube");
        EXPECT_EQ(group.mesh.indices.size(), 18U) << "six triangles per slot";
        materialNames.push_back(group.materialName);
    }
    ASSERT_EQ(materialNames.size(), 2U);
    EXPECT_EQ(materialNames[0], "MatAlpha");
    EXPECT_EQ(materialNames[1], "MatBeta");
}

TEST(ObjImporter, SubmeshsWithoutUsemtlCarryNoMaterialName) {
    // A hand-authored OBJ groups by `g`/`o` only - nothing to carry.
    const auto groups = aether::ObjImporter::parse(assetsDir() / "cornell.obj");
    ASSERT_TRUE(groups.has_value());
    ASSERT_GT(groups->size(), 1U);
    for (const auto& group : *groups) {
        EXPECT_TRUE(group.materialName.empty());
    }
}

TEST(ObjImporter, MissingFileReturnsNullopt) {
    const auto groups = aether::ObjImporter::parse(assetsDir() / "no_such_mesh.obj");
    EXPECT_FALSE(groups.has_value());
}

} // namespace
