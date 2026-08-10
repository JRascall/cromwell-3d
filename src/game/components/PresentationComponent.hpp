/* PresentationComponent.hpp — what an entity is CALLED and how it is drawn.
 *
 * SINGLE RESPONSIBILITY: hold the presentation facts. These were three virtual
 * methods on Unit returning string literals, called only from the HUD and the
 * status line — UI vocabulary on a simulation type. A headless test that never
 * draws anything should not force a body to know what a HUD label is.
 */
#pragma once

#include "cromwell/entities/Component.hpp"

#include <string>
#include <utility>

namespace game {

using namespace cromwell;

/* Which mesh recipe the renderer uses. A closed set: the renderer switches on
 * it, and adding a body means teaching the renderer to draw it, which is
 * exactly the edit the compiler should demand. */
enum class VisualKind {
    Infantry,
    Vehicle,
};

class PresentationComponent : public Component {
public:
    VisualKind visual() const { return visual_; }
    void setVisual(VisualKind visual) { visual_ = visual; }

    const std::string& displayName() const { return displayName_; }
    const std::string& hudLabel() const { return hudLabel_; }
    const std::string& selectionDescription() const { return selectionDescription_; }

    /* Set together: they are three phrasings of one identity, and a body with
     * a new name but a stale HUD label is a bug nobody notices until a
     * screenshot. */
    void setNames(std::string displayName, std::string hudLabel,
                  std::string selectionDescription)
    {
        displayName_          = std::move(displayName);
        hudLabel_             = std::move(hudLabel);
        selectionDescription_ = std::move(selectionDescription);
    }

private:
    VisualKind  visual_ = VisualKind::Infantry;
    std::string displayName_          = "entity";
    std::string hudLabel_             = "entity";
    std::string selectionDescription_ = "selected the entity";
};

}  // namespace game
