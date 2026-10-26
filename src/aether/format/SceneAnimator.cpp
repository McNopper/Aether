#include "aether/format/SceneAnimator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "aether/format/NodeGraph.hpp"

namespace aether {
namespace {

// ── Parse-side helpers ([[animation]] TOML reading) ─────────────────────────

/// Read a TOML array of numbers into @p out (double → float narrowing).
/// @p label names the track in error messages. A missing or empty array and
/// non-numeric or non-finite elements are errors (validity: finite keys only,
/// empty tracks rejected).
[[nodiscard]] bool readFloatArray(const toml::table& tbl,
                                  std::string_view key,
                                  std::vector<float>& out,
                                  const std::string& label,
                                  std::string* error) {
    const toml::array* arr = tbl[key].as_array();
    if (arr == nullptr || arr->empty()) {
        if (error != nullptr) {
            *error = label + ": missing or empty '" + std::string{key} + "'";
        }
        return false;
    }
    out.reserve(out.size() + arr->size());
    for (const toml::node& elem : *arr) {
        const auto value = elem.value<double>();
        if (!value || !std::isfinite(*value)) {
            if (error != nullptr) {
                *error = label + ": '" + std::string{key} + "' must be finite numbers";
            }
            return false;
        }
        out.push_back(static_cast<float>(*value));
    }
    return true;
}

/// Floats per key for @p interpolation on @p binding: `arity` per key, or
/// `3 * arity` for glTF CUBICSPLINE (per key: inTangent, value, outTangent).
[[nodiscard]] std::size_t keyFloatStride(const AnimationTargetBinding& binding,
                                         AnimationTrack::Interpolation interpolation) {
    return static_cast<std::size_t>(binding.arity) *
           (interpolation == AnimationTrack::Interpolation::CubicSpline ? 3U : 1U);
}

} // namespace

bool parseAnimations(const toml::table& root, SceneDesc& desc, std::string* error) {
    const toml::array* entries = root["animation"].as_array();
    if (entries == nullptr) {
        return true; // no [[animation]] — the scene is untouched
    }

    // Nodes are fully parsed (and resolved) before this runs, so targets can
    // bind in the same pass that validates each entry.
    for (const toml::node& elem : *entries) {
        const toml::table* tbl = elem.as_table();
        if (tbl == nullptr) {
            continue; // like every section: non-table entries are skipped
        }
        AnimationTrack track{};

        // target = "nodes/<id>/<parameter>" (required)
        const auto target = (*tbl)["target"].value<std::string>();
        if (!target || target->empty()) {
            if (error != nullptr) {
                *error = "animation without a 'target' (expected \"nodes/<id>/<parameter>\")";
            }
            return false;
        }
        track.target = *target;
        const std::string label = "animation '" + track.target + "'";

        // interpolation (optional, default linear); unknown token: error
        if (const auto interpolation = (*tbl)["interpolation"].value<std::string>()) {
            if (*interpolation == "step") {
                track.interpolation = AnimationTrack::Interpolation::Step;
            } else if (*interpolation == "linear") {
                track.interpolation = AnimationTrack::Interpolation::Linear;
            } else if (*interpolation == "cubic_spline") {
                track.interpolation = AnimationTrack::Interpolation::CubicSpline;
            } else {
                if (error != nullptr) {
                    *error = label + ": unknown interpolation '" + *interpolation + "' (step | linear | cubic_spline)";
                }
                return false;
            }
        }

        // Bind the parameter by name through the registry; the node id
        // resolves below against the completed [[node]] list.
        std::string_view nodeId;
        std::string_view parameterName;
        if (!splitAnimationTarget(track.target, nodeId, parameterName)) {
            if (error != nullptr) {
                *error = label + ": malformed target (expected \"nodes/<id>/<parameter>\")";
            }
            return false;
        }
        const AnimationTargetBinding* binding = findAnimationTargetBinding(parameterName);
        if (binding == nullptr) {
            if (error != nullptr) {
                *error =
                    label + ": unknown parameter '" + std::string{parameterName} + "' (translation | rotation | scale)";
            }
            return false;
        }
        track.parameter = binding->parameter;

        // times + values: present, non-empty, numeric, finite.
        if (!readFloatArray(*tbl, "times", track.times, label, error) ||
            !readFloatArray(*tbl, "values", track.values, label, error)) {
            return false;
        }

        // values size = keys * stride (wrong arity: error)
        const std::size_t stride = keyFloatStride(*binding, track.interpolation);
        if (track.values.size() != track.times.size() * stride) {
            if (error != nullptr) {
                *error = label + ": wrong value arity — expected " + std::to_string(track.times.size() * stride) +
                         " float(s) (" + std::to_string(track.times.size()) + " key(s) x stride " +
                         std::to_string(stride) + "), got " + std::to_string(track.values.size());
            }
            return false;
        }

        // Validity: strictly ascending key times.
        for (std::size_t i = 1; i < track.times.size(); ++i) {
            if (!(track.times[i - 1] < track.times[i])) {
                if (error != nullptr) {
                    *error = label + ": times must be strictly ascending";
                }
                return false;
            }
        }

        // Load-time validity: every key VALUE passes the binding's policy
        // (rotation keys normalize; scale floors). CUBICSPLINE tangents are
        // tangent-space vectors, NOT rotations — only the value third is
        // policy-applied.
        const std::size_t valueOffset =
            track.interpolation == AnimationTrack::Interpolation::CubicSpline ? binding->arity : 0U;
        for (std::size_t k = 0; k < track.times.size(); ++k) {
            binding->makeValid(track.values.data() + k * stride + valueOffset, binding->arity);
        }

        // Node binding: the target's node id must name a [[node]] entry.
        const auto nodeIt = std::find_if(desc.nodes.begin(), desc.nodes.end(), [&nodeId](const NodeDesc& node) {
            return std::string_view{node.id} == nodeId;
        });
        if (nodeIt == desc.nodes.end()) {
            if (error != nullptr) {
                *error = label + ": unknown node '" + std::string{nodeId} + "'";
            }
            return false;
        }
        track.nodeIndex = static_cast<std::size_t>(nodeIt - desc.nodes.begin());

        desc.animations.push_back(std::move(track));
    }
    return true;
}

namespace {

// ── Evaluation core (generic over the target bindings) ─────────────────────

/// Greatest key index with times[index] <= t (0 when t precedes the first
/// key; the u clamp below turns that into the endpoint value).
[[nodiscard]] std::size_t keyAtOrBefore(const float* times, std::size_t keyCount, float t) {
    std::size_t k = 0;
    while (k + 1 < keyCount && times[k + 1] <= t) {
        ++k;
    }
    return k;
}

/// Sample @p track at @p t (glTF 2.0 sampler semantics, 1:1) into @p out
/// (binding.arity floats), then apply the binding's validity policy.
///
/// STEP: the last key with time <= t (constant to the end of each key).
/// LINEAR: binding.blend between the surrounding keys (component lerp; the
/// rotation binding slerps shortest-arc). CUBIC_SPLINE: Hermite basis
/// h00*v_k + h10*dt*outT_k + h01*v_{k+1} + h11*dt*inT_{k+1}, then makeValid.
/// Time outside [first, last] clamps to the endpoint key's value; a
/// single-key track is constant (the k + 1 == keyCount branch).
void sampleTrack(const AnimationTrack& track, const AnimationTargetBinding& binding, float t, float* out) {
    const std::size_t keyCount = track.times.size();
    const bool spline = track.interpolation == AnimationTrack::Interpolation::CubicSpline;
    const std::size_t stride = static_cast<std::size_t>(binding.arity) * (spline ? 3U : 1U);
    // A key's value slot: offset 0 (stride == arity), or the middle third of
    // a CUBICSPLINE key block [inTangent, value, outTangent].
    const std::size_t valueOffset = spline ? binding.arity : 0U;
    const float* times = track.times.data();
    const float* values = track.values.data();
    const std::size_t k = keyAtOrBefore(times, keyCount, t);
    const auto key = [&](std::size_t index) { return values + index * stride + valueOffset; };

    if (track.interpolation == AnimationTrack::Interpolation::Step || k + 1 == keyCount) {
        const float* v = key(k);
        for (std::uint8_t c = 0; c < binding.arity; ++c) {
            out[c] = v[c];
        }
    } else {
        const float u = sm::clamp((t - times[k]) / (times[k + 1] - times[k]), 0.0F, 1.0F);
        if (track.interpolation == AnimationTrack::Interpolation::Linear) {
            binding.blend(key(k), key(k + 1), u, out, binding.arity);
        } else { // CUBIC_SPLINE
            const float dt = times[k + 1] - times[k];
            const float* v0 = key(k);
            const float* v1 = key(k + 1);
            const float* outTangent0 = v0 + binding.arity; // [in, value, out] → out follows the value
            const float* inTangent1 = v1 - binding.arity;  // ... and in precedes it
            const float u2 = u * u;
            const float u3 = u2 * u;
            const float h00 = 2.0F * u3 - 3.0F * u2 + 1.0F;
            const float h10 = u3 - 2.0F * u2 + u;
            const float h01 = -2.0F * u3 + 3.0F * u2;
            const float h11 = u3 - u2;
            for (std::uint8_t c = 0; c < binding.arity; ++c) {
                out[c] = h00 * v0[c] + h10 * dt * outTangent0[c] + h01 * v1[c] + h11 * dt * inTangent1[c];
            }
        }
    }
    binding.makeValid(out, binding.arity);
}

/// Meshed [[node]] entries emit exactly one instance each during resolution
/// (NH1), appended AFTER the flat-authored ones — so the authored flat list
/// is exactly the first `instances.size() - meshedCount` entries.
[[nodiscard]] std::size_t meshedNodeCount(const std::vector<NodeDesc>& nodes) {
    std::size_t count = 0;
    for (const NodeDesc& node : nodes) {
        if (!node.meshName.empty()) {
            ++count;
        }
    }
    return count;
}

} // namespace

SceneDesc SceneAnimator::evaluate(const SceneDesc& scene, float timeSeconds) {
    SceneDesc out = scene; // the ONE per-call copy: sampled tracks mutate the copy's nodes
    if (out.animations.empty()) {
        return out; // animation-free scenes: the parse result verbatim, for any t
    }

    // 1) Evaluate tracks → node local TRS. Declaration order: a later track
    //    driving the same node parameter wins.
    float sampled[kMaxAnimationArity]; // stack scratch, reused across tracks
    for (const AnimationTrack& track : out.animations) {
        const AnimationTargetBinding& binding = animationTargetBinding(track.parameter);
        sampleTrack(track, binding, timeSeconds, sampled);
        binding.apply(out.nodes[track.nodeIndex], sampled);
    }

    // 2) Re-compound: the authored flat instances stay first; the node-derived
    //    ones are re-resolved from the sampled world TRS. Validation passed at
    //    parse time, so this cannot fail for a parser-produced scene (and
    //    resolveNodes validates before it mutates — a hand-built broken graph
    //    simply keeps its flat list).
    const std::size_t flatCount = out.instances.size() - std::min(out.instances.size(), meshedNodeCount(out.nodes));
    out.instances.resize(flatCount);
    [[maybe_unused]] const bool resolved = resolveNodes(out, nullptr);
    return out;
}

} // namespace aether
