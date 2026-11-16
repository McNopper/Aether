// ANI5 (morph targets / blendshapes) unit receipts - mirrors the ANI4 skin
// suite: parser validation, the morph-weight track grammar, the
// evaluateMorphWeights tick API and the deformMorphs CPU reference + bind-time
// vertex-count validator. No GPU, no renderers.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aether/format/Morphing.hpp"
#include "aether/format/SceneAnimator.hpp"
#include "aether/format/SceneParser.hpp"
#include "aether/types/MeshData.hpp"
#include "aether/types/SceneDesc.hpp"

namespace {

constexpr float kEps = 1.0e-5F;

// Writes a scene TOML in a throwaway directory and parses it with the
// error-reporting overload (the test_scene_parser NodeScene pattern).
struct MorphScene {
    std::filesystem::path dir;
    std::optional<aether::SceneDesc> scene;
    std::string error;

    MorphScene(std::string_view name, std::string_view tomlText) {
        namespace fs = std::filesystem;
        dir = fs::temp_directory_path() / ("aether_morph_" + std::string{name});
        fs::create_directories(dir);
        {
            std::ofstream scn(dir / "s.scene.toml");
            scn << tomlText;
        }
        scene = aether::SceneParser::parse(dir / "s.scene.toml", &error);
    }
    ~MorphScene() { std::filesystem::remove_all(dir); }
};

// Procedural box mesh - no OBJ import needed for parser/track tests (the
// vertex COUNT binds later, loader-side, like ANI4).
constexpr std::string_view kBoxMesh = "[[mesh]]\nname = \"m\"\ntype = \"box\"\nhalf_extents = [1, 1, 1]\n\n";

std::string twoTargetScene(bool withTrack, bool withAuthoredWeight) {
    std::string toml;
    if (withTrack) {
        toml += "[animation_clock]\nfps = 30\n\n";
    }
    toml += std::string{kBoxMesh};
    toml += "[[morph]]\n"
            "mesh = \"m\"\n"
            "name = \"smile\"\n";
    if (withAuthoredWeight) {
        toml += "weight = 0.5\n";
    }
    toml += "deltas = [[0, 0, 0], [1, 0, 0]]\n"
            "\n"
            "[[morph]]\n"
            "mesh = \"m\"\n"
            "name = \"blink\"\n"
            "deltas = [[0, 0, 0], [0, 2, 0]]\n";
    if (withTrack) {
        toml += "\n[[animation]]\n"
                "target = \"morphs/m/smile\"\n"
                "interpolation = \"linear\"\n"
                "times = [0.0, 1.0]\n"
                "values = [0.0, 1.0]\n";
    }
    return toml;
}

// Two-vertex procedural mesh for the CPU reference.
aether::MeshData twoVertexMesh() {
    aether::MeshData mesh;
    aether::Vertex a{};
    a.position = aether::Vec3{0.0F, 0.0F, 0.0F};
    aether::Vertex b{};
    b.position = aether::Vec3{1.0F, 0.0F, 0.0F};
    mesh.vertices = {a, b};
    return mesh;
}

} // namespace

// ── parser ───────────────────────────────────────────────────────────────────

TEST(MorphParser, ParsesTargetsWithParallelWeights) {
    MorphScene s("ok", twoTargetScene(/*withTrack=*/false, /*withAuthoredWeight=*/true));
    ASSERT_TRUE(s.scene.has_value()) << s.error;
    ASSERT_EQ(s.scene->morphs.size(), 2U);
    EXPECT_EQ(s.scene->morphs[0].meshName, "m");
    EXPECT_EQ(s.scene->morphs[0].name, "smile");
    ASSERT_EQ(s.scene->morphs[0].deltas.size(), 2U);
    EXPECT_NEAR(s.scene->morphs[0].deltas[1].x, 1.0F, kEps);
    // Weights stay parallel: authored 0.5 where given, default 0 otherwise.
    ASSERT_EQ(s.scene->morphWeights.size(), 2U);
    EXPECT_FLOAT_EQ(s.scene->morphWeights[0], 0.5F);
    EXPECT_FLOAT_EQ(s.scene->morphWeights[1], 0.0F);
    EXPECT_EQ(s.scene->morphs[1].name, "blink");
}

TEST(MorphParser, RejectsUnknownMesh) {
    MorphScene s("mesh", "[[morph]]\nmesh = \"ghost\"\nname = \"t\"\ndeltas = [[0, 0, 0]]\n");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("unknown mesh 'ghost'"), std::string::npos) << s.error;
}

TEST(MorphParser, RejectsDuplicateTargetOnSameMesh) {
    MorphScene s("dup",
                 "[[mesh]]\nname = \"m\"\ntype = \"box\"\nhalf_extents = [1, 1, 1]\n\n"
                 "[[morph]]\n"
                 "mesh = \"m\"\n"
                 "name = \"t\"\n"
                 "deltas = [[0, 0, 0]]\n\n"
                 "[[morph]]\n"
                 "mesh = \"m\"\n"
                 "name = \"t\"\n"
                 "deltas = [[1, 1, 1]]\n");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("duplicate target name"), std::string::npos) << s.error;
}

TEST(MorphParser, RejectsSphereMesh) {
    MorphScene s("sphere",
                 "[[mesh]]\nname = \"ball\"\ntype = \"sphere\"\nradius = 0.5\n\n"
                 "[[morph]]\n"
                 "mesh = \"ball\"\n"
                 "name = \"t\"\n"
                 "deltas = [[0, 0, 0]]\n");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("procedural sphere mesh"), std::string::npos) << s.error;
}

TEST(MorphParser, RejectsMissingOrEmptyDeltas) {
    MorphScene s("nodelta",
                 "[[mesh]]\nname = \"m\"\ntype = \"box\"\nhalf_extents = [1, 1, 1]\n\n"
                 "[[morph]]\n"
                 "mesh = \"m\"\n"
                 "name = \"t\"\n");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("missing or empty 'deltas'"), std::string::npos) << s.error;
}

TEST(MorphParser, RejectsDeltaRowWithoutThreeFloats) {
    MorphScene s("row",
                 "[[mesh]]\nname = \"m\"\ntype = \"box\"\nhalf_extents = [1, 1, 1]\n\n"
                 "[[morph]]\n"
                 "mesh = \"m\"\n"
                 "name = \"t\"\n"
                 "deltas = [[0, 0]]\n");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("exactly 3 floats"), std::string::npos) << s.error;
}

TEST(MorphParser, RejectsNonFiniteDelta) {
    MorphScene s("nandelta",
                 "[[mesh]]\nname = \"m\"\ntype = \"box\"\nhalf_extents = [1, 1, 1]\n\n"
                 "[[morph]]\n"
                 "mesh = \"m\"\n"
                 "name = \"t\"\n"
                 "deltas = [[0, 0, inf]]\n");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("finite"), std::string::npos) << s.error;
}

TEST(MorphParser, RejectsNonFiniteWeight) {
    MorphScene s("nanweight",
                 "[[mesh]]\nname = \"m\"\ntype = \"box\"\nhalf_extents = [1, 1, 1]\n\n"
                 "[[morph]]\n"
                 "mesh = \"m\"\n"
                 "name = \"t\"\n"
                 "weight = nan\n"
                 "deltas = [[0, 0, 0]]\n");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("'weight' must be a finite"), std::string::npos) << s.error;
}

TEST(MorphParser, RejectsMissingName) {
    MorphScene s("noname",
                 "[[mesh]]\nname = \"m\"\ntype = \"box\"\nhalf_extents = [1, 1, 1]\n\n"
                 "[[morph]]\n"
                 "mesh = \"m\"\n"
                 "deltas = [[0, 0, 0]]\n");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("missing 'name'"), std::string::npos) << s.error;
}

// ── morph-weight track grammar ───────────────────────────────────────────────

TEST(MorphTracks, WeightTrackDrivesTheWeightOverTime) {
    MorphScene s("tracked", twoTargetScene(/*withTrack=*/true, /*withAuthoredWeight=*/true));
    ASSERT_TRUE(s.scene.has_value()) << s.error;
    ASSERT_EQ(s.scene->animations.size(), 1U);

    const auto w0 = aether::SceneAnimator::evaluateMorphWeightsAtTick(*s.scene, 0);
    const auto w30 = aether::SceneAnimator::evaluateMorphWeightsAtTick(*s.scene, 30); // t = 1 s
    ASSERT_EQ(w0.size(), 2U);
    ASSERT_EQ(w30.size(), 2U);
    EXPECT_NEAR(w0[0], 0.0F, kEps) << "track wins over the authored 0.5 at t=0";
    EXPECT_NEAR(w30[0], 1.0F, kEps) << "track reaches 1.0 at t=1";
    EXPECT_FLOAT_EQ(w30[1], 0.0F) << "the untracked target keeps its authored weight";
}

TEST(MorphTracks, RejectsUnknownMorphTarget) {
    MorphScene s("badtarget",
                 std::string{kBoxMesh} + "[[morph]]\n"
                                         "mesh = \"m\"\n"
                                         "name = \"smile\"\n"
                                         "deltas = [[0, 0, 0]]\n\n"
                                         "[animation_clock]\nfps = 30\n\n"
                                         "[[animation]]\n"
                                         "target = \"morphs/m/frown\"\n"
                                         "times = [0.0]\n"
                                         "values = [1.0]\n");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("unknown morph target 'm/frown'"), std::string::npos) << s.error;
}

TEST(MorphTracks, NodeGrammarRejectsTheWeightToken) {
    // 'weight' must never resolve as a node parameter - it would silently
    // drive desc.morphWeights[nodeIndex] (documented registry contract).
    MorphScene s("weighttoken",
                 std::string{kBoxMesh} + "[[node]]\n"
                                         "id = \"root\"\n\n"
                                         "[animation_clock]\nfps = 30\n\n"
                                         "[[animation]]\n"
                                         "target = \"nodes/root/weight\"\n"
                                         "times = [0.0]\n"
                                         "values = [1.0]\n");
    EXPECT_FALSE(s.scene.has_value());
    EXPECT_NE(s.error.find("morph-weight token"), std::string::npos) << s.error;
}

TEST(MorphTracks, MorphFreeSceneEvaluatesEmpty) {
    aether::SceneDesc desc;
    EXPECT_TRUE(aether::SceneAnimator::evaluateMorphWeights(desc, 0.0F).empty());
    EXPECT_TRUE(aether::SceneAnimator::evaluateMorphWeightsAtTick(desc, 7).empty());
}

TEST(MorphTracks, AuthoredWeightSurvivesAnimationFreeEvaluation) {
    MorphScene s("staticweight", twoTargetScene(/*withTrack=*/false, /*withAuthoredWeight=*/true));
    ASSERT_TRUE(s.scene.has_value()) << s.error;
    const auto weights = aether::SceneAnimator::evaluateMorphWeights(*s.scene, 123.0F);
    ASSERT_EQ(weights.size(), 2U);
    EXPECT_FLOAT_EQ(weights[0], 0.5F) << "no track -> authored initial weight for any t";
    EXPECT_FLOAT_EQ(weights[1], 0.0F);
}

// ── deformMorphs (CPU reference) ─────────────────────────────────────────────

TEST(DeformMorphs, SingleTargetAppliesWeightedDelta) {
    const aether::MeshData mesh = twoVertexMesh();
    aether::MorphTarget target;
    target.meshName = "m";
    target.name = "push";
    target.deltas = {aether::Vec3{0.0F, 0.0F, 0.0F}, aether::Vec3{1.0F, 0.0F, 0.0F}};

    // weight 0 -> rest pose (the authored vertex stays).
    {
        const std::vector<float> weights{0.0F};
        const auto out = aether::deformMorphs(mesh, {&target, 1}, weights);
        ASSERT_TRUE(out.has_value());
        EXPECT_NEAR(out->positions[1].x, 1.0F, kEps);
        EXPECT_NEAR(out->positions[1].y, 0.0F, kEps);
    }
    // weight 0.5 -> halfway.
    {
        const std::vector<float> weights{0.5F};
        const auto out = aether::deformMorphs(mesh, {&target, 1}, weights);
        ASSERT_TRUE(out.has_value());
        EXPECT_NEAR(out->positions[1].x, 1.5F, kEps);
    }
    // weight 2 -> full delta scaled (weights are unconstrained scalars).
    {
        const std::vector<float> weights{2.0F};
        const auto out = aether::deformMorphs(mesh, {&target, 1}, weights);
        ASSERT_TRUE(out.has_value());
        EXPECT_NEAR(out->positions[1].x, 3.0F, kEps);
        EXPECT_NEAR(out->positions[0].x, 0.0F, kEps) << "zero-delta vertex untouched";
    }
}

TEST(DeformMorphs, TwoTargetsCompose) {
    const aether::MeshData mesh = twoVertexMesh();
    aether::MorphTarget a;
    a.meshName = "m";
    a.name = "a";
    a.deltas = {aether::Vec3{1.0F, 0.0F, 0.0F}, aether::Vec3{0.0F, 0.0F, 0.0F}};
    aether::MorphTarget b;
    b.meshName = "m";
    b.name = "b";
    b.deltas = {aether::Vec3{0.0F, 2.0F, 0.0F}, aether::Vec3{0.0F, 0.0F, 0.0F}};
    const std::vector<aether::MorphTarget> morphs{a, b};
    const std::vector<float> weights{1.0F, 1.0F};

    const auto out = aether::deformMorphs(mesh, morphs, weights);
    ASSERT_TRUE(out.has_value());
    EXPECT_NEAR(out->positions[0].x, 1.0F, kEps);
    EXPECT_NEAR(out->positions[0].y, 2.0F, kEps);
    EXPECT_NEAR(out->positions[1].x, 1.0F, kEps);
}

TEST(DeformMorphs, ContractViolationsReturnNullopt) {
    const aether::MeshData mesh = twoVertexMesh();
    aether::MorphTarget target;
    target.meshName = "m";
    target.name = "t";
    target.deltas = {aether::Vec3{0.0F, 0.0F, 0.0F}, aether::Vec3{1.0F, 0.0F, 0.0F}};

    const std::vector<float> oneWeight{1.0F};
    const std::vector<float> none;
    // weight vector parallelity
    EXPECT_FALSE(aether::deformMorphs(mesh, {&target, 1}, none).has_value());
    // delta row count vs vertex count
    aether::MorphTarget shortRows = target;
    shortRows.deltas.pop_back();
    EXPECT_FALSE(aether::deformMorphs(mesh, {&shortRows, 1}, oneWeight).has_value());
}

// ── bind-time vertex-count validation ────────────────────────────────────────

TEST(ValidateMorphs, AcceptsMatchingCount) {
    aether::SceneDesc desc;
    aether::MorphTarget target;
    target.meshName = "m";
    target.name = "t";
    target.deltas = {aether::Vec3{}, aether::Vec3{}};
    desc.morphs.push_back(target);

    const std::unordered_map<std::string, std::size_t> counts{{"m", 2}};
    std::string error;
    EXPECT_TRUE(aether::validateMorphVertexCounts(desc, counts, &error)) << error;
}

TEST(ValidateMorphs, RejectsCountMismatchAndMissingImport) {
    aether::SceneDesc desc;
    aether::MorphTarget target;
    target.meshName = "m";
    target.name = "t";
    target.deltas = {aether::Vec3{}, aether::Vec3{}};
    desc.morphs.push_back(target);

    std::string error;
    const std::unordered_map<std::string, std::size_t> wrong{{"m", 512}};
    EXPECT_FALSE(aether::validateMorphVertexCounts(desc, wrong, &error));
    EXPECT_NE(error.find("covers 2 vertices but mesh 'm' has 512"), std::string::npos) << error;

    const std::unordered_map<std::string, std::size_t> missing;
    EXPECT_FALSE(aether::validateMorphVertexCounts(desc, missing, &error));
    EXPECT_NE(error.find("was not imported"), std::string::npos) << error;

    // Morph-free scenes validate vacuously.
    const aether::SceneDesc empty;
    EXPECT_TRUE(aether::validateMorphVertexCounts(empty, missing, &error));
}
