#include "cromwell/ui/core/UiDrawList.hpp"

#include <algorithm>

namespace cromwell::ui {

UiRect UiRect::intersected(const UiRect& other) const
{
    const float x0 = std::max(left(), other.left());
    const float y0 = std::max(top(), other.top());
    const float x1 = std::min(right(), other.right());
    const float y1 = std::min(bottom(), other.bottom());
    if (x1 <= x0 || y1 <= y0) {
        /* Empty, and anchored where the overlap would have been rather than at
         * the origin — a zero-size rect in the right place still answers
         * "where did this get clipped away". */
        return { x0, y0, 0.0f, 0.0f };
    }
    return { x0, y0, x1 - x0, y1 - y0 };
}

UiRect alignIn(const UiRect& container, Vec2 size,
               HorizontalAlign horizontal, VerticalAlign vertical)
{
    float x = container.x;
    if (horizontal == HorizontalAlign::Centre) {
        x += (container.width - size.x) * 0.5f;
    } else if (horizontal == HorizontalAlign::Right) {
        x += container.width - size.x;
    }

    float y = container.y;
    if (vertical == VerticalAlign::Middle) {
        y += (container.height - size.y) * 0.5f;
    } else if (vertical == VerticalAlign::Bottom) {
        y += container.height - size.y;
    }

    return { x, y, size.x, size.y };
}

void UiDrawList::clear()
{
    vertices_.clear();
    indices_.clear();
    commands_.clear();
    textRuns_.clear();
    backdropBlurs_.clear();
    clipStack_.clear();
    clipStack_.push_back(UiRect::unbounded());
}

std::uint32_t UiDrawList::addVertex(Vec2 position, const UiColor& colour)
{
    const auto index = static_cast<std::uint32_t>(vertices_.size());
    vertices_.push_back(UiVertex{ position.x, position.y, colour.toSrgb8() });
    return index;
}

UiCommand& UiDrawList::trianglesCommand()
{
    const UiRect& current = clip();
    if (!commands_.empty()) {
        UiCommand& top = commands_.back();
        const bool sameClip = top.clip.x == current.x && top.clip.y == current.y
                           && top.clip.width == current.width && top.clip.height == current.height;
        if (top.kind == UiCommandKind::Triangles && sameClip) {
            return top;
        }
    }

    UiCommand opened;
    opened.kind = UiCommandKind::Triangles;
    opened.clip = current;
    opened.indexBegin = static_cast<std::uint32_t>(indices_.size());
    opened.indexCount = 0;
    commands_.push_back(opened);
    return commands_.back();
}

void UiDrawList::addTriangle(std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
    UiCommand& command = trianglesCommand();
    indices_.push_back(a);
    indices_.push_back(b);
    indices_.push_back(c);
    command.indexCount += 3;
}

void UiDrawList::addQuad(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d)
{
    /* One command lookup for six indices rather than two — addQuad is called
     * once per band column and once per feather edge, which is most of the
     * geometry in the kit. */
    UiCommand& command = trianglesCommand();
    indices_.push_back(a);
    indices_.push_back(b);
    indices_.push_back(c);
    indices_.push_back(a);
    indices_.push_back(c);
    indices_.push_back(d);
    command.indexCount += 6;
}

void UiDrawList::addText(TextRun run)
{
    UiCommand command;
    command.kind = UiCommandKind::Text;
    command.clip = clip();
    command.payloadIndex = static_cast<std::uint32_t>(textRuns_.size());
    commands_.push_back(command);
    textRuns_.push_back(std::move(run));
}

void UiDrawList::addBackdropBlur(const UiBackdropBlur& blur)
{
    UiCommand command;
    command.kind = UiCommandKind::BackdropBlur;
    command.clip = clip();
    command.payloadIndex = static_cast<std::uint32_t>(backdropBlurs_.size());
    commands_.push_back(command);
    backdropBlurs_.push_back(blur);
}

void UiDrawList::pushClip(const UiRect& rect)
{
    clipStack_.push_back(rect.intersected(clipStack_.back()));
}

void UiDrawList::popClip()
{
    /* The bottom entry is the unbounded rect and stays put — an unbalanced pop
     * is a bug in the caller, but leaving the stack empty would turn it into a
     * crash three widgets later instead of a wrong clip here. */
    if (clipStack_.size() > 1) {
        clipStack_.pop_back();
    }
}

}  // namespace cromwell::ui
