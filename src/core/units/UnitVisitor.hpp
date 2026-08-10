/* UnitVisitor.hpp — double dispatch over the unit hierarchy.
 *
 * SINGLE RESPONSIBILITY: let a caller act differently per concrete unit type
 * WITHOUT that behaviour living on Unit.
 *
 * Drawing a soldier and drawing a tank differ, but "how do I look" is not a
 * simulation question and raylib must not reach into core/. Rendering and path
 * articulation therefore visit rather than asking Unit to know about them.
 */
#pragma once

namespace xcom {

class Soldier;
class Vehicle;

class UnitVisitor {
public:
    virtual ~UnitVisitor() = default;
    virtual void visit(const Soldier& soldier) = 0;
    virtual void visit(const Vehicle& vehicle) = 0;
};

}  // namespace xcom
