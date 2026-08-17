// BaPreview - render a clip from inside Unity, as ground truth.
//
// Every other picture in this pipeline is produced by something downstream of
// Unity: the FBX exporter, Blender, the glTF loader, the browser's own
// rasteriser. When an animation looks wrong there is no way to tell which of
// those did it without a reference drawn by the engine that owns the clip.
//
// This poses the real prefab with the real Animator and AnimationMode - the
// same sampling BaBake uses - and renders it to PNG with a plain camera. It is
// deliberately ugly: one directional light, no post, orthographic-ish framing
// on the bounds. The point is what the SKELETON does, not how it looks.
//
//   Unity.exe -projectPath <proj> -executeMethod BaPreview.Shoot \
//             -baRig BALT_LatvianReserve -baClip Stand_run \
//             -baOut D:/ba_extracted/debug/unity -baFrames 6
//
// NOTE: do NOT pass -nographics. Rendering needs a device.
using System;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;
using UnityEngine.Animations;
using UnityEngine.Playables;

public static class BaPreview
{
    static string Arg(string name, string fallback)
    {
        var a = Environment.GetCommandLineArgs();
        for (int i = 0; i < a.Length - 1; i++)
            if (a[i] == name) return a[i + 1];
        return fallback;
    }

    /// Per-bone rotation range over a clip, for each sampling method, printed
    /// side by side. Exists because judging two thumbnail strips by eye said
    /// "they agree" when in fact one of them leaves half the skeleton frozen -
    /// alternating bones down each limb with zero rotation, position AND scale.
    public static void Ranges()
    {
        string rigName = Arg("-baRig", "");
        string clipName = Arg("-baClip", "");
        int frames = int.Parse(Arg("-baFrames", "24"));

        string path = AssetDatabase.FindAssets("t:Prefab")
            .Select(AssetDatabase.GUIDToAssetPath)
            .FirstOrDefault(p => Path.GetFileNameWithoutExtension(p) == rigName);
        var asset = AssetDatabase.LoadAssetAtPath<GameObject>(path);
        var inst = (GameObject)UnityEngine.Object.Instantiate(asset);
        var animator = inst.GetComponentInChildren<Animator>(true);
        var clip = animator.runtimeAnimatorController.animationClips
            .FirstOrDefault(c => c != null && c.name == clipName);
        if (clip == null) { Debug.Log("BARANGE-FAIL no clip"); return; }

        // Apply the real Avatar here too. Without it this measures the broken
        // one and reports ten moving bones no matter what is being compared.
        string avD = Arg("-baAvatarData", "");
        if (!string.IsNullOrEmpty(avD) && animator.avatar != null && animator.avatar.isHuman)
        {
            var real = BuildAvatarFromData(animator.gameObject, animator.avatar.name, avD);
            if (real != null && real.isValid) animator.avatar = real;
        }

        var bones = inst.GetComponentsInChildren<Transform>(true);
        var q = new Quaternion[2][];
        var first = new Quaternion[2][];
        var maxRot = new float[2][];
        for (int m = 0; m < 2; m++) { maxRot[m] = new float[bones.Length]; }

        for (int mode = 0; mode < 2; mode++)
        {
            PlayableGraph graph = default;
            AnimationClipPlayable pl = default;
            if (mode == 0)
            {
                graph = PlayableGraph.Create("r");
                graph.SetTimeUpdateMode(DirectorUpdateMode.Manual);
                var o = AnimationPlayableOutput.Create(graph, "o", animator);
                pl = AnimationClipPlayable.Create(graph, clip);
                pl.SetApplyFootIK(false);
                o.SetSourcePlayable(pl);
            }
            Quaternion[] baseRot = null;
            for (int f = 0; f < frames; f++)
            {
                float t = clip.length * f / Mathf.Max(1, frames - 1);
                if (mode == 0) { pl.SetTime(t); pl.SetTime(t); graph.Evaluate(0f); }
                else
                {
                    AnimationMode.StartAnimationMode();
                    AnimationMode.BeginSampling();
                    AnimationMode.SampleAnimationClip(animator.gameObject, clip, t);
                    AnimationMode.EndSampling();
                }
                if (f == 0)
                {
                    baseRot = bones.Select(b => b.localRotation).ToArray();
                }
                for (int b = 0; b < bones.Length; b++)
                {
                    float ang = Quaternion.Angle(baseRot[b], bones[b].localRotation);
                    if (ang > maxRot[mode][b]) maxRot[mode][b] = ang;
                }
                if (mode == 1) AnimationMode.StopAnimationMode();
            }
            if (mode == 0 && graph.IsValid()) graph.Destroy();
        }

        Debug.Log($"BARANGE {rigName}/{clipName}  bone  playable  animationmode");
        for (int b = 0; b < bones.Length; b++)
            if (maxRot[0][b] > 0.5f || maxRot[1][b] > 0.5f)
                Debug.Log($"BARANGE-BONE\t{bones[b].name}\t{maxRot[0][b]:F1}\t{maxRot[1][b]:F1}");
        Debug.Log($"BARANGE-DONE moving(playable)={maxRot[0].Count(v => v > 0.5f)} " +
                  $"moving(animationmode)={maxRot[1].Count(v => v > 0.5f)} of {bones.Length}");
    }

    /// Is the Avatar actually there, and does it map the bones? A humanoid clip
    /// is muscle values; without a valid human Avatar there is nothing to
    /// retarget them ONTO, and Unity drives only whatever it can resolve -
    /// which comes out as a few bones moving and the rest frozen.
    public static void Avatars()
    {
        foreach (var guid in AssetDatabase.FindAssets("t:Prefab"))
        {
            var path = AssetDatabase.GUIDToAssetPath(guid);
            var go = AssetDatabase.LoadAssetAtPath<GameObject>(path);
            if (go == null) continue;
            var a = go.GetComponentInChildren<Animator>(true);
            if (a == null || a.runtimeAnimatorController == null) continue;
            var clips = a.runtimeAnimatorController.animationClips.Where(c => c != null).ToArray();
            if (clips.Length == 0) continue;
            int human = clips.Count(c => c.isHumanMotion);
            var av = a.avatar;
            Debug.Log($"BAAVATAR\t{Path.GetFileNameWithoutExtension(path)}" +
                      $"\tavatar={(av ? av.name : "NULL")}" +
                      $"\tvalid={(av && av.isValid)}\thuman={(av && av.isHuman)}" +
                      $"\thumanClips={human}/{clips.Length}");
        }
        Debug.Log("BAAVATAR-DONE");
    }

    /// How many of Unity's 54 human bones does this Animator actually resolve?
    /// A humanoid clip can only drive bones the Avatar maps; if the Avatar was
    /// reconstructed without a faithful bone mapping, the rest stay frozen and
    /// the result is a run cycle with one hip and one knee moving.
    public static void HumanBones()
    {
        string rigName = Arg("-baRig", "");
        foreach (var guid in AssetDatabase.FindAssets("t:Prefab"))
        {
            var path = AssetDatabase.GUIDToAssetPath(guid);
            var nm = Path.GetFileNameWithoutExtension(path);
            if (!string.IsNullOrEmpty(rigName) && nm != rigName) continue;
            var go = AssetDatabase.LoadAssetAtPath<GameObject>(path);
            if (go == null) continue;
            var a = go.GetComponentInChildren<Animator>(true);
            if (a == null || a.avatar == null || !a.avatar.isHuman) continue;
            var inst = (GameObject)UnityEngine.Object.Instantiate(go);
            var an = inst.GetComponentInChildren<Animator>(true);
            int mapped = 0, total = 0;
            var missing = new System.Collections.Generic.List<string>();
            foreach (HumanBodyBones hb in Enum.GetValues(typeof(HumanBodyBones)))
            {
                if (hb == HumanBodyBones.LastBone) continue;
                total++;
                Transform t = null;
                try { t = an.GetBoneTransform(hb); } catch { }
                if (t != null) mapped++;
                else if (missing.Count < 12) missing.Add(hb.ToString());
            }
            Debug.Log($"BAHUMAN\t{nm}\tmapped={mapped}/{total}\tmissing={string.Join(",", missing)}");
            UnityEngine.Object.DestroyImmediate(inst);
            if (!string.IsNullOrEmpty(rigName)) break;
        }
        Debug.Log("BAHUMAN-DONE");
    }

    // Unity's human bones against the names this game's rigs actually use.
    // Standard 3ds Max / Mixamo biped naming, which is why a correct mapping
    // can be rebuilt at all.
    static readonly (HumanBodyBones, string)[] HUMAN_MAP =
    {
        (HumanBodyBones.Hips, "Hips"),
        (HumanBodyBones.Spine, "Spine"),
        (HumanBodyBones.Chest, "Spine1"),
        (HumanBodyBones.UpperChest, "Spine2"),
        (HumanBodyBones.Neck, "Neck"),
        (HumanBodyBones.Head, "Head"),
        (HumanBodyBones.LeftShoulder, "LeftShoulder"),
        (HumanBodyBones.LeftUpperArm, "LeftArm"),
        (HumanBodyBones.LeftLowerArm, "LeftForeArm"),
        (HumanBodyBones.LeftHand, "LeftHand"),
        (HumanBodyBones.RightShoulder, "RightShoulder"),
        (HumanBodyBones.RightUpperArm, "RightArm"),
        (HumanBodyBones.RightLowerArm, "RightForeArm"),
        (HumanBodyBones.RightHand, "RightHand"),
        (HumanBodyBones.LeftUpperLeg, "LeftUpLeg"),
        (HumanBodyBones.LeftLowerLeg, "LeftLeg"),
        (HumanBodyBones.LeftFoot, "LeftFoot"),
        (HumanBodyBones.LeftToes, "LeftToeBase"),
        (HumanBodyBones.RightUpperLeg, "RightUpLeg"),
        (HumanBodyBones.RightLowerLeg, "RightLeg"),
        (HumanBodyBones.RightFoot, "RightFoot"),
        (HumanBodyBones.RightToes, "RightToeBase"),
    };

    /// Build a correct human Avatar for a rig from its own bone names.
    ///
    /// THE FIX FOR THE FROZEN HALF-BODY. The Avatar AssetRipper reconstructs -
    /// one `AvatarAvatar` shared by all 221 humanoid rigs - resolves only 22 of
    /// Unity's 55 human bones. A humanoid clip is muscle values and can drive
    /// nothing it cannot map, so Hips, Spine, Head, both shoulders, one hip,
    /// one knee and one foot never move, and a run cycle comes out with half
    /// the skeleton frozen. Rebuilding the mapping from the rig's own bone
    /// names restores it.
    /// Build an Avatar from the game's OWN avatar data, extracted by
    /// tools/ba/ba_avatars.py straight out of the bundles.
    ///
    /// Two things come from there and neither can be inferred: the human bone
    /// mapping, and the reference pose the muscle values were authored against.
    /// Rebuilding from bone names alone gets the mapping right and the
    /// reference wrong - Unity then takes the rig's current pose as the T-pose,
    /// and since these rigs are not in a T-pose every clip retargets onto a bad
    /// reference and plays crouched.
    public static Avatar BuildAvatarFromData(GameObject root, string avatarName, string dataFile)
    {
        if (!File.Exists(dataFile)) { Debug.Log($"BAAVATAR-DATA missing {dataFile}"); return null; }

        var human = new System.Collections.Generic.Dictionary<string, string>();
        var pose = new System.Collections.Generic.Dictionary<string, float[]>();
        var param = new System.Collections.Generic.Dictionary<string, float>();
        bool inOurs = false;
        foreach (var line in File.ReadLines(dataFile))
        {
            var p = line.Split('\t');
            if (p[0] == "AVATAR") { inOurs = p.Length > 1 && p[1] == avatarName; continue; }
            if (!inOurs) continue;
            if (p[0] == "HUMAN" && p.Length >= 3) human[p[1]] = p[2];
            else if (p[0] == "PARAM" && p.Length >= 3 &&
                     float.TryParse(p[2], System.Globalization.NumberStyles.Float,
                                    System.Globalization.CultureInfo.InvariantCulture, out var pv))
                param[p[1]] = pv;
            else if (p[0] == "POSE" && p.Length >= 12)
            {
                var v = new float[10];
                for (int i = 0; i < 10; i++)
                    float.TryParse(p[2 + i], System.Globalization.NumberStyles.Float,
                                   System.Globalization.CultureInfo.InvariantCulture, out v[i]);
                pose[p[1]] = v;
            }
        }
        if (human.Count == 0) { Debug.Log($"BAAVATAR-DATA no entry for {avatarName}"); return null; }

        // Unity's own spelling of each human bone, matched case/space-insensitively.
        var boneNames = HumanTrait.BoneName;
        string Canon(string s) => s.Replace(" ", "").ToLowerInvariant();
        var byCanon = boneNames.ToDictionary(b => Canon(b), b => b);

        var bones = new System.Collections.Generic.List<HumanBone>();
        var transforms = root.GetComponentsInChildren<Transform>(true);
        var have = new System.Collections.Generic.HashSet<string>(transforms.Select(t => t.name));
        foreach (var kv in human)
        {
            if (!byCanon.TryGetValue(Canon(kv.Key), out var unityName)) continue;
            if (!have.Contains(kv.Value)) continue;
            bones.Add(new HumanBone
            {
                humanName = unityName,
                boneName = kv.Value,
                limit = new HumanLimit { useDefaultValues = true },
            });
        }

        // The skeleton array carries the REFERENCE pose. Where the game's avatar
        // gives one for a bone, use it; otherwise fall back to the rig's own
        // local transform so the array stays complete.
        var skel = transforms.Select(t =>
        {
            var sb = new SkeletonBone
            {
                name = t.name,
                position = t.localPosition,
                rotation = t.localRotation,
                scale = t.localScale,
            };
            if (pose.TryGetValue(t.name, out var v))
            {
                sb.position = new Vector3(v[0], v[1], v[2]);
                sb.rotation = new Quaternion(v[3], v[4], v[5], v[6]);
                sb.scale = new Vector3(v[7], v[8], v[9]);
            }
            return sb;
        }).ToArray();

        float P(string k, float dflt) => param.TryGetValue(k, out var v) ? v : dflt;
        var desc = new HumanDescription
        {
            human = bones.ToArray(),
            skeleton = skel,
            upperArmTwist = P("armTwist", 0.5f),
            lowerArmTwist = P("foreArmTwist", 0.5f),
            upperLegTwist = P("upperLegTwist", 0.5f),
            lowerLegTwist = P("legTwist", 0.5f),
            armStretch = P("armStretch", 0.05f),
            legStretch = P("legStretch", 0.05f),
            feetSpacing = P("feetSpacing", 0f),
        };
        var av = AvatarBuilder.BuildHumanAvatar(root, desc);
        Debug.Log($"BAAVATAR-DATA {avatarName}: {bones.Count} human bones, " +
                  $"{pose.Count} pose nodes -> valid={av && av.isValid} human={av && av.isHuman}");
        return av;
    }

    public static Avatar BuildAvatar(GameObject root)
    {
        var byName = root.GetComponentsInChildren<Transform>(true)
            .GroupBy(t => t.name).ToDictionary(g => g.Key, g => g.First());
        var bones = new System.Collections.Generic.List<HumanBone>();
        foreach (var (hb, name) in HUMAN_MAP)
        {
            if (!byName.ContainsKey(name)) continue;
            bones.Add(new HumanBone
            {
                humanName = HumanTrait.BoneName[(int)hb],
                boneName = name,
                limit = new HumanLimit { useDefaultValues = true },
            });
        }
        var skeleton = root.GetComponentsInChildren<Transform>(true)
            .Select(t => new SkeletonBone
            {
                name = t.name,
                position = t.localPosition,
                rotation = t.localRotation,
                scale = t.localScale,
            }).ToArray();
        var desc = new HumanDescription
        {
            human = bones.ToArray(),
            skeleton = skeleton,
            upperArmTwist = 0.5f, lowerArmTwist = 0.5f,
            upperLegTwist = 0.5f, lowerLegTwist = 0.5f,
            armStretch = 0.05f, legStretch = 0.05f, feetSpacing = 0f,
        };
        return AvatarBuilder.BuildHumanAvatar(root, desc);
    }

    /// Rebuild the Avatar for one rig and report how much more of it moves.
    public static void TestAvatar()
    {
        string rigName = Arg("-baRig", "");
        string clipName = Arg("-baClip", "Stand_run");
        var path = AssetDatabase.FindAssets("t:Prefab").Select(AssetDatabase.GUIDToAssetPath)
            .FirstOrDefault(p => Path.GetFileNameWithoutExtension(p) == rigName);
        var go = AssetDatabase.LoadAssetAtPath<GameObject>(path);
        var inst = (GameObject)UnityEngine.Object.Instantiate(go);
        var an = inst.GetComponentInChildren<Animator>(true);
        var clip = an.runtimeAnimatorController.animationClips.First(c => c != null && c.name == clipName);

        int Mapped(Animator a)
        {
            int m = 0;
            foreach (HumanBodyBones hb in Enum.GetValues(typeof(HumanBodyBones)))
                if (hb != HumanBodyBones.LastBone && a.GetBoneTransform(hb) != null) m++;
            return m;
        }
        int before = Mapped(an);

        var avatar = BuildAvatar(an.gameObject);
        Debug.Log($"BATEST built avatar valid={avatar.isValid} human={avatar.isHuman}");
        an.avatar = avatar;
        int after = Mapped(an);

        // How many bones move now?
        var bones = inst.GetComponentsInChildren<Transform>(true);
        var baseRot = new Quaternion[bones.Length];
        var maxRot = new float[bones.Length];
        var graph = PlayableGraph.Create("t");
        graph.SetTimeUpdateMode(DirectorUpdateMode.Manual);
        var o = AnimationPlayableOutput.Create(graph, "o", an);
        var pl = AnimationClipPlayable.Create(graph, clip);
        pl.SetApplyFootIK(false);
        o.SetSourcePlayable(pl);
        for (int f = 0; f < 24; f++)
        {
            float t = clip.length * f / 23f;
            pl.SetTime(t); pl.SetTime(t); graph.Evaluate(0f);
            if (f == 0) for (int b = 0; b < bones.Length; b++) baseRot[b] = bones[b].localRotation;
            for (int b = 0; b < bones.Length; b++)
                maxRot[b] = Mathf.Max(maxRot[b], Quaternion.Angle(baseRot[b], bones[b].localRotation));
        }
        graph.Destroy();
        Debug.Log($"BATEST {rigName}/{clipName} mappedBones {before} -> {after} of 55; " +
                  $"movingBones now {maxRot.Count(v => v > 0.5f)} of {bones.Length}");
        foreach (var b in new[] { "Hips", "Spine", "Head", "LeftUpLeg", "RightUpLeg", "LeftLeg", "RightLeg", "LeftArm", "RightArm" })
        {
            int i = Array.FindIndex(bones, x => x.name == b);
            if (i >= 0) Debug.Log($"BATEST-BONE\t{b}\t{maxRot[i]:F1}");
        }
    }

    /// Does the retargeted pose ALREADY carry the body's position and
    /// orientation, or does root motion have to be added on top?
    ///
    /// BaExport writes the clip's RootT/RootQ curves onto the Animator's own
    /// transform. If the pose already places the hips - which is what humanoid
    /// retargeting does - that is a second application of the same motion, and
    /// a death animation plays as a character standing up and flipping over.
    /// This prints the Animator transform and the Hips world transform per
    /// frame so the two can be told apart.
    public static void RootCheck()
    {
        string rigName = Arg("-baRig", "");
        string clipName = Arg("-baClip", "Stand_death");
        string avData = Arg("-baAvatarData", "");
        var path = AssetDatabase.FindAssets("t:Prefab").Select(AssetDatabase.GUIDToAssetPath)
            .FirstOrDefault(p => Path.GetFileNameWithoutExtension(p) == rigName);
        var go = AssetDatabase.LoadAssetAtPath<GameObject>(path);
        var inst = (GameObject)UnityEngine.Object.Instantiate(go);
        var an = inst.GetComponentInChildren<Animator>(true);
        if (!string.IsNullOrEmpty(avData) && an.avatar != null && an.avatar.isHuman)
        {
            var real = BuildAvatarFromData(an.gameObject, an.avatar.name, avData);
            if (real != null && real.isValid) an.avatar = real;
        }
        var clip = an.runtimeAnimatorController.animationClips.First(c => c != null && c.name == clipName);
        var hips = an.GetBoneTransform(HumanBodyBones.Hips);

        var graph = PlayableGraph.Create("rc");
        graph.SetTimeUpdateMode(DirectorUpdateMode.Manual);
        var o = AnimationPlayableOutput.Create(graph, "o", an);
        var pl = AnimationClipPlayable.Create(graph, clip);
        pl.SetApplyFootIK(false);
        o.SetSourcePlayable(pl);
        for (int f = 0; f < 6; f++)
        {
            float t = clip.length * f / 5f;
            pl.SetTime(t); pl.SetTime(t); graph.Evaluate(0f);
            var ap = an.transform.position;
            var hp = hips ? hips.position : Vector3.zero;
            var hr = hips ? hips.rotation.eulerAngles : Vector3.zero;
            Debug.Log($"BAROOT\t{t:F2}\tanimator=({ap.x:F2},{ap.y:F2},{ap.z:F2})" +
                      $"\thipsWorld=({hp.x:F2},{hp.y:F2},{hp.z:F2})" +
                      $"\thipsEuler=({hr.x:F0},{hr.y:F0},{hr.z:F0})");
        }
        graph.Destroy();
        Debug.Log("BAROOT-DONE");
    }

    /// List every skinned mesh on a rig with the skeleton it hangs from, so a
    /// vehicle's crew can be told apart from its bodywork.
    public static void Crew()
    {
        string rigName = Arg("-baRig", "");
        var path = AssetDatabase.FindAssets("t:Prefab").Select(AssetDatabase.GUIDToAssetPath)
            .FirstOrDefault(p => Path.GetFileNameWithoutExtension(p) == rigName);
        if (path == null) { Debug.Log($"BACREW-FAIL no prefab {rigName}"); return; }
        var go = AssetDatabase.LoadAssetAtPath<GameObject>(path);
        var an = go.GetComponentInChildren<Animator>(true);
        Debug.Log($"BACREW {rigName}  prefab={path}");
        foreach (var s in go.GetComponentsInChildren<SkinnedMeshRenderer>(true))
        {
            var mesh = s.sharedMesh;
            Debug.Log($"BACREW-SMR\t{s.name}\tmesh={(mesh ? mesh.name : "null")}" +
                      $"\tverts={(mesh ? mesh.vertexCount : 0)}\tbones={s.bones.Length}" +
                      $"\trootBone={(s.rootBone ? s.rootBone.name : "-")}" +
                      $"\tmat={(s.sharedMaterial ? s.sharedMaterial.name : "-")}");
        }
        foreach (var a in go.GetComponentsInChildren<Animator>(true))
            Debug.Log($"BACREW-ANIM\t{a.gameObject.name}\tavatar={(a.avatar ? a.avatar.name : "null")}" +
                      $"\thuman={(a.avatar && a.avatar.isHuman)}" +
                      $"\tcontroller={(a.runtimeAnimatorController ? a.runtimeAnimatorController.name : "-")}");
        Debug.Log("BACREW-DONE");
    }

    public static void Shoot()
    {
        string rigName = Arg("-baRig", "");
        string clipName = Arg("-baClip", "");
        string outDir = Arg("-baOut", "D:/ba_extracted/debug/unity");
        int frames = int.Parse(Arg("-baFrames", "6"));
        int size = int.Parse(Arg("-baSize", "220"));
        Directory.CreateDirectory(outDir);

        string path = AssetDatabase.FindAssets("t:Prefab")
            .Select(AssetDatabase.GUIDToAssetPath)
            .FirstOrDefault(p => Path.GetFileNameWithoutExtension(p) == rigName);
        if (path == null) { Debug.Log($"BAPREVIEW-FAIL no prefab {rigName}"); return; }

        var asset = AssetDatabase.LoadAssetAtPath<GameObject>(path);
        var inst = (GameObject)UnityEngine.Object.Instantiate(asset);
        var animator = inst.GetComponentInChildren<Animator>(true);
        var clip = animator.runtimeAnimatorController.animationClips
            .FirstOrDefault(c => c != null && c.name == clipName);
        if (clip == null)
        {
            Debug.Log($"BAPREVIEW-FAIL {rigName} has no clip {clipName}; has " +
                      string.Join(",", animator.runtimeAnimatorController.animationClips
                          .Where(c => c != null).Select(c => c.name).Take(30)));
            return;
        }

        var camGo = new GameObject("BaCam");
        var cam = camGo.AddComponent<Camera>();
        cam.clearFlags = CameraClearFlags.SolidColor;
        cam.backgroundColor = Color.black;
        cam.orthographic = true;
        var lightGo = new GameObject("BaLight");
        var light = lightGo.AddComponent<Light>();
        light.type = LightType.Directional;
        light.intensity = 1.2f;
        lightGo.transform.rotation = Quaternion.Euler(40f, 150f, 0f);

        var rt = new RenderTexture(size, size, 24);
        cam.targetTexture = rt;

        // TWO WAYS TO POSE A HUMANOID CLIP, AND THEY DO NOT AGREE.
        //
        // AnimationMode.SampleAnimationClip is the editor's preview path and is
        // what BaBake and BaExport used. On these clips it puts feet above the
        // waist - the shape you get when muscle values are written onto bones
        // without the Avatar retarget in between.
        //
        // A PlayableGraph driving the Animator is the RUNTIME path: the same
        // code the game runs, so the Avatar is applied by construction. Pass
        // -baSampleMode mode to render either and compare.
        bool usePlayable = Arg("-baSampleMode", "playable") != "animationmode";
        string avData = Arg("-baAvatarData", "");
        if (!string.IsNullOrEmpty(avData))
        {
            var real = BuildAvatarFromData(animator.gameObject,
                                           animator.avatar ? animator.avatar.name : "", avData);
            if (real != null && real.isValid) animator.avatar = real;
        }
        else if (Environment.GetCommandLineArgs().Contains("-baRebuildAvatar"))
        {
            var rebuilt = BuildAvatar(animator.gameObject);
            if (rebuilt != null && rebuilt.isValid) animator.avatar = rebuilt;
            Debug.Log($"BAPREVIEW rebuilt avatar valid={rebuilt && rebuilt.isValid}");
        }
        PlayableGraph graph = default;
        AnimationClipPlayable playable = default;
        if (usePlayable)
        {
            graph = PlayableGraph.Create("BaPreview");
            graph.SetTimeUpdateMode(DirectorUpdateMode.Manual);
            var output = AnimationPlayableOutput.Create(graph, "out", animator);
            playable = AnimationClipPlayable.Create(graph, clip);
            playable.SetApplyFootIK(false);
            output.SetSourcePlayable(playable);
        }

        for (int f = 0; f < frames; f++)
        {
            float t = clip.length * f / Mathf.Max(1, frames - 1);
            if (usePlayable)
            {
                playable.SetTime(t);
                playable.SetTime(t);   // twice: the first call only seeds prev-time
                graph.Evaluate(0f);
            }
            else
            {
                AnimationMode.StartAnimationMode();
                AnimationMode.BeginSampling();
                AnimationMode.SampleAnimationClip(animator.gameObject, clip, t);
                AnimationMode.EndSampling();
            }

            // Frame the skinned bounds each shot: root motion moves the subject.
            var rends = inst.GetComponentsInChildren<Renderer>();
            var b = new Bounds(inst.transform.position, Vector3.one);
            bool first = true;
            foreach (var r in rends)
            {
                if (!(r is SkinnedMeshRenderer) && !(r is MeshRenderer)) continue;
                if (first) { b = r.bounds; first = false; }
                else b.Encapsulate(r.bounds);
            }
            float ext = Mathf.Max(b.size.x, b.size.y, b.size.z) * 0.6f + 0.2f;
            cam.orthographicSize = ext;
            camGo.transform.position = b.center + new Vector3(ext * 2f, ext * 0.6f, ext * 2f);
            camGo.transform.LookAt(b.center);

            cam.Render();
            RenderTexture.active = rt;
            var tex = new Texture2D(size, size, TextureFormat.RGB24, false);
            tex.ReadPixels(new Rect(0, 0, size, size), 0, 0);
            tex.Apply();
            RenderTexture.active = null;
            string tag = usePlayable ? "playable" : "animmode";
            File.WriteAllBytes(Path.Combine(outDir, $"{rigName}_{clipName}_{tag}_{f:D2}.png"),
                               tex.EncodeToPNG());
            if (!usePlayable) AnimationMode.StopAnimationMode();
        }
        if (usePlayable && graph.IsValid()) graph.Destroy();
        Debug.Log($"BAPREVIEW-DONE {rigName}/{clipName} {frames} frames " +
                  $"({(usePlayable ? "playable" : "animationmode")}) -> {outDir}");
    }
}
