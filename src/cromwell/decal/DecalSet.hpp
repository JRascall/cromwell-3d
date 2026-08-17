/* DecalSet.hpp — every decal on the board, and the materials they wear.
 *
 * SINGLE RESPONSIBILITY: own the decal instances and the texture sets they
 * refer to. It knows nothing about the pass that draws them.
 *
 * MATERIALS ARE SHARED AND INSTANCES ARE CHEAP. A scorch mark is one entry in
 * the material table and one Decal per detonation, so a firefight's worth of
 * damage costs a matrix and a few floats each rather than a texture each. The
 * table is loaded once by name from
 *
 *     assets/materials/decals/<name>_albedo.png
 *                             <name>_normal.png
 *                             <name>_mrao.png
 *
 * with the same factor-times-texture rule and the same 1x1 fallbacks the
 * surface materials use (see PbrMaterial.hpp) — so a decal with nothing but an
 * albedo is fully described, and picks up relief and gloss the moment the other
 * two files appear beside it.
 *
 * THE ALBEDO'S ALPHA IS COVERAGE HERE, and unlike a surface material that is
 * not a trap: a decal texture with no alpha is a rectangle, and a rectangle is
 * almost never what a decal is. This is the one place where the fourth channel
 * genuinely means opacity, so it is read as opacity with no mode flag.
 *
 * ORDER IS EXPLICIT. Draw order is by Decal::sortOrder and then by insertion,
 * so blood laid over a scorch mark stays over it regardless of when either was
 * added, and the ordering does not shift when something in the middle is
 * removed.
 */
#pragma once

#include "raylib.h"

#include "cromwell/decal/Decal.hpp"

#include <string>
#include <vector>

namespace cromwell {

class DecalSet {
public:
    DecalSet() = default;
    ~DecalSet();

    DecalSet(const DecalSet&) = delete;
    DecalSet& operator=(const DecalSet&) = delete;

    /* One decal material's textures. Mirrors PbrMaterial's three-map layout so
     * a decal and a surface are authored the same way. */
    struct MaterialTextures {
        Texture2D albedo{};
        Texture2D normal{};
        Texture2D packed{};   /* metal R, rough G, emissive mask B */
    };

    /* Loads the three maps if they exist, or returns the existing id when the
     * name has already been registered. Missing maps fall back rather than
     * failing: a name with no files at all is still a usable white decal. */
    DecalMaterialId findOrLoad(const char* name);

    /* Registers a material whose textures were BUILT rather than loaded, and
     * takes ownership of every non-null one. For generated art — procedural
     * marks, an atlas assembled at runtime, anything with no file behind it.
     * A null texture takes the same 1x1 fallback a missing file would. */
    DecalMaterialId registerTextures(const char* name, Texture2D albedo,
                                     Texture2D normal, Texture2D packed);

    const MaterialTextures& textures(DecalMaterialId id) const;

    /* The material table, for a UI that has to offer a choice of them. The
     * name is the one it was registered under and is stable for the run. */
    std::size_t materialCount() const { return materials_.size(); }
    const char* materialName(DecalMaterialId id) const;

    /* ---- instances -------------------------------------------------------- */
    void add(const Decal& decal);
    void clear() { decals_.clear(); sorted_ = true; }

    std::size_t count() const { return decals_.size(); }

    /* In draw order — sortOrder ascending, insertion order within a tier. The
     * sort is deferred to the first read after a change rather than run per
     * insertion, so a detonation adding twenty marks sorts once. */
    const std::vector<Decal>& inDrawOrder() const;

private:
    void createFallbacks();
    Texture2D loadOrFallback(const char* path, Texture2D fallback);

    struct Slot {
        std::string      name;
        MaterialTextures textures;
    };

    std::vector<Slot> materials_;

    /* MUTABLE FOR THE DEFERRED SORT ALONE. inDrawOrder() is logically a read —
     * it changes which order the same decals come back in, never which decals
     * there are — and making every holder of a DecalSet take it non-const to
     * permit that would push the lie outward instead of containing it here. */
    mutable std::vector<Decal> decals_;

    /* The next Decal::id to hand out. Never rewound, not even by clear() —
     * see the note in add(). */
    int nextId_ = 0;

    /* Owned textures, for the destructor. white_ is NOT in here: it is rlgl's
     * shared default, and unloading it would take the default texture out from
     * under every other material in the program. */
    std::vector<Texture2D> owned_;

    Texture2D white_{};
    Texture2D flatNormal_{};
    bool      fallbacksReady_ = false;

    mutable bool sorted_ = true;
};

}  // namespace cromwell
