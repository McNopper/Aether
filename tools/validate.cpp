// aether-validate -- the scene assembly gate: a thin CLI over the public
// parser (aether::SceneParser::parse) that gives agents an instant validation
// loop for .scene.toml edits. It carries no validation logic of its own: a
// green summary means exactly "the library accepts this file".
//
//   aether-validate <scene.toml> [--tree] [--all]
//
//   (default)  one-line summary: instances / meshes / nodes / material libraries
//   --tree     additionally print the node hierarchy (two spaces per depth)
//   --all      additionally list every resolved instance with its full TRS
//
// Exit codes: 0 = the scene parses and resolves; 1 = the parser rejected it
// (the fail line quotes the library's error verbatim); 2 = usage problem
// (missing or unknown argument, scene file not found).

#include <cstddef>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "aether/format/SceneParser.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr std::string_view kProgramName = "aether-validate";
constexpr std::string_view kUsage = "usage: aether-validate <scene.toml> [--tree] [--all]";

// Summary glyphs as explicit UTF-8 byte escapes (byte-exact under every
// compiler, independent of source/execution charset; no wide-mode console
// writes, so a glyph can never crash the tool - worst case a legacy console
// shows mojibake):
constexpr std::string_view kOkGlyph = "\xE2\x9C\x94";   // U+2714 HEAVY CHECK MARK
constexpr std::string_view kFailGlyph = "\xE2\x9C\x98"; // U+2718 HEAVY BALLOT X

/// Shortest round-trip float text: authored values print back exactly as
/// written (e.g. 0.1 -> "0.1"), so agents can diff output against their TOML.
std::string formatVec3(const aether::Vec3& v) {
    return std::format("[{},{},{}]", v.x, v.y, v.z);
}

/// slang-math quaternion in (x, y, z, w) order -- the on-disk order too.
std::string formatQuat(const aether::Quat& q) {
    return std::format("[{},{},{},{}]", q.x, q.y, q.z, q.w);
}

/// Usage problem: reason plus the short usage line, on stderr (exit 2).
void usageError(const std::string& problem) {
    std::cerr << kProgramName << ": " << problem << '\n' << kUsage << '\n';
}

/// --tree: print the subtree at @p index, two spaces of indent per depth.
/// Children keep declaration order; the parser has already validated that
/// every parent id resolves and that the graph is acyclic.
void printSubtree(const std::vector<aether::NodeDesc>& nodes,
                  const std::vector<std::vector<std::size_t>>& childrenOf,
                  const std::size_t index,
                  const std::size_t depth,
                  std::ostream& out) {
    const aether::NodeDesc& node = nodes[index];
    out << std::string(depth * 2, ' ') << node.id;
    if (!node.meshName.empty()) {
        out << " [mesh: " << node.meshName << ']';
    }
    out << '\n';
    for (const std::size_t child : childrenOf[index]) {
        printSubtree(nodes, childrenOf, child, depth + 1U, out);
    }
}

/// --tree: the node hierarchy, roots in declaration order.
void printTree(const std::vector<aether::NodeDesc>& nodes, std::ostream& out) {
    std::vector<std::vector<std::size_t>> childrenOf(nodes.size());
    std::vector<std::size_t> roots;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].parent.empty()) {
            roots.push_back(i);
            continue;
        }
        for (std::size_t parent = 0; parent < nodes.size(); ++parent) {
            if (nodes[parent].id == nodes[i].parent) {
                childrenOf[parent].push_back(i);
                break;
            }
        }
    }
    for (const std::size_t root : roots) {
        printSubtree(nodes, childrenOf, root, 0U, out);
    }
}

/// --all: every resolved instance in declaration order (0-based), with full
/// TRS and whole-mesh material override ('-' when unbound).
void printInstances(const std::vector<aether::InstanceDesc>& instances, std::ostream& out) {
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const aether::InstanceDesc& instance = instances[i];
        out << std::format("{}: mesh={} t={} q={} s={} material={}\n",
                           i,
                           instance.meshName,
                           formatVec3(instance.translation),
                           formatQuat(instance.rotation),
                           formatVec3(instance.scale),
                           instance.materialName.empty() ? std::string("-") : instance.materialName);
    }
}

struct Options {
    std::filesystem::path sceneFile;
    bool tree = false;
    bool all = false;
};

/// Parse argv; on a usage problem prints the usage line and returns nullopt.
[[nodiscard]] std::optional<Options> parseArgs(const int argc, const char* const* argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--tree") {
            options.tree = true;
        } else if (arg == "--all") {
            options.all = true;
        } else if (arg.starts_with('-')) {
            usageError(std::format("unknown flag '{}'", arg));
            return std::nullopt;
        } else if (options.sceneFile.empty()) {
            options.sceneFile = argv[i];
        } else {
            usageError(std::format("unexpected extra argument '{}'", arg));
            return std::nullopt;
        }
    }
    if (options.sceneFile.empty()) {
        usageError("missing <scene.toml> path");
        return std::nullopt;
    }
    return options;
}

} // namespace

int main(const int argc, char* argv[]) {
#ifdef _WIN32
    // Windows consoles default to a legacy code page in which the summary
    // glyphs are unrepresentable; switch the output code page to UTF-8.
    // Redirected output (the agent loop's capture) is UTF-8 bytes regardless.
    // Deliberately no _setmode/_O_U8TEXT: those wide modes make narrow writes
    // assert, and the tool must never crash on a glyph.
    SetConsoleOutputCP(CP_UTF8);
#endif

    const std::optional<Options> options = parseArgs(argc, argv);
    if (!options.has_value()) {
        return 2;
    }

    // A path that cannot name a scene file is a usage problem, not a parse
    // failure (the parser rejects it with an empty error channel).
    std::error_code fsError;
    if (!std::filesystem::exists(options->sceneFile, fsError)) {
        usageError(std::format("scene file not found: '{}'", options->sceneFile.string()));
        return 2;
    }
    if (!std::filesystem::is_regular_file(options->sceneFile, fsError)) {
        usageError(std::format("not a regular file: '{}'", options->sceneFile.string()));
        return 2;
    }

    std::string error;
    const std::optional<aether::SceneDesc> scene = aether::SceneParser::parse(options->sceneFile, &error);
    if (!scene.has_value()) {
        // The error channel carries every structural rejection verbatim; it
        // stays empty only for malformed TOML or a file that turned
        // unreadable since the check above -- name the file instead of
        // printing an empty fail line.
        if (error.empty()) {
            error = std::format("malformed TOML or unreadable file: '{}'", options->sceneFile.string());
        }
        std::cerr << kFailGlyph << ' ' << error << '\n' << "exit: 1\n";
        return 1;
    }

    const aether::SceneDesc& desc = *scene;
    std::cout << std::format("{} scene OK \xE2\x80\x94 {} instances, {} meshes, {} nodes, {} material libraries\n",
                             kOkGlyph,
                             desc.instances.size(),
                             desc.meshes.size(),
                             desc.nodes.size(),
                             desc.mtllibs.size());
    if (desc.meta.has_value()) {
        std::cout << std::format("  meta: present | animations: {}\n", desc.animations.size());
    }
    if (options->tree) {
        printTree(desc.nodes, std::cout);
    }
    if (options->all) {
        printInstances(desc.instances, std::cout);
    }
    return 0;
}
