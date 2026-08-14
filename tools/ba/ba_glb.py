"""Convert the exported Broken Arrow FBX to .glb, for the asset browser.

    "<blender>" -b --python tools/ba/ba_glb.py -- <in-dir> <out-dir> [--limit N]

WHY CONVERT AT ALL. The browser reads .obj and .glb and plays animation from any
rigged .glb - clip list, playback, repeat, the lot. Teaching it FBX would mean
writing a binary FBX reader with skinning and curve evaluation, which is a large
amount of new surface for a format we only produce as an intermediate. Blender
already reads FBX and writes glTF, so the cheap move is to meet the browser
where it already works.

WHY NOT EMBED THE TEXTURES. A .glb carries its images inside it. Broken Arrow's
rigs share texture sets heavily - every US rifleman variant uses the same body
maps - so embedding would duplicate the same few megabytes across hundreds of
files and turn a 5 GB library into something far larger for no new information.
The textures are already on disk as PNG from the bundle sweep, and the browser
resolves them per material through ba_materials.csv, the same shape as the
Mercenaries route. So geometry, skin and animation here; pixels stay where they
are.

WHAT MATTERS IN THE EXPORT SETTINGS:

  export_animation_mode='NLA_TRACKS' - the one that decides whether this
    works. Blender's default packs the *current* action only, and ACTIONS mode
    only emits actions that are assigned or on a track - the FBX importer
    assigns one and orphans the rest. So stack_actions() puts every action on a
    track named after its take first, and this mode then emits one glTF
    animation per track. Get it wrong and you get a rigged model with one clip,
    and nothing anywhere says the other twenty-one were dropped.

  export_apply=False - modifiers are not baked. There are none worth baking and
    applying them on a skinned mesh loses the armature binding.

Resumable: an existing .glb is skipped, so an interrupted run costs nothing.
"""
import os
import math
import re
import sys
import time

import bpy
import mathutils


def argv_after_ddash():
    return sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []


def stack_actions():
    """Put every imported action on an NLA track named after its take.

    THE ONE THING THAT MAKES THE CLIP LIST WORK. Blender's FBX importer creates
    an action per (object, take) but only ASSIGNS the last one; the other
    twenty-one are orphans held alive by a fake user. glTF export in ACTIONS
    mode only emits actions that are active or on an NLA track, so a 22-take
    rig exported straight after import yields ONE take and says nothing about
    the rest - which is the same silent-partial-output failure as everywhere
    else in this pipeline.

    Actions are named `Object|Take|Layer`, so the take is field 1. Naming the
    NLA track after the take alone means the armature's track and the weapon
    nodes' tracks share a name, and the exporter merges same-named tracks into
    a single glTF animation - so 110 actions across 5 objects come out as 22
    clips called Kneel_run, Stand_death and so on, instead of 110 rows of
    `Hips|Kneel_run|Animation Base Layer` for the user to sort out.
    """
    tracks = 0
    for obj in bpy.data.objects:
        mine = [a for a in bpy.data.actions
                if a.name.split('|')[0] == obj.name]
        if not mine:
            continue
        if obj.animation_data is None:
            obj.animation_data_create()
        # The active action would otherwise be exported a second time, as its
        # own unnamed animation, duplicating one clip in every file.
        obj.animation_data.action = None
        for act in mine:
            parts = act.name.split('|')
            take = parts[1] if len(parts) > 1 else act.name
            track = obj.animation_data.nla_tracks.new()
            track.name = take
            track.strips.new(take, int(act.frame_range[0]), act)
            # MUTED, or the model's rest pose is whatever the first strip
            # happens to evaluate to at frame 1. That is how a rifleman ends up
            # shipped mid-parachute: the geometry is right, the clips are right,
            # and the pose you see before pressing play is a frame of an
            # unrelated animation. The exporter unmutes each track in turn as it
            # walks them, so this costs no clips.
            track.mute = True
            tracks += 1
    return tracks


LOD_RE = re.compile(r'(^|[_\W])lod[_\-]?([1-9]\d*)($|[_\W])', re.I)


def drop_lower_lods():
    """Delete every LOD but the highest.

    Unity ships these units as lod_0/lod_1/lod_2 under one skinned hierarchy and
    the FBX carries all three. They occupy the SAME space, so a preview renders
    two or three interpenetrating soldiers - which reads as a broken mesh rather
    than as three levels of detail, and triples the triangle count of every
    file for a viewer that only ever wants the best one.

    Matched on a word-boundary `lod<n>` with n >= 1 rather than on a substring:
    `lod_0` must survive, and so must anything that merely contains the letters.
    """
    doomed = [o for o in bpy.data.objects if o.type == 'MESH' and LOD_RE.search(o.name)]
    for o in doomed:
        bpy.data.objects.remove(o, do_unlink=True)
    return len(doomed)


def stand_upright():
    """Flip the imported hierarchy so the model's up-axis is +Y.

    Worked out by measurement, not from the specs, because the specs would give
    the wrong answer here. Blender is Z-up and glTF is Y-up, so the expected
    move is export_yup=True - but these FBX come from Unity and Blender's
    importer leaves them Y-tall in Blender's own coordinates (measured
    x=1.80 y=2.14 z=1.13, identical with a forced -Z/Y orientation). Exporting
    with the conversion on therefore lays the character down; exporting with it
    off stands it up and upside down, head at -Y.

    So the model arrives glTF-oriented but inverted, and one 180-degree turn
    about X - (x,y,z) -> (x,-y,-z) - puts up at +Y. Then the export must NOT
    convert again, which is why export_yup is false below.

    The acceptance test is the library that was already right: a Mercenaries
    character measures y=2.04 as its tallest axis and renders standing. A Broken
    Arrow rifleman now measures the same way instead of z=1.88.
    """
    flip = mathutils.Matrix.Rotation(math.pi, 4, 'X')
    for obj in bpy.data.objects:
        if obj.parent is None:
            obj.matrix_world = flip @ obj.matrix_world


def convert(src, dst):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=src)
    dropped = drop_lower_lods()
    stand_upright()

    # Every action in the file, not just whatever happens to be assigned. The
    # FBX carries one action per (object, take), so a 22-take rig arrives as
    # ~110 actions across the armature and its attachment nodes.
    actions = len(bpy.data.actions)
    stack_actions()
    arms = [o for o in bpy.data.objects if o.type == 'ARMATURE']
    bones = sum(len(a.data.bones) for a in arms)

    bpy.ops.export_scene.gltf(
        filepath=dst,
        export_format='GLB',
        export_animation_mode='NLA_TRACKS',
        export_animations=True,
        export_skins=True,
        export_apply=False,
        export_materials='EXPORT',
        export_image_format='NONE',   # names the material, ships no pixels
        # export_yup=False, WHICH LOOKS WRONG AND IS NOT. Blender is Z-up and
        # glTF is Y-up, so the conversion is normally wanted. But these FBX come
        # out of Unity, and Blender's importer leaves them Y-tall in Blender's
        # own coordinates - measured, both with the default import and with a
        # forced -Z/Y orientation: x=1.80 y=2.14 z=1.13, so the character is
        # already lying down in the Blender scene and already glTF-oriented.
        # Converting again stands it on its face. Checked against the library
        # that was already right: a Mercenaries character measures y=2.04 as its
        # tallest axis, and with this false a Broken Arrow rifleman measures the
        # same way instead of z=1.88.
        export_yup=False,
    )
    return actions, bones, len(arms), dropped


def main():
    args = argv_after_ddash()
    if len(args) < 2:
        print('usage: ... -- <in-dir> <out-dir> [--limit N] [--filter S]')
        return
    src_dir, out_dir = args[0], args[1]
    limit = int(args[args.index('--limit') + 1]) if '--limit' in args else 0
    filt = args[args.index('--filter') + 1].lower() if '--filter' in args else ''
    os.makedirs(out_dir, exist_ok=True)

    files = sorted(f for f in os.listdir(src_dir) if f.lower().endswith('.fbx'))
    if filt:
        files = [f for f in files if filt in f.lower()]
    if limit:
        files = files[:limit]

    # Sharding, because exporting every take is the expensive part: ~2 minutes a
    # rig, so 422 of them is most of a day in one process. Blender is
    # single-threaded for this and the machine is not, so the driver runs
    # several instances over interleaved slices. Interleaved rather than
    # contiguous so a shard cannot draw all the 150-bone vehicles and run four
    # times as long as its neighbours.
    shards = int(args[args.index('--shards') + 1]) if '--shards' in args else 1
    shard = int(args[args.index('--shard') + 1]) if '--shard' in args else 0
    if shards > 1:
        files = files[shard::shards]
        print(f'BAGLB-SHARD {shard}/{shards}: {len(files)} files', flush=True)

    done = skipped = failed = 0
    t0 = time.time()
    for i, f in enumerate(files):
        dst = os.path.join(out_dir, os.path.splitext(f)[0] + '.glb')
        if os.path.exists(dst):
            skipped += 1
            continue
        try:
            actions, bones, arms, dropped = convert(os.path.join(src_dir, f), dst)
            done += 1
            print(f'BAGLB {f} -> actions={actions} bones={bones} armatures={arms} '
                  f'lods_dropped={dropped}', flush=True)
        except Exception as e:
            failed += 1
            print(f'BAGLB-FAIL {f}: {e}', flush=True)
        if (i + 1) % 25 == 0:
            print(f'BAGLB-PROGRESS {i + 1}/{len(files)} {time.time() - t0:.0f}s', flush=True)
    print(f'BAGLB-DONE converted={done} skipped={skipped} failed={failed} '
          f'in {time.time() - t0:.0f}s', flush=True)


if __name__ == '__main__':
    main()
