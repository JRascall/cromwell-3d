#include "render/model/PropSet.hpp"

#include "raymath.h"

#include "core/lattice/Constants.hpp"
#include "core/lattice/Lattice.hpp"
#include "render/gpu/ShaderLibrary.hpp"
#include "render/lighting/PbrShader.hpp"

#include <cstdio>
#include <cstring>

namespace xcom {
namespace {

/* Strips a trailing newline and anything after a '#'. */
void trimComment(char* line)
{
    for (char* c = line; *c; c++) {
        if (*c == '#' || *c == '\r' || *c == '\n') { *c = '\0'; return; }
    }
}

bool blank(const char* line)
{
    for (const char* c = line; *c; c++)
        if (*c != ' ' && *c != '\t') return false;
    return true;
}

}  // namespace

const ModelAsset* PropSet::findOrLoadModel(const std::string& file, MaterialLibrary& library)
{
    for (std::size_t i = 0; i < modelFiles_.size(); i++)
        if (modelFiles_[i] == file) {
            const ModelAsset* asset = models_[i].get();
            return asset->valid() ? asset : nullptr;
        }

    const char* root = ShaderLibrary::assetRoot();
    const char* path = TextFormat("%s/models/%s", root, file.c_str());

    /* The stem is the model's name, and the name is what its adopted
     * materials are keyed on. */
    std::string stem = file;
    const std::size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.resize(dot);

    auto asset = std::make_unique<ModelAsset>();
    const bool ok = asset->load(path, stem.c_str(), library,
                                library.handleOf(SurfaceKind::Cover));

    const ModelAsset* raw = asset.get();
    models_.push_back(std::move(asset));
    modelFiles_.push_back(file);
    return ok ? raw : nullptr;
}

/*  <file>  <x> <y> <storey>  [yawDegrees]  [scale]  [heightOffset]
 *
 * POSITION IS IN TILES, not world units, because everything else in this game
 * is: a prop authored against tile (11, 7) stays put if the tile size ever
 * changes, and an artist placing scenery is thinking in tiles anyway.
 *
 * IT IS THE TILE ORIGIN, NOT ITS CENTRE, and the coordinates are fractional.
 * A cell's own coordinate in this lattice is its corner — everything that
 * wants a centre says `+ 0.5f` explicitly — and cover pieces are authored
 * against that corner so abutting segments interlock (see the cinderblock
 * wall's README). Fractional coordinates mean a prop that does want to be
 * centred just says `10.5 14.5`, with no second concept to learn.
 *
 * NOTHING IS AUTO-LIFTED. The model's own origin lands on the storey floor,
 * full stop. Sniffing the bounding box to sit a prop "on the ground" sounds
 * helpful right up to the first asset that is meant to be sunk, overhanging or
 * wall-mounted, and then it is an invisible force nobody can turn off.
 * heightOffset is there for anything that needs to move. */
bool PropSet::parseLine(const char* line, MaterialLibrary& library)
{
    char file[160] = { 0 };
    float x = 0.0f, y = 0.0f;
    int storey = 0;
    float yaw = 0.0f, scale = 1.0f, heightOffset = 0.0f;

    const int fields = std::sscanf(line, "%159s %f %f %d %f %f %f",
                                   file, &x, &y, &storey, &yaw, &scale, &heightOffset);
    if (fields < 4) {
        TraceLog(LOG_WARNING, "PROPS: cannot parse \"%s\"", line);
        return false;
    }
    if (fields < 5) yaw = 0.0f;
    if (fields < 6) scale = 1.0f;
    if (fields < 7) heightOffset = 0.0f;

    const ModelAsset* model = findOrLoadModel(file, library);
    if (!model) return false;

    const Matrix transform =
        MatrixMultiply(MatrixMultiply(MatrixScale(scale, scale, scale),
                                      MatrixRotateY(yaw * DEG2RAD)),
                       MatrixTranslate(x, Lattice::storeyBaseHeight(storey) + heightOffset, y));

    instances_.push_back(Instance{ model, transform });
    return true;
}

void PropSet::loadManifest(MaterialLibrary& library)
{
    const char* root = ShaderLibrary::assetRoot();
    const char* path = TextFormat("%s/models/props.txt", root);

    if (!FileExists(path)) {
        TraceLog(LOG_INFO, "PROPS: no %s - the scene is placeholder geometry only", path);
        return;
    }

    char* text = LoadFileText(path);
    if (!text) return;

    int placed = 0;
    char line[256];
    std::size_t cursor = 0;
    const std::size_t length = std::strlen(text);

    while (cursor < length) {
        std::size_t end = cursor;
        while (end < length && text[end] != '\n') end++;

        const std::size_t count = (end - cursor) < sizeof(line) - 1 ? (end - cursor)
                                                                    : sizeof(line) - 1;
        std::memcpy(line, text + cursor, count);
        line[count] = '\0';
        cursor = end + 1;

        trimComment(line);
        if (blank(line)) continue;
        if (parseLine(line, library)) placed++;
    }

    UnloadFileText(text);
    TraceLog(LOG_INFO, "PROPS: %d instance(s) from %d model(s)",
             placed, static_cast<int>(models_.size()));
}

void PropSet::draw(const Material& material) const
{
    for (const Instance& instance : instances_)
        instance.model->draw(instance.transform, material);
}

void PropSet::drawLit(const MaterialLibrary& library, const PbrShader& shader) const
{
    for (const Instance& instance : instances_)
        instance.model->drawLit(instance.transform, library, shader);
}

}  // namespace xcom
