#include "game/render/ribbon/StripMeshBuilder.hpp"

#include "cromwell/ribbon/RibbonConstants.hpp"

#include <cmath>
#include <cstring>

namespace game {

using namespace cromwell;  /* the engine's names, unqualified. The game sits on top of
                          * cromwell and never the other way round, so there is nothing
                          * here for the engine to collide with. */

Mesh StripMeshBuilder::build(const std::vector<BorderPoint>& points, float halfWidth, float lift,
                             bool closed)
{
    Mesh mesh = { 0 };
    const int n = static_cast<int>(points.size());
    if (n < 2) return mesh;

    const auto at = [&](int i) -> const BorderPoint& {
        return points[static_cast<std::size_t>(i % n)];
    };

    /* SEGMENTS, not points: a closed ring has one per point, an open run has
     * one fewer. Everything that walks pairs stops here. */
    const int segments = closed ? n : n - 1;

    /* ---- per-vertex 2D travel direction -------------------------------- */
    directions_.assign(static_cast<std::size_t>(n) * 2, 0.0f);
    float lastX = 1.0f, lastY = 0.0f;
    for (int i = 0; i < segments; i++) {
        const BorderPoint& p = at(i);
        const BorderPoint& q = at(i + 1);
        const float ddx = q.x - p.x, ddy = q.y - p.y;
        const float length = std::sqrt(ddx * ddx + ddy * ddy);
        /* pure-vertical micro steps reuse the run's 2D direction */
        if (length > 1e-6f) { lastX = ddx / length; lastY = ddy / length; }
        directions_[static_cast<std::size_t>(i) * 2]     = lastX;
        directions_[static_cast<std::size_t>(i) * 2 + 1] = lastY;
    }
    /* The last point of an open run starts no segment, so it inherits the one
     * that arrives at it — its half-width offset is then square to the line
     * rather than to the leftover 1,0 default. */
    if (!closed) {
        directions_[static_cast<std::size_t>(n - 1) * 2]     = lastX;
        directions_[static_cast<std::size_t>(n - 1) * 2 + 1] = lastY;
    }

    /* ---- VERTICAL RISERS ------------------------------------------------
     * Where the line drops down a kerb its 2D direction is degenerate, so the
     * strip has no opinion about which way the quad faces — and the riser
     * lands exactly ON the kerb's face, coplanar with it. The depth fade then
     * reads a difference of ~0 and dissolves it.
     *
     * The lift belongs along the SURFACE NORMAL, not always +Y: flat ground
     * lifts up, a riser lifts OUTWARD. So both ends of a vertical seam get
     * pushed horizontally toward the LOWER side, which stands the quad proud
     * of the kerb face and turns it outward into the road. */
    pushes_.assign(static_cast<std::size_t>(n) * 2, 0.0f);
    for (int i = 0; i < segments; i++) {
        const BorderPoint& p = at(i);
        const BorderPoint& q = at(i + 1);
        const float planar = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
        const float rise   = q.height - p.height;
        if (planar > 1e-4f || std::fabs(rise) < 1e-5f) continue;   /* not a vertical seam */

        const float sign = (rise < 0.0f) ? 1.0f : -1.0f;           /* toward the lower side */
        const int   next = (i + 1) % n;
        const float dirX = directions_[static_cast<std::size_t>(i) * 2];
        const float dirY = directions_[static_cast<std::size_t>(i) * 2 + 1];

        pushes_[static_cast<std::size_t>(i) * 2]        += dirX * sign;
        pushes_[static_cast<std::size_t>(i) * 2 + 1]    += dirY * sign;
        pushes_[static_cast<std::size_t>(next) * 2]     += dirX * sign;
        pushes_[static_cast<std::size_t>(next) * 2 + 1] += dirY * sign;
    }

    /* ---- V runs along the strip in tiles --------------------------------
     * XCOM's UVTilingDistance is 96uu — one repeat per tile. Snapping the
     * loop's total length to a whole number of repeats is what makes the
     * scrolling dashes meet themselves at the seam instead of stuttering. */
    runLengths_.assign(static_cast<std::size_t>(n) + 1, 0.0f);
    for (int i = 0; i < segments; i++) {
        const BorderPoint& p = at(i);
        const BorderPoint& q = at(i + 1);
        const float ddx = q.x - p.x, ddy = q.y - p.y, ddh = q.height - p.height;
        runLengths_[static_cast<std::size_t>(i) + 1] =
            runLengths_[static_cast<std::size_t>(i)] +
            std::sqrt(ddx * ddx + ddy * ddy + ddh * ddh);
    }
    const float total = runLengths_[static_cast<std::size_t>(segments)];

    /* SNAPPING IS FOR RINGS ONLY. Rounding the length to whole repeats is what
     * makes the dashes meet at a closed loop's seam; an open run has no seam,
     * and snapping it would stretch or squeeze its dashes out of step with the
     * ring it runs alongside. */
    const float tiles  = closed ? std::fmax(1.0f, std::floor(total / kRibbonUvTile + 0.5f))
                                : total / kRibbonUvTile;
    const float vScale = (total > 1e-6f) ? tiles / total : 0.0f;

    /* Closed: n+1 vertex pairs, the seam pair duplicating point 0 with
     * V = tiles so the closing quad interpolates forwards like every other
     * one. Open: n pairs and one fewer quad — no seam to close. */
    mesh.vertexCount   = (closed ? n + 1 : n) * 2;
    mesh.triangleCount = segments * 2;
    mesh.vertices  = static_cast<float*>(
        MemAlloc(static_cast<unsigned int>(sizeof(float) * 3 * mesh.vertexCount)));
    mesh.texcoords = static_cast<float*>(
        MemAlloc(static_cast<unsigned int>(sizeof(float) * 2 * mesh.vertexCount)));
    mesh.indices = static_cast<unsigned short*>(
        MemAlloc(static_cast<unsigned int>(sizeof(unsigned short) * 3 * mesh.triangleCount)));

    for (int i = 0; i < n; i++) {
        /* An open run's first point has no arriving segment; mitring it
         * against the last one would twist the cap round to face wherever the
         * run happened to end. It squares off against its own direction. */
        const std::size_t previous = static_cast<std::size_t>(
            closed ? (i - 1 + n) % n : (i > 0 ? i - 1 : 0));
        const std::size_t current  = static_cast<std::size_t>(i);

        const float previousDirX = directions_[previous * 2];
        const float previousDirY = directions_[previous * 2 + 1];
        const float nextDirX     = directions_[current * 2];
        const float nextDirY     = directions_[current * 2 + 1];

        /* the two segment normals, then their bisector */
        const float normal1X = -previousDirY, normal1Y = previousDirX;
        const float normal2X = -nextDirY,     normal2Y = nextDirX;

        float miterX = normal1X + normal2X;
        float miterY = normal1Y + normal2Y;
        const float miterLength = std::sqrt(miterX * miterX + miterY * miterY);

        float offsetX, offsetY;
        if (miterLength < 1e-3f) {                       /* ~180 degree turn */
            offsetX = normal1X * halfWidth;
            offsetY = normal1Y * halfWidth;
        } else {
            miterX /= miterLength;
            miterY /= miterLength;
            const float cosHalf = miterX * normal1X + miterY * normal1Y;
            /* clamp sharp miters so hairpins don't spike */
            const float scale = halfWidth / std::fmax(cosHalf, 0.35f);
            offsetX = miterX * scale;
            offsetY = miterY * scale;
        }

        const BorderPoint& point = at(i);
        const float pushX = pushes_[current * 2]     * lift;
        const float pushY = pushes_[current * 2 + 1] * lift;

        const int a = i * 6, b = a + 3;
        const float v = runLengths_[current] * vScale;

        mesh.vertices[a + 0] = point.x + offsetX + pushX;
        mesh.vertices[a + 1] = point.height + lift;
        mesh.vertices[a + 2] = point.y + offsetY + pushY;
        mesh.vertices[b + 0] = point.x - offsetX + pushX;
        mesh.vertices[b + 1] = point.height + lift;
        mesh.vertices[b + 2] = point.y - offsetY + pushY;

        mesh.texcoords[i * 4 + 0] = 0.0f; mesh.texcoords[i * 4 + 1] = v;
        mesh.texcoords[i * 4 + 2] = 1.0f; mesh.texcoords[i * 4 + 3] = v;
    }

    /* seam: same place as pair 0, one full lap further along V */
    if (closed) {
        std::memcpy(&mesh.vertices[n * 6], &mesh.vertices[0], sizeof(float) * 6);
        mesh.texcoords[n * 4 + 0] = 0.0f; mesh.texcoords[n * 4 + 1] = tiles;
        mesh.texcoords[n * 4 + 2] = 1.0f; mesh.texcoords[n * 4 + 3] = tiles;
    }

    for (int i = 0; i < segments; i++) {
        const auto a = static_cast<unsigned short>(2 * i);
        const auto b = static_cast<unsigned short>(2 * i + 1);
        const auto c = static_cast<unsigned short>(2 * (i + 1));
        const auto d = static_cast<unsigned short>(c + 1);
        const int o = i * 6;
        mesh.indices[o + 0] = a; mesh.indices[o + 1] = b; mesh.indices[o + 2] = c;
        mesh.indices[o + 3] = b; mesh.indices[o + 4] = d; mesh.indices[o + 5] = c;
    }

    UploadMesh(&mesh, false);
    return mesh;
}

}  // namespace game
