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

    static void Run(bool manifestOnly)
    {
        var filter = Arg("-baFilter", "");
        var outDir = Arg("-baOut", "D:/ba_extracted/fbx");
        Directory.CreateDirectory(outDir);

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

            var file = Path.Combine(outDir, names[path] + ".fbx");
            if (File.Exists(file)) { skipped++; continue; }   // resumable

            GameObject inst = null;
            try
            {
                inst = (GameObject)UnityEngine.Object.Instantiate(asset);
                var animator = inst.GetComponentInChildren<Animator>(true);
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

                    var rootCurves = RootCurves(clip);
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
                            // Bone 0 is the Animator object; its motion is the
                            // clip's root curves, which sampling never applies.
                            if (b == 0 && (rootCurves[0] != null || rootCurves[3] != null))
                            {
                                if (rootCurves[0] != null) p.x = rootCurves[0].Evaluate(t);
                                if (rootCurves[1] != null) p.y = rootCurves[1].Evaluate(t);
                                if (rootCurves[2] != null) p.z = rootCurves[2].Evaluate(t);
                                if (rootCurves[3] != null) q.x = rootCurves[3].Evaluate(t);
                                if (rootCurves[4] != null) q.y = rootCurves[4].Evaluate(t);
                                if (rootCurves[5] != null) q.z = rootCurves[5].Evaluate(t);
                                if (rootCurves[6] != null) q.w = rootCurves[6].Evaluate(t);
                            }
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
                ModelExporter.ExportObject(file, legacy, new ExportModelOptions
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
