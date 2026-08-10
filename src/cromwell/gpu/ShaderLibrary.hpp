/* ShaderLibrary.hpp — find and load shaders from disk.
 *
 * SINGLE RESPONSIBILITY: locate the asset root, resolve #include directives,
 * and turn shader filenames into a loaded Shader — complaining by name when
 * one is missing rather than silently substituting raylib's fallback.
 *
 * GLSL HAS NO #include, AND THAT IS THE PROBLEM THIS SOLVES. Without one there
 * are exactly two ways to give a second shader the same BRDF and the same
 * shadow lookup: branch inside one ever-growing uber-shader, or copy the
 * lighting core and let the copies drift. Source 2 takes neither — csgo_glass,
 * csgo_water_fancy and complex are separate shader families that #include a
 * shared common/, and static combos pick features per family.
 *
 * So the loader does the splice itself, before handing the source to GL. It is
 * a textual include with include-once semantics, which is all a shader tree
 * needs: no macros, no conditionals, no search paths.
 */
#pragma once

#include "raylib.h"

#include <string>

namespace cromwell {

class ShaderLibrary {
public:
    /* THE SEARCH PATH, in order. There are two asset trees now, not one:
     * cromwell's own shaders ship with cromwell, and the game's ship with the
     * game. An engine you can drop into another project cannot have its BRDF
     * living in that project's assets directory.
     *
     * Each entry is a directory that CONTAINS a shaders/ subdirectory, and
     * each is probed at three depths so the app runs from the project root or
     * from a build directory one or two levels down — the same reasoning that
     * chose builds/win's depth. Game roots come first so a game can override
     * an engine shader by shadowing its filename.
     *
     * Returns the directory the file was found in, or nullptr. */
    static const char* rootContaining(const char* relativePath);

    /* The first game asset root that exists. Only used to decide where to
     * write diagnostic dumps. */
    static const char* assetRoot();

    /* Either stage may be null. Returns a Shader with id 0 if a named file
     * could not be read. */
    static Shader load(const char* vertexName, const char* fragmentName);

    /* Reads a shader source file and splices in every
     *
     *     #include "name.glsl"
     *
     * it contains, recursively, resolving each name against assets/shaders.
     * A file already pulled in is skipped rather than repeated, so a diamond
     * (two includes that both want common/colour.glsl) does not redeclare
     * anything. Returns an empty string if the top-level file is missing.
     *
     * Public because the driver reports compile errors against line numbers in
     * the SPLICED source, which exists in no file — so when a shader fails to
     * build, load() dumps this to disk and says where. Without that, "error at
     * line 214" names a line nobody can look at. */
    static std::string preprocess(const char* fileName);
};

}  // namespace cromwell
