#version 330
/* splash.fs.glsl — puts the still splash painting into motion.
 *
 * WHAT THIS IS FOR. assets/textures/cromwell.png is one frame of oil paint: a
 * sunset over the Thames at Westminster. It is a good image and a completely
 * dead one, and two seconds of a dead image at the top of every run reads as a
 * loading screen that has hung. This shader adds the four things the painter
 * could not: the river moves, the sun's track on it glitters, the light
 * scatters into shafts around the silhouette, and London fog thickens and
 * drifts in front of the far bank.
 *
 * IT ENHANCES, IT DOES NOT REPLACE. Every term here displaces or blends the
 * painting by a small amount; none of them draws anything on its own. Set the
 * strengths below to zero and the original image comes back exactly, which is
 * the property that keeps this from quietly becoming a different picture.
 *
 * ALL THE TUNING IS IN THIS FILE, ON PURPOSE. The C++ side (SplashPass) passes
 * only things it alone can know — the clock, the ramp, the texture's aspect —
 * and every number that decides how this LOOKS is a constant below. That is
 * what makes F5 worth having: the splash reloads this file live, so tuning is
 * edit-and-look rather than edit, rebuild, relaunch, wait. Moving one of these
 * back into C++ would quietly break that.
 *
 * COST IS NOT A CONSIDERATION and this is one of the few files where that is
 * true. It runs while the splash is up and never again — a hundred texture
 * fetches a pixel at 720p is a fraction of a millisecond, and the project's
 * performance rules explicitly exempt code that runs this rarely. Clarity wins
 * every argument here.
 */
in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;

/* Seconds since the splash appeared, not since the process started: the
 * effects ramp in from nothing over the first moment and that ramp has to
 * begin when the image does. */
uniform float uTime;

/* 0 to 1 over the first half second. The image appears as it was painted and
 * comes to life; opening mid-shimmer looks like a video that was already
 * playing before the window did. */
uniform float uRamp;

/* Texture width over height. Distances are measured in UV, where a circle is
 * an ellipse; multiplying x by this makes the glow round. It is the TEXTURE's
 * aspect and not the window's, and that is exact rather than a simplification
 * — the blit is a cover fit, so the visible source rectangle always has the
 * window's aspect, and the two ratios cancel to precisely this. */
uniform float uAspect;

/* The painted water map, and whether one was found. See sampleWater below for
 * what the channels mean, and assets/textures/README.md for how to make one.
 * Absent is an ordinary state, not an error: the shader falls back to a
 * formula, which is worse and works. */
uniform sampler2D uWaterMap;
uniform float uHasWaterMap;

/* The sky's own map: R where the sky is, G how near it is — white overhead,
 * black at the horizon, the same "white is near" convention the water uses.
 * Without one the shader guesses from height and brightness; see skyMask for
 * why that guess is much weaker here than the equivalent trick over water. */
uniform sampler2D uSkyMap;
uniform float uHasSkyMap;

/* How much UV one screen pixel spans. Only the surface normal wants it, and it
 * wants it badly: it differentiates the wave with dFdx/dFdy, which measure per
 * PIXEL, and a normal built from those without this would tilt differently at
 * every window size. */
uniform vec2 uUvPerPixel;

/* ======================================================================== */
/* THE PICTURE'S GEOMETRY. Measured off assets/textures/cromwell.png and     */
/* meaningless for any other image — replacing the splash means measuring    */
/* these again. The method is recorded in assets/textures/README.md.         */
/* ======================================================================== */

/* The sun's centre, in texture UV. Found as the brightest WARM patch with the
 * baked-in wordmark masked out — the wordmark is pure white and otherwise the
 * brightest thing in the frame by a wide margin, which is the trap anyone
 * re-measuring this will fall into first. */
const vec2 kSunUV = vec2(0.5229, 0.5611);

/* Where open water starts, and how far below that it has fully taken over.
 * The fade is generous because the near shore is not a straight line across
 * the picture — the bridge on the left reaches lower than the wharves on the
 * right — and a soft join is how one number covers both without leaving a
 * visible horizontal seam across the river. */
const float kWaterLine = 0.735;
const float kWaterFade = 0.045;

/* THE RIVER DOES NOT RUN ACROSS THE FRAME, IT RUNS DIAGONALLY. Everything
 * about water here — where it starts, which way the swell lies, where the
 * light lands — was originally measured against horizontal lines, and the
 * result was a river whose surface ran one way and whose light ran another.
 *
 * kWaterTilt is the shoreline's slope in v per unit u: positive drops the
 * water's edge towards the right of frame, and the swell's crests lie parallel
 * to that same line, so setting it turns the whole surface at once.
 *
 * kFlowShift leans the sun's track along the river instead of dropping it
 * straight down the frame — it is how far, in u, the centre of the glitter
 * track slides between the far shore and the bottom edge. Negative leans it
 * left. This one is art direction rather than optics: a mirrored sun really
 * does lie on the line between it and the eye, but a track that ignores the
 * river it is lying on reads as painted on. */
const float kWaterTilt = 0.10;
const float kFlowShift = 0.16;

/* WHICH WAY THE RIVER RUNS, on screen, pointing downstream — the direction
 * wave crests travel and glints drift.
 *
 * THIS IS A SEPARATE THING FROM THE RAMP, and conflating the two was a design
 * mistake worth recording. The first version took the wave phase from the depth
 * ramp alone, on the reasoning that one field could carry both "how far away is
 * this water" and "which way does it run". It cannot: a ramp that gets brighter
 * downwards can only ever produce waves that travel downwards. The river here
 * runs from the top left of the water across to the bottom right, and no
 * painting of the ramp expresses that while still meaning distance.
 *
 * So they are two fields with one job each. The RAMP is depth: it sets how big
 * the waves are and how dense the sparkle is. THIS is direction: crests lie
 * across it and everything travels along it.
 *
 * Measured in aspect-corrected space, so it is the angle you actually see: the
 * water runs from about (0.15, 0.76) to the bottom right corner, which over a
 * band this wide and this shallow is only about nine degrees below horizontal.
 * Rotate it here and the whole surface turns. */
const vec2 kFlowDir = vec2(1.0, 0.16);

/* How fast the glint field drifts downstream, in UV per second. The swell has
 * its own speeds; this is the sparkle, and it wants to be slower than the eye
 * expects or the river reads as a rapid. */
const float kDriftSpeed = 0.0105;

/* ---- the river's own coordinate ----------------------------------------- */

/* THE WATER MOVES IN WORLD SPACE, NOT SCREEN SPACE, and this is the difference
 * between motion that reads as a river and motion that reads as a texture
 * scrolling behind a window.
 *
 * The water band is a quarter of the frame tall and carries almost all of the
 * scene's depth: the near edge is a few yards away and the far edge is most of
 * a mile. A wave crossing that band at a constant number of pixels per second
 * is therefore travelling sixty times faster in the distance than it is at your
 * feet, and the eye reads that immediately even when it cannot say why. It is
 * the same reason distant traffic on a motorway appears to crawl.
 *
 * So position along the river is measured in a coordinate that already contains
 * the perspective, and time advances THAT. Everything then follows without
 * further arrangement: crests bunch up towards the far bank because the
 * coordinate compresses there, and they slow down towards it for exactly the
 * same reason, by exactly the same factor. Two symptoms, one cause, no second
 * constant to keep in step.
 *
 *   kDepthGain  how much of the coordinate is depth rather than sideways
 *               travel. Raise it and the river runs more towards the viewer.
 *   kDepthNear  the floor under the reciprocal. This is the horizon: without
 *               it the far edge of the band is infinitely far away, infinitely
 *               compressed and infinitely aliased. Raising it flattens the
 *               perspective and calms the far water.
 *   kSideGain   how much of it is travel across the frame, from the flow
 *               direction. Zero sends the river straight at the viewer.
 */
const float kDepthGain = 0.10;
const float kDepthNear = 0.30;
const float kSideGain  = 0.15;

/* Wavenumbers for the three trains, in that coordinate. */
const float kWave1 = 55.0;
const float kWave2 = 36.0;
const float kWave3 = 21.0;

/* ---- the mirrored scene ------------------------------------------------- */

/* THE RIVER REFLECTS THE CITY. This is the one term here that changes what the
 * water IS rather than how it moves, and it is the reason the earlier passes
 * felt like polish: displacing, smearing and sparkling the painting's own dark
 * water can only ever produce slightly livelier dark water. A river under a
 * sunset is not dark. It is the brightest thing in the frame, because it is a
 * mirror pointed at the sky.
 *
 * kHorizonV is the waterline the scene is mirrored about — the top of the water
 * mask, measured.
 *
 * kReflectSquash is perspective. A true mirror about the horizon puts the
 * reflection of a thing as far below the water as the thing is above it, but
 * the water plane recedes, so the whole reflected world is compressed towards
 * the shore. This value is chosen so that the SUN'S mirror image lands at the
 * bottom edge of the frame: that is what stretches its reflection into a road
 * of light running the full depth of the river, which is the single most
 * recognisable thing about a low sun over water.
 *
 * kReflectRipple is how much harder the reflection breaks up than the water's
 * own surface detail. Much harder, and that is not a fudge: the reflected ray
 * travels to the sky and back, so the same tilt of the same wavelet moves it
 * enormously further than it moves anything floating on the surface.
 *
 * kReflectStrength fades with depth rather than being flat. Reflection is
 * strongest at grazing angles — the far water, where the line of sight skims
 * the surface — and weakest looking almost straight down at your feet. That is
 * Fresnel, and it is also what keeps the near foreground readable. */
const float kHorizonV        = 0.756;
const float kReflectSquash   = 0.80;
const float kReflectRipple   = 2.6;
const float kReflectStrength = 0.50;

/* How much the wave fronts brighten and the backs darken, MULTIPLICATIVELY.
 * Adding light can only ever lift; multiplying digs the troughs out as well,
 * and the trough is half of what makes a wave visible. */
const float kCrestContrast = 0.40;

/* ---- the surface, as opposed to the light on it ------------------------- */

/* ---- the surface as a surface ------------------------------------------- */

/* How strongly the wave gradient tilts the normal. This is the single knob for
 * "how choppy does the surface behave" as distinct from how far it displaces:
 * raise it and facets swing further, so the reflection breaks up harder and the
 * highlights get more scattered, without the image moving any more than it did.
 */
const float kNormalStrength = 0.011;

/* How far the normal pushes the water's own colour, and how far it pushes the
 * REFLECTION. The second is much larger and that ratio is physics, not taste —
 * see the note at the mix. */
const float kSurfaceDistort = 0.0022;
const float kReflectDistort = 0.0180;

/* Schlick's base reflectance for water, near enough. It is what stops the
 * surface becoming a perfect mirror when a facet turns edge-on. */
const float kFresnelBase = 0.22;

/* The sun's height above the water plane, in the same units as the
 * aspect-corrected UV the specular is computed in. Small because the sun is
 * LOW — that is what stretches the highlight path towards the viewer instead of
 * pooling it under the disc. */
const float kSunHeight = 0.16;

/* The specular lobe. The power is the tightness: high numbers give small, hard,
 * rare flashes, low numbers give a broad sheen over half the river. */
const float kSpecularStrength = 1.30;
const float kSpecularPower    = 90.0;

/* How far the reflection is dragged out vertically, in UV. The first is the
 * standing softness of any moving water; the second is added in proportion to
 * how steeply the surface is tilted, so the smear pulses with the swell. Both
 * are scaled by depth, because a reflection a long way off occupies too few
 * pixels to smear within. */
const float kBaseSmear  = 0.0016;
const float kFacetSmear = 0.0060;

/* The three glint sizes, weighted against each other. Small is the pinprick
 * that flashes and goes, medium is a whole wave face turning over, fuzz is the
 * bloom under both. Their RATIO is what reads as water; turning them all up
 * together just makes a brighter texture. */
const float kGlintSmall  = 1.00;
const float kGlintMedium = 0.70;
const float kGlintFuzz   = 0.14;

/* The shimmer running along the wave fronts. kShimmerSharp is the exponent on
 * the facet term: low numbers light up most of the surface and read as a sheen,
 * high numbers restrict it to the crest faces and read as glitter. Somewhere
 * around 4 is a wave front catching the sun. */
const float kShimmerStrength = 0.55;
const float kShimmerSharp    = 4.0;

/* How much the water map's blue channel adds where it is painted. 0 there
 * means "as calm as the rest of the river", so an unpainted channel changes
 * nothing — which is what makes B optional. */
const float kChopBoost = 1.5;

/* ======================================================================== */
/* STRENGTHS. 1.0 is "as authored" and 0.0 removes a term outright, which is  */
/* also how to see what one of them was contributing.                        */
/* ======================================================================== */

const float kRippleStrength  = 1.0;
const float kGlitterStrength = 1.0;
const float kGlowStrength    = 0.9;

/* Shafts take the picture over easily: at 0.85 a third of the frame was
 * measurably brighter than the painting and the sky read as a starburst rather
 * than as air. They went back up once the ghosting below was fixed and the fog
 * gave them something to actually scatter through. */
const float kShaftStrength = 0.75;

/* The fog is the loudest term here and the one most worth playing with. It is
 * London: the far bank should half dissolve. */
const float kFogStrength = 1.0;

/* The sky's weather. kCloudStrength is how much the drifting density
 * brightens and shades it — this is a MULTIPLIER on what the painter put
 * there, so it deepens the existing clouds rather than drawing new ones. The
 * warm term relights the near edge of a thickening cloud from the sun, which
 * is what stops the whole thing reading as a brightness wobble. */
const float kCloudStrength = 0.34;
const float kCloudWarmth   = 0.50;

/* How fast the weather crosses, in world units per second, and the floor under
 * the sky's distance — see cloudField. kSkyNear is the horizon: lower it for a
 * deeper sky whose far cloud barely moves, raise it to flatten the perspective
 * and calm the aliasing that a very deep one invites near the roof line. */
const float kWindSpeed  = 0.014;

/* How far the painted sky is actually pushed. kSkyDrift is the amplitude of the
 * wind's travel in UV and kSkyDriftRate how slowly it reverses — the SPEED you
 * see is THEIR PRODUCT, which is the thing to change and the thing that is easy
 * to get wrong by adjusting only one of them. It comes to about 0.006 UV a
 * second overhead, or eight pixels a second at a 1280-wide window, and the
 * reversal takes a minute and a half — long enough that nobody sees the sky
 * turn round. kSkyWarp is the local churn that stops the slide being
 * rigid. All three fade to nothing at the horizon. */
/* How far a parcel of sky travels in one advection cycle, and how many cycles
 * pass a second. THE SPEED YOU SEE IS THEIR PRODUCT — about 0.004 UV a second
 * overhead, five pixels at a 1280-wide window.
 *
 * They are not interchangeable, though. The travel is also how far the image
 * gets distorted before the cross-fade rescues it: raise it and the cloud
 * stretches further and looks more alive, until it starts to smear. The rate is
 * how often that rescue happens: raise it and the distortion stays modest, but
 * the sky visibly pulses as the cross-fade comes round. Between them, travel is
 * the look and rate is the ceiling on how far the look is allowed to go. */
const float kSkyAdvect = 0.090;
const float kSkyCycle  = 0.048;

/* How much the flow direction wanders from straight downwind. This is what
 * makes the sky shear rather than slide — zero is a rigid translation, which is
 * exactly what this whole mechanism exists to avoid. */
const float kSkyShear = 0.55;
const float kSkyNear    = 0.50;
const float kSkyFarthest = 1.6;

/* ------------------------------------------------------------------------ */

/* Value noise. Hash-based rather than a texture lookup because it needs no
 * asset and no sampler, and its quality only has to survive being squared and
 * used as glitter. */
float hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float valueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);   /* smoothstep weights: C1, no lattice creases */

    return mix(mix(hash(i + vec2(0.0, 0.0)), hash(i + vec2(1.0, 0.0)), u.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

/* ---- the water plane ---------------------------------------------------- */

/* WHERE THE WATER IS AND WHICH WAY IT RUNS. Two sources, one shape.
 *
 * THE PAINTED MAP IS THE REAL ANSWER. assets/textures/cromwell_water.png, if
 * it is there, carries three channels: R says how much of a pixel is water,
 * G ramps from black at the far end of the river to white at the near end, and
 * B adds local chop. Those three between them describe a river that bends,
 * that runs behind a bridge and around moored barges, and that recedes at
 * whatever rate the painting's perspective actually chose — none of which a
 * formula over uv can express. See assets/textures/README.md for how to author
 * one.
 *
 * THE FORMULA IS THE FALLBACK, kept because everything else here degrades
 * rather than fails and because it is what makes the splash work at all on a
 * checkout that has the painting but not the map. It is a straight tilted line
 * with a linear ramp below it, which is exactly as good as it sounds: it puts
 * the shoreline through the bridge, ripples the moored boats, and cannot bend.
 *
 * THE RAMP IS DOING TWO JOBS AT ONCE and that is the part worth understanding
 * before touching either. It is a distance field, so it sets how big waves are
 * and how dense the sparkle is; and because the swell takes its PHASE from the
 * ramp value rather than from a screen coordinate, the crests automatically
 * come out perpendicular to the ramp's gradient — which is to say across the
 * river — and automatically compress wherever the painter compressed the ramp.
 * Paint the ramp following the river and the waves follow the river, with no
 * direction vector anywhere in this shader. */
struct Water {
    float mask;    /* 0 dry, 1 open water                        */
    float near;    /* 0 at the far end of the river, 1 nearest   */
    float chop;    /* extra local agitation, 0 for calm          */
};

Water sampleWater(vec2 uv)
{
    Water w;

    if (uHasWaterMap > 0.5) {
        vec3 m = texture(uWaterMap, uv).rgb;
        w.mask = m.r;
        w.near = m.g;
        w.chop = m.b;
        return w;
    }

    float shore = kWaterLine + kWaterTilt * (uv.x - 0.5);
    w.mask = smoothstep(shore, shore + kWaterFade, uv.y);
    w.near = clamp((uv.y - shore) / max(1.0 - shore, 1e-4), 0.0, 1.0);
    w.chop = 0.0;
    return w;
}

/* ---- 1. the swell ------------------------------------------------------- */

/* Position along the river, in the river's own units — the coordinate every
 * moving thing on the water is expressed in, so that they cannot disagree
 * about which way the water is going or how fast.
 *
 * It is a distance TOWARDS the viewer: it increases as the water approaches,
 * so anything advanced with `- uTime * speed` travels downstream. The
 * reciprocal is the perspective; see the constants for what each term does.
 *
 * Its gradient is deliberately steep at the far edge, because that is what
 * perspective is, and the swell's amplitude falls to nothing there to cover
 * it — past a certain compression there is no honest way to draw a wave that
 * is smaller than a pixel, and the only choice is whether it fades out or
 * aliases. */
float riverCoord(vec2 uv, float ramp)
{
    float sideways = dot(uv * vec2(uAspect, 1.0), normalize(kFlowDir));
    float depth = kDepthGain / (ramp + kDepthNear);

    return kSideGain * sideways - depth;
}

/* The vertical displacement of the water surface at a point, in UV.
 *
 * PERSPECTIVE IS THE WHOLE TRICK. A river seen from the bank does not show
 * waves of one size: the near ones are large, slow and far apart on screen,
 * the distant ones compress towards the horizon until they are a fine shimmer.
 * So frequency FALLS and amplitude RISES with depth, both faster than
 * linearly. Waves of a constant screen size are the single thing that makes an
 * animated still look like a bedsheet, and getting it right is worth more than
 * any amount of detail in the wave shape itself.
 *
 * Three sine trains at unrelated frequencies and speeds, because two visibly
 * beat against each other on a loop and three do not. */
/* Returns BOTH the height and the slope, and the slope is the more useful half.
 *
 * A wave you can only displace with is a wobble. What makes water read as water
 * is that its surface has FACETS: every part of the swell is tilted, the tilt
 * sweeps along as the wave travels, and a facet tilted towards the light
 * flashes while its neighbours do not. Height alone cannot say which way a
 * point is tilted, so everything downstream of it — the shimmer along the wave
 * fronts, the smear that drags reflections out where the surface is steep — has
 * to have the derivative.
 *
 * It comes out analytically for nothing, since the derivative of a sum of sines
 * is a sum of cosines with the same phases. Sampling the wave twice to
 * difference it would cost another set of transcendentals and be less accurate.
 *
 *     .x  vertical displacement, in UV
 *     .y  surface slope along the flow, normalised to about -1..1 so that the
 *         responses built on it do not have to know the coefficients */
vec2 swell(vec2 uv, Water w, float flow)
{
    float ramp = w.near;

    /* PHASE IS THE RIVER COORDINATE, FULL STOP. Not a screen coordinate, and
     * emphatically not a screen coordinate multiplied by a distance-dependent
     * frequency — that version is kept in mind here because it produced the
     * worst artefact this shader has had, and it is an easy mistake to make
     * twice. Its phase gradient was
     *
     *     d(phase)/dv  =  k * d(along)/dv  +  along * dk/dv
     *                                         ^^^^^^^^^^^^^
     *
     * and `along` is a POSITION spanning the whole frame, so the second term
     * reached 220 radians per unit v: a full wave every twenty pixels,
     * vertically, in a train whose crests are supposed to be vertical. The
     * river filled with fine horizontal ribbing that no amount of tuning the
     * wavelengths could touch, because the ribbing was not made of
     * wavelengths — it was the frequency term differentiating.
     *
     * The rule, worth carrying to any other wave field: a spatially varying
     * frequency multiplied onto a large coordinate is not a frequency, it is a
     * chirp, and it will alias in whichever direction it varies. Put the
     * perspective in the COORDINATE and advance it with time; then one field
     * gives bunching and slowing together, and neither can be tuned out of
     * agreement with the other. */
    float waveCoord = flow;

    /* THE NUMBER THAT MATTERS IS NOT THE AMPLITUDE, IT IS THE SLOPE. A
     * displacement field stretches and squashes what it displaces in
     * proportion to its own derivative, so amplitude and frequency trade off
     * and only their product is visible. Above a slope of about 1 the mapping
     * folds over and the image tears; well below that it still visibly
     * compresses, and a painting squeezed by half over a few rows reads as
     * horizontal slabs sliding past each other — which is exactly what the
     * first attempt produced.
     *
     *     slope ~ amplitude * d(phase)/d(screen v)
     *
     * and the perspective is now inside that derivative, which is the point:
     * the far water has a steep phase gradient AND a vanishing amplitude, and
     * those two are what have to multiply out to something small. That is why
     * the amplitude fade below is not cosmetic — it is the only thing keeping
     * the horizon from tearing.
     *
     * PRESS F5 AND WATCH THE WATER, not the numbers. Slabs sliding past each
     * other means the product is too big; a flat river means too small.
     *
     * The small perpendicular terms bend the crests so they are not dead
     * straight lines, and the three unrelated speeds stop the sum from visibly
     * repeating. */
    float across = dot(uv * vec2(uAspect, 1.0), normalize(vec2(-kFlowDir.y, kFlowDir.x)));

    /* WARP THE PHASE BEFORE USING IT, or the sum of three sines shows itself.
     * Three regular trains, however carefully their frequencies are chosen,
     * produce regular corrugations — parallel ridges marching in step, which is
     * the single most recognisable signature of a shader pretending to be
     * water. Real wavelets wander: they bend, break, and lose alignment over a
     * few wavelengths.
     *
     * Displacing the phase coordinate itself by a slow noise field is the
     * cheapest cure. The crests stay crests, so nothing above depends on
     * anything changing, but they stop being straight and stop being parallel,
     * and the periodicity disappears without a single extra sine. The noise
     * drifts, so the pattern never settles into one arrangement either. */
    vec2 warpAt = vec2(waveCoord, across) * 2.6 + vec2(uTime * 0.05, uTime * 0.02);
    waveCoord += (valueNoise(warpAt) - 0.5) * 0.075;
    across    += (valueNoise(warpAt.yx + 19.0) - 0.5) * 0.075;

    /* Constant wavenumbers. Everything perspective used to do to these is done
     * once, in the river coordinate, where it cannot differentiate into a
     * chirp. */
    float k1 = kWave1, k2 = kWave2, k3 = kWave3;

    float p1 = waveCoord * k1 + across *  9.0 - uTime * 1.00;
    float p2 = waveCoord * k2 - across * 15.0 - uTime * 0.68;
    float p3 = waveCoord * k3 + across * 23.0 - uTime * 0.43;

    float wave = sin(p1) + sin(p2) * 0.7 + sin(p3) * 0.4;

    /* The same sum differentiated with respect to `along`, then divided by what
     * it could at most be, so the result lands in about -1..1 whatever the
     * coefficients are. Everything that consumes the slope is a shaping curve,
     * and a shaping curve whose input silently rescales when a wavelength
     * changes is a set of magic numbers waiting to drift. */
    float slope = (k1 * cos(p1) + k2 * 0.7 * cos(p2) + k3 * 0.4 * cos(p3)) /
                  max(k1 + k2 * 0.7 + k3 * 0.4, 1e-4);

    /* The far water has to go genuinely still rather than merely quieter, so
     * the amplitude falls faster than linearly — but SQUARED was too fast. It
     * put all the motion into the last few rows and left the middle of the
     * river, which is most of the water anyone looks at, dead. Three halves
     * reaches the middle distance without disturbing the far bank.
     *
     * Chop is additive on top: black in that channel means "as calm as the
     * rest of the river", which is what an unpainted channel gives you. */
    float agitation = ramp * sqrt(ramp) * (1.0 + kChopBoost * w.chop);

    /* The slope is NOT scaled by the amplitude. It is a direction — which way
     * this piece of surface is facing — and a distant wave is tilted just as
     * much as a near one even though it displaces the image far less. It does
     * get the depth fade, so the far bank does not shimmer at full strength. */
    return vec2(wave * agitation * 0.0022, slope * ramp);
}

/* ---- 1a. the surface normal ---------------------------------------------- */

/* THE NORMAL IS WHAT MAKES WATER LOOK LIKE WATER, and everything before this
 * was working with half of one. A height field and a single slope along the
 * flow can tell you that the surface is tilted; they cannot tell you WHICH WAY
 * it faces in two dimensions, and every property people recognise in water
 * depends on that:
 *
 *   - the reflection is displaced SIDEWAYS as well as up and down, which is
 *     what breaks a mirrored tower into a wandering column instead of a
 *     wobbling one;
 *   - a facet lights up when it happens to face the sun, and "faces the sun"
 *     is a statement about a direction, not a gradient. Done properly the
 *     glitter path appears on its own, because the facets that can send light
 *     to the eye are the ones lying between the viewer and the sun — no
 *     hand-placed column required.
 *
 * TAKEN FROM SCREEN-SPACE DERIVATIVES rather than differentiated by hand. The
 * height passes through the river coordinate, the painted depth ramp and a
 * noise warp before it gets here, and chain-ruling all of that would be a page
 * of algebra that silently rots the next time any of them changes. dFdx and
 * dFdy differentiate whatever the expression actually was. The field is smooth
 * over tens of pixels, so a one-pixel difference is a perfectly good gradient.
 *
 * uUvPerPixel converts them from per-pixel to per-UV, which is what makes the
 * result the SAME at any window size — without it the water would get glassier
 * as the window grew, which is the sort of bug that only shows up on someone
 * else's monitor. */
vec3 waterNormal(vec2 uv, Water w, float flow, float height)
{
    vec2 gradient = vec2(dFdx(height), dFdy(height)) / max(uUvPerPixel, vec2(1e-6));

    /* z is the flatness. A big number here is a calm surface whose normal
     * barely leaves vertical; kNormalStrength scales the tilt against it. */
    return normalize(vec3(-gradient * kNormalStrength, 1.0));
}

/* ---- 1b. the reflection smear ------------------------------------------- */

/* Reads the painting through a vertical blur, and this is the cheapest large
 * improvement in the whole shader.
 *
 * WHY VERTICAL, AND WHY IT MATTERS SO MUCH. A reflection in moving water is not
 * a displaced copy of what is above it — it is a copy DRAWN OUT along the line
 * towards the thing being reflected, because every ripple between here and
 * there redirects a slightly different part of it towards the eye. That is why
 * a lamp over a canal is a long streak and not a lamp. The first version only
 * displaced, which moves a sharp reflection about; the eye reads that as a
 * photograph being wobbled, because a sharp reflection is exactly what water
 * does not produce.
 *
 * THE RADIUS FOLLOWS THE SLOPE. Steep facets gather light from a wide spread of
 * directions and smear hard; flat water between crests reflects nearly cleanly.
 * So the blur breathes with the swell instead of sitting at a constant
 * softness, which is the difference between water and frosted glass.
 *
 * NINE TAPS, because a fixed tap count is only a blur up to a radius and past
 * it becomes a comb — five taps spread across the thirty-odd pixels the
 * reflection smear reaches would lay down five separate copies with gaps
 * between them rather than blurring anything. If the radius grows again, so
 * must this.
 *
 * (This was briefly and wrongly blamed for the ribbing across the river, on
 * that reasoning. Raising the count changed nothing, because the ribs were the
 * wave's own phase chirping — see swell(). Worth recording: the tap count was
 * a real latent bug and not the one being hunted, and fixing it while believing
 * otherwise would have hidden the actual cause behind a plausible story.) */
vec3 smearedSample(vec2 uv, float radius, float lod)
{
    if (radius < 1e-5) return textureLod(texture0, uv, lod).rgb;

    const int kTaps = 4;   /* either side of centre */

    vec3 sum = textureLod(texture0, uv, lod).rgb;
    float total = 1.0;

    for (int i = 1; i <= kTaps; i++) {
        float offset = radius * float(i) / float(kTaps);

        /* Triangular weights. A box would be cheaper and would leave a visible
         * hard end to the smear; the point of a reflection stretching out is
         * that it fades rather than stops. */
        float weight = 1.0 - float(i) / float(kTaps + 1);

        sum += textureLod(texture0, uv + vec2(0.0, offset), lod).rgb * weight;
        sum += textureLod(texture0, uv - vec2(0.0, offset), lod).rgb * weight;
        total += weight * 2.0;
    }

    return sum / total;
}

/* The scene mirrored into the water at this point, already rippled and already
 * smeared. See the constants above for why each term is shaped the way it is.
 *
 * NOTE WHAT IS NOT REFLECTED: the wordmark. It sits at v 0.44 to 0.52 and the
 * squash puts the deepest mirrored sample at 0.561, so the text falls outside
 * the band by a comfortable margin. That is luck rather than design, and it is
 * worth knowing before anyone moves the wordmark or retunes the squash — a
 * mirrored "cromwell" wobbling in the Thames is not the intended effect. */
vec3 mirroredScene(vec2 uv, vec3 normal, float smear)
{
    vec2 mirrored = vec2(uv.x, kHorizonV - (uv.y - kHorizonV) * kReflectSquash);
    mirrored += normal.xy * kReflectDistort;

    /* Twice the smear of the surface itself, for the same reason the ripple is
     * larger: the reflected path is longer, so everything along it spreads
     * more. This is what turns the sun's mirror image from a disc into a
     * shivering column.
     *
     * AND A COARSE MIP, which turned out to matter more than any of it. The
     * band being mirrored is the far bank — wharves, moored craft, brickwork,
     * all of it fine horizontal detail. Reflected at full resolution that
     * detail survives, and a compressed flipped copy of a busy bank reads as
     * ribbing across the river: it looked exactly like a wave artefact and was
     * nothing of the sort. A real reflection of something that far off does not
     * carry its texture either, because every ripple in the intervening water
     * has already averaged it away. Dropping two mip levels does that averaging
     * and leaves what a reflection actually is — the broad shape of the light.
     *
     * This is the same lesson as the light shafts, which needed the same fix
     * for the same reason: when a term is integrating over a surface, sampling
     * the sharp image is not more accurate, it is wrong. */
    return smearedSample(mirrored, smear * 2.0, 4.0);
}

/* ---- 2. the glitter track ----------------------------------------------- */

/* The broken path of light a low sun lays on water — the one feature of a
 * sunset river everyone recognises. It is not a reflected disc: each wave facet
 * that happens to tilt towards the eye flashes for an instant, so what you see
 * is a column of independent sparks, widening as it comes towards you.
 *
 * Built as noise thresholded and then squared, which turns a smooth field into
 * sparse bright points, drifting upstream. Masked three ways, and all three
 * matter:
 *   - to the water,
 *   - to the track beneath the sun: widening with depth, because the facets
 *     that can catch the eye fan out as they approach, and LEANING with the
 *     river rather than dropping straight down the frame (kFlowShift),
 *   - to what the painting already has lit there. This last is what keeps the
 *     sparks off the barges and boat hulls in the foreground without needing
 *     to know where any of them are: they are dark, so they cannot sparkle. */
float glitter(vec2 uv, Water w, float flow)
{
    float ramp = w.near;

    float across = dot(uv * vec2(uAspect, 1.0), normalize(vec2(-kFlowDir.y, kFlowDir.x)));

    float centre = kSunUV.x + kFlowShift * ramp;
    float track = abs(uv.x - centre) / (0.10 + ramp * 0.34);
    float column = exp(-track * track);

    /* Anisotropic sampling: stretched across the flow and compressed along it,
     * so each spark is a short dash like a wave crest catching the light
     * rather than a round dot.
     *
     * THESE FREQUENCIES ARE CAPPED BY THE DISPLAY, not chosen for looks. The
     * first attempt used 620 cycles vertically, which at an 800-pixel window is
     * 0.78 cycles per pixel — comfortably past the 0.5 a sampler can carry. The
     * result was not fine sparkle but broad horizontal moiré bands rolling
     * across the whole river: the noise field aliasing against the pixel grid.
     * A spark has to be a few pixels across to be a spark at all, so the
     * ceiling here is the window height — and the second octave counts too,
     * since it multiplies the frequency.
     *
     * THE FIELD IS LAID OUT IN THE RIVER'S COORDINATE, not on the screen, and
     * advanced along it. Two things fall out of that, and both were wrong when
     * this was a screen-space scroll:
     *
     *   - the glints travel the way the water travels, because they are being
     *     carried by the same coordinate the swell is. A sparkle path drifting
     *     one way over water moving another is the most obvious tell there is
     *     that this is a still image with an effect on top of it.
     *   - the glints SLOW DOWN with distance, and they get smaller and denser,
     *     because the coordinate compresses towards the far bank. Sparkle
     *     crossing the far water at the same screen speed as the near water is
     *     the same error as a wave doing it, and just as visible. */
    /* THREE SIZES OF GLINT, and the point is that they are different SIZES
     * rather than more of the same. Sun on water is not one grain: there are
     * pinpricks that flash and vanish, broader flashes a few times their size
     * where a whole wave face turns over, and a soft bloom under both because
     * every small intense source spreads. One scale, however carefully tuned,
     * reads as a texture; three read as light.
     *
     * THE COORDINATE IS ANISOTROPIC, and picking which axis gets which
     * frequency needs care, because neither of the river's own axes points the
     * way it first appears to. `flow` looks like the along-river axis and so
     * like the horizontal one — but it is dominated by its DEPTH term, which
     * varies vertically, so a high frequency on it makes horizontal streaks.
     * That was the first attempt and it came out looking like scratches.
     *
     * `sideways` is the genuinely horizontal one. High frequency there and low
     * frequency along `flow` gives a glint a few pixels wide and a few tens
     * tall: a VERTICAL streak, which is what a specular highlight on water
     * always is. It is drawn out along the line towards the eye by every ripple
     * between here and the source, and that elongation is the single detail
     * separating a glint from a speck of dust on the lens. */
    float sideways = dot(uv * vec2(uAspect, 1.0), normalize(kFlowDir));

    /* BOTH AXES HAVE TO CARRY THE PERSPECTIVE OR NEITHER SHOULD. The first
     * attempt paired `flow`, which is built on the reciprocal of depth, with
     * raw screen `sideways`, which is not — so the two axes compressed at
     * completely different rates as the river receded and the glints changed
     * shape with distance. Near the viewer, where flow's gradient collapses,
     * they stretched into long vertical bars: it looked like rain on the
     * window, and no single choice of coefficients could have fixed it, because
     * the ratio itself was varying.
     *
     * The lateral world position is the screen position times the distance —
     * the perspective divide, undone. Paired with flow, which already contains
     * the same distance, the cell now shrinks with depth in both directions at
     * once and keeps its shape. */
    float distance = kDepthGain / (ramp + kDepthNear);
    float lateral = sideways * distance;

    /* Only the along-river axis is advanced by time. It is the material
     * coordinate — a parcel of water keeps its lane while it travels
     * downstream — so glints move along the flow's gradient, down and to the
     * right, and inherit its perspective for free. */
    float travelled = flow - uTime * kDriftSpeed;

    /* Sized by working back from pixels rather than picked by eye:
     *
     *     coefficient = (frame size in pixels / wanted size) / that gradient
     *
     * which for a glint about six pixels wide and fourteen tall in the near
     * water comes out at roughly 1580 and 220. The ratio between them looks
     * absurd and is not: they are coefficients on two coordinates with wildly
     * different scales, and it is the PIXELS that come out at a bit over 2:1 —
     * enough to read as drawn towards the eye, not so much that it reads as
     * falling. */
    vec2 fine   = vec2(lateral * 1580.0, travelled * 220.0);
    vec2 medium = vec2(lateral *  620.0, travelled *  86.0);

    /* Fine: dense, hard-edged, brief. Squared after the threshold so what
     * survives is a point rather than a blob. */
    float fineNoise = valueNoise(fine) * 0.68 + valueNoise(fine * 1.9 + 31.0) * 0.32;
    float small = smoothstep(0.62, 0.96, fineNoise);
    small *= small;

    /* Medium: rarer and wider, a whole facet catching the light rather than a
     * ripple edge. Higher threshold makes it rarer; no squaring keeps it broad. */
    float mediumNoise = valueNoise(medium) * 0.70 + valueNoise(medium * 2.1 + 17.0) * 0.30;
    float mid = smoothstep(0.66, 0.98, mediumNoise);

    /* The bloom under both: lower frequency again, low threshold, soft. Hard
     * sparks with nothing beneath them read as dust on the lens. */
    float haze = valueNoise(medium * 0.42 + 7.0);
    float fuzzy = smoothstep(0.42, 0.88, haze);

    float spark = small * kGlintSmall + mid * kGlintMedium + fuzzy * kGlintFuzz;

    /* A second, slower and much coarser field gates the first, so individual
     * sparks come and go instead of the whole track pulsing together. In the
     * river's coordinate as well, and drifting at a different rate — a gate
     * that sat still on screen would show as fixed patches of "sparkly" and
     * "not sparkly" that the water slides through. */
    float gate = smoothstep(0.35, 0.85,
                            valueNoise(vec2(across * 12.0,
                                            (flow - uTime * kDriftSpeed * 0.6) * 34.0)));

    return spark * gate * column * ramp * (1.0 + kChopBoost * w.chop);
}

/* ---- 3. light shafts ---------------------------------------------------- */

/* Radial blur towards the sun, the standard screen-space stand-in for light
 * scattering through air, and the reason it works is worth stating: smearing
 * the image along the lines of sight to the sun accumulates whatever is bright
 * along each of those lines. Bright sky between two towers smears into a
 * shaft; a solid tower smears into nothing and so casts one. The shafts come
 * out of the silhouette for free, without the shader knowing there is one.
 *
 * A THRESHOLD DECIDES WHAT SCATTERS. Without it the whole image smears and the
 * result is a soft-focus blur rather than shafts. With it only the sky near the
 * sun and the sun itself contribute, which is also physically the right answer:
 * those are the bright sources.
 *
 * THREE THINGS HERE EXIST ONLY TO KILL THE GHOST, and the first version had all
 * three wrong, so they are worth spelling out. A radial blur is a sum of
 * shifted copies of the image; where the image has a hard, high-contrast edge,
 * those copies do not blend into a smear — they stack up as recognisable
 * translucent duplicates. Big Ben appeared three times in the sky. The fixes,
 * in the order of how much each bought:
 *
 *   1. SAMPLE A COARSE MIP, not the sharp image. There is no detail left in a
 *      120x68 version of the painting to duplicate, only broad brightness —
 *      which is all this integral ever wanted. It is also closer to the
 *      physics: scattering integrates over a solid angle, it does not resample
 *      the scene at full resolution. This one change did most of the work.
 *   2. JITTER THE STARTING OFFSET per pixel. Taps at identical positions in
 *      neighbouring pixels put their error in the same place, and error in the
 *      same place is a visible band; scattering the phase turns that band into
 *      fine noise, which at this contrast cannot be seen.
 *   3. MORE TAPS, SHORTER REACH. Twenty-four across 60% of the distance rather
 *      than sixteen across 85% — closer spacing, and a smear local enough to
 *      read as haze around the silhouette rather than as a second image of it.
 *
 * The clamp is a fourth and smaller thing, about this image in particular: the
 * wordmark is pure white and comfortably the brightest object in the frame, so
 * left alone it behaves as a second sun and lays its own shafts down towards
 * the first. Capping what a single tap may contribute lets it read as a bright
 * cloud instead. */
vec3 lightShafts(vec2 uv)
{
    vec2 toSun = kSunUV - uv;

    /* Fade with distance, or the shafts reach corners of the frame where there
     * is no plausible line of sight left. */
    float reach = length(toSun * vec2(uAspect, 1.0));
    float falloff = exp(-reach * 2.1);
    if (falloff < 0.004) return vec3(0.0);

    const int kTaps = 24;
    const float kLod = 4.0;          /* ~120x68 of a 1920x1080 source */

    /* Neither `step` nor `sample` may be used as a name here: the first shadows
     * a built-in, the second is a reserved word from GLSL 4.0's sample-shading
     * qualifiers that some drivers reject even in a 330 shader. */
    vec2 stride = toSun / float(kTaps) * 0.6;

    vec3 sum = vec3(0.0);
    float weight = 1.0;
    float total = 0.0;
    vec2 tap = uv + stride * hash(uv * 511.0);

    for (int i = 0; i < kTaps; i++) {
        tap += stride;

        /* textureLod, not texture, and the level has to be forced: left to
         * itself the hardware picks one from the screen-space derivative of the
         * coordinate, which for a tap walking towards a fixed point is near
         * zero. It would sample the sharp image and bring the ghost straight
         * back. */
        vec3 c = min(textureLod(texture0, tap, kLod).rgb, vec3(0.75));

        /* Only the bright part scatters, taken smoothly so a cloud edge
         * crossing the threshold does not pop as the blur walks over it. */
        float bright = smoothstep(0.30, 0.85, luminance(c));

        sum += c * bright * weight;
        total += weight;
        weight *= 0.90;
    }

    return sum / max(total, 1e-4) * falloff;
}

/* ---- 3b. the sky ---------------------------------------------------------*/

/* THE LIGHT ON THE CLOUDS, as distinct from the clouds themselves.
 *
 * This term thickens and thins the sky in place — brightening where it clears,
 * shading where it gathers. It is the WEATHER, not the movement: the painted
 * clouds are moved by skyFlow below, which displaces the image itself.
 *
 * The two were one job to begin with and this one was doing it alone, which is
 * why it read as "white clouds scrolling over the picture". A density field
 * added on top can only ever draw its own clouds; it cannot animate the ones
 * the painter put there, no matter how well it is shaped. What it IS good for
 * is the light — a sky whose only motion is translation looks like a printed
 * backdrop being pulled past, because real cloud changes brightness as it
 * turns and thins. So it stays, quieter, doing the half it is suited to.
 *
 * THREE OCTAVES, drifting at different speeds. The big one is the weather, the
 * small ones are its edges catching light; giving them separate speeds means
 * the pattern shears against itself and never repeats within a splash. */
float cloudField(vec2 uv, float skyNear)
{
    /* THE SKY HAS THE SAME PERSPECTIVE PROBLEM THE RIVER HAD, mirrored. Cloud
     * overhead is close and crosses the eye quickly; cloud at the horizon is
     * miles off and barely moves. A field scrolling at one screen speed
     * everywhere gets that exactly as wrong as the water did, and the fix is
     * the same one: build a coordinate that already contains the distance and
     * advance THAT.
     *
     * Distance is the reciprocal of nearness, so a world position across the
     * sky is the screen position TIMES that distance. Wind then moves features
     * at a constant world rate, which comes out as fast overhead and slow at
     * the horizon without a second constant to arrange it.
     *
     * AND IT HAS TO BE BOUNDED TWICE OVER. World position across the sky is the
     * screen position MULTIPLIED by distance, and a large coordinate multiplied
     * by a quantity that varies is the chirp that ribbed the river — here it is
     * not even a mistake, it is what perspective is, and a real horizon really
     * does compress to infinity. Something has to stop it, or the last rows
     * before the roof line become dense aliased contours hugging the silhouette
     * — which is exactly what they did.
     *
     * So: a floor under the reciprocal (kSkyNear), a hard ceiling on the
     * distance itself, and a fade that quietens what is left of the far sky.
     * A renderer with geometry would use mip levels for this; the fade is the
     * same idea with no mip chain to reach for. */
    float distance = min(1.0 / (skyNear + kSkyNear), kSkyFarthest);

    float worldX = uv.x * uAspect * distance - uTime * kWindSpeed;

    /* THE SCALE HAS TO BE FINE ENOUGH TO SEE THE PERSPECTIVE. The first version
     * ran the broadest octave at about one cell across the whole sky, and at
     * that size the compression towards the horizon is real, correct and
     * completely invisible — there is no structure for it to compress. It also
     * meant the single dominant cell swept over as one enormous blotch.
     *
     * Cloud has structure at several scales at once, which is both what makes
     * it read as cloud and what makes the depth legible: you can see the same
     * kind of detail getting smaller and closer together as it recedes, and
     * that is the only way perspective ever announces itself. */
    vec2 p = vec2(worldX * 6.0, distance * 4.5);

    /* Four octaves at different rates, so the pattern shears against itself and
     * never repeats. Each is offset in the WORLD coordinate, so they all obey
     * the same perspective; the finest is weighted low because at the far end
     * it is approaching the size of a pixel. */
    float n  = valueNoise(p) * 0.46;
    n += valueNoise(p * 2.1 + vec2(uTime * 0.02, 11.0)) * 0.27;
    n += valueNoise(p * 4.3 + vec2(uTime * 0.05, 37.0)) * 0.16;
    n += valueNoise(p * 8.9 + vec2(uTime * 0.09, 61.0)) * 0.11;

    /* Never all the way to nothing: the strip of sky just above the roof line
     * is where the light is and where the eye goes, so it keeps some weather —
     * just not the finest of it, which is the part that cannot be drawn at that
     * compression without turning into noise. */
    float horizonFade = mix(0.45, 1.0, smoothstep(0.0, 0.30, skyNear));

    /* EXPANDED, because three octaves of value noise summed to one do not use
     * the range they appear to. Each octave averages 0.5 and they are weighted
     * to sum to 1, so the total sits around 0.5 — but the deviations are
     * INDEPENDENT and partly cancel, and the sum lands within about 0.15 of the
     * middle almost everywhere. Taken raw that is a three-percent change in the
     * sky's brightness: present in a difference of two frames, measurably so,
     * and completely invisible to anyone watching.
     *
     * This is the arithmetic behind "the clouds do not do anything". The fix is
     * to use the range: stretch the deviation, then clamp so the rare pixel at
     * the extreme does not blow out.
     *
     * BUT NOT SYMMETRICALLY, and this is the correction to that fix. Stretched
     * evenly and multiplied by a strength of 0.55, the darkest cell came out at
     * 0.45 — a patch of sky at less than half brightness, sweeping across as one
     * enormous dark blotch. Real cloud does not do that: it blocks the sun, but
     * the rest of the sky goes on lighting it from every other direction, so
     * shadow SATURATES quickly while a lit edge can be as bright as it likes.
     * Halving the dark side is the cheap form of that, and it is the difference
     * between weather and a hole in the sky. */
    float density = clamp((n - 0.5) * 2.2, -1.0, 1.0) * horizonFade;

    return density < 0.0 ? density * 0.45 : density;
}

/* Where the sky is.
 *
 * THE SILHOUETTE DOES NOT MASK ITSELF HERE, and it is worth knowing why,
 * because the same trick works perfectly two hundred lines up. The glints stay
 * off the barges because a barge is genuinely dark and lit water is genuinely
 * bright, so brightness separates them. Measured on this painting, the sky is
 * 0.200 and Big Ben's silhouette is 0.146 — the towers are not black, they are
 * hazy, which is most of what makes the picture good. At any threshold that
 * keeps the whole sky, 86% of the tower comes with it.
 *
 * So brightness is used only as a gentle BIAS, not a cutout, and the term does
 * not need to be a cutout: density modulation is multiplicative, so it changes
 * how brightly the stone is lit and never moves an edge. Cloud shadow crossing
 * the Palace is a real thing and reads as one. It was displacement that could
 * not survive a bad silhouette mask, and nothing here displaces.
 *
 * A painted sky mask (assets/textures/cromwell_sky_mask.png) overrides all of
 * this and is the right answer for anything more assertive. */
/* HOW FAR TO PUSH THE PAINTED SKY, in UV. This is the term that actually moves
 * the clouds in the picture, and it is only possible because the sky mask is
 * painted: sliding the sky and not the towers shears the boundary between them,
 * and there is no way to hide that on the most recognisable shape in the frame
 * without an exact cutout of every spire and flagpole. With one, the offset can
 * be faded to nothing exactly along the silhouette and the buildings never
 * move at all.
 *
 * THIS RETURNS A VELOCITY, NOT AN OFFSET, and that distinction is what finally
 * made the painted clouds look like clouds rather than like a picture sliding.
 *
 * An offset can only translate. Cloud does not translate — it deforms as it
 * goes, and a sky that keeps every shape exactly while moving is read as a
 * backdrop on rollers within about a second, however slowly it is pulled. What
 * is wanted is ADVECTION: every point carried along its own path, so the shapes
 * stretch where the flow diverges and pile up where it converges.
 *
 * The obstacle is that advecting a still image distorts it without limit — a
 * few seconds in, the sky is smeared to ruin. The standard answer, and the one
 * used here, is to run the distortion on a CYCLE and cross-fade two copies half
 * a cycle apart, so whichever copy is visible is always near the middle of its
 * distortion and each fades out before its own gets bad. The seam where a copy
 * resets happens while its weight is zero, so it cannot be seen. It is the same
 * trick used to make flowing water out of a still normal map.
 *
 * The result is cloud that moves, shears, pulls apart and continuously reforms,
 * out of one still frame of oil paint.
 *
 * BOTH SCALE WITH NEARNESS, from the painted sky depth. Cloud overhead crosses
 * the eye; cloud at the horizon barely moves however hard the wind blows. This
 * is the same reciprocal that governs the river, and it is why the sky needed a
 * depth mask of its own rather than a single wind speed. */
vec2 skyVelocity(vec2 uv, float skyNear)
{
    /* Mostly downwind, but not uniformly: a noise field bends the direction
     * from place to place so the sky SHEARS as it goes. Uniform translation is
     * what makes a painted sky read as a backdrop on rollers — real cloud
     * stretches, folds and pulls apart because no two parts of it are going
     * quite the same way. */
    vec2 bendAt = uv * 1.9 + vec2(uTime * 0.012, uTime * 0.005);
    vec2 bend = vec2(valueNoise(bendAt) - 0.5,
                     valueNoise(bendAt.yx + 43.0) - 0.5) * 2.0;

    vec2 direction = normalize(vec2(1.0, 0.0) + bend * kSkyShear);

    /* Nearness, squared: the falloff towards the horizon wants to be steeper
     * than linear, or the far sky still slides enough to notice against a
     * skyline that is by definition not moving at all. */
    float perspective = skyNear * skyNear;

    return direction * kSkyAdvect * perspective;
}

/* 1 inside the baked-in wordmark, 0 outside, soft across the join.
 *
 * Measured off the painting: the letters run u 0.427 to 0.590 and v 0.435 to
 * 0.558, and nothing else in the frame comes near their brightness. The margin
 * is generous because it is not the letters that have to be covered but their
 * soft edge and the gap the advection could drag one across. */
float wordmarkBox(vec2 uv)
{
    const vec2 kMin = vec2(0.405, 0.415);
    const vec2 kMax = vec2(0.612, 0.578);
    const float kSoft = 0.020;

    vec2 inside = smoothstep(kMin - kSoft, kMin + kSoft, uv) *
                  (1.0 - smoothstep(kMax - kSoft, kMax + kSoft, uv));

    return inside.x * inside.y;
}

/* One advected tap of the painted sky, with the guard that makes displacing a
 * sky past a silhouette safe at all.
 *
 * KNOWING THE DESTINATION IS SKY SAYS NOTHING ABOUT THE SOURCE. A sky pixel
 * next to Big Ben, offset by any amount, reads FROM Big Ben — and the result
 * was a ghosted second outline of every spire dragged out into the air beside
 * the real one. So the source is checked as well, and where a sample would have
 * come from a building the original pixel is kept.
 *
 * That leaves a narrow band along the silhouette which does not move. It is
 * invisible; a doubled Big Ben was the only thing anyone could see. */
vec3 advectedSky(vec2 uv, vec2 offset, float skyMask, vec3 original)
{
    vec2 source = uv - offset;
    float sourceIsSky = uHasSkyMap > 0.5 ? texture(uSkyMap, source).r : skyMask;

    vec3 sampled = texture(texture0, source).rgb;

    /* AND THE WORDMARK MUST NOT MOVE EITHER. It is painted ON the sky, so the
     * sky mask quite correctly calls it sky — and the first version of this
     * advected it, cross-fading two offset copies into a doubled "cromwelll"
     * standing beside itself. The mask cannot help: it answers "is this the sky
     * region", and the wordmark is inside it.
     *
     * BRIGHTNESS WAS TRIED AND IS NOT ENOUGH, which is worth recording because
     * it very nearly works. The letters are white against a sky of 0.20, so any
     * threshold catches their middles — but their ANTI-ALIASED EDGE runs the
     * whole way from 1.0 down to the sky, and whatever threshold is chosen, the
     * pixels just under it advect while the ones just over it do not. The result
     * was a thin outline ghost tracing every letter. Lowering the threshold far
     * enough to catch the edge starts freezing the genuinely bright sky around
     * the sun, which is brighter than the faint end of the text.
     *
     * So: a rectangle, measured. Ugly, exact, and honest about being tied to
     * this image — the wordmark is baked into the painting at a fixed place, and
     * a box round it with a soft margin costs two smoothsteps and cannot be
     * fooled by a gradient. MOVE THE WORDMARK AND THIS MOVES WITH IT.
     *
     * Both ends are gated, for the same reason the sky mask is: a pixel BESIDE
     * the text would otherwise sample the text and drag a copy of it out. */
    float notWordmark = 1.0 - max(wordmarkBox(uv), wordmarkBox(source));

    return mix(original, sampled, skyMask * sourceIsSky * notWordmark);
}

vec2 sampleSky(vec2 uv, vec3 colour, float waterMask)
{
    if (uHasSkyMap > 0.5) {
        vec2 m = texture(uSkyMap, uv).rg;
        return vec2(m.r * (1.0 - waterMask), m.g);
    }

    float bias = smoothstep(0.10, 0.22, luminance(colour));
    float high = 1.0 - smoothstep(0.50, 0.72, uv.y);

    /* Nearness guessed as height: overhead is near, the roof line is far. It is
     * the same shape a painted ramp has and none of its accuracy. */
    float near = 1.0 - smoothstep(0.05, 0.55, uv.y);

    return vec2(bias * high * (1.0 - waterMask), near);
}

/* ---- 4. the fog --------------------------------------------------------- */

/* FOG IS EXTINCTION, NOT ADDITION, and that distinction is the whole reason
 * this term exists separately from the shafts above. Adding light to a scene
 * brightens it and leaves every edge exactly as sharp as it was — which is a
 * glow, and reads as one. Air does something else: it scatters its own light
 * towards you AND swallows what is behind it, so contrast collapses, blacks
 * lift, and the far bank half dissolves. That is a mix towards a fog colour,
 * and nothing else produces it.
 *
 * Two densities, added:
 *   - a HORIZON BAND, thickest along the far bank and thinning upwards into
 *     clear sky and downwards into the near foreground. This is depth of air:
 *     the horizontal sight lines are the long ones.
 *   - a BLOOM AROUND THE SUN, because a light source in fog lights the fog. It
 *     is the same term that makes headlights visible in it.
 *
 * The whole thing drifts. A low-frequency noise creeping sideways varies the
 * density by a quarter either way, so the bank thickens and clears rather than
 * sitting under a fixed gradient. This is the cheapest part of the shader and
 * the one that most makes it look alive. */
float fogDensity(vec2 uv, float sunDistance)
{
    const float kBandV        = 0.66;   /* where the air is deepest: the far bank */
    const float kBandFalloff  = 5.5;    /* how fast it thins away from that line  */
    const float kBandAmount   = 0.42;
    const float kSunAmount    = 0.38;
    const float kMaxDensity   = 0.72;   /* never a total whiteout                 */

    float band = exp(-abs(uv.y - kBandV) * kBandFalloff);
    float nearSun = exp(-sunDistance * 2.2);

    /* Slow, large and sideways — river fog moves along the valley. The time
     * scale is deliberately far below the swell's: fog that visibly churns
     * reads as smoke. */
    float drift = 0.75 + 0.5 * valueNoise(vec2(uv.x * 2.4 + uTime * 0.015,
                                               uv.y * 1.4 - uTime * 0.004));

    return clamp((band * kBandAmount + nearSun * kSunAmount) * drift, 0.0, kMaxDensity);
}

/* ------------------------------------------------------------------------ */

void main()
{
    vec2 uv = fragTexCoord;

    /* Sampled at the UNDISPLACED coordinate, and it has to be: the map says
     * where the water is on the painting, so looking it up with a coordinate
     * the water itself has already moved would feed the displacement back into
     * its own mask. */
    Water water = sampleWater(uv);

    /* THE SKY IS SAMPLED AND DISPLACED FIRST, before anything reads the
     * painting, because moving the painted clouds means moving the lookup — it
     * is the same kind of operation the swell performs on the water, and the
     * two regions are disjoint, so they simply add.
     *
     * The mask is read at the UNDISPLACED coordinate for the same reason the
     * water's is: it says where the sky is IN THE PAINTING, and looking it up
     * with a coordinate the sky has already moved would drag the silhouette's
     * outline along with the clouds. */
    vec2 sky = sampleSky(uv, texture(texture0, uv).rgb, water.mask);
    vec2 skyVelocity_ = skyVelocity(uv, sky.y) * uRamp;

    /* ONE COORDINATE FOR EVERYTHING THAT MOVES. The swell and the glints are
     * both laid out in it and both advanced along it, so they cannot end up
     * travelling in different directions or at different speeds — which they
     * did, when each carried its own idea of where downstream was. */
    float flow = riverCoord(uv, water.near);

    /* THE SURFACE IS SOLVED ONCE and three separate things read it: the
     * displacement moves the reflection, the slope decides how far it smears,
     * and the slope again decides which facets catch the light. Before this
     * they were three unrelated noise fields, and the giveaway was that the
     * shimmer did not travel with the waves — the surface looked like two
     * effects over a photograph rather than one body of water. */
    vec2 surface = vec2(0.0);
    if (water.mask > 0.0 && kRippleStrength > 0.0)
        surface = swell(uv, water, flow);

    /* The full surface normal, from which the distortion and the specular both
     * come. Solved before anything is displaced, because the normal describes
     * the surface at THIS point, not at the point the surface happens to be
     * showing. */
    vec3 normal = waterNormal(uv, water, flow, surface.x);
    normal = normalize(mix(vec3(0.0, 0.0, 1.0), normal, water.mask * uRamp));

    /* Displace first, then read: everything after this works on the moved
     * image, which is what makes the brush detail in the water travel WITH the
     * swell rather than sit on top of a moving surface.
     *
     * TWO-DIMENSIONAL NOW, and driven by the normal rather than by the height.
     * The vertical-only version was a defensible simplification — with a low sun
     * and the camera on the bank, most of the displacement really is vertical —
     * but it meant every wave moved the image the same way, and a surface whose
     * every facet displaces along one axis reads as a sheet of corrugated glass
     * rather than as water. Refraction follows the surface normal; a facet
     * tilted left moves what is behind it left. */
    uv += normal.xy * kSurfaceDistort * water.mask * kRippleStrength * uRamp;

    /* The smear is what turns a wobbled photograph into a reflection. Gated to
     * the water and faded by depth, so the far bank keeps its detail and the
     * near river goes soft. */
    float smear = (kBaseSmear + kFacetSmear * abs(surface.y)) *
                  water.near * water.mask * uRamp;

    /* THE DISTURBED SURFACE LOSES ITS FINE DETAIL, and this one line is what
     * separates water from corrugated iron.
     *
     * The painting's water is full of fine horizontal brushwork. Shear that
     * vertically with a wave and every one of those strokes turns into a
     * ripple of its own, so the surface fills with regular ribbing that follows
     * the BRUSHWORK rather than the swell. Isolating the terms showed it
     * plainly: reflection alone was clean, displacement alone produced all of
     * it, and no amount of tuning the wave maths touched it — because the
     * artefact was never in the wave.
     *
     * Real disturbed water does not preserve fine detail either; the ripple
     * that moves the image is the same ripple that scatters it. So the surface
     * is read a little coarser exactly where it is being tilted hardest,
     * leaving the long swell and losing the brush strokes it would otherwise
     * corrugate. */
    float surfaceLod = clamp(abs(surface.y) * water.near * 1.7, 0.0, 2.0);
    vec3 colour = smearedSample(uv, smear, surfaceLod);

    /* ---- THE PAINTED SKY, MOVED. It is done as a blend rather than by adding
     * the offset to `uv` above, and the difference between those two is the
     * whole problem of moving a sky past a silhouette.
     *
     * Displacing the lookup means a sky pixel reads from wherever the offset
     * points — and near the towers that is the TOWER. The result was a ghosted
     * second outline of every spire and pinnacle, dragged out into the air
     * beside the real one: the exact artefact the sky mask was supposed to
     * prevent, and it did not, because knowing that the DESTINATION is sky says
     * nothing about the source.
     *
     * So the source is checked too. A moved sample is only used where the place
     * it came from was also sky; where it would have come from a building the
     * original pixel stays. That leaves a narrow rim along the silhouette which
     * does not drift, which is invisible, whereas a doubled Big Ben is the only
     * thing anyone would see. */
    if (sky.x > 0.0 && dot(skyVelocity_, skyVelocity_) > 0.0) {
        /* The two phases, half a cycle apart. Each copy's distortion runs from
         * none to full over its own cycle, and the weight below is zero at both
         * ends of that, so a copy is only ever seen while it is somewhere near
         * the middle — and its reset is invisible. */
        float phaseA = fract(uTime * kSkyCycle);
        float phaseB = fract(uTime * kSkyCycle + 0.5);

        vec3 a = advectedSky(uv, skyVelocity_ * phaseA, sky.x, colour);
        vec3 b = advectedSky(uv, skyVelocity_ * phaseB, sky.x, colour);

        colour = mix(a, b, abs(1.0 - 2.0 * phaseA));
    }

    /* ---- 1c. THE REFLECTION, mixed into the water.
     *
     * Blended rather than added, because a mirror REPLACES what is behind it
     * rather than glowing on top of it — adding would have washed the river
     * pale without ever making it look reflective. The tint is the water's own
     * absorption: a reflection off a river is always a little darker and a
     * little cooler than what it reflects. */
    if (water.mask > 0.0 && kReflectStrength > 0.0) {
        /* THE REFLECTION IS DISTORTED BY THE NORMAL, several times harder than
         * the surface itself. That ratio is not a preference: the reflected ray
         * leaves the surface, travels to whatever it is reflecting and comes
         * back, so the same few degrees of tilt sweep it across an enormous
         * angle — while the water's own colour, which is right there at the
         * surface, barely moves. Distorting both by the same amount is what
         * makes a shader look like a wobbling photograph. */
        vec3 mirrored = mirroredScene(uv, normal, smear) * vec3(0.92, 0.90, 0.88);

        /* FRESNEL, from the normal rather than from height in the frame. Water
         * is a near-perfect mirror at a grazing angle and nearly transparent
         * looking straight down into it, and with the eye on the bank the
         * viewing angle is decided by which way each facet happens to be
         * tilted — so this varies wave by wave, not just with distance. The
         * Schlick curve is the standard cheap form of it.
         *
         * The depth term stays as well, because the geometric part of the angle
         * genuinely does depend on how far off the water is. */
        float facing = clamp(normal.z, 0.0, 1.0);
        float fresnel = kFresnelBase + (1.0 - kFresnelBase) * pow(1.0 - facing, 5.0);
        float grazing = mix(1.0, 0.45, water.near);

        colour = mix(colour, mirrored,
                     clamp(kReflectStrength * grazing * (0.55 + fresnel), 0.0, 1.0) *
                     water.mask * uRamp);
    }

    /* ---- 1d. crest contrast. Multiplicative, so the backs of the waves go
     * DOWN as the fronts come up — an additive term can only ever lift the
     * whole surface, and a wave you can only brighten reads as a light on the
     * water rather than as a shape in it. */
    if (water.mask > 0.0)
        colour *= 1.0 + kCrestContrast * surface.y * water.mask * uRamp;

    /* The sun's own colour in the painting, rather than a chosen orange, so
     * every warm term below belongs to THIS image — a different splash gets a
     * different palette for nothing. */
    vec3 sunColour = texture(texture0, kSunUV).rgb;
    float sunDistance = length((uv - kSunUV) * vec2(uAspect, 1.0));

    /* ---- 3b. weather. Before the fog, because a cloud thickening is a change
     * to the sky itself and the fog then sits in front of the result — doing it
     * after would have the near air brightening and darkening with the clouds
     * behind it, which is backwards. */
    if (kCloudStrength > 0.0) {
        /* `sky` was solved before the displacement, at the top of main. Reusing
         * it rather than re-sampling matters: the mask read at the moved
         * coordinate would be the mask of somewhere else. */
        float density = cloudField(uv, sky.y) * sky.x * uRamp;

        /* Thinning brightens, gathering shades. Multiplicative, so it works on
         * whatever the painter already had there instead of laying a grey wash
         * over it — an additive version lifts the black of the towers as much
         * as it lifts the sky, and the silhouette goes soft. */
        colour *= 1.0 + density * kCloudStrength;

        /* A gathering cloud catches the sun on the side facing it, and this is
         * what makes the movement read as cloud rather than as the sky
         * dimming. Only positive density, and only near the sun. */
        colour += sunColour * max(density, 0.0) * kCloudWarmth *
                  exp(-sunDistance * 1.7);
    }

    /* ---- 2. glitter, warm and additive. */
    if (water.mask > 0.0 && kGlitterStrength > 0.0) {
        float lit = smoothstep(0.06, 0.34, luminance(colour));
        float spark = glitter(uv, water, flow) * water.mask * lit;

        /* NORMALISED, unlike the glow below. A glint off a wave facet is a
         * near-specular reflection of the source, so it keeps the source's HUE
         * but not its exposure — it is the brightest thing in the frame, not a
         * dim orange. Using the sun's colour raw left the sparks at the same
         * value as the sun's disc after everything else had attenuated them,
         * which is to say invisible. */
        vec3 tint = sunColour / max(max(sunColour.r, max(sunColour.g, sunColour.b)), 1e-3);

        const float kSparkGain = 2.2;
        colour += tint * spark * kSparkGain * kGlitterStrength * uRamp;

        /* ---- 2b. SHIMMER ALONG THE WAVE FRONTS, which is a different thing
         * from the glints above and the reason the surface reads as having
         * depth rather than as having sparkles on it.
         *
         * The glints are independent: a noise field that happens to sit over
         * water. This is the swell itself catching the light — the facet term
         * comes straight out of the wave's derivative, so the bright bands ARE
         * the wave fronts and they travel at exactly the speed the wave
         * travels. Nothing else in the shader ties the light to the motion, and
         * without it the eye eventually notices that the sparkle and the ripple
         * are two unrelated things.
         *
         * ONE-SIDED, and that is the physics rather than a preference: only the
         * faces tilted towards the light can send any of it to the eye, so the
         * back of every wave stays dark. Taking abs() here would light both
         * sides and halve the sense of direction.
         *
         * Modulated by what the painting already has lit, exactly as the glints
         * are, which is what keeps the wave fronts from glowing across dark
         * water where there is nothing to reflect. */
        /* ---- 2c. THE SPECULAR, which is the honest version of everything
         * above it and the reason the surface now behaves like a surface.
         *
         * A glint is not a noise field that happens to sit near the sun; it is
         * a facet oriented so that the sun's reflection goes into the eye.
         * Given a normal, that is one dot product — and the SHAPE of the
         * glitter path falls out of it for nothing. The facets that can send
         * light to a viewer standing behind the frame are the ones lying
         * between the eye and the sun, so the highlights concentrate into a
         * column below the sun and fan out towards the viewer, exactly as they
         * do on any water at sunset. The hand-built `column` term above was an
         * approximation of a shape the physics produces on its own.
         *
         * The half-vector, with the view direction taken as straight into the
         * screen. That is not quite true of a projected painting, but the sun
         * is low and the water is far, and the error is a slow shift in where
         * the path sits rather than anything anyone could name. */
        vec3 toSun = normalize(vec3((kSunUV - uv) * vec2(uAspect, 1.0), kSunHeight));
        vec3 halfway = normalize(toSun + vec3(0.0, 0.0, 1.0));

        float specular = pow(max(dot(normal, halfway), 0.0), kSpecularPower);

        /* The noise glints ride ON the specular rather than beside it: they
         * break a smooth highlight into the countless separate flashes real
         * water shows, instead of laying a second unrelated sparkle over it. */
        float broken = mix(0.35, 1.0, spark);

        colour += tint * specular * broken * lit * water.mask *
                  kSpecularStrength * kGlitterStrength * uRamp;
    }

    /* ---- 4. fog, BEFORE the shafts and the glow. Order matters and this is
     * the physical one: fog sits between the eye and the painting, so it
     * washes out what the painting contains — but the light scattering through
     * it and the sun's own halo are in front of, or within, that fog and must
     * not be washed out by it. Fogging last would have grey air dimming the
     * very shafts the air is supposed to be carrying. */
    if (kFogStrength > 0.0) {
        /* The fog's own colour, taken from the painting's sky at a mip coarse
         * enough that it is an average rather than a patch of cloud — then
         * warmed towards the sun near the sun, which is the whole point of fog
         * in a sunset: it is a screen the light lands on. */
        vec3 hazeBase = textureLod(texture0, vec2(0.5, 0.30), 6.0).rgb * 1.30;
        vec3 fogColour = mix(hazeBase, sunColour * 1.15, exp(-sunDistance * 1.9));

        colour = mix(colour, fogColour,
                     fogDensity(uv, sunDistance) * kFogStrength * uRamp);
    }

    /* ---- 3. shafts, over the whole frame including the water, where the haze
     * lies on the river as much as it does in the sky. */
    if (kShaftStrength > 0.0)
        colour += lightShafts(uv) * kShaftStrength * uRamp;

    /* ---- 5. the disc's own bloom. Two lobes: a wide atmospheric halo and a
     * tight core, which is what a bright source through haze actually looks
     * like and what a single exponential never manages — one falloff is either
     * too tight to be a haze or too broad to have a centre.
     *
     * The breath is slow and shallow, a few percent. It is there so the frame
     * is never completely static even where there is no water, and it has to
     * stay below the level anyone would consciously notice or it becomes a
     * pulsing light rather than air. */
    if (kGlowStrength > 0.0) {
        float halo = exp(-sunDistance * 9.0);
        float core = exp(-sunDistance * 42.0);
        float breath = 1.0 + 0.05 * sin(uTime * 0.9);

        colour += sunColour * (halo * 0.55 + core * 1.10) * breath * kGlowStrength * uRamp;
    }

    finalColor = vec4(colour * fragColor.rgb, 1.0);
}
