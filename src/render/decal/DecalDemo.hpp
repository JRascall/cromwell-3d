/* DecalDemo.hpp — decals to actually look at.
 *
 * SINGLE RESPONSIBILITY: build a handful of procedural decal materials and
 * scatter instances of them across the board, so the projector pass has
 * something to project and the four material channels each have something that
 * exercises them.
 *
 * SCAFFOLDING, AND LABELLED AS SUCH. Nothing in the game places decals yet —
 * scorch marks at a detonation and blood under a wounded soldier are the
 * obvious first two customers and neither exists — so without this the whole
 * system renders an empty buffer and cannot be reviewed, tuned or trusted. It
 * is behind --decals and no other code path reaches it. Delete it the day
 * something real places decals; do not build on it.
 *
 * IT IS ALSO THE TEST FOR THE THING THAT IS HARD TO GET RIGHT. Each mark is
 * projected straight down through a box two storeys tall, so anything standing
 * under one — a kerb, a crate, a step, a ramp — gets the decal wrapped over it.
 * That is the case a flat quad cannot do and the case the angle fade has to
 * survive, and it is visible at a glance rather than needing a specific camera.
 *
 * WHAT EACH MATERIAL IS FOR
 *   scorch   albedo and roughness UP — a dry, matte burn. The plain case.
 *   blood    albedo dark red and roughness DOWN to 0.12, which is the one that
 *            proves the surface channel is reaching the BRDF: a wet pool reads
 *            as wet only because the sun and sky give it a tight highlight the
 *            rough road around it cannot produce.
 *   sigil    an emissive ring with a bevelled normal — the remaining two
 *            channels, and the one mark that should stay bright inside a
 *            shadow.
 */
#pragma once

namespace xcom {

class DecalSet;
class World;

/* Registers the demo materials — "example" from
 * assets/materials/decals/example_albedo.png, plus three procedural ones — and
 * nothing else. No instances are placed.
 *
 * SEPARATE FROM THE SCATTER, AND ALWAYS RUN. The dev panel's decal tool needs
 * something to offer in its material list, and it has to be useful without
 * --decals: "there are no materials" and "the decal pass is broken" look
 * identical from the panel, and only one of them is worth debugging. Four small
 * textures at startup is a price worth paying to keep those distinguishable. */
void registerDemoMaterials(DecalSet& decals);

/* Registers the materials (via the above) and adds the scattered instances.
 *
 * IT READS THE WORLD RATHER THAN SCATTERING BLIND. The first version dropped
 * marks at random coordinates and every one of them missed — the ground decals
 * because the projector box was placed with the floor exactly on its far face,
 * where the depth fade is zero, and the wall decals because there were none.
 * "Put it somewhere and hope" is not a demo, it is a coin flip that has to be
 * debugged; asking the lattice where the walls actually are costs twenty lines
 * and cannot miss.
 *
 * Safe to call once, at startup, after the GL context exists. */
void populateDemoDecals(DecalSet& decals, const World& world);

}  // namespace xcom
