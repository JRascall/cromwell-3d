// BaBake - turn Broken Arrow's humanoid Mecanim clips into bone animation.
//
// WHY THIS EXISTS. AssetRipper decodes the clips correctly, but 83 of the 472
// are Unity *humanoid* clips: their curves are muscle values (Chest Front-Back,
// Left Arm Down-Up) plus root and IK goals, with an empty binding path. Muscle
// values are not bone rotations - turning them into bone transforms is Unity's
// retargeting, which needs the Avatar's T-pose and per-muscle limits. Rather
// than reimplement that, this runs it in the engine that owns it: sample the
// clip onto the real rig, read the bone transforms back out, and write them as
// an ordinary generic AnimationClip that anything can read.
//
// Generic clips are baked too, so the output is uniform and one downstream
// path handles everything.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

public static class BaBake
{
    static string Arg(string name, string fallback)
    {
        var args = Environment.GetCommandLineArgs();
        for (int i = 0; i < args.Length - 1; i++)
            if (args[i] == name) return args[i + 1];
        return fallback;
    }

    struct Rig
    {
        public GameObject Prefab;
        public string Path;
        public Animator Animator;
        public AnimationClip[] Clips;
    }

    static List<Rig> FindRigs(string filter)
    {
        var rigs = new List<Rig>();
        foreach (var guid in AssetDatabase.FindAssets("t:Prefab"))
        {
            var path = AssetDatabase.GUIDToAssetPath(guid);
            if (!string.IsNullOrEmpty(filter) && path.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0)
                continue;
            var go = AssetDatabase.LoadAssetAtPath<GameObject>(path);
            if (go == null) continue;
            var animator = go.GetComponentInChildren<Animator>(true);
            if (animator == null || animator.runtimeAnimatorController == null) continue;
            var clips = animator.runtimeAnimatorController.animationClips
                .Where(c => c != null).Distinct().ToArray();
            if (clips.Length == 0) continue;
            rigs.Add(new Rig { Prefab = go, Path = path, Animator = animator, Clips = clips });
        }
        return rigs;
    }

    public static void Report()
    {
        var rigs = FindRigs(Arg("-baFilter", ""));
        int human = 0, generic = 0, humanClips = 0, genericClips = 0;
        foreach (var r in rigs)
        {
            bool isHuman = r.Animator.avatar != null && r.Animator.avatar.isHuman;
            if (isHuman) human++; else generic++;
            foreach (var c in r.Clips)
            {
                if (c.isHumanMotion) humanClips++; else genericClips++;
            }
        }
        Debug.Log($"BABAKE-REPORT rigs={rigs.Count} humanoidRigs={human} genericRigs={generic} " +
                  $"humanClipRefs={humanClips} genericClipRefs={genericClips}");
        foreach (var r in rigs.Take(15))
        {
            var av = r.Animator.avatar;
            Debug.Log($"BABAKE-RIG {Path.GetFileNameWithoutExtension(r.Path)} " +
                      $"avatar={(av ? av.name : "none")} human={(av && av.isHuman)} clips={r.Clips.Length} " +
                      $"[{string.Join(",", r.Clips.Take(6).Select(c => c.name))}]");
        }
    }

    public static void Bake()
    {
        var filter = Arg("-baFilter", "");
        var outDir = Arg("-baOut", "D:/ba_extracted/anim");
        Directory.CreateDirectory(outDir);

        var rigs = FindRigs(filter);
        Debug.Log($"BABAKE-START rigs={rigs.Count} out={outDir}");

        int done = 0, clipsDone = 0, failed = 0;
        foreach (var rig in rigs)
        {
            GameObject inst = null;
            try
            {
                inst = (GameObject)UnityEngine.Object.Instantiate(rig.Prefab);
                inst.name = rig.Prefab.name;
                var animator = inst.GetComponentInChildren<Animator>(true);
                if (animator == null) { failed++; continue; }

                // THE ROOT IS THE ANIMATOR'S OWN GAMEOBJECT, NOT THE PREFAB ROOT.
                // A clip's bindings are relative to the object the Animator is
                // on, and these prefabs wrap the rig in an outer transform - so
                // sampling the prefab root silently animates nothing at all. It
                // does not error: you get a full set of correctly named bones
                // holding their rest pose, which reads as "the clip is empty"
                // rather than "you sampled the wrong object".
                var root = animator.transform;
                var bones = new List<Transform>();
                var paths = new List<string>();
                // THE ANIMATOR'S OWN TRANSFORM IS BONE 0, PATH "". Humanoid
                // retargeting puts root motion - RootT/RootQ, the whole
                // translation of a run cycle - on the Animator object, not on
                // Hips. Recording only its children loses it, and the loss is
                // quiet: the pose is right, the limbs swing, and the character
                // runs on the spot.
                bones.Add(root);
                paths.Add("");
                Collect(root, root, bones, paths);

                var rigOut = Path.Combine(outDir, Sanitise(rig.Prefab.name));
                Directory.CreateDirectory(rigOut);

                foreach (var clip in rig.Clips)
                {
                    // 60 Hz unless the clip says otherwise. Sampling below the
                    // authored rate is the one way to silently lose motion here,
                    // so this never samples sparser than the clip's own rate.
                    float fps = Mathf.Max(clip.frameRate, 30f);
                    int frames = Mathf.Max(1, Mathf.RoundToInt(clip.length * fps) + 1);

                    // ROOT MOTION HAS TO BE READ OFF THE CLIP, NOT SAMPLED.
                    // AnimationMode.SampleAnimationClip poses the skeleton but
                    // never writes RootT/RootQ to a transform - root motion is a
                    // per-frame delta the Animator applies at play time, and in
                    // the editor nothing applies it. Dropping it is not merely
                    // "runs on the spot": for these clips the *stance height*
                    // lives in RootT.y (0.17 prone against 0.88 standing), so a
                    // prone soldier would come out standing-height and floating.
                    var rootCurves = new AnimationCurve[7];
                    string[] rootProps = { "RootT.x", "RootT.y", "RootT.z",
                                           "RootQ.x", "RootQ.y", "RootQ.z", "RootQ.w" };
                    foreach (var binding in AnimationUtility.GetCurveBindings(clip))
                    {
                        int idx = Array.IndexOf(rootProps, binding.propertyName);
                        if (idx >= 0 && string.IsNullOrEmpty(binding.path))
                            rootCurves[idx] = AnimationUtility.GetEditorCurve(clip, binding);
                    }

                    var track = new float[bones.Count * frames * 10];
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
                            int o = (f * bones.Count + b) * 10;
                            var p = tr.localPosition; var q = tr.localRotation; var s = tr.localScale;
                            track[o + 0] = p.x; track[o + 1] = p.y; track[o + 2] = p.z;
                            track[o + 3] = q.x; track[o + 4] = q.y; track[o + 5] = q.z; track[o + 6] = q.w;
                            track[o + 7] = s.x; track[o + 8] = s.y; track[o + 9] = s.z;
                        }

                        // Overwrite bone 0 - the Animator's own transform - with
                        // the root curves where the clip has them. A generic
                        // clip has none and keeps whatever sampling produced.
                        if (rootCurves[0] != null || rootCurves[3] != null)
                        {
                            int o = (f * bones.Count) * 10;
                            for (int k = 0; k < 3; k++)
                                if (rootCurves[k] != null) track[o + k] = rootCurves[k].Evaluate(t);
                            for (int k = 3; k < 7; k++)
                                if (rootCurves[k] != null) track[o + k] = rootCurves[k].Evaluate(t);
                        }
                    }
                    AnimationMode.StopAnimationMode();

                    // Binary, not JSON. A 50-bone 200-frame clip is 100,000
                    // floats; as text that is megabytes per clip and minutes of
                    // parsing on the Blender side for no gain.
                    var file = Path.Combine(rigOut, Sanitise(clip.name) + ".batrk");
                    using (var w = new BinaryWriter(File.Create(file)))
                    {
                        w.Write(new char[] { 'B', 'A', 'T', 'K' });
                        w.Write(1);                       // format version
                        w.Write(clip.name);
                        w.Write(fps);
                        w.Write(frames);
                        w.Write(clip.isHumanMotion ? 1 : 0);
                        w.Write(bones.Count);
                        foreach (var p in paths) w.Write(p);
                        foreach (var v in track) w.Write(v);
                    }
                    clipsDone++;
                }
                done++;
                if (done % 25 == 0) Debug.Log($"BABAKE-PROGRESS rigs={done}/{rigs.Count} clips={clipsDone}");
            }
            catch (Exception e)
            {
                failed++;
                Debug.LogWarning($"BABAKE-FAIL {rig.Path}: {e.Message}");
            }
            finally
            {
                if (inst != null) UnityEngine.Object.DestroyImmediate(inst);
            }
        }
        Debug.Log($"BABAKE-DONE rigs={done} clips={clipsDone} failed={failed}");
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

    static string Sanitise(string s)
    {
        foreach (var c in Path.GetInvalidFileNameChars()) s = s.Replace(c, '_');
        return s;
    }
}
