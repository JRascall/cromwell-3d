// BaExport - write Broken Arrow's rigs out as FBX with their animation on them.
//
// BaBake.cs solved the hard half: Unity's humanoid retargeting turns muscle
// curves into bone transforms. This turns that into a file the rest of the
// world can open.
//
// WHY THE EXPORT RUNS THROUGH UNITY AND NOT BLENDER. The baked tracks are Unity
// local transforms: left-handed, Y up, and expressed against Unity's own bone
// rest poses. Blender is right-handed and Z up, and its FBX importer rewrites
// bone rest orientations on the way in - so applying these numbers to an
// imported rig means composing an axis conversion with a per-bone rest-basis
// change, by hand, and being wrong about it in a way that looks *almost* right.
// Unity already knows how to write an FBX. Let it.
//
// WHY THE CLIPS ARE REBUILT AS LEGACY. The FBX exporter takes its takes from an
// Animation or Animator component. The original clips cannot be used - they are
// the humanoid muscle clips this whole exercise exists to get away from, and
// handing them straight to the exporter would write muscle curves into the FBX.
// So each clip is re-created as an ordinary generic clip of transform curves
// from the sampled poses. Legacy clips on an Animation component are used
// because they can be built in memory: an AnimatorController would mean writing
// thousands of .anim and .controller assets to disk for files nobody keeps.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Formats.Fbx.Exporter;
using UnityEngine;

public static class BaExport
{
    static string Arg(string name, string fallback)
    {
        var args = Environment.GetCommandLineArgs();
        for (int i = 0; i < args.Length - 1; i++)
            if (args[i] == name) return args[i + 1];
        return fallback;
    }

    // The ten channels an FBX take needs per bone, in the order Unity names
    // them. Scale is included because a few of the vehicle rigs animate it.
    static readonly string[] Props =
    {
        "m_LocalPosition.x", "m_LocalPosition.y", "m_LocalPosition.z",
        "m_LocalRotation.x", "m_LocalRotation.y", "m_LocalRotation.z", "m_LocalRotation.w",
        "m_LocalScale.x", "m_LocalScale.y", "m_LocalScale.z",
    };

    // Emit the name->prefab mapping and export nothing. Exists because the
    // naming scheme changed once (plain prefab name, then disambiguated by
    // path hash), which left files on disk under names the current scheme
    // assigns to a *different* rig. Nothing about that is visible from a
    // directory listing, so the mapping has to come from the same code that
    // would do the exporting, and be diffed against what is there.
    public static void Manifest()
    {
        Run(manifestOnly: true);
    }

    public static void Export()
    {
        Run(manifestOnly: false);
    }

    /// Report how each rig's skinned meshes relate to the object being
    /// exported. Written after two wrong guesses at why Blender refuses 54 of
    /// these files: the point is to find out where the bones actually live
    /// rather than to keep proposing causes.
    public static void Diagnose()
    {
        var listFile = Arg("-baList", "");
        HashSet<string> only = null;
        if (File.Exists(listFile))
            only = new HashSet<string>(File.ReadAllLines(listFile).Select(l => l.Trim())
                .Where(l => l.Length > 0), StringComparer.OrdinalIgnoreCase);

        foreach (var guid in AssetDatabase.FindAssets("t:Prefab"))
        {
            var path = AssetDatabase.GUIDToAssetPath(guid);
            var name = Path.GetFileNameWithoutExtension(path);
            if (only != null && !only.Contains(name)) continue;
            var go = AssetDatabase.LoadAssetAtPath<GameObject>(path);
            if (go == null) continue;
            var animator = go.GetComponentInChildren<Animator>(true);
            if (animator == null || animator.runtimeAnimatorController == null) continue;

            var root = animator.transform;
            var smrs = go.GetComponentsInChildren<SkinnedMeshRenderer>(true);
            int outside = 0, nullRoot = 0;
            var rootBones = new HashSet<string>();
            foreach (var s in smrs)
            {
                if (s.rootBone == null) { nullRoot++; continue; }
                rootBones.Add(s.rootBone.name);
                // Is every bone this mesh is skinned to inside the subtree we
                // export? If not, the FBX names a skeleton it does not contain.
                foreach (var b in s.bones)
                {
                    if (b == null) continue;
                    if (!b.IsChildOf(root)) { outside++; break; }
                }
            }
            // Does any single mesh skin across TWO skeletons? That is the shape
            // io_scene_fbx cannot represent: it builds one armature per
            // skeleton root and then looks the mesh up under a single one.
            int spanning = 0;
            foreach (var s in smrs)
            {
                var tops = new HashSet<Transform>();
                foreach (var b in s.bones)
                {
                    if (b == null) continue;
                    var t = b;
                    while (t.parent != null && t.parent != root) t = t.parent;
                    tops.Add(t);
                }
                if (tops.Count > 1) spanning++;
            }
            foreach (var s in smrs)
            {
                var lp = root.InverseTransformPoint(s.transform.position);
                var rb = s.rootBone != null ? root.InverseTransformPoint(s.rootBone.position)
                                            : Vector3.zero;
                if (lp.magnitude > 20f || rb.magnitude > 20f)
                    Debug.Log($"BADIAG-FAR {name}\t{s.name}\tmat={(s.sharedMaterial ? s.sharedMaterial.name : "-")}" +
                              $"\tobj=({lp.x:F1},{lp.y:F1},{lp.z:F1})\trootBone={(s.rootBone ? s.rootBone.name : "-")}" +
                              $"=({rb.x:F1},{rb.y:F1},{rb.z:F1})");
            }
            Debug.Log($"BADIAG {name}\tspanningMeshes={spanning}\tanimatorOn={root.name}\tprefabRoot={go.name}" +
                      $"\tanimatorIsRoot={(root == go.transform)}\tsmr={smrs.Length}" +
                      $"\tsmrWithBonesOutside={outside}\tnullRootBone={nullRoot}" +
                      $"\trootBones={string.Join("|", rootBones)}");
        }
        Debug.Log("BADIAG-DONE");
    }

    static void Run(bool manifestOnly)
    {
        var filter = Arg("-baFilter", "");
        var outDir = Arg("-baOut", "D:/ba_extracted/fbx");
        var listFile = Arg("-baList", "");
        // Strip crew rigs nested inside a vehicle rig. Off by default because
        // it removes geometry; on for the rigs Blender cannot otherwise read.
        bool stripNested = Environment.GetCommandLineArgs().Contains("-baStripNested");
        Directory.CreateDirectory(outDir);

        // An explicit list, because the rigs that need this are 54 specific
        // ones and -baFilter is a single substring. Names are prefab
        // basenames, one per line.
        HashSet<string> only = null;
        if (!string.IsNullOrEmpty(listFile) && File.Exists(listFile))
        {
            only = new HashSet<string>(File.ReadAllLines(listFile)
                .Select(l => l.Trim()).Where(l => l.Length > 0),
                StringComparer.OrdinalIgnoreCase);
            Debug.Log($"BAEXPORT-LIST {only.Count} names from {listFile}");
        }

        // RIGS ONLY, and the filtering has to happen BEFORE names are assigned.
        // There are 1,410 prefabs and 422 rigs; if a non-rig prefab sharing a
        // name is allowed into the grouping below, it takes the plain name and
        // pushes an actual rig onto a hashed one. Nothing would ever write a
        // file for the non-rig, so the plain name would simply never appear.
        var prefabs = new List<string>();
        foreach (var guid in AssetDatabase.FindAssets("t:Prefab"))
        {
            var path = AssetDatabase.GUIDToAssetPath(guid);
            if (!string.IsNullOrEmpty(filter) && path.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0)
                continue;
            var go = AssetDatabase.LoadAssetAtPath<GameObject>(path);
            if (go == null) continue;
            var a = go.GetComponentInChildren<Animator>(true);
            if (a == null || a.runtimeAnimatorController == null) continue;
            if (!a.runtimeAnimatorController.animationClips.Any(c => c != null)) continue;
            if (only != null && !only.Contains(Path.GetFileNameWithoutExtension(path))) continue;
            prefabs.Add(path);
        }

        // SIX PREFABS IN THIS GAME SHARE A BASENAME - US_MARINE_rifle,
        // US_LAV_M, Morskaya and friends are each two different rigs. Naming
        // the output after the prefab alone means the second one is skipped as
        // "already exported", and the loss is invisible: you get 420 files for
        // 422 rigs and nothing anywhere says which two went missing.
        //
        // So names are assigned up front over the whole candidate set: within a
        // group sharing a name, the first by asset path keeps the plain name
        // and the rest get an 8-hex suffix from their path. Deterministic, so
        // a re-run assigns exactly the same names and the resume-by-existence
        // check stays honest.
        var names = new Dictionary<string, string>();
        foreach (var group in prefabs.GroupBy(p => Sanitise(Path.GetFileNameWithoutExtension(p))))
        {
            var ordered = group.OrderBy(p => p, StringComparer.Ordinal).ToList();
            for (int i = 0; i < ordered.Count; i++)
                names[ordered[i]] = i == 0
                    ? group.Key
                    : $"{group.Key}__{PathHash(ordered[i])}";
            if (ordered.Count > 1)
                Debug.Log($"BAEXPORT-DUPNAME {group.Key} x{ordered.Count}");
        }

        if (manifestOnly)
        {
            foreach (var kv in names.OrderBy(k => k.Value, StringComparer.Ordinal))
                Debug.Log($"BAEXPORT-MAP\t{kv.Value}\t{kv.Key}");
            Debug.Log($"BAEXPORT-MAPDONE count={names.Count}");
            return;
        }

        Debug.Log($"BAEXPORT-START candidates={prefabs.Count} out={outDir}");
        int written = 0, skipped = 0, failed = 0, takes = 0;

        foreach (var path in prefabs)
        {
            var asset = AssetDatabase.LoadAssetAtPath<GameObject>(path);
            if (asset == null) { skipped++; continue; }
            var srcAnimator = asset.GetComponentInChildren<Animator>(true);
            if (srcAnimator == null || srcAnimator.runtimeAnimatorController == null) { skipped++; continue; }
            var clips = srcAnimator.runtimeAnimatorController.animationClips
                .Where(c => c != null).Distinct().ToArray();
            if (clips.Length == 0) { skipped++; continue; }

            var file = Path.Combine(outDir, names[path] + Arg("-baNameSuffix", "") + ".fbx");
            if (File.Exists(file)) { skipped++; continue; }   // resumable

            GameObject inst = null;
            try
            {
                inst = (GameObject)UnityEngine.Object.Instantiate(asset);
                var animator = inst.GetComponentInChildren<Animator>(true);

                // NESTED CREW RIGS ARE WHAT BREAK BLENDER. An An-72 prefab
                // contains RU_Pilot_Plane; the Mi-8s, the Su-24s and the
                // RU_Morskaya family all carry a second Animator with its own
                // skeleton inside the first. The FBX then holds two armatures,
                // and io_scene_fbx dies on it with `KeyError: root` at
                // mesh.armature_setup[self] - every import option, including
                // use_anim=False, so there is no setting that rescues it.
                //
                // FBX2glTF reads those files, which is why they existed at all,
                // but it wrote RU_Morskaya with FOUR joints against a ~50-bone
                // rig: most vertices bind to joints absent from the skin and
                // the mesh scrambles the moment it is posed. A correctly
                // skinned airframe without its pilot beats a complete file
                // that cannot deform - and the crew rigs are exported in their
                // own right anyway, since they are prefabs with Animators too.
                // ONE SKELETON PER FILE. These prefabs carry an airframe rig and
                // a crew rig side by side - `root`/`body` plus `Hips` - and that
                // plurality is what Blender's FBX importer cannot represent
                // (`KeyError: root`). FBX2glTF reads such files but writes the
                // crew mesh into a broken bind space: the pilot of a MiG-35
                // lands 145 m from the aircraft with a 185 m span, against an
                // 18 m airframe, and no amount of fixing the viewer helps
                // because the geometry in the file is already wrong.
                //
                // So the crew comes out. Skinned meshes are grouped by the
                // top-level bone they hang from and only the largest group
                // survives; the rest, and their skeletons, are deleted. Nothing
                // is lost from the library - the crew are prefabs with their own
                // Animators and export as rigs in their own right.
                if (Environment.GetCommandLineArgs().Contains("-baOneSkeleton"))
                {
                    // GROUPED BY rootBone ITSELF, not by its top-level ancestor.
                    // A pilot is parented INTO the cockpit, so his `Hips` hangs
                    // under the airframe's `body` bone and walking up to the
                    // top merges the two rigs back into one group - which is
                    // why the first attempt at this dropped nothing at all.
                    var smrs = inst.GetComponentsInChildren<SkinnedMeshRenderer>(true);
                    var groups = smrs.Where(s => s.rootBone != null)
                        .GroupBy(s => s.rootBone)
                        .OrderByDescending(g => g.Sum(s => s.sharedMesh ? s.sharedMesh.vertexCount : 0))
                        .ToList();
                    // Deleting a bone deletes its whole subtree, so a later
                    // group's root - or the Animator - can already be gone by
                    // the time the loop reaches it. Unity reports that as
                    // "the object has been destroyed but you are still trying
                    // to access it", which killed 27 of 134 rigs on the first
                    // run. Hence the guards: skip anything already dead, and
                    // never delete a bone that the kept rig or the Animator
                    // lives under.
                    // WHICH skeleton to keep. 0 is the largest - the vehicle
                    // itself. Higher indices are the crew: a HMMWV gunner is a
                    // 40-bone humanoid with three LODs rooted at `Hips`, using
                    // the same shared US_Army material as the standalone
                    // infantry. Running the exporter once per index, with
                    // -baNameSuffix to keep the outputs apart, gets the vehicle
                    // AND its crew as separate single-skeleton files - which is
                    // what Blender wants regardless, and loses nothing.
                    int keepIndex = int.TryParse(Arg("-baSkeletonIndex", "0"), out var ki) ? ki : 0;
                    if (keepIndex >= groups.Count)
                    {
                        Debug.Log($"BAEXPORT-NOSKEL {asset.name}: only {groups.Count} skeletons");
                        skipped++;
                        continue;
                    }
                    if (keepIndex > 0)
                    {
                        var swap = groups[0];
                        groups[0] = groups[keepIndex];
                        groups[keepIndex] = swap;
                    }
                    var keepRoot = groups.Count > 0 ? groups[0].Key : null;
                    foreach (var g in groups.Skip(1))
                    {
                        var bone = g.Key;
                        int verts = g.Sum(s => s != null && s.sharedMesh ? s.sharedMesh.vertexCount : 0);
                        Debug.Log($"BAEXPORT-DROPSKEL {asset.name}: {(bone ? bone.name : "?")} " +
                                  $"({g.Count()} meshes, {verts} verts)");

                        // THE MESHES ALWAYS GO. Only the BONES need guarding.
                        // A vehicle's crew hangs off the vehicle's own skeleton
                        // - a HMMWV gunner's `Hips` is a descendant of the
                        // chassis `root` - so deleting the chassis bones would
                        // take the gunner with them. An earlier version guarded
                        // the whole group and skipped the meshes too, which for
                        // a crew-only export left the entire vehicle in the
                        // file: 59,938 verts of HMMWV under a "crew" name.
                        foreach (var s in g)
                            if (s != null && s.gameObject != null)
                                UnityEngine.Object.DestroyImmediate(s.gameObject);

                        if (bone == null) continue;
                        if (animator != null && animator.transform.IsChildOf(bone)) continue;
                        if (keepRoot != null && keepRoot.IsChildOf(bone)) continue;
                        if (bone.gameObject != null)
                            UnityEngine.Object.DestroyImmediate(bone.gameObject);
                    }
                }

                if (stripNested)
                {
                    var nested = inst.GetComponentsInChildren<Animator>(true)
                        .Where(a => a != animator).ToArray();
                    foreach (var n in nested)
                    {
                        if (n == null || n.gameObject == null) continue;
                        Debug.Log($"BAEXPORT-STRIP {asset.name}: {n.gameObject.name}");
                        UnityEngine.Object.DestroyImmediate(n.gameObject);
                    }
                }

                // THE REAL AVATAR, OR EVERY HUMAN CLIP IS WRONG.
                // Humanoid clips are muscle values; they retarget onto whatever
                // the Avatar maps and freeze everything it does not. The Avatar
                // AssetRipper reconstructs resolves 22 of 55 human bones, so
                // hips, spine, head, both shoulders, one hip, one knee and one
                // foot never move - which is why every human animation in this
                // library was wrong while the vehicles, which use generic clips
                // and no Avatar at all, were fine.
                //
                // tools/ba/ba_avatars.py pulls the game's own Avatar objects out
                // of the bundles: the bone mapping AND the reference pose the
                // muscles were authored against. Rebuilding from bone names
                // alone gets the mapping and misses the reference, and Unity
                // then treats the rig's current pose as the T-pose.
                string avatarData = Arg("-baAvatarData", "");
                if (!string.IsNullOrEmpty(avatarData) && animator.avatar != null
                    && animator.avatar.isHuman)
                {
                    var real = BaPreview.BuildAvatarFromData(
                        animator.gameObject, animator.avatar.name, avatarData);
                    if (real != null && real.isValid)
                        animator.avatar = real;
                    else
                        Debug.LogWarning($"BAEXPORT-AVATAR {asset.name}: could not rebuild " +
                                         $"{animator.avatar.name}, keeping the original");
                }

                var root = animator.transform;

                var bones = new List<Transform>();
                var paths = new List<string>();
                bones.Add(root); paths.Add("");
                Collect(root, root, bones, paths);

                var built = new List<AnimationClip>();
                foreach (var clip in clips)
                {
                    float fps = Mathf.Max(clip.frameRate, 30f);
                    int frames = Mathf.Max(1, Mathf.RoundToInt(clip.length * fps) + 1);

                    var keys = new Keyframe[bones.Count][][];
                    for (int b = 0; b < bones.Count; b++)
                    {
                        keys[b] = new Keyframe[Props.Length][];
                        for (int c = 0; c < Props.Length; c++) keys[b][c] = new Keyframe[frames];
                    }

                    AnimationMode.StartAnimationMode();
                    for (int f = 0; f < frames; f++)
                    {
                        float t = Mathf.Min(f / fps, clip.length);
                        AnimationMode.BeginSampling();
                        AnimationMode.SampleAnimationClip(animator.gameObject, clip, t);
                        AnimationMode.EndSampling();

                        for (int b = 0; b < bones.Count; b++)
                        {
                            var tr = bones[b];
                            Vector3 p = tr.localPosition; Quaternion q = tr.localRotation; Vector3 s = tr.localScale;
                            // NO ROOT-CURVE OVERWRITE ON BONE 0.
                            //
                            // An earlier version wrote the clip's RootT/RootQ
                            // onto the Animator's transform here, reasoning that
                            // sampling never applies root motion and that stance
                            // height lives in RootT.y - 0.17 prone against 0.88
                            // standing. That was reasoning about the clip data
                            // rather than about what retargeting produces, and
                            // it was wrong.
                            //
                            // Measured on Stand_death with the real Avatar: the
                            // Animator transform stays at (0,0,0) for the entire
                            // clip while the HIPS fall from y=0.99 to y=0.25,
                            // travel a metre and rotate 88 degrees onto the
                            // character's back. The retargeted pose already
                            // carries the whole body motion, so adding the root
                            // curves applied every bit of it twice - a soldier
                            // who dies standing up and then flips over. Correct
                            // pose, corrupted placement.
                            var v = new float[] { p.x, p.y, p.z, q.x, q.y, q.z, q.w, s.x, s.y, s.z };
                            for (int c = 0; c < Props.Length; c++) keys[b][c][f] = new Keyframe(t, v[c]);
                        }
                    }
                    AnimationMode.StopAnimationMode();

                    var made = new AnimationClip { frameRate = fps, legacy = true, name = clip.name };
                    for (int b = 0; b < bones.Count; b++)
                        for (int c = 0; c < Props.Length; c++)
                        {
                            var curve = new AnimationCurve(keys[b][c]);
                            for (int k = 0; k < curve.length; k++)
                                AnimationUtility.SetKeyLeftTangentMode(curve, k, AnimationUtility.TangentMode.ClampedAuto);
                            made.SetCurve(paths[b], typeof(Transform), Props[c], curve);
                        }
                    built.Add(made);
                    takes++;
                }

                // The Animator has to go: leaving it on means the exporter sees
                // both it and the Animation component and prefers the humanoid
                // controller, which is exactly what must not be written.
                var legacy = animator.gameObject;   // grab it BEFORE the destroy
                UnityEngine.Object.DestroyImmediate(animator);
                var animation = legacy.AddComponent<Animation>();
                foreach (var c in built) animation.AddClip(c, c.name);
                if (built.Count > 0) animation.clip = built[0];

                // BOTH OF THESE DEFAULTS ARE WRONG FOR THIS JOB, and neither
                // announces itself:
                //   ExportFormat defaults to ASCII, which Blender refuses
                //     outright - "ASCII FBX files are not supported". At least
                //     that one fails loudly.
                //   AnimateSkinnedMesh defaults to FALSE, which does not fail
                //     at all: you get a valid FBX with the mesh, the skeleton
                //     and the takes, and no animation on the skinned mesh. That
                //     is the same silent-empty-output failure as every other
                //     stage of this pipeline, one layer further out.
                // EXPORT THE PREFAB ROOT WHEN THE ANIMATOR IS NOT ON IT.
                // The US Marines put their Animator on a child called
                // `SupportBones`. The skeleton is under it, but the fourteen
                // SkinnedMeshRenderers are siblings of it - so exporting the
                // Animator's subtree wrote 10 MB of animation curves and not
                // one triangle. Nine rigs came out with a working clip list and
                // no geometry at all, which in the browser is an invisible
                // model rather than an error.
                //
                // The Animation component stays on the Animator's object, so
                // the clip paths built above are still relative to the right
                // thing; only the exported hierarchy widens.
                var exportRoot = legacy == inst ? legacy : inst;
                ModelExporter.ExportObject(file, exportRoot, new ExportModelOptions
                {
                    ExportFormat = ExportFormat.Binary,
                    ModelAnimIncludeOption = Include.ModelAndAnim,
                    ObjectPosition = ObjectPosition.LocalCentered,
                    LODExportType = LODExportType.All,
                    AnimateSkinnedMesh = true,
                });
                written++;
                if (written % 20 == 0)
                    Debug.Log($"BAEXPORT-PROGRESS written={written} takes={takes}");
            }
            catch (Exception e)
            {
                failed++;
                Debug.LogWarning($"BAEXPORT-FAIL {path}: {e.Message}");
            }
            finally
            {
                if (inst != null) UnityEngine.Object.DestroyImmediate(inst);
            }
        }
        Debug.Log($"BAEXPORT-DONE written={written} takes={takes} skipped={skipped} failed={failed}");
    }

    static AnimationCurve[] RootCurves(AnimationClip clip)
    {
        var result = new AnimationCurve[7];
        string[] props = { "RootT.x", "RootT.y", "RootT.z", "RootQ.x", "RootQ.y", "RootQ.z", "RootQ.w" };
        foreach (var binding in AnimationUtility.GetCurveBindings(clip))
        {
            int idx = Array.IndexOf(props, binding.propertyName);
            if (idx >= 0 && string.IsNullOrEmpty(binding.path))
                result[idx] = AnimationUtility.GetEditorCurve(clip, binding);
        }
        return result;
    }

    static void Collect(Transform root, Transform t, List<Transform> bones, List<string> paths)
    {
        foreach (Transform child in t)
        {
            bones.Add(child);
            paths.Add(AnimationUtility.CalculateTransformPath(child, root));
            Collect(root, child, bones, paths);
        }
    }

    // FNV-1a over the asset path. Not for security - just a short, stable tag
    // that distinguishes two rigs which happen to share a name.
    static string PathHash(string s)
    {
        uint h = 2166136261;
        foreach (char c in s) { h ^= c; h *= 16777619; }
        return h.ToString("x8");
    }

    static string Sanitise(string s)
    {
        foreach (var c in Path.GetInvalidFileNameChars()) s = s.Replace(c, '_');
        return s;
    }
}
