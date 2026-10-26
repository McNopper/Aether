#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>

#include "aether/format/SceneAnimator.hpp"
#include "aether/format/SceneParser.hpp"

namespace {

std::filesystem::path assetsDir() {
    return std::filesystem::path{AETHER_ASSETS_DIR};
}

// Writes an [[animation]] scene from @p tomlText in a throwaway directory
// (one per test, like the NodeScene fixture in test_scene_parser.cpp) and
// parses it with the error-reporting overload, so animation rejections can
// be checked against their message.
struct AnimScene {
    std::filesystem::path dir;
    std::optional<aether::SceneDesc> scene;
    std::string error;

    AnimScene(std::string_view name, std::string_view tomlText) {
        namespace fs = std::filesystem;
        dir = fs::temp_directory_path() / ("aether_anim_" + std::string{name});
        fs::create_directories(dir);
        {
            std::ofstream scn(dir / "s.scene.toml");
            scn << tomlText;
        }
        scene = aether::SceneParser::parse(dir / "s.scene.toml", &error);
    }
    ~AnimScene() { std::filesystem::remove_all(dir); }
};

// Shared scene body: one meshed node "n" (identity TRS) so single-node
// targets like nodes/n/rotation bind. Tests append [[animation]] entries.
constexpr std::string_view kBallNode = R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "n"
mesh = "ball"

)toml";

// Component-wise near-equality for evaluated TRS values (slerp / Hermite go
// through trig, so exact float equality is not meaningful).
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

// Norm of a quaternion as stored (x, y, z, w) — for unit-length assertions.
float quatLength(const aether::Quat& q) {
    return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}

// ── STEP (glTF: value = last key with time <= t) ────────────────────────────

TEST(SceneAnimator, StepHoldsPreviousKeyUntilNextKeyTime) {
    AnimScene f{"step",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/translation"
interpolation = "step"
times = [0.0, 1.0, 2.0]
values = [0.0, 0.0, 0.0, 10.0, 0.0, 0.0, 20.0, 0.0, 0.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    const auto at = [&](float t) { return aether::SceneAnimator::evaluate(*f.scene, t).instances[0].translation; };
    expectVec3Near(at(0.0F), 0.0F, 0.0F, 0.0F);   // first key
    expectVec3Near(at(0.5F), 0.0F, 0.0F, 0.0F);   // holds key 0
    expectVec3Near(at(0.999F), 0.0F, 0.0F, 0.0F); // still key 0 just below the next key time
    expectVec3Near(at(1.0F), 10.0F, 0.0F, 0.0F);  // AT the next key time: the key flips
    expectVec3Near(at(1.5F), 10.0F, 0.0F, 0.0F);  // holds key 1
}

// ── LINEAR ──────────────────────────────────────────────────────────────────

TEST(SceneAnimator, LinearInterpolatesExactMidpoint) {
    AnimScene f{"linear",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/translation"
times = [0.0, 2.0]
values = [0.0, 0.0, 0.0, 4.0, 8.0, 12.0]
)toml"}; // interpolation omitted → the "linear" default
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->animations.size(), 1U);
    EXPECT_EQ(f.scene->animations[0].interpolation, aether::AnimationTrack::Interpolation::Linear);

    const auto at = [&](float t) { return aether::SceneAnimator::evaluate(*f.scene, t).instances[0].translation; };
    const aether::Vec3 half = at(1.0F); // exact lerp midpoint
    EXPECT_FLOAT_EQ(half.x, 2.0F);
    EXPECT_FLOAT_EQ(half.y, 4.0F);
    EXPECT_FLOAT_EQ(half.z, 6.0F);
    expectVec3Near(at(0.5F), 1.0F, 2.0F, 3.0F);
}

TEST(SceneAnimator, LinearRotationSlerpsHalfRotation) {
    // Keys: identity → Ry(180°) (0,1,0,0). The u = 0.5 sample must be the
    // half rotation Ry(90°) = (0, sin45°, 0, cos45°), unit length; endpoints
    // are the exact key values.
    AnimScene f{"slerp",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/rotation"
interpolation = "linear"
times = [0.0, 2.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    const auto at = [&](float t) { return aether::SceneAnimator::evaluate(*f.scene, t).instances[0].rotation; };
    expectQuatNear(at(0.0F), 0.0F, 0.0F, 0.0F, 1.0F);
    const aether::Quat mid = at(1.0F);
    expectQuatNear(mid, 0.0F, 0.70710678F, 0.0F, 0.70710678F);
    EXPECT_NEAR(quatLength(mid), 1.0F, kTrsEpsilon);
    expectQuatNear(at(2.0F), 0.0F, 1.0F, 0.0F, 0.0F);
}

TEST(SceneAnimator, LinearRotationSlerpTakesShortestArc) {
    // Keys: identity → Ry(240°) = (0, sin120°, 0, cos120°) — the naive path
    // is 240°; the shortest arc flips the second key to Ry(-120°), so the
    // u = 0.5 sample is Ry(-60°) = (0, -sin30°, 0, cos30°).
    AnimScene f{"slerp_arc",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/rotation"
interpolation = "linear"
times = [0.0, 2.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.8660254, 0.0, -0.5]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    const aether::Quat mid = aether::SceneAnimator::evaluate(*f.scene, 1.0F).instances[0].rotation;
    expectQuatNear(mid, 0.0F, -0.5F, 0.0F, 0.8660254F);
    EXPECT_NEAR(quatLength(mid), 1.0F, kTrsEpsilon);
}

// ── CUBIC_SPLINE (glTF layout: per key inTangent, value, outTangent) ───────

TEST(SceneAnimator, CubicSplineMatchesHandComputedHermite) {
    // Translation, keys at t = 0 and t = 1 (dt = 1):
    //   key0: in (0,0,0), value (0,0,0), out (3,0,0)
    //   key1: in (0,0,0), value (3,0,0), out (0,0,0)
    // Hermite h00*v0 + h10*dt*out0 + h01*v1 + h11*dt*in1 at
    //   u = 0.25: h10 = 0.140625, h01 = 0.15625 → x = 0.140625*3 + 0.15625*3 = 0.890625
    //   u = 0.50: h10 = 0.125,    h01 = 0.5     → x = 0.125*3 + 0.5*3     = 1.875
    //   u = 0 / 1: the endpoint key values exactly.
    AnimScene f{"spline",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/translation"
interpolation = "cubic_spline"
times = [0.0, 1.0]
values = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 3.0, 0.0, 0.0,
          0.0, 0.0, 0.0, 3.0, 0.0, 0.0, 0.0, 0.0, 0.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    const auto at = [&](float t) { return aether::SceneAnimator::evaluate(*f.scene, t).instances[0].translation; };
    const aether::Vec3 start = at(0.0F);
    EXPECT_FLOAT_EQ(start.x, 0.0F);
    const aether::Vec3 quarter = at(0.25F);
    EXPECT_FLOAT_EQ(quarter.x, 0.890625F);
    const aether::Vec3 half = at(0.5F);
    EXPECT_FLOAT_EQ(half.x, 1.875F);
    const aether::Vec3 end = at(1.0F);
    EXPECT_FLOAT_EQ(end.x, 3.0F);
    expectVec3Near(half, 1.875F, 0.0F, 0.0F);
}

// ── Clamping (time outside [first, last] → endpoint key's value) ────────────

TEST(SceneAnimator, ClampsOutsideTrackRangeAllModes) {
    // Translation keys: t = 0 → (1,2,3), t = 1 → (7,8,9). Samples before the
    // first and after the last key must be exactly those endpoint values in
    // every interpolation mode.
    const auto checkMode = [](std::string_view name, std::string_view interpolation, std::string_view values) {
        AnimScene f{std::string{name},
                    std::string{kBallNode} + R"toml([[animation]]
target = "nodes/n/translation"
interpolation = )toml" + std::string{interpolation} +
                        R"toml(
times = [0.0, 1.0]
values = )toml" + std::string{values} +
                        "\n"};
        EXPECT_TRUE(f.scene.has_value()) << name << ": " << f.error;
        if (!f.scene.has_value()) {
            return;
        }
        const auto at = [&](float t) { return aether::SceneAnimator::evaluate(*f.scene, t).instances[0].translation; };
        expectVec3Near(at(-5.0F), 1.0F, 2.0F, 3.0F);
        expectVec3Near(at(100.0F), 7.0F, 8.0F, 9.0F);
    };
    checkMode("clamp_step", "\"step\"", "[1.0, 2.0, 3.0, 7.0, 8.0, 9.0]");
    checkMode("clamp_linear", "\"linear\"", "[1.0, 2.0, 3.0, 7.0, 8.0, 9.0]");
    checkMode("clamp_spline",
              "\"cubic_spline\"",
              "[0.0, 0.0, 0.0, 1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 7.0, 8.0, 9.0, 0.0, 0.0, 0.0]");
}

// ── Validity policies ───────────────────────────────────────────────────────

TEST(SceneAnimator, RotationKeysRenormalizedAtLoadAndSample) {
    // Deliberately denormalized key quaternions: (0,0,0,2) → identity, and
    // (0, 1.4142136, 0, 1.4142136) → Ry(90°). Keys normalize at load, so the
    // stored values are unit; every sample is unit length.
    AnimScene f{"quat_normalize",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/rotation"
interpolation = "linear"
times = [0.0, 1.0]
values = [0.0, 0.0, 0.0, 2.0, 0.0, 1.4142136, 0.0, 1.4142136]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->animations.size(), 1U);
    const aether::AnimationTrack& track = f.scene->animations[0];
    ASSERT_EQ(track.values.size(), 8U);
    // Key 0 normalized at load: (0,0,0,2) → (0,0,0,1).
    EXPECT_FLOAT_EQ(track.values[0], 0.0F);
    EXPECT_FLOAT_EQ(track.values[1], 0.0F);
    EXPECT_FLOAT_EQ(track.values[2], 0.0F);
    EXPECT_FLOAT_EQ(track.values[3], 1.0F);
    // Key 1 normalized at load: → (0, sin45°, 0, cos45°).
    EXPECT_NEAR(track.values[5], 0.70710678F, kTrsEpsilon);
    EXPECT_NEAR(track.values[7], 0.70710678F, kTrsEpsilon);

    // Sample halfway → Ry(45°) (the midpoint of 0° and 90°), unit length.
    const aether::Quat sampled = aether::SceneAnimator::evaluate(*f.scene, 0.5F).instances[0].rotation;
    expectQuatNear(sampled, 0.0F, 0.38268346F, 0.0F, 0.92387953F);
    EXPECT_NEAR(quatLength(sampled), 1.0F, kTrsEpsilon);
}

TEST(SceneAnimator, ScaleTrackFloorsAwayFromZero) {
    // Scale driven to zero must clamp at the magnitude floor (kScaleFloor),
    // never collapse an axis.
    AnimScene f{"scale_floor",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/scale"
interpolation = "linear"
times = [0.0, 2.0]
values = [1.0, 1.0, 1.0, 0.0, 0.0, 0.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    const auto scaleAt = [&](float t) { return aether::SceneAnimator::evaluate(*f.scene, t).instances[0].scale; };
    const aether::Vec3 floored = scaleAt(2.0F); // the zero key, floored
    EXPECT_FLOAT_EQ(floored.x, aether::kScaleFloor);
    EXPECT_FLOAT_EQ(floored.y, aether::kScaleFloor);
    EXPECT_FLOAT_EQ(floored.z, aether::kScaleFloor);
    const aether::Vec3 half = scaleAt(1.0F); // midpoint of 1 → floor
    EXPECT_NEAR(half.x, 0.5F, kTrsEpsilon);
    EXPECT_NEAR(half.y, 0.5F, kTrsEpsilon);
    EXPECT_NEAR(half.z, 0.5F, kTrsEpsilon);
}

TEST(SceneAnimator, SingleKeyTrackIsConstantAllModes) {
    // One key: constant everywhere, in every mode (values [3,4,5]; the
    // spline layout adds the in/out tangent thirds).
    const auto checkMode = [](std::string_view name, std::string_view interpolation, std::string_view values) {
        AnimScene f{std::string{name},
                    std::string{kBallNode} + R"toml([[animation]]
target = "nodes/n/translation"
interpolation = )toml" + std::string{interpolation} +
                        R"toml(
times = [0.5]
values = )toml" + std::string{values} +
                        "\n"};
        EXPECT_TRUE(f.scene.has_value()) << name << ": " << f.error;
        if (!f.scene.has_value()) {
            return;
        }
        const auto at = [&](float t) { return aether::SceneAnimator::evaluate(*f.scene, t).instances[0].translation; };
        expectVec3Near(at(0.0F), 3.0F, 4.0F, 5.0F);
        expectVec3Near(at(0.5F), 3.0F, 4.0F, 5.0F);
        expectVec3Near(at(9.0F), 3.0F, 4.0F, 5.0F);
    };
    checkMode("single_step", "\"step\"", "[3.0, 4.0, 5.0]");
    checkMode("single_linear", "\"linear\"", "[3.0, 4.0, 5.0]");
    checkMode("single_spline", "\"cubic_spline\"", "[0.0, 0.0, 0.0, 3.0, 4.0, 5.0, 0.0, 0.0, 0.0]");
}

// ── Validation (parse errors, each naming the track) ────────────────────────

TEST(SceneAnimator, AnimationUnknownNodeFailsParse) {
    AnimScene f{"unknown_node",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/ghost/translation"
times = [0.0, 1.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(f.scene.has_value());
    EXPECT_NE(f.error.find("ghost"), std::string::npos) << "error: " << f.error;
}

TEST(SceneAnimator, AnimationUnknownParameterFailsParse) {
    AnimScene f{"unknown_param",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/color"
times = [0.0, 1.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(f.scene.has_value());
    EXPECT_NE(f.error.find("color"), std::string::npos) << "error: " << f.error;
}

TEST(SceneAnimator, AnimationMalformedTargetFailsParse) {
    // Missing the parameter segment, and missing the nodes/ prefix.
    AnimScene noParam{"target_no_param",
                      std::string{kBallNode} +
                          R"toml([[animation]]
target = "nodes/n"
times = [0.0]
values = [0.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(noParam.scene.has_value());
    EXPECT_NE(noParam.error.find("malformed"), std::string::npos) << "error: " << noParam.error;

    AnimScene noPrefix{"target_no_prefix",
                       std::string{kBallNode} +
                           R"toml([[animation]]
target = "hand/translation"
times = [0.0]
values = [0.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(noPrefix.scene.has_value());
    EXPECT_NE(noPrefix.error.find("malformed"), std::string::npos) << "error: " << noPrefix.error;
}

TEST(SceneAnimator, AnimationNonAscendingTimesFailParse) {
    AnimScene descending{"times_descending",
                         std::string{kBallNode} +
                             R"toml([[animation]]
target = "nodes/n/translation"
times = [1.0, 0.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(descending.scene.has_value());
    EXPECT_NE(descending.error.find("ascending"), std::string::npos) << "error: " << descending.error;

    // Equal times are not strictly ascending either (a zero-length linear
    // segment would divide by zero).
    AnimScene equal{"times_equal",
                    std::string{kBallNode} +
                        R"toml([[animation]]
target = "nodes/n/translation"
times = [1.0, 1.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(equal.scene.has_value());
    EXPECT_NE(equal.error.find("ascending"), std::string::npos) << "error: " << equal.error;
}

TEST(SceneAnimator, AnimationNonFiniteValuesFailParse) {
    // A NaN key value, and an infinite key time (TOML `nan` / `inf` floats).
    AnimScene nan{"values_nan",
                  std::string{kBallNode} +
                      R"toml([[animation]]
target = "nodes/n/translation"
times = [0.0, 1.0]
values = [0.0, nan, 0.0, 1.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(nan.scene.has_value());
    EXPECT_NE(nan.error.find("finite"), std::string::npos) << "error: " << nan.error;

    AnimScene inf{"times_inf",
                  std::string{kBallNode} +
                      R"toml([[animation]]
target = "nodes/n/translation"
times = [0.0, inf]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(inf.scene.has_value());
    EXPECT_NE(inf.error.find("finite"), std::string::npos) << "error: " << inf.error;
}

TEST(SceneAnimator, AnimationWrongArityFailsParse) {
    // translation arity 3: two keys need 6 floats, 8 given.
    AnimScene translation{"arity_translation",
                          std::string{kBallNode} +
                              R"toml([[animation]]
target = "nodes/n/translation"
times = [0.0, 1.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 2.0, 0.0]
)toml"};
    EXPECT_FALSE(translation.scene.has_value());
    EXPECT_NE(translation.error.find("arity"), std::string::npos) << "error: " << translation.error;

    // rotation arity 4: two keys need 8 floats, 6 given.
    AnimScene rotation{"arity_rotation",
                       std::string{kBallNode} +
                           R"toml([[animation]]
target = "nodes/n/rotation"
times = [0.0, 1.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(rotation.scene.has_value());
    EXPECT_NE(rotation.error.find("arity"), std::string::npos) << "error: " << rotation.error;

    // cubic_spline stride: two keys need 3x6 = 18 floats, 12 given.
    AnimScene spline{"arity_spline",
                     std::string{kBallNode} +
                         R"toml([[animation]]
target = "nodes/n/translation"
interpolation = "cubic_spline"
times = [0.0, 1.0]
values = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 3.0, 0.0, 0.0, 0.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(spline.scene.has_value());
    EXPECT_NE(spline.error.find("arity"), std::string::npos) << "error: " << spline.error;
}

TEST(SceneAnimator, AnimationEmptyTrackFailsParse) {
    AnimScene noTimes{"empty_times",
                      std::string{kBallNode} +
                          R"toml([[animation]]
target = "nodes/n/translation"
times = []
values = []
)toml"};
    EXPECT_FALSE(noTimes.scene.has_value());
    EXPECT_NE(noTimes.error.find("empty"), std::string::npos) << "error: " << noTimes.error;

    // Times present but values missing entirely.
    AnimScene noValues{"missing_values",
                       std::string{kBallNode} +
                           R"toml([[animation]]
target = "nodes/n/translation"
times = [0.0, 1.0]
)toml"};
    EXPECT_FALSE(noValues.scene.has_value());
    EXPECT_NE(noValues.error.find("empty"), std::string::npos) << "error: " << noValues.error;
}

TEST(SceneAnimator, AnimationUnknownInterpolationFailsParse) {
    AnimScene f{"bad_interp",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/translation"
interpolation = "bezier"
times = [0.0]
values = [0.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(f.scene.has_value());
    EXPECT_NE(f.error.find("interpolation"), std::string::npos) << "error: " << f.error;
}

TEST(SceneAnimator, AnimationMissingTargetFailsParse) {
    AnimScene f{"missing_target",
                std::string{kBallNode} +
                    R"toml([[animation]]
times = [0.0]
values = [0.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(f.scene.has_value());
    EXPECT_NE(f.error.find("target"), std::string::npos) << "error: " << f.error;
}

// ── Parsed track data ───────────────────────────────────────────────────────

TEST(SceneAnimator, TrackFieldsRoundTrip) {
    AnimScene f{"fields",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/rotation"
interpolation = "step"
times = [0.0, 1.0, 2.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.3826834, 0.0, 0.9238795, 0.0, 0.7071068, 0.0, 0.7071068]

[[animation]]
target = "nodes/n/translation"
times = [0.0, 2.0]
values = [0.0, 0.0, 0.0, 4.0, 0.0, 0.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->animations.size(), 2U);

    const aether::AnimationTrack& rotation = f.scene->animations[0];
    EXPECT_EQ(rotation.target, "nodes/n/rotation");
    EXPECT_EQ(rotation.parameter, aether::AnimationTrack::Parameter::Rotation);
    EXPECT_EQ(rotation.interpolation, aether::AnimationTrack::Interpolation::Step);
    ASSERT_LT(rotation.nodeIndex, f.scene->nodes.size());
    EXPECT_EQ(f.scene->nodes[rotation.nodeIndex].id, "n");
    ASSERT_EQ(rotation.times.size(), 3U);
    EXPECT_FLOAT_EQ(rotation.times[2], 2.0F);
    ASSERT_EQ(rotation.values.size(), 12U); // 3 keys x arity 4, normalized at load

    const aether::AnimationTrack& translation = f.scene->animations[1];
    EXPECT_EQ(translation.target, "nodes/n/translation");
    EXPECT_EQ(translation.parameter, aether::AnimationTrack::Parameter::Translation);
    EXPECT_EQ(translation.interpolation, aether::AnimationTrack::Interpolation::Linear); // default
    ASSERT_EQ(translation.values.size(), 6U);                                            // raw, not normalized
    EXPECT_FLOAT_EQ(translation.values[3], 4.0F);
}

// ── Evaluation end-to-end (sampling composed with the NH1 compounding) ──────

TEST(SceneAnimator, AnimatedRotationCompoundsIntoWorldTransform) {
    // The grouping node "arm" rotates (identity → Ry(90°)) over [0, 2] s; the
    // static child "hand" holds the mesh at local +X. The world translation
    // of the instance must be the sampled parent rotation applied to the
    // child translation.
    AnimScene f{"e2e_rotation", R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "arm"

[[node]]
id = "hand"
parent = "arm"
mesh = "ball"
translate = [1.0, 0.0, 0.0]

[[animation]]
target = "nodes/arm/rotation"
interpolation = "linear"
times = [0.0, 2.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.7071068, 0.0, 0.7071068]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->instances.size(), 1U);

    // The parse result itself stays resolved at the authored (t = key-0) TRS.
    expectVec3Near(f.scene->instances[0].translation, 1.0F, 0.0F, 0.0F);

    const auto translationAt = [&](float t) {
        return aether::SceneAnimator::evaluate(*f.scene, t).instances[0].translation;
    };
    expectVec3Near(translationAt(0.0F), 1.0F, 0.0F, 0.0F);              // identity arm
    expectVec3Near(translationAt(1.0F), 0.7071068F, 0.0F, -0.7071068F); // Ry(45°) * (+X)
    expectVec3Near(translationAt(2.0F), 0.0F, 0.0F, -1.0F);             // Ry(90°) * (+X) = -Z

    // evaluate() never mutates its input scene.
    expectVec3Near(f.scene->instances[0].translation, 1.0F, 0.0F, 0.0F);
}

TEST(SceneAnimator, AnimatedChannelsCombineOnOneNode) {
    // Translation and scale tracks drive the same node; both sampled values
    // must land in the node TRS and flow into the world instance.
    AnimScene f{"e2e_channels",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/translation"
interpolation = "linear"
times = [0.0, 2.0]
values = [0.0, 0.0, 0.0, 2.0, 0.0, 0.0]

[[animation]]
target = "nodes/n/scale"
interpolation = "linear"
times = [0.0, 2.0]
values = [1.0, 1.0, 1.0, 3.0, 1.0, 1.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    const aether::SceneDesc out = aether::SceneAnimator::evaluate(*f.scene, 1.0F);

    expectVec3Near(out.nodes[0].translation, 1.0F, 0.0F, 0.0F);
    expectVec3Near(out.nodes[0].scale, 2.0F, 1.0F, 1.0F);
    ASSERT_EQ(out.instances.size(), 1U);
    expectVec3Near(out.instances[0].translation, 1.0F, 0.0F, 0.0F);
    expectVec3Near(out.instances[0].scale, 2.0F, 1.0F, 1.0F);
}

TEST(SceneAnimator, NoAnimationSceneEvaluatesToSameFlatList) {
    // Animation-free scenes: evaluate(t) is the parse result verbatim, for
    // any t — with and without a node graph present.
    const auto cornell = aether::SceneParser::parse(assetsDir() / "cornell_classic.scene.toml");
    ASSERT_TRUE(cornell.has_value());
    ASSERT_TRUE(cornell->animations.empty());
    for (const float t : {0.0F, 0.5F, 7.3F}) {
        const aether::SceneDesc out = aether::SceneAnimator::evaluate(*cornell, t);
        ASSERT_EQ(out.instances.size(), cornell->instances.size());
        for (std::size_t i = 0; i < cornell->instances.size(); ++i) {
            EXPECT_EQ(out.instances[i].meshName, cornell->instances[i].meshName);
            EXPECT_FLOAT_EQ(out.instances[i].translation.x, cornell->instances[i].translation.x);
            EXPECT_FLOAT_EQ(out.instances[i].translation.y, cornell->instances[i].translation.y);
            EXPECT_FLOAT_EQ(out.instances[i].translation.z, cornell->instances[i].translation.z);
            EXPECT_FLOAT_EQ(out.instances[i].rotation.x, cornell->instances[i].rotation.x);
            EXPECT_FLOAT_EQ(out.instances[i].rotation.w, cornell->instances[i].rotation.w);
            EXPECT_FLOAT_EQ(out.instances[i].scale.x, cornell->instances[i].scale.x);
            EXPECT_FLOAT_EQ(out.instances[i].scale.z, cornell->instances[i].scale.z);
            EXPECT_EQ(out.instances[i].materialName, cornell->instances[i].materialName);
        }
    }

    // The same holds for a compounded node scene without [[animation]].
    AnimScene f{"no_anim_nodes", R"toml([[mesh]]
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
    ASSERT_TRUE(f.scene->animations.empty());
    ASSERT_EQ(f.scene->instances.size(), 1U);
    for (const float t : {0.0F, 3.5F}) {
        const aether::SceneDesc out = aether::SceneAnimator::evaluate(*f.scene, t);
        ASSERT_EQ(out.instances.size(), 1U);
        expectVec3Near(out.instances[0].translation, 6.0F, 2.0F, 3.0F);
        expectVec3Near(out.instances[0].scale, 1.0F, 1.0F, 1.0F);
        expectQuatNear(out.instances[0].rotation, 0.0F, 0.0F, 0.0F, 1.0F);
    }
}

} // namespace
