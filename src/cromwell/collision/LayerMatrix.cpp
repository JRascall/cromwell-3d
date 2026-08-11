#include "cromwell/collision/LayerMatrix.hpp"

#include <cstdio>

namespace cromwell {

LayerMatrix::LayerMatrix() = default;

void LayerMatrix::nameLayer(LayerId layer, std::string_view name)
{
    if (!layer.valid()) return;
    names_[static_cast<std::size_t>(layer.index())] = std::string(name);
}

std::string LayerMatrix::layerName(LayerId layer) const
{
    if (!layer.valid()) return "invalid layer";

    const std::string& name = names_[static_cast<std::size_t>(layer.index())];
    if (!name.empty()) return name;

    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "layer %d", layer.index());
    return buffer;
}

LayerId LayerMatrix::findLayer(std::string_view name) const
{
    for (int index = 0; index < LayerId::kCount; ++index) {
        if (names_[static_cast<std::size_t>(index)] == name) return LayerId(index);
    }
    return LayerId{};  /* invalid — see LayerId's default */
}

void LayerMatrix::setResponse(LayerId a, LayerId b, Response response)
{
    if (!a.valid() || !b.valid()) return;

    const auto write = [&](LayerId from, LayerId to) {
        const auto index = static_cast<std::size_t>(from.index());

        /* Cleared from both masks first, so setting a response twice replaces it
         * rather than accumulating. Without this, Block then Overlap would leave
         * the pair in both masks and Block would keep winning — a rule that
         * cannot be relaxed once written, which is a maddening thing to debug in
         * a configuration table. */
        blocks_[index] = blocks_[index].without(to);
        overlaps_[index] = overlaps_[index].without(to);

        if (response == Response::Block) blocks_[index] = blocks_[index].with(to);
        else if (response == Response::Overlap) overlaps_[index] = overlaps_[index].with(to);
    };

    write(a, b);
    write(b, a);  /* symmetric — see the header */
}

void LayerMatrix::setResponseToAll(LayerId layer, Response response)
{
    for (int index = 0; index < LayerId::kCount; ++index) {
        setResponse(layer, LayerId(index), response);
    }
}

Response LayerMatrix::response(LayerId a, LayerId b) const
{
    if (!a.valid() || !b.valid()) return Response::Ignore;

    const auto index = static_cast<std::size_t>(a.index());
    if (blocks_[index].has(b)) return Response::Block;
    if (overlaps_[index].has(b)) return Response::Overlap;
    return Response::Ignore;
}

TraceFilter LayerMatrix::filterFor(LayerId tracer) const
{
    return TraceFilter{ blockedBy(tracer), overlappedBy(tracer) };
}

LayerMask LayerMatrix::blockedBy(LayerId layer) const
{
    return layer.valid() ? blocks_[static_cast<std::size_t>(layer.index())] : LayerMask::none();
}

LayerMask LayerMatrix::overlappedBy(LayerId layer) const
{
    return layer.valid() ? overlaps_[static_cast<std::size_t>(layer.index())] : LayerMask::none();
}

std::string LayerMatrix::describe(LayerId layer) const
{
    const auto list = [&](LayerMask mask) {
        std::string out;
        for (int index = 0; index < LayerId::kCount; ++index) {
            const LayerId other(index);
            if (!mask.has(other)) continue;
            if (!out.empty()) out += ", ";
            out += layerName(other);
        }
        return out.empty() ? std::string("nothing") : out;
    };

    return layerName(layer) + ": blocks [" + list(blockedBy(layer))
           + "] overlaps [" + list(overlappedBy(layer)) + "]";
}

}  // namespace cromwell
