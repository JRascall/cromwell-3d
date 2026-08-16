"""Tkinter front end for asset_browser.py. Import it from there, not directly.

Split from the core so the catalogue, the rasteriser and the channel split stay
headless and testable -- the same reason xcom_parcel_render.py is a script rather
than a viewer. Everything here is presentation.

Three interaction notes that are not obvious from the code:

*The orbit camera renders twice per gesture -- unless there is a GPU.* The
triangle rasteriser is a Python loop over faces and costs ~60us each whatever the
resolution, so it cannot be turned down into an interactive renderer -- 1,500
triangles is already only 11 fps. Without moderngl the preview scatters samples
over triangle interiors while a button is held (vectorised, ~19 fps) and the
shaded raster returns on release; that is why dragging used to lose the texture
and the surface detail. With moderngl the same GPU render serves both, so the
picture no longer changes when you touch it. Zoom coalesces either way -- the
wheel redraws immediately and schedules one sharp redraw 180 ms later.

*Animation playback is just a moving camera.* A clip drives the same redraw path
a drag does, posing the mesh on the CPU and handing the renderer a new vertex
array. It advances by wall clock rather than a fixed step per frame, so a clip
runs at its authored speed instead of at whatever the renderer manages.

*Channel buttons are the point of the texture view.* These engines pack unrelated
data per channel, so RGB is often meaningless -- see the module docstring in
asset_browser.py.
"""
import math
import threading
import tkinter as tk
from tkinter import ttk
from pathlib import Path

from PIL import Image, ImageTk

import asset_browser as ab

BG = '#16161a'
FG = '#d8d8dc'
ACCENT = '#3b7dd8'
PREVIEW = 560
MAX_RESULTS = 3000        # listbox stops being useful long before this


class Browser:
    def __init__(self, root, rows):
        self.root = root
        self.rows = rows
        self.results = []
        self.current = None
        self.mode = 'flat'
        self.channel = 'rgb'
        self.flip_v = True   # see _uv_row in asset_browser: genuinely ambiguous
        self.yaw, self.pitch = 35.0, 20.0
        self.zoom = 1.0
        self.pan = [0.0, 0.0]
        # Free-fly camera. Orbiting a fitted bounding box is right for one
        # object and useless for an assembled world: a 4 km map fits the frame
        # as a smudge and there is no way to get inside it. In fly mode the
        # camera has a position, looks where it is pointed, and moves.
        self.fly = False
        self.cam = [0.0, 0.0, 0.0]
        self.fov = 70.0
        # 600 m, not further: measured on an assembled world the splat renderer
        # runs at 12 fps with no cull, 11 at 1200 m, 19 at 600 and 37 at 300.
        # Beyond ~1 km almost nothing is rejected on a 4 km map, so the cull
        # costs its own test and buys nothing.
        self.far = 600.0                  # cull distance, metres; 0 disables
        self.speed = 20.0                 # metres per key press
        self._bounds = None               # last mesh's (lo, hi), for placing the camera
        self._drag = None
        self._photo = None
        self._mesh_cache = {}
        self._loading = set()     # paths currently being parsed on a worker thread
        self._glb_cache = {}      # path -> embedded glTF materials, decoded once
        self._thumbs = []

        # Animation playback. A .glb that carries a skin and clips gets a clip
        # list beside the preview; everything else never sees the panel.
        self._rig_cache = {}      # path -> skin + clip names, no curve data
        self._clip_source = {}    # path -> [(kind, ref, name)], 'glb' or 'anim'
        self._rig_path = None     # which path the clip list currently describes
        self._clip = None         # index into the file's own clips, or None
        self._anim = None         # a loaded standalone .anim, or None
        self._clip_name = ''      # shown while playing, whichever source
        self._clip_len = 0.0
        self._clip_t = 0.0
        self._playing = False
        self._loop = True         # asked for, and it is the useful default: a
                                  # 1.2 s walk cycle is unreadable played once
        self._play_job = None
        self._last_tick = 0.0
        self._frame_ms = 0.0      # smoothed, only to report the playback rate
        self._clip_rows = []      # (index, name) currently shown, after filtering

        root.title('Extracted asset browser')
        root.configure(bg=BG)
        root.geometry('1180x760')

        self._build_toolbar()
        self._build_body()
        self.do_search()

    # -- layout ---------------------------------------------------------
    def _build_toolbar(self):
        bar = tk.Frame(self.root, bg=BG)
        bar.pack(fill='x', padx=8, pady=6)

        tk.Label(bar, text='search', bg=BG, fg=FG).pack(side='left')
        self.q = tk.Entry(bar, width=28, bg='#22222a', fg=FG, insertbackground=FG,
                          relief='flat')
        self.q.pack(side='left', padx=6)
        self.q.bind('<Return>', lambda e: self.do_search())

        libs = ['(all)'] + sorted({r[0] for r in self.rows})
        kinds = ['(all)'] + sorted({r[1] for r in self.rows})
        self.lib = ttk.Combobox(bar, values=libs, width=10, state='readonly')
        self.lib.set('(all)')
        self.lib.pack(side='left', padx=4)
        self.kind = ttk.Combobox(bar, values=kinds, width=12, state='readonly')
        self.kind.set('(all)')
        self.kind.pack(side='left', padx=4)

        # Role and material are the two filters that answer real questions:
        # "every normal map" and "meshes whose textures I can actually find".
        roles = ['(all)'] + sorted({r[ab.ROLE] for r in self.rows if r[ab.ROLE]})
        mats = ['(all)'] + sorted({r[ab.MAT] for r in self.rows if r[ab.MAT]})
        self.role = ttk.Combobox(bar, values=roles, width=10, state='readonly')
        self.role.set('(all)')
        self.role.pack(side='left', padx=4)
        self.material = ttk.Combobox(bar, values=mats, width=10, state='readonly')
        self.material.set('(all)')
        self.material.pack(side='left', padx=4)

        for c in (self.lib, self.kind, self.role, self.material):
            c.bind('<<ComboboxSelected>>', lambda e: self.do_search())

        tk.Button(bar, text='search', command=self.do_search, bg='#2a2a34', fg=FG,
                  relief='flat', padx=10).pack(side='left', padx=6)
        self.count = tk.Label(bar, text='', bg=BG, fg='#8a8a96')
        self.count.pack(side='left', padx=10)

    def _build_body(self):
        body = tk.Frame(self.root, bg=BG)
        body.pack(fill='both', expand=True, padx=8, pady=(0, 8))

        left = tk.Frame(body, bg=BG)
        left.pack(side='left', fill='y')
        self.listbox = tk.Listbox(left, width=52, bg='#1c1c22', fg=FG,
                                  selectbackground=ACCENT, relief='flat',
                                  activestyle='none', exportselection=False)
        self.listbox.pack(side='left', fill='y', expand=True)
        sb = tk.Scrollbar(left, command=self.listbox.yview)
        sb.pack(side='left', fill='y')
        self.listbox.config(yscrollcommand=sb.set)
        self.listbox.bind('<<ListboxSelect>>', lambda e: self.select())

        right = tk.Frame(body, bg=BG)
        right.pack(side='left', fill='both', expand=True, padx=(10, 0))

        self.canvas = tk.Label(right, bg='#0e0e12')
        self.canvas.pack()
        # Orbit on left drag, pan on right (or shift+left, for trackpads), zoom on
        # the wheel. Linux reports the wheel as buttons 4/5 rather than <MouseWheel>.
        self.canvas.bind('<ButtonPress-1>', lambda e: self._press(e, 'orbit'))
        self.canvas.bind('<B1-Motion>', self._motion)
        self.canvas.bind('<ButtonRelease-1>', self._release)
        self.canvas.bind('<Shift-ButtonPress-1>', lambda e: self._press(e, 'pan'))
        self.canvas.bind('<ButtonPress-3>', lambda e: self._press(e, 'pan'))
        self.canvas.bind('<B3-Motion>', self._motion)
        self.canvas.bind('<ButtonRelease-3>', self._release)
        self.canvas.bind('<MouseWheel>', self._wheel)
        self.canvas.bind('<Button-4>', lambda e: self._wheel(e, +1))
        self.canvas.bind('<Button-5>', lambda e: self._wheel(e, -1))
        self.canvas.bind('<Double-Button-1>', lambda e: self.reset_view())
        self.root.bind('<KeyPress-r>', lambda e: self.reset_view())
        # Fly controls. Bound on the root so they work without clicking the
        # canvas first, and skipped while the search box has focus so typing a
        # query does not walk the camera across the map.
        self.root.bind('<KeyPress-f>', lambda e: self._toggle_fly())
        self.root.bind('<KeyPress-bracketleft>', lambda e: self._set_far(1 / 1.4))
        self.root.bind('<KeyPress-bracketright>', lambda e: self._set_far(1.4))
        for key, vec in (('w', (0, 0, 1)), ('s', (0, 0, -1)),
                         ('a', (-1, 0, 0)), ('d', (1, 0, 0)),
                         ('e', (0, 1, 0)), ('q', (0, -1, 0))):
            self.root.bind('<KeyPress-%s>' % key,
                           lambda ev, v=vec: self._move(v, fast=bool(ev.state & 0x1)))
            self.root.bind('<KeyPress-%s>' % key.upper(),
                           lambda ev, v=vec: self._move(v, fast=True))

        self.modebar = tk.Frame(right, bg=BG)
        self.modebar.pack(fill='x', pady=6)

        self.info = tk.Label(right, text='', bg=BG, fg='#8a8a96', anchor='w',
                             justify='left', wraplength=PREVIEW)
        self.info.pack(fill='x')

        self.matbar = tk.Frame(right, bg=BG)
        self.matbar.pack(fill='x', pady=6)

        # -- animation column, hidden until a mesh with clips is selected.
        # A third column rather than a strip under the preview: a Mercenaries
        # character carries 1,639 clips, and a list that long is only usable
        # full height with a filter box over it.
        self.animcol = tk.Frame(body, bg=BG)
        self.animtitle = tk.Label(self.animcol, text='animations', bg=BG, fg=FG,
                                  anchor='w')
        self.animtitle.pack(fill='x')
        self.animq = tk.Entry(self.animcol, bg='#22222a', fg=FG,
                              insertbackground=FG, relief='flat')
        self.animq.pack(fill='x', pady=3)
        self.animq.bind('<KeyRelease>', lambda e: self._fill_clips())
        row = tk.Frame(self.animcol, bg=BG)
        row.pack(fill='both', expand=True)
        self.animlist = tk.Listbox(row, width=30, bg='#1c1c22', fg=FG,
                                   selectbackground=ACCENT, relief='flat',
                                   activestyle='none', exportselection=False)
        self.animlist.pack(side='left', fill='both', expand=True)
        asb = tk.Scrollbar(row, command=self.animlist.yview)
        asb.pack(side='left', fill='y')
        self.animlist.config(yscrollcommand=asb.set)
        self.animlist.bind('<<ListboxSelect>>', lambda e: self._pick_clip())
        ctl = tk.Frame(self.animcol, bg=BG)
        ctl.pack(fill='x', pady=4)
        self.playbtn = self._button(ctl, 'play', self._toggle_play)
        self.playbtn.pack(side='left', padx=2)
        self.loopbtn = self._button(ctl, 'repeat', self._toggle_loop, active=self._loop)
        self.loopbtn.pack(side='left', padx=2)
        self._button(ctl, 'bind pose', self._clear_clip).pack(side='left', padx=2)
        self.animinfo = tk.Label(self.animcol, text='', bg=BG, fg='#8a8a96',
                                 anchor='w', justify='left')
        self.animinfo.pack(fill='x')

    def _button(self, parent, text, cmd, active=False):
        return tk.Button(parent, text=text, command=cmd, relief='flat', padx=9,
                         bg=ACCENT if active else '#2a2a34', fg='white' if active else FG)

    # -- search ---------------------------------------------------------
    def do_search(self):
        def pick(w):
            v = w.get()
            return None if v == '(all)' else v

        allhits = ab.search(self.rows, self.q.get(), pick(self.lib), pick(self.kind),
                            pick(self.role), pick(self.material))
        self.results = allhits[:MAX_RESULTS]
        self.listbox.delete(0, 'end')
        for r in self.results:
            tag = r[ab.ROLE] or r[ab.MAT]
            tag = f' <{tag}>' if tag else ''
            self.listbox.insert('end', f'[{r[ab.LIB]}]{tag} {r[ab.NAME][:52]}')
        extra = '' if len(allhits) <= MAX_RESULTS else f'  (showing first {MAX_RESULTS:,})'
        self.count.config(text=f'{len(allhits):,} matches{extra}')

    def select(self):
        sel = self.listbox.curselection()
        if not sel:
            return
        self.current = self.results[sel[0]]
        # Full camera reset on every selection: carrying a 10x zoom and a large
        # pan onto the next mesh shows an empty canvas and reads as a broken tool.
        self.yaw, self.pitch, self.zoom = 35.0, 20.0, 1.0
        self.pan = [0.0, 0.0]
        # Show the material if there is one. Defaulting to flat meant an XCOM prop
        # opened untextured next to a row of its own texture thumbnails, which
        # looks like the textures failed to apply rather than like a mode choice.
        if self.current[ab.KIND] in ab.MESH_KINDS:
            # 'embedded' is Helldivers, whose maps live inside the .glb. Ask for
            # diffuse either way; _draw_mesh falls back to flat if the asset has
            # no albedo, so this cannot land on an empty mode.
            mat = self.current[ab.MAT]
            self.mode = 'diffuse' if ('dif' in mat or mat == 'embedded') else 'flat'
            # Reset with the selection, like the camera: the flip is a property
            # of the FORMAT, and carrying a manual override onto the next asset
            # would silently invert it.
            self.flip_v = ab.default_flip_v(self.current[ab.PATH])
        self.draw()

    # -- drawing --------------------------------------------------------
    def draw(self):
        if not self.current:
            return
        lib, kind, name, path = (self.current[ab.LIB], self.current[ab.KIND],
                                 self.current[ab.NAME], self.current[ab.PATH])
        for w in self.modebar.winfo_children():
            w.destroy()
        for w in self.matbar.winfo_children():
            w.destroy()
        self._thumbs = []

        try:
            if kind in ab.MESH_KINDS:
                self._draw_mesh(path)
            else:
                self._draw_texture(path)
        except Exception as e:
            self.canvas.config(image='')
            self.info.config(text=f'{name}\n\ncould not open: {type(e).__name__}: {e}')

    def _show(self, img):
        self._photo = ImageTk.PhotoImage(img)
        self.canvas.config(image=self._photo)

    def _load_async(self, path):
        """Parse the mesh and its materials off the Tk thread.

        A composed world is 18.7 MB of .obj and 105 textures, which is seconds
        of work. Done inline it looks exactly like a hang: the window stops
        redrawing and there is nothing to say why. Neither call touches Tk, so
        both are safe in a worker; only the caches and the redraw are handed
        back to the main thread.
        """
        def work():
            try:
                mesh = ab.load_mesh(path)
                mats = ab.mesh_materials(path)
            except Exception as e:                      # reported on the UI thread
                self.root.after(0, lambda: self._load_failed(path, e))
                return
            rig, clips = None, []
            if str(path).lower().endswith('.glb'):
                # A rig is optional, so a file without one - or with one this
                # loader cannot read - still previews as geometry.
                try:
                    rig = ab.load_glb_rig(path)
                except Exception:
                    rig = None
            if rig:
                clips = [('glb', i, n) for i, n in enumerate(rig['clips'])]
                seen = {n.lower() for _, _, n in clips}
                try:
                    # Indexing the library is ~1,800 header reads. It belongs
                    # here on the worker for the same reason the mesh parse
                    # does: on the Tk thread it would read as a hang.
                    #
                    # By NAME, keeping the file's own copy: the three animated
                    # merc exports carry the same 1,639 clips the library holds,
                    # and without this every one of them appears twice.
                    for n, p in ab.anim_library(path, rig):
                        if n.lower() not in seen:
                            seen.add(n.lower())
                            clips.append(('anim', p, n))
                except Exception:
                    pass
            self.root.after(0,
                            lambda: self._load_done(path, mesh, mats, rig, clips))
        self._loading.add(path)
        threading.Thread(target=work, daemon=True).start()

    def _load_done(self, path, mesh, mats, rig=None, clips=()):
        self._loading.discard(path)
        self._mesh_cache[path] = mesh
        self._glb_cache[path] = mats
        self._rig_cache[path] = rig
        self._clip_source[path] = list(clips)
        if self.current and self.current[ab.PATH] == path:
            self.draw()

    # -- animation ------------------------------------------------------
    def _default_clip(self, rig):
        """Which clip to show a rig in when nothing is playing.

        Not clip 0: these files list alphabetically, so clip 0 is `Kneel_death`
        on most infantry and the browser would open every soldier lying dead on
        the floor. Preference order is a standing idle, then any idle, then
        anything not obviously a death or prone pose.
        """
        names = [str(c).lower() for c in rig.get('clips') or ()]
        for want in ('stand_idle', 'stand_aim', 'idle', 'stand', 'default'):
            for i, n in enumerate(names):
                if want in n and 'death' not in n:
                    return i
        for i, n in enumerate(names):
            if not any(bad in n for bad in ('death', 'prone', 'die', 'dead')):
                return i
        return 0

    def _sync_anim_panel(self, path):
        """Show the clip list for this mesh, or hide the column entirely.

        Two sources, and the second is the important one. A .glb may carry its
        own clips; separately, any rig can be driven by the standalone .anim
        library, because every human in the game shares one skeleton and clips
        bind by joint name. Without that, 85 of the 88 human models would show
        an empty panel purely because the export chose three of them to carry
        the curves.

        Rebuilt only when the path changes: filling thousands of rows on every
        camera move would make the orbit unusable.
        """
        rig = self._rig_cache.get(path)
        clips = self._clip_source.get(path) or []
        if not clips:
            self.animcol.pack_forget()
            self._rig_path = None
            self._stop()
            self._clip = self._anim = None
            return
        self.animcol.pack(side='left', fill='y', padx=(10, 0))
        if self._rig_path == path:
            return
        self._rig_path = path
        self._stop()
        self._clip = self._anim = None
        self.animq.delete(0, 'end')
        self._fill_clips()
        own = sum(1 for c in clips if c[0] == 'glb')
        self.animinfo.config(
            text='%d joints - %s\nclick a clip' %
                 (len(rig['joints']),
                  'in this file' if own == len(clips) else
                  '%d in file, %d from the library' % (own, len(clips) - own)))

    def _fill_clips(self):
        clips = self._clip_source.get(self._rig_path) or []
        q = self.animq.get().strip().lower()
        self._clip_rows = [c for c in clips if not q or q in c[2].lower()]
        self.animlist.delete(0, 'end')
        for c in self._clip_rows[:MAX_RESULTS]:
            self.animlist.insert('end', c[2])
        self.animtitle.config(text='animations  (%d of %d)'
                                   % (len(self._clip_rows), len(clips)))

    def _pick_clip(self):
        sel = self.animlist.curselection()
        if not sel or sel[0] >= len(self._clip_rows):
            return
        rig = self._rig_cache.get(self._rig_path)
        kind, ref, name = self._clip_rows[sel[0]]
        self._anim = None
        self._clip = None
        try:
            if kind == 'glb':
                self._clip = ref
                self._clip_len = ab.clip_duration(rig, ref)
            else:
                self._anim = ab.load_anim(ref)
                self._clip_len = self._anim['duration']
        except Exception as e:
            self.animinfo.config(text='could not load %s: %s' % (name[:30], e))
            return
        self._clip_name = name
        self._clip_t = 0.0
        self._play()

    def _has_clip(self):
        """One test for 'something is posed', whichever source it came from."""
        return self._clip is not None or self._anim is not None

    def _clear_clip(self):
        self._stop()
        self._clip = self._anim = None
        self.animlist.selection_clear(0, 'end')
        self.draw()

    def _toggle_loop(self):
        self._loop = not self._loop
        self.loopbtn.config(bg=ACCENT if self._loop else '#2a2a34',
                            fg='white' if self._loop else FG)

    def _toggle_play(self):
        if self._playing:
            self._stop()
            self.draw()                      # sharp frame once it settles
        elif self._clip is not None:
            self._play()

    def _play(self):
        import time as _t
        if not self._has_clip():
            return
        self._playing = True
        self.playbtn.config(text='pause', bg=ACCENT, fg='white')
        self._last_tick = _t.perf_counter()
        if self._play_job is None:
            self._tick()

    def _stop(self):
        self._playing = False
        if self._play_job is not None:
            self.root.after_cancel(self._play_job)
            self._play_job = None
        if hasattr(self, 'playbtn'):
            self.playbtn.config(text='play', bg='#2a2a34', fg=FG)

    def _tick(self):
        """Advance by WALL CLOCK, not by a fixed step per frame.

        The software renderer's frame time is not the clip's frame time, so
        stepping a constant amount per redraw plays every animation at whatever
        speed the rasteriser happens to manage - and makes a slow machine
        disagree with a fast one about what the animation looks like. Timing
        against the clock costs nothing and means the clip runs at its authored
        speed or drops frames trying.
        """
        import time as _t
        self._play_job = None
        if not self._playing or not self._has_clip():
            return
        now = _t.perf_counter()
        dt = now - self._last_tick
        self._clip_t += dt
        self._last_tick = now
        # Smoothed, so the reported rate is readable rather than flickering.
        self._frame_ms = dt if not self._frame_ms else self._frame_ms * 0.8 + dt * 0.2
        if self._clip_len > 0 and self._clip_t >= self._clip_len:
            if self._loop:
                self._clip_t %= self._clip_len
            else:
                self._clip_t = self._clip_len
                self._stop()
                self.draw()
                return
        if self.current:
            self._draw_mesh(self.current[ab.PATH], interactive=True)
        self._show_clip_status()
        self._play_job = self.root.after(16, self._tick)

    def _show_clip_status(self):
        if not self._has_clip():
            return
        self.animinfo.config(
            text='%s\n%.2f / %.2f s   %d fps   %s'
                 % (self._clip_name[:34], self._clip_t, self._clip_len,
                    round(1 / max(1e-6, self._frame_ms)) if self._frame_ms else 0,
                    'repeat' if self._loop else 'once'))

    def _load_failed(self, path, err):
        self._loading.discard(path)
        self.info.config(text='%s\n\ncould not open: %s: %s'
                              % (Path(path).name, type(err).__name__, err))

    def _draw_mesh(self, path, interactive=False):
        if path in self._loading:
            return                                      # already on its way
        if path not in self._mesh_cache or path not in self._glb_cache:
            self.canvas.config(image='')
            self.info.config(text='%s\n\nloading - parsing geometry and textures...'
                                  % Path(path).name)
            self.root.update_idletasks()
            self._load_async(path)
            return
        self._sync_anim_panel(path)
        mesh = self._mesh_cache[path]
        rig = self._rig_cache.get(path)
        # BROKEN ARROW'S BIND POSE IS FACE DOWN, and legitimately so. Its .glb
        # come from Unity via Blender, and the upright orientation lives in the
        # clips' root bone rather than in the rest pose - the inverse bind
        # matrices cancel the joint hierarchy exactly, so the file's rest pose
        # IS the raw mesh, lying on its face. Nothing is broken; there is simply
        # no pose to show until a clip is applied. Showing the first frame of a
        # real clip is the only way the default preview reads as a character.
        #
        # Scoped to this library on purpose: a Mercenaries or Helldivers rig has
        # a proper T-pose bind, and replacing that with an arbitrary clip frame
        # would be a regression there.
        clip = self._clip
        # The clock belongs to whatever the user picked, from EITHER source -
        # a .glb clip in self._clip or a standalone .anim in self._anim. Only
        # the fallback below is a still frame, so only it pins t to zero.
        # Testing self._clip alone froze every .anim at frame 0: the tick kept
        # counting and the info line kept reading, but the rig never moved.
        clip_t = self._clip_t
        if (clip is None and self._anim is None and rig is not None
                and rig.get('clips') and ab._in_library(path, 'ba')):
            clip = self._default_clip(rig)
            clip_t = 0.0
        if rig is not None and (clip is not None or self._anim is not None):
            # Substitute posed positions and normals into the same tuple the
            # renderers already take, so every material mode, camera mode and
            # texture path below works on an animated frame unchanged.
            try:
                pv, pn = ab.pose_glb(rig, mesh, clip, clip_t, anim=self._anim)
                mesh = (pv, mesh[1], pn, mesh[3], mesh[4])
            except Exception as e:
                self._stop()
                self.animinfo.config(text='clip failed: %s' % e)
        # Playing is a moving camera by another name: the face rasteriser costs
        # ~200 ms a frame whatever the resolution, so playback takes the same
        # splat path a drag does and the sharp frame returns when it stops.
        interactive = interactive or self._playing
        V, T, N, F = mesh[0], mesh[1], mesh[2], mesh[3]

        # Helldivers keeps its textures inside the .glb, so they come from the file
        # itself rather than from a side-table. Cached per path: decoding several
        # 2048px PNGs on every redraw would make the orbit unusable.
        glb_mats = None
        glb_extra = []
        # Materials come from wherever the library keeps them - embedded in a
        # .glb, or resolved through materials.csv for Mercenaries. Cached per
        # path either way, since selecting a mesh re-renders on every camera move.
        if path not in self._glb_cache:
            try:
                self._glb_cache[path] = ab.mesh_materials(path)
            except Exception:
                self._glb_cache[path] = ([], [])
        glb_mats, glb_extra = self._glb_cache[path]

        mats = ab.xcom_material(path) or {}
        # Every map the asset actually has becomes a view mode, so a spec or mask
        # map can be seen ON the surface rather than only as a flat thumbnail --
        # which is where the UV layout and the packing actually become legible.
        slots = [s for s in ('diffuse', 'normal', 'mask', 'specular') if mats.get(s)]
        # For a .glb the maps come from the embedded materials instead, unioned
        # across every material the mesh uses.
        if glb_mats:
            present = set()
            for mm in glb_mats:
                present |= set(mm)
            slots = [s for s in ('diffuse', 'normal', 'specular', 'emissive', 'occlusion')
                     if s in present]
        modes = ['flat', 'normals'] + slots
        extra_modes = [f'+{lbl}{i}' for i, (nm, lbl, im) in enumerate(glb_extra)]
        modes += extra_modes
        # 'emissive' only appears when the MSK's alpha actually looks like a glow
        # mask, so it is not offered on assets that do not self-illuminate.
        if not glb_mats and mats.get('diffuse') and mats.get('emissive'):
            modes.append('emissive')
        if not glb_mats and mats.get('diffuse') and (mats.get('specular') or mats.get('emissive')):
            modes.append('lit')
        if self.mode not in modes:
            self.mode = 'diffuse' if 'diffuse' in slots else 'flat'

        tex = spec = emis = None
        tex_by_mat = None
        if self.mode.startswith('+'):
            # An unassigned embedded image: no material owns it, so it goes on the
            # whole mesh. Labelled with a '+' and a guessed role to keep it clearly
            # separate from a map the file actually declares.
            tex = glb_extra[extra_modes.index(self.mode)][2]
        elif glb_mats and self.mode in slots:
            # One texture per material, selected per triangle by the renderer: a
            # unit can use several materials across a single mesh.
            tex_by_mat = {i: mm.get(self.mode) for i, mm in enumerate(glb_mats)
                          if mm.get(self.mode)}
        elif self.mode in slots:
            tex = ab.load_texture(mats[self.mode])
        elif self.mode in ('lit', 'emissive'):
            tex = ab.load_texture(mats['diffuse'])
            if mats.get('specular') and self.mode == 'lit':
                spec = ab.load_texture(mats['specular'])
            if mats.get('emissive'):
                emis = ab.load_texture(mats['emissive'])

        render_mode = ('flat' if self.mode == 'flat' else
                       'normals' if self.mode == 'normals' else
                       'lit' if self.mode == 'lit' else
                       'emissive' if self.mode == 'emissive' else 'textured')

        # While the camera is moving, scatter samples over triangle interiors
        # instead of rasterising faces. The triangle path costs per face, not per
        # pixel, so turning the resolution down does not buy responsiveness --
        # swapping renderer does. Interiors rather than vertices, or the surfaces
        # vanish mid-gesture and reappear when it ends.
        # Remember the extent so entering fly mode can place the camera relative
        # to whatever is loaded rather than at a fixed spot.
        if len(V):
            self._bounds = (V.min(0), V.max(0))

        cam = None
        if self.fly:
            cam = {'pos': tuple(self.cam), 'yaw': self.yaw, 'pitch': self.pitch,
                   'fov': self.fov, 'far': self.far}

        # THE GPU PATH, when there is one. It draws the same picture as the face
        # rasteriser -- same projection constants, same shading, measured at 0.97
        # silhouette IoU -- about 50x faster, so a moving or playing frame no
        # longer has to drop to scattered samples and lose its texture and
        # surface detail. 'lit' and 'emissive' stay on numpy: they layer spec and
        # glow maps that the shader here does not reproduce, and showing a
        # different picture under the same button would be worse than being slow.
        gpu = None
        if render_mode in ('flat', 'normals', 'textured'):
            try:
                gpu = ab.render_gpu(mesh, size=PREVIEW, yaw=self.yaw,
                                    pitch=self.pitch, mode=render_mode,
                                    texture=tex, zoom=self.zoom,
                                    pan=tuple(self.pan), flip_v=self.flip_v,
                                    tex_by_mat=tex_by_mat, camera=cam)
            except Exception:
                gpu = None                       # any driver trouble: fall back

        if interactive:
            # Without a GPU both cameras use the splat path while moving. It is
            # numpy end to end, where the face rasteriser is a Python loop - the
            # difference on an assembled world is about 0.05 s a frame against
            # 0.30 s, at the cost of the detail.
            self._show(gpu if gpu is not None else
                       ab.render_points(mesh, size=PREVIEW, yaw=self.yaw,
                                        pitch=self.pitch, zoom=self.zoom,
                                        pan=tuple(self.pan), texture=tex,
                                        budget=300000, camera=cam))
            return

        # render_points has no perspective path, so flying renders faces even
        # while moving. Half resolution and a triangle cap keep that responsive;
        # the deferred full redraw restores detail once the keys stop.
        # The rasteriser is a Python loop over faces at roughly 60 us each, so
        # the triangle count IS the frame time: 172,000 of them is ten seconds
        # of frozen window. Cap it. render_mesh subsamples evenly, so a capped
        # frame still reads as the whole scene rather than a corner of it.
        size = PREVIEW // 2 if (interactive and self.fly) else PREVIEW
        cap = 15000 if interactive else 60000
        if gpu is not None:
            img = gpu
        else:
            img = ab.render_mesh(mesh, size=size, yaw=self.yaw, pitch=self.pitch,
                                 mode=render_mode, texture=tex, spec_texture=spec,
                                 emis_texture=emis, zoom=self.zoom,
                                 pan=tuple(self.pan), flip_v=self.flip_v,
                                 tex_by_mat=tex_by_mat, camera=cam, max_tris=cap)
            if size != PREVIEW:
                img = img.resize((PREVIEW, PREVIEW))
        self._show(img)

        for m in modes:
            b = self._button(self.modebar, m, lambda mm=m: self._set_mode(mm),
                             active=(m == self.mode))
            b.pack(side='left', padx=3)
        if slots:
            self._button(self.modebar, 'flipV', self._toggle_flip,
                         active=self.flip_v).pack(side='left', padx=(14, 3))
        self._button(self.modebar, 'fly', self._toggle_fly,
                     active=self.fly).pack(side='left', padx=(14, 3))

        # Name the map set on the mesh itself, so "what does this asset have"
        # is answerable without opening the material row.
        maps = self.current[ab.MAT] if self.current else ''
        # Which renderer drew this. Not decoration: the two look different while
        # moving, and "why is it grainy when I drag" should be answerable from
        # the window rather than from the source.
        ok, why = ab.gpu_status()
        note = '\nmaps: %s   renderer: %s' % (maps or '-',
                                              why if ok else 'numpy (%s)' % why)
        if self.mode == 'textured' and tex is None:
            # Siege ships no names and no material links, so nothing can be paired.
            note += ('\nno material link for this library -- textured mode needs '
                     'materials.csv (XCOM only)')
        if self.fly:
            # Say when the frame is subsampled. A capped render looks like the
            # whole scene, so without this it is impossible to tell a thinned
            # frame from a scene that is genuinely missing geometry.
            shown = ('  showing %s of %s tris' % (format(60000, ','), format(len(F), ','))
                     if len(F) > 60000 else '')
            nav = (f'FLY  pos {self.cam[0]:.0f}, {self.cam[1]:.0f}, {self.cam[2]:.0f}   '
                   f'yaw {self.yaw:.0f}deg  pitch {self.pitch:.0f}deg  '
                   f'step {self.speed:.1f}m  cull {self.far:.0f}m{shown}\n'
                   f'- drag look | WASD move | Q/E down/up | shift faster | '
                   f'wheel step size | [ ] cull | F orbit | R reset')
        else:
            nav = (f'yaw {self.yaw:.0f}deg  pitch {self.pitch:.0f}deg  zoom {self.zoom:.2f}x   '
                   f'- drag orbit | right/shift-drag pan | wheel zoom | '
                   f'double-click or R reset | F fly')
        self.info.config(text=f'{Path(path).name}\n{len(V):,} verts   {len(F):,} tris   '
                              f'uv={"yes" if len(T) > 1 else "no"}\n{nav}{note}')

        if mats:
            tk.Label(self.matbar, text='material:', bg=BG, fg='#8a8a96').pack(side='left')
            for slot in ('diffuse', 'normal', 'mask', 'specular'):
                p = mats.get(slot)
                if not p:
                    continue
                th = ab.load_texture(p).convert('RGB')
                th.thumbnail((96, 96))
                ph = ImageTk.PhotoImage(th)
                self._thumbs.append(ph)
                b = tk.Button(self.matbar, image=ph, text=slot, compound='top',
                              bg='#2a2a34', fg=FG, relief='flat',
                              command=lambda pp=p: self._open_texture(pp))
                b.pack(side='left', padx=4)

    def _open_texture(self, path):
        self.current = ('xcom', 'texture', ab._role_of('xcom', Path(path).name),
                        '', Path(path).name, path)
        self.channel = 'rgb'
        self.draw()

    def _draw_texture(self, path):
        im = ab.load_texture(path)
        view = ab.channel_view(im, self.channel).convert('RGB')
        fit = view.copy()
        fit.thumbnail((PREVIEW, PREVIEW), Image.NEAREST)
        self._show(fit)

        chans = ['rgb'] + list(im.getbands())
        if ab.is_two_channel_normal(im):
            chans.append('z')          # rebuild the third component; see core module
        for c in chans:
            b = self._button(self.modebar, c.upper(), lambda cc=c: self._set_channel(cc),
                             active=(c.lower() == self.channel.lower()))
            b.pack(side='left', padx=3)

        # Say what the channels are believed to hold. These maps pack unrelated
        # data, and a viewer that shows the composite without saying so invites
        # the reader to interpret a false colour as the surface's colour.
        lib = self.current[ab.LIB] if self.current else ''
        role = self.current[ab.ROLE] if self.current else ''
        packing = ab.CHANNEL_NOTES.get((lib, role), '')
        self.info.config(text=f'{Path(path).name}\n{ab.texture_info(im, path)}   '
                              f'channels: {", ".join(im.getbands())}'
                              + (f'\n{packing}' if packing else ''))

    def _toggle_flip(self):
        self.flip_v = not self.flip_v
        self.draw()

    def _set_mode(self, m):
        self.mode = m
        self.draw()

    def _set_channel(self, c):
        self.channel = c
        self.draw()

    # -- orbit camera ---------------------------------------------------
    def _is_mesh(self):
        return bool(self.current) and self.current[ab.KIND] in ab.MESH_KINDS

    def reset_view(self):
        self.yaw, self.pitch, self.zoom = 35.0, 20.0, 1.0
        self.pan = [0.0, 0.0]
        if self.fly:
            self._place_camera()
        if self._is_mesh():
            self.draw()

    # ------------------------------------------------------------------ fly
    def _place_camera(self):
        """Drop the camera outside the mesh, looking at it.

        Derived from the mesh's own bounds so the same key speed suits a 4 km
        world and a single vehicle: a fixed start position would put the camera
        inside one and a mile from the other.
        """
        if not self._bounds:
            self.cam, self.speed = [0.0, 50.0, -200.0], 20.0
            return
        lo, hi = self._bounds
        centre = [(lo[i] + hi[i]) / 2.0 for i in range(3)]
        extent = max(hi[i] - lo[i] for i in range(3)) or 1.0
        self.yaw, self.pitch = 0.0, 15.0
        self.cam = [centre[0], centre[1] + extent * 0.25, centre[2] - extent * 0.7]
        self.speed = max(0.5, extent / 60.0)

    def _set_far(self, factor):
        """Widen or narrow the cull distance. Measured on an assembled world:
        no cull 5.8 s a frame, 1200 m 5.1 s, 400 m 3.0 s. It is a real lever,
        just not enough on its own - hence the triangle cap as well."""
        if not self.fly or not self._is_mesh():
            return
        self.far = max(50.0, min(20000.0, self.far * factor))
        self.draw()

    def _toggle_fly(self):
        if str(self.root.focus_get()) == str(self.q):
            return                        # typing in the search box
        self.fly = not self.fly
        if self.fly:
            self._place_camera()
        if self._is_mesh():
            self.draw()

    def _basis(self):
        """(forward, right, up) in world space for the current yaw and pitch.

        These are the rows of the same rotation matrix render_mesh builds, so
        the camera moves exactly along the axes it is looking down. Deriving
        them separately is how a fly camera ends up strafing at an angle.
        """
        ry, rx = math.radians(self.yaw), math.radians(self.pitch)
        cy, sy = math.cos(ry), math.sin(ry)
        cx, sx = math.cos(rx), math.sin(rx)
        return ((cx * sy, -sx, cx * cy), (cy, 0.0, -sy), (sx * sy, cx, sx * cy))

    def _move(self, vec, fast=False):
        if not self.fly or not self._is_mesh():
            return
        if str(self.root.focus_get()) == str(self.q):
            return
        fwd, right, up = self._basis()
        step = self.speed * (5.0 if fast else 1.0)
        for i in range(3):
            self.cam[i] += (right[i] * vec[0] + (0, 1, 0)[i] * vec[1]
                            + fwd[i] * vec[2]) * step
        self._draw_mesh(self.current[ab.PATH], interactive=True)
        self._after_zoom()                # settle to a sharp frame when keys stop

    def _press(self, e, what):
        self._drag = (what, e.x, e.y, self.yaw, self.pitch, self.pan[0], self.pan[1])

    def _motion(self, e):
        if not self._drag or not self._is_mesh():
            return
        what, x0, y0, yaw0, pitch0, px0, py0 = self._drag
        if what == 'orbit':
            self.yaw = yaw0 + (e.x - x0) * 0.5
            # Clamped just short of the poles: at exactly +-90 the rotation basis
            # degenerates and the model flips.
            self.pitch = max(-89.0, min(89.0, pitch0 + (e.y - y0) * 0.5))
        else:
            # Pan is kept in zoom-1 pixels (see render_mesh), so divide the mouse
            # delta by zoom to keep dragging 1:1 with the cursor at any zoom.
            self.pan = [px0 + (e.x - x0) / self.zoom, py0 + (e.y - y0) / self.zoom]
        self._draw_mesh(self.current[ab.PATH], interactive=True)

    def _release(self, _e):
        if self._drag and self._is_mesh():
            self.draw()
        self._drag = None

    def _wheel(self, e, direction=None):
        if not self._is_mesh():
            return
        if direction is None:
            direction = 1 if getattr(e, 'delta', 0) > 0 else -1
        if self.fly:
            # Flying has no zoom - the wheel sets how far a key press moves,
            # which is what you actually need when the same scene holds a 4 km
            # map and a doorway.
            self.speed = max(0.05, min(500.0,
                                       self.speed * (1.3 if direction > 0 else 1 / 1.3)))
            self._draw_mesh(self.current[ab.PATH], interactive=True)
            self._after_zoom()
            return
        self.zoom = max(0.15, min(12.0, self.zoom * (1.15 if direction > 0 else 1 / 1.15)))
        self._draw_mesh(self.current[ab.PATH], interactive=True)
        self._after_zoom()

    def _after_zoom(self):
        # Coalesce: redraw sharp once the wheel stops, rather than at full
        # resolution for every notch of a fast scroll.
        if getattr(self, '_zoom_job', None):
            self.root.after_cancel(self._zoom_job)
        self._zoom_job = self.root.after(180, self.draw)


def run_gui(rows):
    root = tk.Tk()
    try:
        style = ttk.Style()
        style.theme_use('clam')
    except Exception:
        pass
    Browser(root, rows)
    root.mainloop()
