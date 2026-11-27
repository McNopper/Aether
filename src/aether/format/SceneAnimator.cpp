#include "aether/format/SceneAnimator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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

bool parseMorphs(const toml::table& root, SceneDesc& desc, std::string* error) {
    const toml::array* morphs = root["morph"].as_array();
    if (morphs == nullptr) {
        return true; // morph-free scenes
    }

    auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };

    for (const toml::node& elem : *morphs) {
        const toml::table* t = elem.as_table();
        if (t == nullptr) {
            return fail("morph entry must be a table");
        }

        MorphTarget morph;
        morph.meshName = (*t)["mesh"].value_or<std::string>("");
        morph.name = (*t)["name"].value_or<std::string>("");
        if (morph.meshName.empty()) {
            return fail("morph entry is missing 'mesh'");
        }
        if (morph.name.empty()) {
            return fail("morph entry on mesh '" + morph.meshName + "' is missing 'name'");
        }

        // The referenced mesh must exist and be morphable at parse time:
        // procedural spheres have renderer-defined tessellation (no stable
        // vertex count), exactly the ANI4 skin rule.
        const auto meshIt = std::find_if(desc.meshes.begin(), desc.meshes.end(), [&morph](const MeshDesc& mesh) {
            return mesh.name == morph.meshName;
        });
        if (meshIt == desc.meshes.end()) {
            return fail("morph '" + morph.name + "' references unknown mesh '" + morph.meshName + "'");
        }
        if (meshIt->kind == MeshDesc::Kind::Sphere) {
            return fail("morph '" + morph.name + "' references procedural sphere mesh '" + morph.meshName +
                        "' - unsupported (morph object/box meshes)");
        }

        const std::string label = "morph '" + morph.meshName + "/" + morph.name + "'";
        // Duplicate (mesh, name): a track target must address exactly one delta set.
        const auto dupIt = std::find_if(desc.morphs.begin(), desc.morphs.end(), [&morph](const MorphTarget& prior) {
            return prior.meshName == morph.meshName && prior.name == morph.name;
        });
        if (dupIt != desc.morphs.end()) {
            return fail(label + ": duplicate target name on the same mesh");
        }

        // Initial weight (optional): finite scalar, default 0. The live vector
        // stays parallel to desc.morphs; morph-weight tracks overwrite it.
        float weight = 0.0F;
        if (const auto authored = (*t)["weight"].value<double>()) {
            if (!std::isfinite(*authored)) {
                return fail(label + ": 'weight' must be a finite number");
            }
            weight = static_cast<float>(*authored);
        }

        const toml::array* deltas = (*t)["deltas"].as_array();
        if (deltas == nullptr || deltas->empty()) {
            return fail(label + ": missing or empty 'deltas' (one [dx, dy, dz] row per vertex)");
        }
        morph.deltas.reserve(deltas->size());
        for (const toml::node& rowNode : *deltas) {
            const toml::array* row = rowNode.as_array();
            if (row == nullptr || row->size() != 3) {
                return fail(label + ": each 'deltas' row must hold exactly 3 floats (dx, dy, dz)");
            }
            Vec3 delta{};
            for (std::size_t c = 0; c < 3; ++c) {
                const auto value = (*row)[c].value<double>();
                if (!value || !std::isfinite(*value)) {
                    return fail(label + ": 'deltas' must be finite numbers");
                }
                delta[static_cast<std::int32_t>(c)] = static_cast<float>(*value);
            }
            morph.deltas.push_back(delta);
        }

        desc.morphWeights.push_back(weight);
        desc.morphs.push_back(std::move(morph));
    }
    return true;
}

bool parseAnimations(const toml::table& root, SceneDesc& desc, std::string* error) {
    const toml::array* entries = root["animation"].as_array();

    // [animation_clock] — the scene's tick clock (ANI2): the deterministic
    // ticks-per-second base the tick APIs divide by (seconds = tick / fps),
    // independent of the render rate. Required (positive) whenever
    // [[animation]] tracks exist; validated whenever authored. Track times
    // remain seconds, exactly as before.
    if (const toml::table* clock = root["animation_clock"].as_table()) {
        const auto fps = (*clock)["fps"].value<double>();
        if (!fps || !std::isfinite(*fps) || *fps <= 0.0) {
            if (error != nullptr) {
                *error =
                    "animation clock: 'fps' must be a positive number ([animation_clock] fps = <ticks per second>)";
            }
            return false;
        }
        desc.animationFps = *fps;
    } else if (entries != nullptr && !entries->empty()) {
        if (error != nullptr) {
            *error = "animation clock: missing [animation_clock] fps — required (a positive ticks-per-second) "
                     "when [[animation]] tracks exist";
        }
        return false;
    }

    if (entries == nullptr) {
        return true; // no [[animation]] — only the optional (validated) clock was read
    }

    // Nodes are fully parsed (and resolved) before this runs, so targets can
    // bind in the same pass that validates each entry.
    for (const toml::node& elem : *entries) {
        const toml::table* tbl = elem.as_table();
        if (tbl == nullptr) {
            continue; // like every section: non-table entries are skipped
        }
        AnimationTrack track{};

        // target = "nodes/<id>/<parameter>" or, ANI5, "morphs/<mesh>/<name>"
        // (required)
        const auto target = (*tbl)["target"].value<std::string>();
        if (!target || target->empty()) {
            if (error != nullptr) {
                *error = "animation without a 'target' (expected \"nodes/<id>/<parameter>\" or "
                         "\"morphs/<mesh>/<name>\")";
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

        // Bind the parameter through the registry: ANI5 morph-weight tracks
        // resolve through the `morphs/<mesh>/<name>` grammar against the
        // completed morph list; everything else is `nodes/<id>/<parameter>`
        // and resolves against the completed [[node]] list below.
        std::string_view nodeId;
        std::string_view parameterName;
        std::size_t morphIndex = 0;
        const bool isMorphTarget = track.target.starts_with("morphs/");
        const AnimationTargetBinding* binding = nullptr;
        if (isMorphTarget) {
            std::string_view meshName;
            std::string_view targetName;
            if (!splitMorphTarget(track.target, meshName, targetName)) {
                if (error != nullptr) {
                    *error = label + ": malformed target (expected \"morphs/<mesh>/<name>\")";
                }
                return false;
            }
            const auto morphIt = std::find_if(desc.morphs.begin(), desc.morphs.end(), [&](const MorphTarget& morph) {
                return std::string_view{morph.meshName} == meshName && std::string_view{morph.name} == targetName;
            });
            if (morphIt == desc.morphs.end()) {
                if (error != nullptr) {
                    *error = label + ": unknown morph target '" + std::string{meshName} + "/" +
                             std::string{targetName} + "'";
                }
                return false;
            }
            morphIndex = static_cast<std::size_t>(morphIt - desc.morphs.begin());
            binding = &animationTargetBinding(AnimationTrack::Parameter::MorphWeight, AnimationTrack::kNoComponent);
            track.parameter = binding->parameter;
            track.componentIndex = binding->componentIndex;
        } else {
            if (!splitAnimationTarget(track.target, nodeId, parameterName)) {
                if (error != nullptr) {
                    *error = label + ": malformed target (expected \"nodes/<id>/<parameter>\" or "
                                     "\"morphs/<mesh>/<name>\")";
                }
                return false;
            }
            binding = findAnimationTargetBinding(parameterName);
            if (binding == nullptr) {
                if (error != nullptr) {
                    // A 4-segment target names a component: when the parameter
                    // itself is known, report the component case precisely.
                    const std::size_t slash = parameterName.find('/');
                    if (slash != std::string_view::npos &&
                        findAnimationTargetBinding(parameterName.substr(0, slash)) != nullptr) {
                        *error = label + ": unknown component '" + std::string{parameterName.substr(slash + 1)} +
                                 "' for parameter '" + std::string{parameterName.substr(0, slash)} +
                                 "' (translation/scale: x | y | z; rotation: x | y | z | w)";
                    } else {
                        *error = label + ": unknown parameter '" + std::string{parameterName} +
                                 "' (translation | rotation | scale)";
                    }
                }
                return false;
            }
            if (binding->parameter == AnimationTrack::Parameter::MorphWeight) {
                // 'weight' is reachable ONLY through the morphs/ grammar - a
                // node must never silently drive desc.morphWeights[nodeIndex].
                if (error != nullptr) {
                    *error = label + ": 'weight' is a morph-weight token - address it as \"morphs/<mesh>/<name>\"";
                }
                return false;
            }
            track.parameter = binding->parameter;
            track.componentIndex = binding->componentIndex;
        }

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

        // Target index binding: morph tracks were resolved above against the
        // morph list (nodeIndex = morph index); node tracks need the id to
        // name a [[node]] entry.
        if (isMorphTarget) {
            track.nodeIndex = morphIndex;
        } else {
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
        }

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

/// ANI2 - the tick clock, shared by every tick entry point: integer tick →
/// seconds in double precision, then the float the sampler runs on. The
/// conversion is a pure function of (tick, fps): the same tick always yields
/// bit-identical times, so repeated evaluations - and evaluations across
/// SceneDesc copies - produce identical instance lists regardless of the
/// render rate.
[[nodiscard]] float tickSeconds(const SceneDesc& scene, std::uint64_t tick) {
    return static_cast<float>(static_cast<double>(tick) / scene.animationFps);
}

/// The output storage must be a distinct object: the evaluation reads the
/// immutable input while it writes the output. Aliasing the two would let the
/// sampled tracks stamp the input scene, so it is rejected before any write
/// (the library's evaluation API has no error out-parameter; an invalid
/// argument is reported as an exception, in every build type).
void requireDistinctOutput(const SceneDesc& scene, const SceneDesc& out) {
    if (&out == &scene) {
        throw std::invalid_argument(
            "SceneAnimator: the output SceneDesc must not be the input scene - evaluate into a separate SceneDesc");
    }
}

} // namespace

SceneDesc SceneAnimator::evaluate(const SceneDesc& scene, float timeSeconds) {
    SceneDesc out;
    evaluate(scene, timeSeconds, out);
    return out;
}

void SceneAnimator::evaluate(const SceneDesc& scene, float timeSeconds, SceneDesc& out) {
    requireDistinctOutput(scene, out);

    // The ONE sync with the input: a copy assignment. Every element is
    // assigned in place, so a reused @p out keeps its storage (vector
    // capacities, string buffers, material-map nodes) — a repeated evaluation
    // of the same scene copies values, not allocations.
    out = scene; // the sampled tracks mutate the copy's nodes
    if (out.animations.empty()) {
        return; // animation-free scenes: the parse result verbatim, for any t
    }

    // 1) Evaluate tracks -> node local TRS (or, ANI5, the morph weight).
    //    Declaration order: a later track driving the same target wins
    //    (whole-parameter or component).
    float sampled[kMaxAnimationArity]; // stack scratch, reused across tracks
    for (const AnimationTrack& track : out.animations) {
        const AnimationTargetBinding& binding = animationTargetBinding(track.parameter, track.componentIndex);
        sampleTrack(track, binding, timeSeconds, sampled);
        if (track.parameter == AnimationTrack::Parameter::MorphWeight) {
            // ANI5: nodeIndex is the morph index for these tracks; the sink
            // is the live weight vector, not a NodeDesc (binding.apply is
            // null - the registry row documents this special case).
            if (track.nodeIndex < out.morphWeights.size()) {
                out.morphWeights[track.nodeIndex] = sampled[0];
            }
            continue;
        }
        binding.apply(out.nodes[track.nodeIndex], sampled);
    }

    // 2) Re-compound: the authored flat instances stay first; the node-derived
    //    ones are re-resolved from the sampled world TRS, overwriting the
    //    parse-time entries in place. Validation passed at parse time, so this
    //    cannot fail for a parser-produced scene (and resolveNodes validates
    //    before it mutates - a hand-built broken graph simply keeps its flat
    //    list, like before).
    const std::size_t flatCount = out.instances.size() - std::min(out.instances.size(), meshedNodeCount(out.nodes));
    if (!resolveNodes(out, nullptr, flatCount)) {
        out.instances.resize(flatCount);
    }
}

SceneDesc SceneAnimator::evaluateAtTick(const SceneDesc& scene, std::uint64_t tick) {
    SceneDesc out;
    evaluateAtTick(scene, tick, out);
    return out;
}

void SceneAnimator::evaluateAtTick(const SceneDesc& scene, std::uint64_t tick, SceneDesc& out) {
    requireDistinctOutput(scene, out);
    if (!(scene.animationFps > 0.0)) {
        out = scene; // degenerate hand-built clock: no meaningful time axis
        return;
    }
    evaluate(scene, tickSeconds(scene, tick), out);
}

std::optional<CameraPose> SceneAnimator::evaluateCamera(const SceneDesc& scene, float timeSeconds) {
    SceneDesc stamped;
    return evaluateCamera(scene, timeSeconds, stamped);
}

std::optional<CameraPose> SceneAnimator::evaluateCamera(const SceneDesc& scene, float timeSeconds, SceneDesc& out) {
    requireDistinctOutput(scene, out);
    if (scene.camera.nodeId) {
        // evaluate() stamps the node locals AND re-resolves nodeWorlds, so
        // the world the pose extraction reads is exactly this frame's (same
        // contract the skin evaluation relies on).
        evaluate(scene, timeSeconds, out);
        return cameraPoseFromEvaluated(out);
    }
    // A static placement does not depend on time: the scene is its own
    // evaluated scene, so nothing is evaluated and @p out stays untouched.
    return cameraPoseFromEvaluated(scene);
}

std::optional<CameraPose> SceneAnimator::cameraPoseFromEvaluated(const SceneDesc& stamped) {
    if (stamped.camera.nodeId) {
        const std::string& want = *stamped.camera.nodeId;
        const auto it = std::find_if(
            stamped.nodes.begin(), stamped.nodes.end(), [&want](const NodeDesc& node) { return node.id == want; });
        if (it == stamped.nodes.end()) {
            return std::nullopt; // parse-validated; defensive for hand-built scenes
        }
        const auto index = static_cast<std::size_t>(it - stamped.nodes.begin());
        if (index >= stamped.nodeWorlds.size()) {
            return std::nullopt; // unresolved graph - no world to read
        }
        const Mat4& world = stamped.nodeWorlds[index];

        CameraPose pose;
        pose.position = Vec3{world[0][3], world[1][3], world[2][3]};
        // Column-normalized basis: camera-node scale is ignored (the DQS
        // rigid projection - sm::toQuaternion's input contract is orthonormal).
        Vec3 col[3];
        for (std::size_t j = 0; j < 3; ++j) {
            const auto ji = static_cast<std::int32_t>(j);
            col[j] = Vec3{world[0][ji], world[1][ji], world[2][ji]};
            const float len = sm::length(col[j]);
            if (len <= 1.0e-20F) {
                return std::nullopt; // degenerate basis: not a pose
            }
            col[j] = col[j] * (1.0F / len);
        }
        const sm::float3x3 rotation{
            {col[0].x, col[1].x, col[2].x}, {col[0].y, col[1].y, col[2].y}, {col[0].z, col[1].z, col[2].z}};
        pose.rotation = sm::toQuaternion(rotation);
        return pose;
    }

    if (!stamped.camera.translation && !stamped.camera.rotation) {
        return std::nullopt; // no placement authored (no node, no TRS)
    }
    CameraPose pose;
    pose.position = stamped.camera.translation.value_or(Vec3{0.0F, 0.0F, 0.0F});
    pose.rotation = stamped.camera.rotation.value_or(Quat{0.0F, 0.0F, 0.0F, 1.0F});
    return pose;
}

std::optional<CameraPose> SceneAnimator::evaluateCameraAtTick(const SceneDesc& scene, std::uint64_t tick) {
    SceneDesc stamped;
    return evaluateCameraAtTick(scene, tick, stamped);
}

std::optional<CameraPose>
SceneAnimator::evaluateCameraAtTick(const SceneDesc& scene, std::uint64_t tick, SceneDesc& out) {
    requireDistinctOutput(scene, out);
    if (!(scene.animationFps > 0.0)) {
        return evaluateCamera(scene, 0.0F, out); // degenerate hand-built clock: static placement
    }
    return evaluateCamera(scene, tickSeconds(scene, tick), out);
}

std::vector<SkinPose> SceneAnimator::evaluateSkins(const SceneDesc& scene, float timeSeconds) {
    SceneDesc stamped;
    return evaluateSkins(scene, timeSeconds, stamped);
}

std::vector<SkinPose> SceneAnimator::evaluateSkins(const SceneDesc& scene, float timeSeconds, SceneDesc& out) {
    requireDistinctOutput(scene, out);
    if (scene.skins.empty()) {
        return {}; // skin-free scenes: no poses for any t (mirrors evaluate)
    }
    // evaluate() stamps the joint node locals AND re-resolves nodeWorlds, so
    // the world transforms read below are exactly the frame's.
    evaluate(scene, timeSeconds, out);
    const SceneDesc& stamped = out;
    if (stamped.nodeWorlds.size() != stamped.nodes.size()) {
        return {}; // hand-built scene without a resolved graph - nothing to read
    }

    std::unordered_map<std::string, std::size_t> nodeIndex;
    nodeIndex.reserve(stamped.nodes.size());
    for (std::size_t i = 0; i < stamped.nodes.size(); ++i) {
        nodeIndex.emplace(stamped.nodes[i].id, i);
    }

    std::vector<SkinPose> poses;
    poses.reserve(stamped.skins.size());
    for (const SkinDesc& skin : stamped.skins) {
        const auto nodeIt = nodeIndex.find(skin.nodeId);
        if (nodeIt == nodeIndex.end()) {
            continue; // unknown skinned node: parser-validated, defensive for hand-built
        }
        SkinPose pose;
        pose.nodeId = skin.nodeId;
        pose.skinMatrices.reserve(skin.joints.size());
        bool complete = true;
        const Mat4 invNodeWorld = sm::inverse(stamped.nodeWorlds[nodeIt->second]);
        for (std::size_t j = 0; j < skin.joints.size(); ++j) {
            const auto jointIt = nodeIndex.find(skin.joints[j]);
            if (jointIt == nodeIndex.end()) {
                complete = false;
                break;
            }
            Mat4 inverseBind;
            for (std::size_t r = 0; r < 4; ++r) {
                const std::size_t base = j * 16 + r * 4;
                const auto b = static_cast<std::int32_t>(base);
                inverseBind[static_cast<std::int32_t>(r)] = Vec4{
                    skin.inverseBind[b], skin.inverseBind[b + 1], skin.inverseBind[b + 2], skin.inverseBind[b + 3]};
            }
            pose.skinMatrices.push_back(invNodeWorld * stamped.nodeWorlds[jointIt->second] * inverseBind);
        }
        if (complete) {
            poses.push_back(std::move(pose));
        }
    }
    return poses;
}

std::vector<SkinPose> SceneAnimator::evaluateSkinsAtTick(const SceneDesc& scene, std::uint64_t tick) {
    SceneDesc stamped;
    return evaluateSkinsAtTick(scene, tick, stamped);
}

std::vector<SkinPose> SceneAnimator::evaluateSkinsAtTick(const SceneDesc& scene, std::uint64_t tick, SceneDesc& out) {
    requireDistinctOutput(scene, out);
    if (!(scene.animationFps > 0.0)) {
        return {};
    }
    return evaluateSkins(scene, tickSeconds(scene, tick), out);
}

std::vector<float> SceneAnimator::evaluateMorphWeights(const SceneDesc& scene, float timeSeconds) {
    SceneDesc stamped;
    return evaluateMorphWeights(scene, timeSeconds, stamped);
}

std::vector<float> SceneAnimator::evaluateMorphWeights(const SceneDesc& scene, float timeSeconds, SceneDesc& out) {
    requireDistinctOutput(scene, out);
    if (scene.morphs.empty()) {
        return {}; // morph-free scenes: no weights for any t (mirrors evaluateSkins)
    }
    // evaluate() stamps the morph-weight tracks into the output's morphWeights
    // (and re-resolves the node graph as a side effect); animation-free scenes
    // short-circuit inside evaluate() to the verbatim copy - authored weights.
    evaluate(scene, timeSeconds, out);
    if (out.morphWeights.size() != scene.morphs.size()) {
        return {}; // hand-built scene with a non-parallel weight vector
    }
    return out.morphWeights;
}

std::vector<float> SceneAnimator::evaluateMorphWeightsAtTick(const SceneDesc& scene, std::uint64_t tick) {
    SceneDesc stamped;
    return evaluateMorphWeightsAtTick(scene, tick, stamped);
}

std::vector<float>
SceneAnimator::evaluateMorphWeightsAtTick(const SceneDesc& scene, std::uint64_t tick, SceneDesc& out) {
    requireDistinctOutput(scene, out);
    if (!(scene.animationFps > 0.0)) {
        return {};
    }
    return evaluateMorphWeights(scene, tickSeconds(scene, tick), out);
}

} // namespace aether
