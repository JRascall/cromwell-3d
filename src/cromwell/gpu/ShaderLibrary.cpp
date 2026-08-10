#include "cromwell/gpu/ShaderLibrary.hpp"

#include <set>
#include <string>

namespace cromwell {
namespace {

/* Parses `#include "some/name.glsl"` and returns the quoted name, or an empty
 * string when the line is not an include. Deliberately strict: a directive
 * that is nearly right is a mistake worth seeing as a GLSL compile error on
 * the untouched line, not a silently ignored include. */
std::string includeTarget(const std::string& line)
{
    std::size_t at = line.find_first_not_of(" \t");
    if (at == std::string::npos) return {};
    if (line.compare(at, 8, "#include") != 0) return {};

    const std::size_t open = line.find('"', at + 8);
    if (open == std::string::npos) return {};
    const std::size_t close = line.find('"', open + 1);
    if (close == std::string::npos) return {};

    return line.substr(open + 1, close - open - 1);
}

/* `seen` carries include-once across the whole tree, which doubles as the
 * cycle guard: a file that includes itself, directly or through a loop, is
 * already in the set the second time round. */
void splice(const std::string& name, std::set<std::string>& seen, std::string& out)
{
    if (!seen.insert(name).second) return;

    /* Resolved against the whole search path, so a game shader may include
     * cromwell/assets/shaders/common/brdf.glsl by bare name and vice versa —
     * the tree a file lives in is not something its includers should have to
     * know. */
    const std::string relative = "shaders/" + name;
    const char* root = ShaderLibrary::rootContaining(relative.c_str());
    if (root == nullptr) {
        TraceLog(LOG_ERROR, "SHADER: missing include %s", name.c_str());
        return;
    }

    char* text = LoadFileText(TextFormat("%s/%s", root, relative.c_str()));
    if (text == nullptr) {
        TraceLog(LOG_ERROR, "SHADER: unreadable include %s/%s", root, relative.c_str());
        return;
    }

    const std::string source = text;
    UnloadFileText(text);

    /* Line numbers in the spliced output no longer match any one file, so the
     * origin is stamped in as a comment. GLSL's own #line would be better, but
     * it renumbers the driver's error messages against a file the driver
     * cannot open, which trades one confusion for a worse one.
     *
     * Not for the root file: #version has to come before anything but comments
     * and whitespace, and while a comment is legal there by the letter of the
     * spec it is not worth betting a black screen on one driver reading it
     * differently. */
    if (!out.empty()) {
        out += "/* ---- ";
        out += name;
        out += " ---- */\n";
    }

    std::size_t start = 0;
    while (start <= source.size()) {
        std::size_t end = source.find('\n', start);
        if (end == std::string::npos) end = source.size();

        const std::string line = source.substr(start, end - start);
        const std::string target = includeTarget(line);

        if (target.empty()) {
            out += line;
            out += '\n';
        } else {
            splice(target, seen, out);
        }

        if (end == source.size()) break;
        start = end + 1;
    }
}

/* Writes the spliced source beside the shader it came from, so the driver's
 * line numbers point at something readable. Only ever called on failure, and
 * the name is distinctive enough to delete on sight. */
void dumpForDiagnosis(const char* name, const std::string& source)
{
    if (name == nullptr || source.empty()) return;

    const char* path = TextFormat("%s/shaders/%s.spliced.txt",
                                  ShaderLibrary::assetRoot(), name);
    if (SaveFileText(const_cast<char*>(path), const_cast<char*>(source.c_str())))
        TraceLog(LOG_WARNING, "SHADER: %s failed - spliced source written to %s",
                 name, path);
}

}  // namespace

namespace {

/* Game roots first, then cromwell's own. Both are listed at three depths
 * because the runnable product sits two directories down (builds/win) and the
 * app is also run straight from the project root.
 *
 * The engine's tree is named relative to the SOURCE layout rather than being
 * copied into the staging directory, which is exactly what the game's assets
 * already do — builds/ can be deleted whole without costing anything, and a
 * shader edit is live on the next run instead of on the next build. A shipping
 * build would copy both trees next to the exe; the first entry of each pair
 * finds them there when it does. */
const char* const kRoots[] = {
    "assets",    "../assets",    "../../assets",
    "cromwell/assets", "../cromwell/assets", "../../cromwell/assets",
    "src/cromwell/assets", "../src/cromwell/assets", "../../src/cromwell/assets",
};

}  // namespace

const char* ShaderLibrary::rootContaining(const char* relativePath)
{
    if (relativePath == nullptr) return nullptr;
    for (const char* root : kRoots)
        if (FileExists(TextFormat("%s/%s", root, relativePath))) return root;
    return nullptr;
}

const char* ShaderLibrary::assetRoot()
{
    static const char* candidates[] = { "assets", "../assets", "../../assets" };
    for (const char* candidate : candidates)
        if (DirectoryExists(candidate)) return candidate;
    return candidates[0];
}

std::string ShaderLibrary::preprocess(const char* fileName)
{
    if (fileName == nullptr) return {};

    std::set<std::string> seen;
    std::string out;
    splice(fileName, seen, out);
    return out;
}

Shader ShaderLibrary::load(const char* vertexName, const char* fragmentName)
{
    const std::string vertexSource   = preprocess(vertexName);
    const std::string fragmentSource = preprocess(fragmentName);

    if (vertexName && vertexSource.empty()) {
        TraceLog(LOG_ERROR, "SHADER: missing %s", vertexName);
        return Shader{ 0 };
    }
    if (fragmentName && fragmentSource.empty()) {
        TraceLog(LOG_ERROR, "SHADER: missing %s", fragmentName);
        return Shader{ 0 };
    }

    const Shader shader =
        LoadShaderFromMemory(vertexName ? vertexSource.c_str() : nullptr,
                             fragmentName ? fragmentSource.c_str() : nullptr);

    /* raylib falls back to its default shader rather than returning 0 when a
     * stage fails to compile, so id != 0 is not proof of success — but a
     * failure has already been logged by then, and what is missing from that
     * log is the source the line numbers refer to. Dump it. */
    if (shader.id != 0) return shader;

    dumpForDiagnosis(vertexName, vertexSource);
    dumpForDiagnosis(fragmentName, fragmentSource);
    return shader;
}

}  // namespace cromwell
