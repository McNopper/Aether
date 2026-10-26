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

// Shared scene body: the animation tick clock (ANI2 — required whenever
// [[animation]] tracks exist) plus one meshed node "n" (identity TRS) so
// single-node targets like nodes/n/rotation bind. Tests append [[animation]]
// entries. (Clock-validation tests that must author scenes WITHOUT the clock
// use kBallNodeNoClock below.)
constexpr std::string_view kBallNodeNoClock = R"toml([[mesh]]
name = "ball"
type = "sphere"
radius = 0.5

[[node]]
id = "n"
mesh = "ball"

)toml";

constexpr std::string_view kBallNode = R"toml([animation_clock]
fps = 30

[[mesh]]
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
    AnimScene f{"e2e_rotation", R"toml([animation_clock]
fps = 30

[[mesh]]
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

// ── Tick clock (ANI2): [animation_clock] fps + evaluateAtTick ───────────────

TEST(SceneAnimator, TickClockParsesIntoSceneDesc) {
    // The clock lands on SceneDesc::animationFps and coexists with the tracks.
    AnimScene f{"clock_parses",
                std::string{kBallNodeNoClock} +
                    R"toml([animation_clock]
fps = 24

[[animation]]
target = "nodes/n/translation"
times = [0.0, 1.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->animations.size(), 1U);
    EXPECT_DOUBLE_EQ(f.scene->animationFps, 24.0);
}

TEST(SceneAnimator, TickClockMissingFailsParseWhenTracksExist) {
    // Tracks without a clock: parse error naming the issue.
    AnimScene f{"clock_missing",
                std::string{kBallNodeNoClock} +
                    R"toml([[animation]]
target = "nodes/n/translation"
times = [0.0, 1.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0]
)toml"};
    EXPECT_FALSE(f.scene.has_value());
    EXPECT_NE(f.error.find("animation clock"), std::string::npos) << "error: " << f.error;
    EXPECT_NE(f.error.find("fps"), std::string::npos) << "error: " << f.error;
}

TEST(SceneAnimator, TickClockNonPositiveFailsParse) {
    // fps = 0, negative fps and a wrong-typed fps are all rejected — with or
    // without tracks (an authored clock must be valid whenever present).
    const auto check = [](std::string_view name, std::string_view clockTable) {
        AnimScene f{std::string{name}, std::string{kBallNodeNoClock} + std::string{clockTable}};
        EXPECT_FALSE(f.scene.has_value()) << name;
        if (!f.scene.has_value()) {
            EXPECT_NE(f.error.find("positive"), std::string::npos) << name << ": " << f.error;
        }
    };
    check("clock_zero", "[animation_clock]\nfps = 0\n");
    check("clock_negative", "[animation_clock]\nfps = -30\n");
    check("clock_string", "[animation_clock]\nfps = \"thirty\"\n");
    check("clock_no_fps", "[animation_clock]\n");
}

TEST(SceneAnimator, TickClockWithoutTracksIsInert) {
    // A valid clock with no [[animation]] entries parses (and lands on the
    // SceneDesc) but animates nothing: evaluateAtTick is the parse result
    // verbatim for any tick.
    AnimScene f{"clock_no_tracks", std::string{kBallNodeNoClock} + R"toml([animation_clock]
fps = 30
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_TRUE(f.scene->animations.empty());
    EXPECT_DOUBLE_EQ(f.scene->animationFps, 30.0);
    ASSERT_EQ(f.scene->instances.size(), 1U);
    for (const std::uint64_t tick : {std::uint64_t{0}, std::uint64_t{7}, std::uint64_t{1000000}}) {
        const aether::SceneDesc out = aether::SceneAnimator::evaluateAtTick(*f.scene, tick);
        ASSERT_EQ(out.instances.size(), 1U);
        expectVec3Near(out.instances[0].translation, 0.0F, 0.0F, 0.0F);
    }
}

TEST(SceneAnimator, EvaluateAtTickIsRepeatableAndRenderRateIndependent) {
    // The determinism contract: the same tick evaluated twice — and across
    // two copies of the same parsed scene — produces identical instance
    // lists. Seconds = tick / fps only; no wall-clock or render-rate input.
    AnimScene f{"tick_determinism",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/rotation"
interpolation = "linear"
times = [0.0, 2.0]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.7071068, 0.0, 0.7071068]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;

    aether::SceneDesc copy = *f.scene; // an independent SceneDesc copy
    for (const std::uint64_t tick : {std::uint64_t{0}, std::uint64_t{17}, std::uint64_t{61}}) {
        const aether::SceneDesc first = aether::SceneAnimator::evaluateAtTick(*f.scene, tick);
        const aether::SceneDesc second = aether::SceneAnimator::evaluateAtTick(*f.scene, tick);
        const aether::SceneDesc fromCopy = aether::SceneAnimator::evaluateAtTick(copy, tick);
        ASSERT_EQ(first.instances.size(), second.instances.size());
        ASSERT_EQ(first.instances.size(), fromCopy.instances.size());
        for (std::size_t i = 0; i < first.instances.size(); ++i) {
            EXPECT_EQ(first.instances[i].meshName, second.instances[i].meshName);
            EXPECT_EQ(first.instances[i].meshName, fromCopy.instances[i].meshName);
            EXPECT_FLOAT_EQ(first.instances[i].translation.x, second.instances[i].translation.x);
            EXPECT_FLOAT_EQ(first.instances[i].translation.y, second.instances[i].translation.y);
            EXPECT_FLOAT_EQ(first.instances[i].translation.z, second.instances[i].translation.z);
            EXPECT_FLOAT_EQ(first.instances[i].rotation.x, second.instances[i].rotation.x);
            EXPECT_FLOAT_EQ(first.instances[i].rotation.y, second.instances[i].rotation.y);
            EXPECT_FLOAT_EQ(first.instances[i].rotation.z, second.instances[i].rotation.z);
            EXPECT_FLOAT_EQ(first.instances[i].rotation.w, second.instances[i].rotation.w);
            EXPECT_FLOAT_EQ(first.instances[i].scale.x, fromCopy.instances[i].scale.x);
            EXPECT_FLOAT_EQ(first.instances[i].scale.y, fromCopy.instances[i].scale.y);
            EXPECT_FLOAT_EQ(first.instances[i].scale.z, fromCopy.instances[i].scale.z);
        }
    }
}

TEST(SceneAnimator, EvaluateAtTickAdvancesSequentialTicks) {
    // fps 30: ticks 0, 1, 2 map to seconds 0, 1/30, 2/30 — spot-checked
    // against a linear translation track (x = t, keys (0 s → 0) and (1 s → 1))
    // and a step track keyed exactly at the tick times.
    AnimScene linear{"tick_linear",
                     std::string{kBallNode} +
                         R"toml([[animation]]
target = "nodes/n/translation"
interpolation = "linear"
times = [0.0, 1.0]
values = [0.0, 0.0, 0.0, 1.0, 1.0, 1.0]
)toml"};
    ASSERT_TRUE(linear.scene.has_value()) << linear.error;
    const auto tickX = [&](std::uint64_t tick) {
        return aether::SceneAnimator::evaluateAtTick(*linear.scene, tick).instances[0].translation.x;
    };
    EXPECT_FLOAT_EQ(tickX(0), 0.0F);
    EXPECT_FLOAT_EQ(tickX(1), static_cast<float>(1.0 / 30.0)); // 1 tick = 1/30 s
    EXPECT_FLOAT_EQ(tickX(2), static_cast<float>(2.0 / 30.0)); // 2 ticks = 2/30 s
    EXPECT_FLOAT_EQ(tickX(30), 1.0F);                          // a full second is 30 ticks

    // A step track keyed AT the tick times flips keys exactly on tick
    // boundaries (the key time and the tick→seconds conversion produce the
    // same float).
    AnimScene step{"tick_step",
                   std::string{kBallNode} +
                       R"toml([[animation]]
target = "nodes/n/translation"
interpolation = "step"
times = [0.0, 0.0333333333333333, 0.0666666666666667]
values = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 2.0, 0.0, 0.0]
)toml"};
    ASSERT_TRUE(step.scene.has_value()) << step.error;
    const auto tickT = [&](std::uint64_t tick) {
        return aether::SceneAnimator::evaluateAtTick(*step.scene, tick).instances[0].translation;
    };
    expectVec3Near(tickT(0), 0.0F, 0.0F, 0.0F);
    expectVec3Near(tickT(1), 1.0F, 0.0F, 0.0F);
    expectVec3Near(tickT(2), 2.0F, 0.0F, 0.0F);
}

// ── Scalar component targets (registry extension) ───────────────────────────

TEST(SceneAnimator, ComponentTrackDrivesOnlyItsComponent) {
    // A translation-x track moves ONLY x; y and z keep the authored TRS.
    AnimScene f{"component_only",
                std::string{kBallNode} +
                    R"toml([[node]]
id = "posed"
parent = "n"
mesh = "ball"
translate = [5.0, 6.0, 7.0]

[[animation]]
target = "nodes/posed/translation/x"
interpolation = "linear"
times = [0.0, 1.0]
values = [4.0, 9.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    // "posed" is the second meshed node (declared after "n") → instance 1.
    ASSERT_EQ(f.scene->instances.size(), 2U);
    const auto at = [&](float t) { return aether::SceneAnimator::evaluate(*f.scene, t).instances[1].translation; };
    expectVec3Near(at(0.0F), 4.0F, 6.0F, 7.0F);
    expectVec3Near(at(0.5F), 6.5F, 6.0F, 7.0F);
    expectVec3Near(at(1.0F), 9.0F, 6.0F, 7.0F);
}

TEST(SceneAnimator, MixedComponentKeyTimesParseAndEvaluate) {
    // The exporter-limitation case, now working: component F-curves with
    // MISMATCHED key times parse and evaluate each component on its own
    // schedule (previously a property like this could not be represented).
    AnimScene f{"component_mismatch",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/translation/x"
interpolation = "linear"
times = [0.0, 1.0]
values = [5.0, 10.0]

[[animation]]
target = "nodes/n/translation/y"
interpolation = "linear"
times = [0.0, 0.5, 1.0]
values = [6.0, 8.0, 2.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->animations.size(), 2U);
    const auto at = [&](float t) { return aether::SceneAnimator::evaluate(*f.scene, t).instances[0].translation; };
    expectVec3Near(at(0.0F), 5.0F, 6.0F, 0.0F);
    expectVec3Near(at(0.25F), 6.25F, 7.0F, 0.0F); // x on [0,1]; y mid-segment
    expectVec3Near(at(0.75F), 8.75F, 5.0F, 0.0F); // y already on its second segment
    expectVec3Near(at(1.0F), 10.0F, 2.0F, 0.0F);
}

TEST(SceneAnimator, RotationComponentTrackRenormalizesWholeQuaternion) {
    // The rotation components keep the whole-quaternion validity policy: the
    // sampled scalar is written into the component, then the ENTIRE
    // quaternion is renormalized. An identity node + a rotation-x track
    // 0 → 1 samples normalize(x, 0, 0, 1): Ry(90°) at the end, unit length
    // everywhere.
    AnimScene f{"rotation_component",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/rotation/x"
interpolation = "linear"
times = [0.0, 1.0]
values = [0.0, 1.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    const auto at = [&](float t) { return aether::SceneAnimator::evaluate(*f.scene, t).instances[0].rotation; };
    expectQuatNear(at(0.0F), 0.0F, 0.0F, 0.0F, 1.0F); // normalize(0,0,0,1) = identity
    const aether::Quat half = at(0.5F);               // normalize(0.5,0,0,1)
    expectQuatNear(half, 0.4472136F, 0.0F, 0.0F, 0.8944272F);
    const aether::Quat end = at(1.0F); // normalize(1,0,0,1) = Ry(90°)
    expectQuatNear(end, 0.7071068F, 0.0F, 0.0F, 0.7071068F);
    EXPECT_NEAR(quatLength(half), 1.0F, kTrsEpsilon);
    EXPECT_NEAR(quatLength(end), 1.0F, kTrsEpsilon);
}

TEST(SceneAnimator, RotationWComponentTrackPreservesOtherComponents) {
    // A w-only track on a non-identity node: x/y/z components keep their
    // authored values, w drives, the whole quaternion renormalizes. Node
    // rotation Ry(90°) = (0,1,0,0); w: 0 → 1.
    AnimScene f{"rotation_w_component",
                std::string{kBallNode} +
                    R"toml([[node]]
id = "spun"
parent = "n"
mesh = "ball"
rotate = [0.0, 1.0, 0.0, 0.0]

[[animation]]
target = "nodes/spun/rotation/w"
interpolation = "linear"
times = [0.0, 1.0]
values = [0.0, 1.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    // "spun" is the second meshed node (declared after "n") → instance 1.
    ASSERT_EQ(f.scene->instances.size(), 2U);
    const aether::Quat mid = aether::SceneAnimator::evaluate(*f.scene, 0.5F).instances[1].rotation;
    expectQuatNear(mid, 0.0F, 0.8944272F, 0.0F, 0.4472136F); // normalize(0, 1, 0, 0.5)
    EXPECT_NEAR(quatLength(mid), 1.0F, kTrsEpsilon);
}

TEST(SceneAnimator, ScaleComponentTrackFloorsAwayFromZero) {
    // The parent parameter's validity policy at arity 1: the driven component
    // floors away from zero; the untouched components keep their TRS values.
    AnimScene f{"scale_component",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/scale/x"
interpolation = "linear"
times = [0.0, 1.0]
values = [1.0, 0.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    const auto scaleAt = [&](float t) { return aether::SceneAnimator::evaluate(*f.scene, t).instances[0].scale; };
    const aether::Vec3 floored = scaleAt(1.0F);
    EXPECT_FLOAT_EQ(floored.x, aether::kScaleFloor);
    EXPECT_FLOAT_EQ(floored.y, 1.0F);
    EXPECT_FLOAT_EQ(floored.z, 1.0F);
    EXPECT_NEAR(scaleAt(0.5F).x, 0.5F, kTrsEpsilon);
}

TEST(SceneAnimator, ComponentTrackCubicSplineMatchesHandComputedHermite) {
    // Arity-1 spline layout: 3 floats per key (inTangent, value, outTangent).
    // Keys at t = 0 / 1 (dt = 1), value 0 → 3 with out/in tangents 3 / 0:
    // x(0.5) = h10*3 + h01*3 = 0.125*3 + 0.5*3 = 1.875.
    AnimScene f{"component_spline",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/translation/x"
interpolation = "cubic_spline"
times = [0.0, 1.0]
values = [0.0, 0.0, 3.0, 0.0, 3.0, 0.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    const aether::Vec3 half = aether::SceneAnimator::evaluate(*f.scene, 0.5F).instances[0].translation;
    EXPECT_FLOAT_EQ(half.x, 1.875F);
    EXPECT_FLOAT_EQ(half.y, 0.0F);
    EXPECT_FLOAT_EQ(half.z, 0.0F);
}

TEST(SceneAnimator, ComponentTrackFieldsRoundTrip) {
    // The resolved binding carries the parameter AND the component index
    // (0-based, x..w order); the target string stays verbatim.
    AnimScene f{"component_fields",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/translation/x"
times = [0.0]
values = [1.0]

[[animation]]
target = "nodes/n/rotation/w"
times = [0.0]
values = [2.0]

[[animation]]
target = "nodes/n/scale/z"
times = [0.0]
values = [3.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    ASSERT_EQ(f.scene->animations.size(), 3U);
    EXPECT_EQ(f.scene->animations[0].target, "nodes/n/translation/x");
    EXPECT_EQ(f.scene->animations[0].parameter, aether::AnimationTrack::Parameter::Translation);
    EXPECT_EQ(f.scene->animations[0].componentIndex, 0U);
    EXPECT_EQ(f.scene->animations[1].target, "nodes/n/rotation/w");
    EXPECT_EQ(f.scene->animations[1].parameter, aether::AnimationTrack::Parameter::Rotation);
    EXPECT_EQ(f.scene->animations[1].componentIndex, 3U);
    EXPECT_EQ(f.scene->animations[2].target, "nodes/n/scale/z");
    EXPECT_EQ(f.scene->animations[2].parameter, aether::AnimationTrack::Parameter::Scale);
    EXPECT_EQ(f.scene->animations[2].componentIndex, 2U);

    // A whole-parameter track keeps kNoComponent.
    AnimScene whole{"component_fields_whole",
                    std::string{kBallNode} +
                        R"toml([[animation]]
target = "nodes/n/translation"
times = [0.0]
values = [1.0, 2.0, 3.0]
)toml"};
    ASSERT_TRUE(whole.scene.has_value()) << whole.error;
    EXPECT_EQ(whole.scene->animations[0].componentIndex, aether::AnimationTrack::kNoComponent);
}

TEST(SceneAnimator, UnknownComponentFailsParse) {
    // Component names validate per parameter (translation/scale: x|y|z;
    // rotation: x|y|z|w); unknown components name the parameter in the error.
    const auto check = [](std::string_view name, std::string_view target) {
        AnimScene f{std::string{name},
                    std::string{kBallNode} + R"toml([[animation]]
target = ")toml" + std::string{target} +
                        R"toml("
times = [0.0]
values = [1.0]
)toml"};
        EXPECT_FALSE(f.scene.has_value()) << name;
        if (!f.scene.has_value()) {
            EXPECT_NE(f.error.find("unknown component"), std::string::npos) << name << ": " << f.error;
        }
    };
    check("component_translation_q", "nodes/n/translation/q");
    check("component_rotation_a", "nodes/n/rotation/a");
    check("component_scale_w", "nodes/n/scale/w"); // w exists only on rotation
    check("component_deeper_path", "nodes/n/translation/x/y");

    // An unknown PARAMETER with a component-looking tail stays a parameter
    // error (the parameter token does not resolve either).
    AnimScene unknownParam{"component_unknown_param",
                           std::string{kBallNode} +
                               R"toml([[animation]]
target = "nodes/n/color/red"
times = [0.0]
values = [1.0]
)toml"};
    EXPECT_FALSE(unknownParam.scene.has_value());
    EXPECT_NE(unknownParam.error.find("unknown parameter"), std::string::npos) << "error: " << unknownParam.error;
}

TEST(SceneAnimator, ComponentTargetsEvaluateAtTick) {
    // Component tracks compose with the tick clock end-to-end.
    AnimScene f{"component_tick",
                std::string{kBallNode} +
                    R"toml([[animation]]
target = "nodes/n/translation/x"
interpolation = "linear"
times = [0.0, 1.0]
values = [0.0, 1.0]
)toml"};
    ASSERT_TRUE(f.scene.has_value()) << f.error;
    const auto x = [&](std::uint64_t tick) {
        return aether::SceneAnimator::evaluateAtTick(*f.scene, tick).instances[0].translation.x;
    };
    EXPECT_FLOAT_EQ(x(0), 0.0F);
    EXPECT_FLOAT_EQ(x(15), 0.5F); // tick 15 at fps 30 = 0.5 s
    EXPECT_FLOAT_EQ(x(30), 1.0F);
}

} // namespace
