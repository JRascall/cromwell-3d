#include "game/render/dev/DecalDemo.hpp"

#include "raylib.h"

#include "game/lattice/Constants.hpp"
#include "game/lattice/Direction.hpp"
#include "game/world/Tile.hpp"
#include "game/world/World.hpp"
#include "cromwell/decal/DecalSet.hpp"

#include <cmath>
#include <vector>

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */
namespace {

constexpr int kTextureSize = 256;

/* A fixed-seed value noise, so two runs of --shot produce the same marks. The
 * same reason AmbientOcclusion generates its kernel from a fixed seed rather
 * than rand(). */
struct Random {
    unsigned int state;

    float unit()
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;   /* [0, 1) */
    }
    float range(float low, float high) { return low + (high - low) * unit(); }
};

/* Smooth 2D value noise in [0, 1], a few octaves. Enough to break up a circle
 * into something that reads as a stain rather than as a sticker, which is all
 * a placeholder mark has to do. */
float noise(float x, float y, unsigned int seed)
{
    const auto hash = [seed](int i, int j) {
        unsigned int h = static_cast<unsigned int>(i) * 374761393u
                       + static_cast<unsigned int>(j) * 668265263u + seed;
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<float>((h ^ (h >> 16)) & 0xFFFFFFu) / 16777215.0f;
    };

    float total = 0.0f, amplitude = 1.0f, sum = 0.0f, frequency = 1.0f;
    for (int octave = 0; octave < 4; octave++) {
        const float fx = x * frequency, fy = y * frequency;
        const int ix = static_cast<int>(std::floor(fx));
        const int iy = static_cast<int>(std::floor(fy));
        float tx = fx - static_cast<float>(ix);
        float ty = fy - static_cast<float>(iy);
        tx = tx * tx * (3.0f - 2.0f * tx);
        ty = ty * ty * (3.0f - 2.0f * ty);

        const float a = hash(ix, iy),     b = hash(ix + 1, iy);
        const float c = hash(ix, iy + 1), d = hash(ix + 1, iy + 1);
        const float top = a + (b - a) * tx;
        const float bottom = c + (d - c) * tx;

        total += (top + (bottom - top) * ty) * amplitude;
        sum += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return total / sum;
}

/* Radial coverage with a ragged edge: 1 at the centre, 0 outside, with the
 * boundary pushed in and out by the noise so the mark has no circle in it. */
float splatCoverage(float u, float v, unsigned int seed, float raggedness)
{
    const float dx = u - 0.5f, dy = v - 0.5f;
    const float radius = std::sqrt(dx * dx + dy * dy) * 2.0f;   /* 0 at centre, 1 at edge */

    const float edge = 0.62f + raggedness * (noise(u * 6.0f, v * 6.0f, seed) - 0.5f);
    const float coverage = 1.0f - (radius - (edge - 0.30f)) / 0.30f;
    return (coverage < 0.0f) ? 0.0f : ((coverage > 1.0f) ? 1.0f : coverage);
}

unsigned char toByte(float value)
{
    const float clamped = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
    return static_cast<unsigned char>(clamped * 255.0f + 0.5f);
}

/* Builds an RGBA texture from a per-texel function of the normalised
 * coordinate. Mipmapped and clamped, exactly as a loaded decal map would be —
 * a placeholder that filters differently from the real thing is a placeholder
 * that lies about how the real thing will look. */
template <typename Fn>
Texture2D build(Fn&& texel)
{
    std::vector<unsigned char> pixels(static_cast<std::size_t>(kTextureSize) * kTextureSize * 4);

    for (int y = 0; y < kTextureSize; y++) {
        for (int x = 0; x < kTextureSize; x++) {
            const float u = (static_cast<float>(x) + 0.5f) / kTextureSize;
            const float v = (static_cast<float>(y) + 0.5f) / kTextureSize;

            const Color colour = texel(u, v);
            const std::size_t at = (static_cast<std::size_t>(y) * kTextureSize + x) * 4;
            pixels[at + 0] = colour.r;
            pixels[at + 1] = colour.g;
            pixels[at + 2] = colour.b;
            pixels[at + 3] = colour.a;
        }
    }

    Image image{};
    image.data    = pixels.data();
    image.width   = kTextureSize;
    image.height  = kTextureSize;
    image.mipmaps = 1;
    image.format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

    /* LoadTextureFromImage copies into GL, so the vector may die here. */
    Texture2D texture = LoadTextureFromImage(image);
    if (texture.id == 0) return texture;

    GenTextureMipmaps(&texture);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
    SetTextureFilter(texture, TEXTURE_FILTER_ANISOTROPIC_8X);
    SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
    return texture;
}

/* ---- the three materials ------------------------------------------------- */

DecalMaterialId makeScorch(DecalSet& decals)
{
    const Texture2D albedo = build([](float u, float v) {
        const float coverage = splatCoverage(u, v, 11u, 0.55f);
        /* Sootier toward the middle, and never fully black — a burn on concrete
         * still shows the concrete through it at the rim. */
        const float soot = 0.06f + 0.16f * (1.0f - coverage);
        return Color{ toByte(soot), toByte(soot * 0.94f), toByte(soot * 0.88f),
                      toByte(coverage) };
    });

    /* Metal 0, rough 1 — a burn is the roughest thing on the board. */
    const Texture2D packed = build([](float, float) {
        return Color{ 0, 250, 0, 255 };
    });

    return decals.registerTextures("demo_scorch", albedo, Texture2D{}, packed);
}

DecalMaterialId makeBlood(DecalSet& decals)
{
    const Texture2D albedo = build([](float u, float v) {
        const float coverage = splatCoverage(u, v, 29u, 0.75f);
        const float depth = noise(u * 4.0f, v * 4.0f, 71u);
        return Color{ toByte(0.20f + 0.16f * depth), toByte(0.012f), toByte(0.014f),
                      toByte(coverage) };
    });

    /* THE POINT OF THIS ONE. Roughness 0.12 against a road authored at 0.62,
     * so the pool takes a tight specular highlight the surface around it
     * cannot — which is what makes it read as WET rather than as dark paint.
     * Slightly rougher at the rim, where a real pool is drying. */
    const Texture2D packed = build([](float u, float v) {
        const float dx = u - 0.5f, dy = v - 0.5f;
        const float radius = std::sqrt(dx * dx + dy * dy) * 2.0f;
        return Color{ 0, toByte(0.12f + 0.5f * radius), 0, 255 };
    });

    return decals.registerTextures("demo_blood", albedo, Texture2D{}, packed);
}

DecalMaterialId makeSigil(DecalSet& decals)
{
    /* A ring, so there is a clear inside and outside to check the projection
     * against, and an obvious shape to see wrapped over a step. */
    const auto ring = [](float u, float v) {
        const float dx = u - 0.5f, dy = v - 0.5f;
        const float radius = std::sqrt(dx * dx + dy * dy) * 2.0f;
        const float band = 1.0f - std::fabs(radius - 0.66f) / 0.16f;
        return (band < 0.0f) ? 0.0f : ((band > 1.0f) ? 1.0f : band);
    };

    const Texture2D albedo = build([&ring](float u, float v) {
        const float mask = ring(u, v);
        return Color{ toByte(0.15f), toByte(0.85f), toByte(0.75f), toByte(mask) };
    });

    /* Emissive mask in blue, at full strength across the whole ring. */
    const Texture2D packed = build([&ring](float u, float v) {
        return Color{ 0, toByte(0.35f), toByte(ring(u, v)), 255 };
    });

    /* A BEVEL, so the ring reads as cut into the surface rather than printed on
     * it. The gradient of the ring mask along each axis IS the slope, which is
     * all a normal map is — and it doubles as the check that the decal's normal
     * is being applied in the RECEIVER's frame: on a sloped surface the bevel
     * should follow the slope, not flatten it. */
    const Texture2D normal = build([&ring](float u, float v) {
        const float step = 1.0f / kTextureSize;
        const float dx = ring(u + step, v) - ring(u - step, v);
        const float dy = ring(u, v + step) - ring(u, v - step);
        return Color{ toByte(0.5f - dx * 4.0f), toByte(0.5f + dy * 4.0f),
                      toByte(1.0f), 255 };
    });

    return decals.registerTextures("demo_sigil", albedo, normal, packed);
}

/* One outward-facing wall plane the demo can stick something to. */
struct WallFace {
    Vector3 point;    /* on the wall's surface, mid-height */
    Vector3 normal;   /* out of it                         */
};

/* Every full-height wall on the ground storey, both faces of each.
 *
 * BOTH FACES, DELIBERATELY. Which side of a wall the camera can see is a
 * question about the camera, and this function has no camera — so rather than
 * guess and put half the marks on the far side of the building, each wall gets
 * one on each face. It doubles as the check that the angle fade is doing its
 * job: a decal on the +Z face must be completely absent from the -Z face, and
 * from the floor and roof the box also passes through.
 *
 * Faces are stored canonically — North and East on every tile, South and West
 * only at the grid border — so scanning those two directions visits each
 * physical wall exactly once. */
std::vector<WallFace> findWalls(const World& world)
{
    std::vector<WallFace> faces;

    const Lattice& lattice = world.lattice();
    const int z = 0;                                  /* the ground storey */
    const float midHeight = kStoreyHeight * 0.5f;

    /* Half the wall's own thickness, so the point lands ON the visible surface
     * rather than inside the box the emitter built. See StoreyGeometryEmitter:
     * a full wall is 0.09 thick, centred on the tile boundary. */
    const float halfThickness = 0.045f;

    for (int y = 0; y < lattice.height(); y++) {
        for (int x = 0; x < lattice.width(); x++) {
            const Tile& tile = world.at(Cell{ x, y, z });

            for (Dir d : { Dir::North, Dir::East }) {
                const Edge& edge = tile.edge(d);

                /* Full cover only, and never a window: a decal box through
                 * glass would find the pane, and pbr.fs.glsl deliberately
                 * refuses decals on blended surfaces — so it would simply
                 * vanish, which is the most confusing possible demo. */
                if (edge.cover != Cover::Full || edge.window) continue;

                const Vector3 outward = (d == Dir::North) ? Vector3{ 0.0f, 0.0f, 1.0f }
                                                          : Vector3{ 1.0f, 0.0f, 0.0f };

                /* The lattice's y is the world's z. A North face sits at the
                 * far edge of its tile, an East face at the right-hand edge. */
                const Vector3 centre{
                    (d == Dir::East)  ? static_cast<float>(x) + 1.0f
                                      : static_cast<float>(x) + 0.5f,
                    midHeight,
                    (d == Dir::North) ? static_cast<float>(y) + 1.0f
                                      : static_cast<float>(y) + 0.5f
                };

                for (float side : { 1.0f, -1.0f }) {
                    WallFace face;
                    face.normal = Vector3{ outward.x * side, 0.0f, outward.z * side };
                    face.point  = Vector3{ centre.x + face.normal.x * halfThickness,
                                           centre.y,
                                           centre.z + face.normal.z * halfThickness };
                    faces.push_back(face);
                }
            }
        }
    }
    return faces;
}

}  // namespace

void registerDemoMaterials(DecalSet& decals)
{
    if (decals.materialCount() > 0) return;   /* idempotent: called from two places */

    /* THE HERO, and the only one loaded from a file:
     * assets/materials/decals/example_albedo.png. A piece of authored art with
     * real alpha beats a procedural splat for judging this system, because the
     * two things that are hard to get right are both legibility questions —
     * whether the projection holds its shape when it wraps over a step, and
     * whether the edges stay crisp rather than fringing. Fine lettering answers
     * both at a glance; a soft-edged stain hides both.
     *
     * FIRST, so it is what the dev tool's material combo defaults to. */
    decals.findOrLoad("example");

    makeScorch(decals);
    makeBlood(decals);
    makeSigil(decals);
}

void populateDemoDecals(DecalSet& decals, const World& world)
{
    registerDemoMaterials(decals);

    const int width  = world.lattice().width();
    const int height = world.lattice().height();

    const DecalMaterialId example = 0;
    const DecalMaterialId scorch  = 1;
    const DecalMaterialId blood   = 2;
    const DecalMaterialId sigil   = 3;

    if (decals.materialCount() < 4) return;

    Random random{ 0x5EED1234u };

    /* ---- THE WALLS, which is where to look first -------------------------
     * A wall is the honest test of a projected decal, because the artwork is
     * face-on to the camera and any stretch, fringe or misalignment is right
     * there to be read. It is also where you would actually want one: a poster,
     * a stencil, a bullet scar.
     *
     * The box is SHALLOW here, 0.6 against a wall 0.09 thick. It only has to
     * contain the wall's own surface and the couple of centimetres of relief
     * around it — a deep box on a vertical surface reaches into the room
     * behind and out into the street in front, and the further it reaches the
     * more chance it finds some unrelated visible surface at a glancing angle
     * and inks that too. Depth is the wrap budget: spend it where the geometry
     * actually varies. */
    if (example != kInvalidDecalMaterial) {
        const std::vector<WallFace> walls = findWalls(world);

        /* Every wall on the ground storey would be a wall of text; a stride
         * spreads a handful around the building instead of crowding one
         * corner. Both faces of a wall are adjacent in the list, so the stride
         * is even — take a face and its opposite number together. */
        const std::size_t wanted = 12;
        const std::size_t stride = (walls.size() > wanted) ? (walls.size() / wanted) | 1u : 1u;

        int placed = 0;
        for (std::size_t i = 0; i < walls.size(); i += stride) {
            const WallFace& face = walls[i];

            /* 1.5 square on a wall 1.04 wide and 2.0 tall — slightly wider than
             * the wall, so the label runs off its ends. That is on purpose: a
             * decal that stops exactly at the geometry proves nothing, and the
             * overrun is what shows the projection dying cleanly at the corner
             * rather than smearing round it. */
            Decal decal = Decal::onSurface(face.point, face.normal, 0.0f,
                                           Vector2{ 1.5f, 1.5f }, 0.6f);
            decal.material  = example;
            decal.sortOrder = 2;
            decal.roughness = 0.75f;   /* a printed sticker, matte but not dead */
            decals.add(decal);
            placed++;
        }
        TraceLog(LOG_INFO, "DECAL: %d wall marks on %d candidate faces",
                 placed, static_cast<int>(walls.size()));
    }

    /* ---- THE GROUND ------------------------------------------------------
     * PROJECTED STRAIGHT DOWN, THROUGH A TALL BOX. Three units of depth means
     * a mark laid on the floor also reaches anything standing on that floor, so
     * it wraps over kerbs, crates and stair treads instead of stopping at them.
     * That is the case worth looking at, and it needs no particular camera.
     *
     * THE BOX IS CENTRED ON THE FLOOR, NOT SITTING ON IT. Placing it at
     * boxDepth * 0.5 put the ground plane exactly on the box's far face — where
     * the depth fade is by definition zero — and every single ground mark
     * discarded, silently, with the pass otherwise running perfectly. The
     * surface a decal is FOR belongs at the centre of its box, which is what
     * onSurface() builds and what the fade is measured from. */
    const Vector3 down{ 0.0f, 1.0f, 0.0f };
    const float boxDepth = 3.0f;
    const float floorHeight = 0.0f;   /* storey 0's base — see Lattice */

    /* The middle half of the board, which on the demo map is where the built
     * geometry is rather than open ground. */
    const float lowX = width * 0.25f, highX = width * 0.75f;
    const float lowZ = height * 0.25f, highZ = height * 0.75f;

    struct Spread {
        DecalMaterialId material;
        int             count;
        float           minSize;
        float           maxSize;
        int             sortOrder;
        float           emissive;
        float           roughness;
        float           minOpacity;
    };
    const Spread spreads[] = {
        /* Scorch underneath, blood over it, the label over that, sigils last —
         * which is also the check that sortOrder does what it claims.
         *
         * The three procedural marks pass roughness 1.0 because each supplies
         * its own packed map and the factor must not override it; the label has
         * no packed map, so its factor IS its roughness — a printed sticker,
         * matte but not as dead as a burn. */
        { scorch,  10, 1.6f, 3.4f, 0, 0.0f, 1.0f, 0.65f },
        { blood,    8, 0.8f, 1.8f, 1, 0.0f, 1.0f, 0.70f },
        /* BIG, because the point of this one is legibility. At three to four
         * and a half tiles the lettering is readable from the tactical camera,
         * so a decal that is stretching, fringing or losing its shape where it
         * crosses a step is obvious rather than arguable — and at full opacity,
         * because a half-faded label cannot answer that question either. */
        { example,  5, 3.0f, 4.5f, 2, 0.0f, 0.75f, 1.0f },
        { sigil,    3, 1.4f, 2.2f, 3, 1.0f, 1.0f, 0.85f },
    };

    for (const Spread& spread : spreads) {
        if (spread.material == kInvalidDecalMaterial) continue;

        for (int i = 0; i < spread.count; i++) {
            const Vector3 at{ random.range(lowX, highX), floorHeight,
                              random.range(lowZ, highZ) };

            const float size = random.range(spread.minSize, spread.maxSize);

            Decal decal = Decal::onSurface(at, down, random.range(0.0f, 6.2831853f),
                                           Vector2{ size, size }, boxDepth);
            decal.material  = spread.material;
            decal.sortOrder = spread.sortOrder;
            decal.emissive  = spread.emissive;
            decal.roughness = spread.roughness;
            decal.opacity   = random.range(spread.minOpacity, 1.0f);

            /* A downward projection has the whole floor squarely facing it, so
             * the fade only has to kill the walls — which it does well before
             * these thresholds. Left at the defaults deliberately: if the demo
             * needed custom angle fades to look right, the defaults would be
             * wrong. */
            decals.add(decal);
        }
    }

    TraceLog(LOG_INFO, "DECAL: %d demo marks placed across the board's middle half",
             static_cast<int>(decals.count()));
}

}  // namespace game
