#ifndef AETHER_FORMAT_SCENEANIMATOR_HPP
#define AETHER_FORMAT_SCENEANIMATOR_HPP

#include <cstdint>
#include <slang-math/slang-math.hpp>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

#include "aether/types/Math.hpp"
#include "aether/types/SceneDesc.hpp"

namespace aether {

/// ANI1 — keyframed transform channels: schema, evaluation.
///
/// THE ARCHITECTURE DIRECTIVE: the renderers (Harmonia / Theia / Hyperion)
/// ALWAYS receive only the flat, world-resolved instance list. ALL animation
/// logic — tracks, interpolation, evaluation — lives in Aether: a renderer
/// advances the clock and calls `SceneAnimator::evaluate(scene, t)`, which
/// samples every `[[animation]]` track, writes the node-local TRS and re-runs
/// the NH1 compounding. Keyframes never leave Aether.
///
/// THE EVALUATOR IS GENERIC: sampling works on timed float sequences, and
/// what a track drives is a TARGET BINDING — `AnimationTargetBinding`, the
/// constexpr registry below — carrying arity, blend, validity and write-back
/// policies. No channel is special-cased in the sampling core; a new
/// drivable parameter is one registry row (plus an `AnimationTrack::Parameter`
/// enumerator), never a new branch in the evaluator.
///
/// TOML SCHEMA (one `[[animation]]` per track; see SceneParser.hpp):
///   [animation_clock] fps = 30        # the scene's tick clock (ANI2):
///                                     # positive ticks-per-second; required
///                                     # when any [[animation]] track exists
///   target = "nodes/<id>/<parameter>"  # parameter: translation|rotation|scale
///                                      # or "<parameter>/<component>" — a
///                                      # scalar component target (translation
///                                      # /scale: x|y|z, rotation: x|y|z|w)
///   interpolation = "step" | "linear" | "cubic_spline"  # optional, default
///                                                      # "linear" (glTF 2.0
///                                                      # sampler semantics)
///   times  = [ ... ]  # finite, strictly ascending
///   values = [ ... ]  # flat floats; `arity` per key — for cubic_spline each
///                     # key carries inTangent, value, outTangent (3 * arity)
///
/// VALIDITY (clamp/normalize on the whole valid parameter range): time
/// outside `[times.front, times.back]` clamps to the endpoint key's value
/// (loop/wrap reserved, not in scope); rotation keys are normalized at load
/// and every sampled quaternion is renormalized; scale components floor away
/// from zero (`kScaleFloor`); non-finite values, non-ascending times, wrong
/// value arity, empty tracks and unknown targets are PARSE ERRORS; a
/// single-key track is constant everywhere. The clock, when authored, must be
/// a positive number, and it is REQUIRED when tracks exist — the tick APIs
/// divide ticks by it.

/// Scale validity floor: per-component magnitude clamp away from zero, so a
/// degenerate (zero / axis-collapsing) scale can never enter the TRS.
inline constexpr float kScaleFloor = 1.0e-6F;

// ── Binding policies ────────────────────────────────────────────────────────
// The three axes every binding defines. Function-pointer tables follow the
// MaterialKwTables setter convention: constexpr data, no dispatch branching.

/// LINEAR blend between adjacent keys (`u` in [0, 1]); the rotation binding
/// slerps. `arity` floats are read from `a`/`b` and written to `out`.
using BlendPolicy = void (*)(const float* a, const float* b, float u, float* out, std::uint8_t arity);
/// Validity enforcement, applied to every loaded key VALUE and to every
/// sample (renormalize / floor / nothing, per binding).
using ValidityPolicy = void (*)(float* v, std::uint8_t arity);
/// Write one sampled value sequence into the target node's TRS.
using ApplyPolicy = void (*)(NodeDesc& node, const float* v);

/// Component-wise lerp (the translation and scale blend).
inline void lerpComponents(const float* a, const float* b, float u, float* out, std::uint8_t arity) {
    for (std::uint8_t c = 0; c < arity; ++c) {
        out[c] = a[c] + u * (b[c] - a[c]);
    }
}

/// Rotation blend: shortest-arc slerp. The rotation binding is always arity 4
/// (x, y, z, w per family convention), so the trailing arity is unused.
inline void slerpQuatShortestArc(const float* a, const float* b, float u, float* out, std::uint8_t) {
    const Quat q = sm::slerp(Quat{a[0], a[1], a[2], a[3]}, Quat{b[0], b[1], b[2], b[3]}, u);
    out[0] = q.x;
    out[1] = q.y;
    out[2] = q.z;
    out[3] = q.w;
}

/// "Output components free" (translation): no validity constraint.
inline void keepComponents(float*, std::uint8_t) {}

/// Rotation validity: renormalize AS A WHOLE (sm::normalize maps a degenerate
/// zero quaternion to identity instead of NaN).
inline void normalizeQuatWhole(float* v, std::uint8_t) {
    const Quat q = sm::normalize(Quat{v[0], v[1], v[2], v[3]});
    v[0] = q.x;
    v[1] = q.y;
    v[2] = q.z;
    v[3] = q.w;
}

/// Scale validity: per-component magnitude floor (clamp away from zero).
inline void floorScaleComponents(float* v, std::uint8_t arity) {
    for (std::uint8_t c = 0; c < arity; ++c) {
        v[c] = (v[c] < kScaleFloor) ? kScaleFloor : v[c];
    }
}

inline void applyTranslation(NodeDesc& node, const float* v) {
    node.translation = Vec3{v[0], v[1], v[2]};
}
inline void applyRotation(NodeDesc& node, const float* v) {
    node.rotation = Quat{v[0], v[1], v[2], v[3]};
}
inline void applyScale(NodeDesc& node, const float* v) {
    node.scale = Vec3{v[0], v[1], v[2]};
}

/// Component-target write-backs: ONE sampled scalar into one TRS component.
/// The rotation writes renormalize the WHOLE quaternion after the component
/// write — the rotation components keep the whole-quaternion validity policy
/// (a scalar key cannot be normalized in isolation; the other components come
/// from the node's current rotation).
inline void applyTranslationX(NodeDesc& node, const float* v) {
    node.translation.x = v[0];
}
inline void applyTranslationY(NodeDesc& node, const float* v) {
    node.translation.y = v[0];
}
inline void applyTranslationZ(NodeDesc& node, const float* v) {
    node.translation.z = v[0];
}
inline void applyRotationX(NodeDesc& node, const float* v) {
    node.rotation.x = v[0];
    node.rotation = sm::normalize(node.rotation);
}
inline void applyRotationY(NodeDesc& node, const float* v) {
    node.rotation.y = v[0];
    node.rotation = sm::normalize(node.rotation);
}
inline void applyRotationZ(NodeDesc& node, const float* v) {
    node.rotation.z = v[0];
    node.rotation = sm::normalize(node.rotation);
}
inline void applyRotationW(NodeDesc& node, const float* v) {
    node.rotation.w = v[0];
    node.rotation = sm::normalize(node.rotation);
}
inline void applyScaleX(NodeDesc& node, const float* v) {
    node.scale.x = v[0];
}
inline void applyScaleY(NodeDesc& node, const float* v) {
    node.scale.y = v[0];
}
inline void applyScaleZ(NodeDesc& node, const float* v) {
    node.scale.z = v[0];
}

/// One target binding: what a track's timed floats MEAN. The first registry
/// (constexpr data): the addressable node parameters — whole-parameter rows
/// AND scalar component rows (same policies as their parent parameter, arity
/// 1). `nodes/<id>/<name>` binds a track through `name` (a whole parameter or
/// a `<parameter>/<component>` token); an unknown node, parameter or
/// component name is a parse error. LINEAR interpolation uses `blend`; STEP
/// and the clamp paths copy keys; CUBIC_SPLINE is Hermite over the key values
/// + tangents component-wise, then `makeValid` (a documented tangent-space
/// approximation for rotations, renormalized — not a true spherical spline).
/// A component row's `blend`/`makeValid` are the parent policies applied at
/// arity 1 (component lerp / per-component floor); the rotation components
/// keep the whole-quaternion validity policy in their `apply` (renormalize
/// the entire quaternion after the component write).
struct AnimationTargetBinding {
    AnimationTrack::Parameter parameter = AnimationTrack::Parameter::Translation; ///< registry key
    std::string_view name;                                                        ///< TOML parameter token
    std::uint8_t arity = 0;                                                       ///< floats per key value
    std::uint8_t componentIndex = AnimationTrack::kNoComponent;                   ///< driven component (0-based x..w)
    BlendPolicy blend = nullptr;        ///< LINEAR blend between adjacent keys
    ValidityPolicy makeValid = nullptr; ///< every loaded key value and every sample
    ApplyPolicy apply = nullptr;        ///< sample → node TRS
};

/// The target registry, as constexpr data (declaration order = registry
/// order; whole parameters first, then their scalar components).
inline constexpr AnimationTargetBinding kAnimationTargetBindings[]{
    {AnimationTrack::Parameter::Translation,
     "translation",
     3,
     AnimationTrack::kNoComponent,
     &lerpComponents,
     &keepComponents,
     &applyTranslation},
    {AnimationTrack::Parameter::Rotation,
     "rotation",
     4,
     AnimationTrack::kNoComponent,
     &slerpQuatShortestArc,
     &normalizeQuatWhole,
     &applyRotation},
    {AnimationTrack::Parameter::Scale,
     "scale",
     3,
     AnimationTrack::kNoComponent,
     &lerpComponents,
     &floorScaleComponents,
     &applyScale},

    {AnimationTrack::Parameter::Translation,
     "translation/x",
     1,
     0,
     &lerpComponents,
     &keepComponents,
     &applyTranslationX},
    {AnimationTrack::Parameter::Translation,
     "translation/y",
     1,
     1,
     &lerpComponents,
     &keepComponents,
     &applyTranslationY},
    {AnimationTrack::Parameter::Translation,
     "translation/z",
     1,
     2,
     &lerpComponents,
     &keepComponents,
     &applyTranslationZ},

    {AnimationTrack::Parameter::Rotation, "rotation/x", 1, 0, &lerpComponents, &keepComponents, &applyRotationX},
    {AnimationTrack::Parameter::Rotation, "rotation/y", 1, 1, &lerpComponents, &keepComponents, &applyRotationY},
    {AnimationTrack::Parameter::Rotation, "rotation/z", 1, 2, &lerpComponents, &keepComponents, &applyRotationZ},
    {AnimationTrack::Parameter::Rotation, "rotation/w", 1, 3, &lerpComponents, &keepComponents, &applyRotationW},

    {AnimationTrack::Parameter::Scale, "scale/x", 1, 0, &lerpComponents, &floorScaleComponents, &applyScaleX},
    {AnimationTrack::Parameter::Scale, "scale/y", 1, 1, &lerpComponents, &floorScaleComponents, &applyScaleY},
    {AnimationTrack::Parameter::Scale, "scale/z", 1, 2, &lerpComponents, &floorScaleComponents, &applyScaleZ},
};

/// Registry lookup by TOML parameter token (nullptr when unknown).
[[nodiscard]] inline const AnimationTargetBinding* findAnimationTargetBinding(std::string_view name) {
    for (const AnimationTargetBinding& binding : kAnimationTargetBindings) {
        if (binding.name == name) {
            return &binding;
        }
    }
    return nullptr;
}

/// Registry lookup by the track's resolved binding — the parameter plus the
/// component index (`kNoComponent` for a whole-parameter track). Always
/// found: the parser only stores registry-known bindings; row 0 is the
/// documented fallback for a hand-built track.
[[nodiscard]] inline const AnimationTargetBinding& animationTargetBinding(AnimationTrack::Parameter parameter,
                                                                          std::uint8_t componentIndex) {
    for (const AnimationTargetBinding& binding : kAnimationTargetBindings) {
        if (binding.parameter == parameter && binding.componentIndex == componentIndex) {
            return binding;
        }
    }
    return kAnimationTargetBindings[0];
}

/// Largest arity across the registry — sizes the evaluator's stack scratch,
/// so the sampling core stays allocation-free per track.
inline constexpr std::uint8_t kMaxAnimationArity = [] {
    std::uint8_t largest = 0;
    for (const AnimationTargetBinding& binding : kAnimationTargetBindings) {
        largest = (binding.arity > largest) ? binding.arity : largest;
    }
    return largest;
}();

/// Split an authored animation target `nodes/<id>/<parameter>` — where
/// `<parameter>` is either a registry parameter token (`translation`,
/// `rotation`, `scale`) or a component token `<parameter>/<component>` (a
/// 4-segment target, e.g. `nodes/hand/rotation/w`) — into the node id and the
/// parameter token (@p nodeId / @p parameter are views into @p target; the
/// parameter view may contain exactly the one component separator; deeper
/// paths simply miss the registry and become "unknown" parse errors). False
/// unless the grammar matches — the `nodes/` prefix and non-empty id and
/// parameter segments (node ids never contain `/`).
[[nodiscard]] inline bool
splitAnimationTarget(std::string_view target, std::string_view& nodeId, std::string_view& parameter) {
    constexpr std::string_view kPrefix = "nodes/";
    if (!target.starts_with(kPrefix)) {
        return false;
    }
    const std::size_t separator = target.find('/', kPrefix.size());
    if (separator == std::string_view::npos) {
        return false;
    }
    nodeId = target.substr(kPrefix.size(), separator - kPrefix.size());
    parameter = target.substr(separator + 1);
    return !nodeId.empty() && !parameter.empty();
}

/// Parse, validate and bind every `[[animation]]` entry of @p root into
/// @p desc.animations. Called by SceneParser after the `[[node]]` block —
/// bindings resolve against the completed node list. Also parses the
/// `[animation_clock]` tick clock into @p desc.animationFps: a positive
/// number whenever authored, REQUIRED (positive) when any `[[animation]]`
/// track exists. Parse errors (reported in @p error, naming the track's
/// target; @c false return): malformed or missing target, unknown node,
/// parameter or component, unknown interpolation token, non-finite or
/// non-numeric values/times, non-ascending times, wrong value arity, empty
/// times/values, a missing or non-positive clock when tracks exist. A scene
/// without `[[animation]]` entries only parses (and validates) the optional
/// clock — its tracks stay empty and the clock, if valid, is inert.
[[nodiscard]] bool parseAnimations(const toml::table& root, SceneDesc& desc, std::string* error);

/// The evaluation API (the directive's deliverable — the renderers call
/// these; ANI1 ships `evaluate` with tests as its only caller, ANI2 adds the
/// tick clock).
class SceneAnimator {
  public:
    /// Evaluate @p scene at @p timeSeconds: sample every `[[animation]]` track
    /// (clamped to the track's key range), write the sampled TRS into the
    /// node locals, re-run the NH1 compounding and return the fully resolved
    /// SceneDesc — the flat instance list contract a parse produces, now
    /// time-parameterized. The input scene is never modified.
    ///
    /// Scenes without `[[animation]]` evaluate to an unchanged copy for any
    /// @p timeSeconds. Per-frame cost path: evaluate tracks → update node
    /// local TRS → re-compound. Allocation budget: ONE SceneDesc copy per
    /// call (the tracks mutate the copy) — nothing per sample or per track;
    /// sampling itself reads the registry-sized stack scratch. @p scene must
    /// come from a successful parse (bindings and the node graph are then
    /// valid by construction). A later track driving the same node parameter
    /// wins (declaration order).
    [[nodiscard]] static SceneDesc evaluate(const SceneDesc& scene, float timeSeconds);

    /// Evaluate @p scene at @p tick over the scene's animation clock
    /// (`animationFps`): `seconds = tick / animationFps`, then exactly
    /// `evaluate(scene, seconds)`. The tick clock (ANI2) is the deterministic
    /// time base — animation is authored and sampled on integer ticks at the
    /// scene's own fps, independent of the render rate; a renderer advances
    /// its tick counter and calls this, and two runs at the same tick always
    /// produce bit-identical instance lists. @p scene must come from a
    /// successful parse (the parser guarantees a positive clock whenever
    /// tracks exist; a degenerate non-positive clock on a hand-built scene
    /// evaluates to the unchanged copy). Scenes without `[[animation]]`
    /// return the unchanged copy for any tick, like `evaluate`.
    [[nodiscard]] static SceneDesc evaluateAtTick(const SceneDesc& scene, std::uint64_t tick);
};

} // namespace aether
#endif // AETHER_FORMAT_SCENEANIMATOR_HPP
