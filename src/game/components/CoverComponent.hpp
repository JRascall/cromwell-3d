/* CoverComponent.hpp — how an entity interacts with the cover system.
 *
 * SINGLE RESPONSIBILITY: answer the two cover questions. A body that carries
 * armour rather than taking cover says so here, and nothing else has to know
 * why the shields are missing.
 */
#pragma once

#include "cromwell/entities/Component.hpp"

namespace game {

using namespace cromwell;

class CoverComponent : public Component {
public:
    /* Does standing beside this body grant infantry full cover? XCOM treats a
     * big unit as mobile high cover. */
    bool grantsHullCover() const { return grantsHullCover_; }
    void setGrantsHullCover(bool grants) { grantsHullCover_ = grants; }

    /* Should the cover shields be drawn for this body? A hull carries armour,
     * not cover, so shields would misreport it. */
    bool showsCoverShields() const { return showsCoverShields_; }
    void setShowsCoverShields(bool shows) { showsCoverShields_ = shows; }

private:
    bool grantsHullCover_ = false;
    bool showsCoverShields_ = true;
};

}  // namespace game
