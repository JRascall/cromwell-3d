"""Tkinter front end for asset_browser.py. Import it from there, not directly.

Split from the core so the catalogue, the rasteriser and the channel split stay
headless and testable -- the same reason xcom_parcel_render.py is a script rather
than a viewer. Everything here is presentation.

Two interaction notes that are not obvious from the code:

*The orbit camera renders twice per gesture.* The triangle rasteriser is a Python
loop over faces and costs ~60us each whatever the resolution, so it cannot be
turned down into an interactive renderer -- 1,500 triangles is already only 11
fps. While a button is held the preview scatters samples over triangle interiors
instead (vectorised, ~19 fps at 87% of the full render's coverage) and the shaded
raster returns on release. Sampling interiors rather than vertices matters: the
first version splatted vertices only, so surfaces visibly disappeared mid-drag.
Zoom coalesces the same way -- the wheel redraws coarse immediately and schedules
one sharp redraw 180 ms later.

*Channel buttons are the point of the texture view.* These engines pack unrelated
data per channel, so RGB is often meaningless -- see the module docstring in
asset_browser.py.
"""
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
        self._drag = None
        self._photo = None
        self._mesh_cache = {}
        self._glb_cache = {}      # path -> embedded glTF materials, decoded once
        self._thumbs = []

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

        self.modebar = tk.Frame(right, bg=BG)
        self.modebar.pack(fill='x', pady=6)

        self.info = tk.Label(right, text='', bg=BG, fg='#8a8a96', anchor='w',
                             justify='left', wraplength=PREVIEW)
        self.info.pack(fill='x')

        self.matbar = tk.Frame(right, bg=BG)
        self.matbar.pack(fill='x', pady=6)

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
        if self.current[ab.KIND] == 'mesh':
            # 'embedded' is Helldivers, whose maps live inside the .glb. Ask for
            # diffuse either way; _draw_mesh falls back to flat if the asset has
            # no albedo, so this cannot land on an empty mode.
            mat = self.current[ab.MAT]
            self.mode = 'diffuse' if ('dif' in mat or mat == 'embedded') else 'flat'
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
            if kind == 'mesh':
                self._draw_mesh(path)
            else:
                self._draw_texture(path)
        except Exception as e:
            self.canvas.config(image='')
            self.info.config(text=f'{name}\n\ncould not open: {type(e).__name__}: {e}')

    def _show(self, img):
        self._photo = ImageTk.PhotoImage(img)
        self.canvas.config(image=self._photo)

    def _draw_mesh(self, path, interactive=False):
        mesh = self._mesh_cache.get(path)
        if mesh is None:
            mesh = ab.load_mesh(path)
            self._mesh_cache[path] = mesh
        V, T, N, F = mesh[0], mesh[1], mesh[2], mesh[3]

        # Helldivers keeps its textures inside the .glb, so they come from the file
        # itself rather than from a side-table. Cached per path: decoding several
        # 2048px PNGs on every redraw would make the orbit unusable.
        glb_mats = None
        glb_extra = []
        if str(path).lower().endswith('.glb'):
            if path not in self._glb_cache:
                try:
                    self._glb_cache[path] = ab.glb_materials(path)
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
        if interactive:
            self._show(ab.render_points(mesh, size=PREVIEW, yaw=self.yaw,
                                        pitch=self.pitch, zoom=self.zoom,
                                        pan=tuple(self.pan), texture=tex,
                                        budget=300000))
            return

        img = ab.render_mesh(mesh, size=PREVIEW, yaw=self.yaw, pitch=self.pitch,
                             mode=render_mode, texture=tex, spec_texture=spec,
                             emis_texture=emis, zoom=self.zoom, pan=tuple(self.pan),
                             flip_v=self.flip_v, tex_by_mat=tex_by_mat)
        self._show(img)

        for m in modes:
            b = self._button(self.modebar, m, lambda mm=m: self._set_mode(mm),
                             active=(m == self.mode))
            b.pack(side='left', padx=3)
        if slots:
            self._button(self.modebar, 'flipV', self._toggle_flip,
                         active=self.flip_v).pack(side='left', padx=(14, 3))

        # Name the map set on the mesh itself, so "what does this asset have"
        # is answerable without opening the material row.
        maps = self.current[ab.MAT] if self.current else ''
        note = '\nmaps: ' + (maps or '-')
        if self.mode == 'textured' and tex is None:
            # Siege ships no names and no material links, so nothing can be paired.
            note += ('\nno material link for this library -- textured mode needs '
                     'materials.csv (XCOM only)')
        self.info.config(text=f'{Path(path).name}\n{len(V):,} verts   {len(F):,} tris   '
                              f'uv={"yes" if len(T) > 1 else "no"}\n'
                              f'yaw {self.yaw:.0f}deg  pitch {self.pitch:.0f}deg  zoom {self.zoom:.2f}x   '
                              f'- drag orbit | right/shift-drag pan | wheel zoom | '
                              f'double-click or R reset{note}')

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
        return bool(self.current) and self.current[ab.KIND] == 'mesh'

    def reset_view(self):
        self.yaw, self.pitch, self.zoom = 35.0, 20.0, 1.0
        self.pan = [0.0, 0.0]
        if self._is_mesh():
            self.draw()

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
