/* PropSet.hpp — loaded models, and where they stand in the world.
 *
 * SINGLE RESPONSIBILITY: own the model assets, own the list of instances
 * placed from them, and draw those instances in whichever pass is running.
 *
 * PLACEMENT LIVES IN A MANIFEST, not in code. assets/models/props.txt is read
 * at startup, so adding a prop is a model file and one line — no rebuild. That
 * matters more than it sounds: the whole point of this module is that art can
 * be iterated on without a programmer, and a hardcoded placement list would
 * put a compile between the artist and the result.
 *
 * PROPS ARE RENDER-ONLY. They cast shadows, occlude, and light like everything
 * else, but the tile lattice does not know they exist — cover, line of sight
 * and pathing all still come from the tile data. That separation is
 * deliberate: a prop is scenery until someone decides what it MEANS, and
 * inventing gameplay for it here would put art in charge of the rules. A
 * crate model standing on a tile authored as half cover is the intended way to
 * dress the board.
 */
#pragma once

#include "raylib.h"

#include "render/material/MaterialLibrary.hpp"
#include "render/model/ModelAsset.hpp"

#include <memory>
#include <string>
#include <vector>

namespace xcom {

class PbrShader;

class PropSet {
public:
    PropSet() = default;

    PropSet(const PropSet&) = delete;
    PropSet& operator=(const PropSet&) = delete;

    /* Reads assets/models/props.txt and loads everything it names. Missing
     * manifest, missing model and malformed line are all survivable: the prop
     * is skipped with a complaint and the rest of the scene still draws. */
    void loadManifest(MaterialLibrary& library);

    bool empty() const { return instances_.empty(); }
    int  instanceCount() const { return static_cast<int>(instances_.size()); }
    int  modelCount() const { return static_cast<int>(models_.size()); }

    /* Shadow map and scene prepass: one shader over everything. */
    void draw(const Material& material) const;

    /* Lit pass: each mesh through the material its file gave it. */
    void drawLit(const MaterialLibrary& library, const PbrShader& shader) const;

private:
    struct Instance {
        const ModelAsset* model = nullptr;
        Matrix transform{};
    };

    /* Returns nullptr if the file could not be loaded. Models are shared, so
     * ten crates cost one upload. */
    const ModelAsset* findOrLoadModel(const std::string& file, MaterialLibrary& library);

    /* One manifest line. Returns false on anything it cannot parse. */
    bool parseLine(const char* line, MaterialLibrary& library);

    /* Held by pointer so the addresses handed to instances survive the vector
     * growing as later lines load more models. */
    std::vector<std::unique_ptr<ModelAsset>> models_;
    std::vector<std::string>                 modelFiles_;
    std::vector<Instance>                    instances_;
};

}  // namespace xcom
