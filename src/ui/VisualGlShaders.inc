// The Visualizer's GLSL (round 165e). LYNX's own shaders, written for this renderer (no ZGameEditor / Shadertoy code):
// GLSL 1.10-style (attribute / varying / gl_FragColor / texture2D), translated by JUCE when the context is v3.
// Every fragment shader writes premultiplied colour. Loops have fixed small bounds (the GPU is never asked for more
// than a few dozen steps a pixel).

namespace lynx::glsl
{
constexpr const char* vertex = R"(
attribute vec2 position;
varying vec2 uv;
void main()
{
    uv = position * 0.5 + 0.5;
    gl_Position = vec4 (position, 0.0, 1.0);
}
)";

// shared by every effect shader
constexpr const char* common = R"(
varying vec2 uv;
uniform sampler2D uSrc;   // what the layers below drew (feedback, post-process)
uniform sampler2D uPrev;  // feedback: this layer's previous output; blur passes: the blurred picture
uniform sampler2D uSpec;  // 64 spectrum bands (0..1) in .r
uniform sampler2D uHist;  // 64 bands x 128 frames of spectrum history in .r (rows wrap)
uniform vec2 uRes;
uniform float uTime;      // this layer's own clock (its Speed and the global Speed)
uniform float uAbs;       // the song time in seconds
uniform float uLevel, uBass, uMid, uHigh, uBeat;
uniform float uBand;      // the band the layer reacts to
uniform float uReact, uSpeed, uAmount, uSize, uCount, uOpacity;
uniform float uHistRow;   // the newest row of uHist (0..127)
uniform vec2 uPos;
uniform vec4 uColA, uColB;
uniform vec2 uDir;        // blur passes: the step

float aspect() { return uRes.x / max (uRes.y, 1.0); }
float drive() { return clamp (uBand * uReact, 0.0, 1.5); }
vec2 centred() { return (uv - uPos) * vec2 (aspect(), 1.0); }
mat2 rot (float a) { float c = cos (a), s = sin (a); return mat2 (c, -s, s, c); }
float hash (vec2 p)
{
    p = fract (p * vec2 (123.34, 456.21));
    p += dot (p, p + 45.32);
    return fract (p.x * p.y);
}
vec2 hash2 (vec2 p) { return vec2 (hash (p), hash (p + 17.17)); }
float noise (vec2 p)
{
    vec2 i = floor (p), f = fract (p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix (mix (hash (i), hash (i + vec2 (1.0, 0.0)), u.x), mix (hash (i + vec2 (0.0, 1.0)), hash (i + vec2 (1.0, 1.0)), u.x), u.y);
}
float fbm (vec2 p)
{
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++)
    {
        v += a * noise (p);
        p = rot (0.5) * p * 2.03 + 3.1;
        a *= 0.5;
    }
    return v;
}
float spec (float x) { return texture2D (uSpec, vec2 (clamp (x, 0.0, 1.0) * (63.0 / 64.0) + 0.5 / 64.0, 0.5)).r; }
// age 0 = newest frame, 1 = 127 frames ago
float hist (float x, float age)
{
    float row = uHistRow - clamp (age, 0.0, 1.0) * 126.0;
    return texture2D (uHist, vec2 (clamp (x, 0.0, 1.0) * (63.0 / 64.0) + 0.5 / 64.0, (row + 0.5) / 128.0)).r;
}
vec3 pal (float t) { return mix (uColA.rgb, uColB.rgb, clamp (t, 0.0, 1.0)); }
vec4 premul (vec3 c, float a) { a = clamp (a, 0.0, 1.0) * uOpacity; return vec4 (c * a, a); }
float luma (vec3 c) { return dot (c, vec3 (0.299, 0.587, 0.114)); }
)";

// ---------------------------------------------------------------------------------------------------------------
// Canvas: full-frame pictures
// ---------------------------------------------------------------------------------------------------------------
constexpr const char* canvasPlasma = R"(
void main()
{
    vec2 p = centred() * (2.0 + (1.0 - uSize) * 8.0);
    float t = uTime;
    float waves = 2.0 + floor (clamp (uCount / 16.0, 0.0, 4.0));
    float v = sin (p.x + t) + sin ((p.y + t) * 0.5) + sin ((p.x + p.y + t) * 0.5);
    vec2 c = p + vec2 (sin (t * 0.33) * 3.0, cos (t * 0.5) * 3.0);
    v += sin (sqrt (dot (c, c) + 1.0) + t);
    if (waves > 2.5) v += sin (length (p - vec2 (cos (t * 0.21), sin (t * 0.17)) * 4.0) * 1.3 - t);
    if (waves > 3.5) v += 0.5 * sin (p.x * 2.1 - p.y * 1.3 + t * 1.7);
    float k = 0.5 + 0.5 * sin (v * (1.0 + uAmount * 3.0) + drive() * 3.0);
    vec3 col = pal (k) * (0.65 + 0.7 * drive());
    gl_FragColor = premul (col, 1.0);
}
)";

constexpr const char* canvasTunnel = R"(
void main()
{
    vec2 p = centred() * 2.0;
    float r = length (p), a = atan (p.y, p.x);
    float z = (0.3 + uSize) / max (r, 0.03) + uTime * 2.0;
    float tw = a / 3.14159 + uAmount * z * 0.05;
    float check = mod (floor (z) + floor (tw * 8.0), 2.0);
    float edge = smoothstep (0.0, 0.08, abs (fract (z) - 0.5)) * smoothstep (0.0, 0.08, abs (fract (tw * 8.0) - 0.5));
    vec3 col = mix (uColA.rgb, uColB.rgb, check) * (0.35 + 0.65 * edge);
    float ring = exp (-abs (fract (z * 0.25 - uAbs * 0.0) - 0.5) * 12.0) * drive();
    col += uColB.rgb * ring;
    col *= clamp (r * 1.6, 0.0, 1.0) * (0.7 + 0.6 * drive());
    gl_FragColor = premul (col, 1.0);
}
)";

constexpr const char* canvasCells = R"(
void main()
{
    vec2 p = centred() * (3.0 + (1.0 - uSize) * 10.0);
    vec2 i = floor (p), f = fract (p);
    float d1 = 8.0, d2 = 8.0;
    float idHash = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            vec2 g = vec2 (float (x), float (y));
            vec2 o = hash2 (i + g);
            o = 0.5 + 0.45 * sin (uTime + 6.2831 * o);
            vec2 r = g + o - f;
            float d = dot (r, r);
            if (d < d1) { d2 = d1; d1 = d; idHash = hash (i + g); }
            else if (d < d2) d2 = d;
        }
    float edge = sqrt (d2) - sqrt (d1);
    float border = 1.0 - smoothstep (0.0, 0.04 + uAmount * 0.12, edge);
    float lit = spec (idHash);
    vec3 col = pal (idHash) * (0.25 + 0.75 * sqrt (d1)) * (0.4 + lit * uReact * 1.4);
    col += uColB.rgb * border * (0.6 + drive() * 1.5);
    gl_FragColor = premul (col, 1.0);
}
)";

constexpr const char* canvasKaleido = R"(
void main()
{
    vec2 p = centred() * (0.6 + (1.0 - uSize) * 2.0);
    p = rot (uTime * 0.2) * p;
    float folds = 3.0 + floor (uAmount * 9.0);
    float seg = 6.28318 / folds;
    float a = mod (atan (p.y, p.x), seg);
    a = abs (a - seg * 0.5);
    p = length (p) * vec2 (cos (a), sin (a));
    float iters = clamp (uCount / 8.0, 3.0, 8.0);
    float acc = 0.0;
    float k = 0.78 + 0.12 * sin (uTime * 0.3) + 0.08 * drive();
    for (int i = 0; i < 8; i++)
    {
        if (float (i) >= iters) break;
        p = abs (p) / max (dot (p, p), 0.02) - k;
        acc += exp (-length (p) * 2.5);
    }
    vec3 col = pal (fract (acc * 0.35 + uTime * 0.05)) * acc * (0.45 + 0.5 * drive());
    gl_FragColor = premul (col, 1.0);
}
)";

constexpr const char* canvasClouds = R"(
void main()
{
    vec2 p = centred() * (1.0 + (1.0 - uSize) * 4.0);
    float q = fbm (p * 1.5 - vec2 (0.0, uTime * 0.1));
    float n = fbm (p + vec2 (uTime * 0.3, 0.0) + q * 1.5);
    float d = smoothstep (0.25, 0.9, n * (0.6 + uAmount * 0.8) + drive() * 0.3);
    vec3 col = pal (n) * (0.5 + d) * (0.8 + drive() * 0.6);
    gl_FragColor = premul (col, d);
}
)";

constexpr const char* canvasWarp = R"(
void main()
{
    vec2 p = centred();
    float a = atan (p.y, p.x) / 6.28318 + 0.5;
    float r = length (p);
    float n = 40.0 + uCount * 2.0;
    float c = 0.0;
    for (int k = 0; k < 3; k++)
    {
        float fk = float (k);
        float cell = floor (a * n + fk * 0.33);
        float h = hash (vec2 (cell, fk));
        float z = fract (h * 13.7 + uTime * (0.25 + h * 0.5));
        float sr = z * z * 1.6;
        float len = 0.01 + uAmount * 0.3 * z * (1.0 + drive());
        float da = abs (fract (a * n + fk * 0.33) - 0.5) / n * 6.28318 * r;
        float dr = r - sr;
        c += smoothstep (len, 0.0, abs (dr + len * 0.5)) * smoothstep (0.002 + uSize * 0.012, 0.0, da) * (0.3 + z);
    }
    vec3 col = pal (clamp (r, 0.0, 1.0)) * c * (1.0 + drive());
    gl_FragColor = premul (col, clamp (c, 0.0, 1.0));
}
)";

constexpr const char* canvasElectric = R"(
void main()
{
    vec2 p = centred();
    float bolts = clamp (floor (uCount / 12.0) + 1.0, 1.0, 6.0);
    float c = 0.0;
    for (int i = 0; i < 6; i++)
    {
        float fi = float (i);
        if (fi >= bolts) break;
        float y = (fbm (vec2 (p.x * 3.0 + fi * 10.0, uTime * 2.0 + fi * 3.0)) - 0.5) * (0.3 + uAmount * 0.9)
                + (fi - (bolts - 1.0) * 0.5) * 0.06;
        float d = abs (p.y - y);
        c += (0.0015 + uSize * 0.006) / max (d, 0.0008) * (0.4 + drive());
    }
    vec3 col = mix (uColB.rgb, uColA.rgb, clamp (c * 0.15, 0.0, 1.0)) * clamp (c * 0.3, 0.0, 3.0);
    col += vec3 (1.0) * clamp (c * 0.05 - 0.2, 0.0, 1.0);
    gl_FragColor = premul (col, clamp (c * 0.3, 0.0, 1.0));
}
)";

constexpr const char* canvasRays = R"(
void main()
{
    vec2 p = centred();
    float r = length (p), a = atan (p.y, p.x);
    float n = floor (4.0 + uCount / 4.0);
    float rays = pow (0.5 + 0.5 * sin (a * n + uTime), 3.0 - uAmount * 2.0);
    float radius = uSize * 0.5 * (1.0 + drive() * 0.25);
    float sun = smoothstep (radius, radius - 0.01, r);
    float glow = exp (-max (r - radius, 0.0) * 4.0);
    vec3 col = uColA.rgb * sun + uColB.rgb * rays * glow * (0.4 + drive()) + uColA.rgb * glow * 0.3;
    gl_FragColor = premul (col, clamp (sun + rays * glow + glow * 0.3, 0.0, 1.0));
}
)";

// ---------------------------------------------------------------------------------------------------------------
// Terrain: a landscape of the spectrum's history (rows newest in front)
// ---------------------------------------------------------------------------------------------------------------
constexpr const char* terrainCommon = R"(
float heightAt (vec2 xz)
{
    float band = abs (xz.x) / 1.6;            // the lows in the middle
    float age = (xz.y - 0.15) / 3.2;
    if (age < 0.0 || band > 1.0) return 0.0;
    float v = hist (band, age);
    return pow (v, 1.6 - uReact * 1.1) * (0.08 + uSize * 0.45);
}
vec3 sky (vec2 p)
{
    return mix (uColB.rgb * 0.25, vec3 (0.0), clamp (p.y * 1.5, 0.0, 1.0)) + uColB.rgb * exp (-abs (p.y) * 18.0) * 0.6;
}
)";

constexpr const char* terrainMountains = R"(
void main()
{
    vec2 p = (uv - vec2 (0.5, 0.5)) * vec2 (aspect(), 1.0);
    vec3 ro = vec3 (0.0, 0.32 + uSize * 0.25, 0.0);
    vec3 rd = normalize (vec3 (p.x, p.y - 0.18 + (0.5 - uPos.y) * 0.6, 1.1));
    float t = 0.05, hit = -1.0;
    for (int i = 0; i < 90; i++)
    {
        vec3 q = ro + rd * t;
        if (q.y < heightAt (q.xz)) { hit = t; break; }
        t += 0.02 + t * 0.018;
        if (t > 4.0) break;
    }
    if (hit < 0.0) { gl_FragColor = premul (sky (p), 0.0); return; }
    float lo = hit - (0.02 + hit * 0.018), hi = hit;
    for (int i = 0; i < 5; i++)
    {
        float m = (lo + hi) * 0.5;
        vec3 q = ro + rd * m;
        if (q.y < heightAt (q.xz)) hi = m; else lo = m;
    }
    vec3 q = ro + rd * hi;
    float rows = 20.0 + uCount * 0.5;
    float gx = abs (fract (q.x * 12.0) - 0.5) / max (fwidth (q.x * 12.0), 0.0001);
    float gz = abs (fract (q.z / 3.2 * rows) - 0.5) / max (fwidth (q.z / 3.2 * rows), 0.0001);
    float line = 1.0 - clamp (min (gx, gz * 0.7) - 0.5, 0.0, 1.0);
    float h = q.y / (0.08 + uSize * 0.45);
    float fog = exp (-hi * 0.55);
    vec3 col = mix (uColA.rgb * 0.08, mix (uColA.rgb, uColB.rgb, clamp (h, 0.0, 1.0)) * (1.2 + uAmount * 2.0), line) * fog;
    gl_FragColor = premul (col, 1.0);
}
)";

constexpr const char* terrainHills = R"(
float hillsAt (vec2 xz)
{
    return heightAt (xz) * 0.7 + fbm (xz * 1.3 + vec2 (0.0, uTime * 0.5)) * (0.06 + uSize * 0.2);
}
void main()
{
    vec2 p = (uv - vec2 (0.5, 0.5)) * vec2 (aspect(), 1.0);
    vec3 ro = vec3 (0.0, 0.35 + uSize * 0.2, 0.0);
    vec3 rd = normalize (vec3 (p.x, p.y - 0.15 + (0.5 - uPos.y) * 0.6, 1.0));
    float t = 0.05, hit = -1.0;
    for (int i = 0; i < 80; i++)
    {
        vec3 q = ro + rd * t;
        if (q.y < hillsAt (q.xz)) { hit = t; break; }
        t += 0.025 + t * 0.02;
        if (t > 4.0) break;
    }
    if (hit < 0.0) { gl_FragColor = premul (sky (p) + uColB.rgb * 0.15, 1.0); return; }
    vec3 q = ro + rd * hit;
    float e = 0.01;
    vec3 n = normalize (vec3 (hillsAt (q.xz - vec2 (e, 0.0)) - hillsAt (q.xz + vec2 (e, 0.0)), 2.0 * e,
                              hillsAt (q.xz - vec2 (0.0, e)) - hillsAt (q.xz + vec2 (0.0, e))));
    vec3 light = normalize (vec3 (-0.4, 0.7, -0.5));
    float dif = clamp (dot (n, light), 0.0, 1.0);
    vec3 col = mix (uColA.rgb * 0.15, uColA.rgb, dif) + uColB.rgb * pow (1.0 - clamp (n.y, 0.0, 1.0), 2.0) * 0.4;
    float fog = 1.0 - exp (-hit * (0.3 + uAmount * 1.2));
    col = mix (col, uColB.rgb * 0.35, fog);
    gl_FragColor = premul (col * (0.8 + drive() * 0.4), 1.0);
}
)";

constexpr const char* terrainRoad = R"(
void main()
{
    vec2 p = (uv - vec2 (0.5, 0.5)) * vec2 (aspect(), 1.0);
    float horizon = (uPos.y - 0.5) * -0.6 + 0.02;
    vec3 col = vec3 (0.0);
    float alpha = 1.0;
    if (p.y > horizon)
    {
        // the sky, the sun with its stripes, the spectrum's mountains on the horizon
        float y = p.y - horizon;
        col = mix (uColB.rgb * 0.45, vec3 (0.02, 0.0, 0.05), clamp (y * 2.2, 0.0, 1.0));
        vec2 c = vec2 (0.0, horizon + 0.18);
        float radius = 0.1 + uSize * 0.25 * (1.0 + drive() * 0.15);
        float d = length (p - c);
        float stripes = step (0.5, fract ((p.y - c.y) * 22.0 + uTime * 0.6)) + step (c.y + radius * 0.15, p.y);
        float sun = smoothstep (radius, radius - 0.006, d) * clamp (stripes, 0.0, 1.0);
        col = mix (col, mix (uColA.rgb, vec3 (1.0, 0.85, 0.3), clamp ((p.y - c.y) / radius * 0.5 + 0.5, 0.0, 1.0)), sun);
        col += uColA.rgb * exp (-max (d - radius, 0.0) * 9.0) * 0.35;
        float m = spec (clamp (abs (p.x) / (aspect() * 0.5), 0.0, 1.0)) * (0.05 + uSize * 0.12) * (0.6 + uReact);
        if (y < m) col = mix (vec3 (0.02, 0.0, 0.04), uColB.rgb, smoothstep (m - 0.004, m, y));
    }
    else
    {
        float y = horizon - p.y;
        float z = 0.25 / max (y, 0.001);
        float x = p.x * z;
        float gz = abs (fract (z * 0.5 + uTime * 0.8) - 0.5) / max (fwidth (z * 0.5), 0.0001);
        float gx = abs (fract (x * 2.0) - 0.5) / max (fwidth (x * 2.0), 0.0001);
        float line = 1.0 - clamp (min (gx, gz) - 0.6, 0.0, 1.0);
        float fade = clamp (y * 6.0, 0.0, 1.0);
        col = vec3 (0.02, 0.0, 0.05) + uColA.rgb * line * (0.6 + uAmount * 1.5) * fade * (0.7 + drive() * 0.6);
        col += uColA.rgb * exp (-y * 40.0) * 0.5;
    }
    gl_FragColor = premul (col, alpha);
}
)";

// ---------------------------------------------------------------------------------------------------------------
// 3D objects: ray-marched shapes over transparent space
// ---------------------------------------------------------------------------------------------------------------
constexpr const char* objectCommon = R"(
float sceneSdf (vec3 p);
vec3 normalAt (vec3 p)
{
    vec2 e = vec2 (0.002, -0.002);
    return normalize (e.xyy * sceneSdf (p + e.xyy) + e.yyx * sceneSdf (p + e.yyx) + e.yxy * sceneSdf (p + e.yxy) + e.xxx * sceneSdf (p + e.xxx));
}
vec4 shade (vec3 ro, vec3 rd, float stepScale, float far)
{
    float t = 0.0;
    for (int i = 0; i < 72; i++)
    {
        vec3 p = ro + rd * t;
        float d = sceneSdf (p);
        if (d < 0.0015)
        {
            vec3 n = normalAt (p);
            vec3 l = normalize (vec3 (0.6, 0.8, -0.6));
            float dif = clamp (dot (n, l), 0.0, 1.0);
            float spe = pow (clamp (dot (reflect (rd, n), l), 0.0, 1.0), 24.0);
            float rim = pow (1.0 - clamp (dot (n, -rd), 0.0, 1.0), 3.0);
            vec3 col = uColA.rgb * (0.15 + 0.85 * dif) + vec3 (spe) * 0.6 + uColB.rgb * rim * (0.8 + drive());
            col *= exp (-t * 0.05);
            return vec4 (col, 1.0);
        }
        t += d * stepScale;
        if (t > far) break;
    }
    return vec4 (0.0);
}
float sdTorus (vec3 p, vec2 t) { vec2 q = vec2 (length (p.xz) - t.x, p.y); return length (q) - t.y; }
float sdBox (vec3 p, vec3 b) { vec3 q = abs (p) - b; return length (max (q, 0.0)) + min (max (q.x, max (q.y, q.z)), 0.0); }
vec3 spin (vec3 p)
{
    p.xz = rot (uTime * 0.7) * p.xz;
    p.xy = rot (uTime * 0.43) * p.xy;
    return p;
}
)";

constexpr const char* objectBlob = R"(
float sceneSdf (vec3 p)
{
    p -= vec3 ((uPos.x - 0.5) * 3.0, (0.5 - uPos.y) * 2.0, 0.0);
    p = spin (p);
    float r = 0.5 + uSize * 0.9;
    float k = 3.0 + uCount * 0.05;
    float disp = sin (k * p.x + uTime * 1.3) * sin (k * p.y + uTime) * sin (k * p.z - uTime * 0.7);
    disp *= 0.08 + uAmount * 0.25 * (0.5 + uBass * uReact * 2.0);
    return (length (p) - r * (1.0 + uBeat * uReact * 0.08)) * 0.6 + disp * 0.6;
}
void main()
{
    vec2 p = (uv - 0.5) * vec2 (aspect(), 1.0);
    vec4 c = shade (vec3 (0.0, 0.0, -4.0), normalize (vec3 (p, 1.4)), 0.8, 9.0);
    gl_FragColor = premul (c.rgb, c.a);
}
)";

constexpr const char* objectTorus = R"(
float sceneSdf (vec3 p)
{
    p -= vec3 ((uPos.x - 0.5) * 3.0, (0.5 - uPos.y) * 2.0, 0.0);
    p = spin (p);
    float r = 0.6 + uSize * 1.0;
    float d = sdTorus (p, vec2 (r * (1.0 + uBeat * uReact * 0.08), 0.08 + uAmount * 0.4));
    d += sin (atan (p.z, p.x) * floor (3.0 + uCount * 0.1) + uTime * 2.0) * 0.03 * drive();
    return d * 0.8;
}
void main()
{
    vec2 p = (uv - 0.5) * vec2 (aspect(), 1.0);
    vec4 c = shade (vec3 (0.0, 0.0, -4.0), normalize (vec3 (p, 1.4)), 0.9, 9.0);
    gl_FragColor = premul (c.rgb, c.a);
}
)";

constexpr const char* objectCubes = R"(
float cellH (vec2 id, float n)
{
    float band = clamp (abs (id.x) / n, 0.0, 1.0);
    float age = clamp ((id.y + n) / (2.0 * n), 0.0, 1.0);
    float v = hist (band, age * 0.5);
    return 0.05 + pow (v, 1.6 - uReact * 1.1) * (0.3 + uSize * 1.4);
}
float sceneSdf (vec3 p)
{
    float n = clamp (floor (uCount / 6.0), 2.0, 12.0);
    float c = 0.22 + uAmount * 0.2;
    vec2 id = clamp (floor (p.xz / c + 0.5), vec2 (-n), vec2 (n));
    vec2 q = p.xz - id * c;
    float h = cellH (id, n);
    return sdBox (vec3 (q.x, p.y - h * 0.5, q.y), vec3 (c * 0.38, h * 0.5, c * 0.38)) - 0.01;
}
void main()
{
    vec2 p = (uv - 0.5) * vec2 (aspect(), 1.0);
    vec3 ro = vec3 (0.0, 2.2, -3.4);
    ro.xz = rot (uTime * 0.25) * ro.xz;
    vec3 fw = normalize (-ro + vec3 (0.0, 0.3, 0.0));
    vec3 rt = normalize (cross (vec3 (0.0, 1.0, 0.0), fw));
    vec3 up = cross (fw, rt);
    vec3 rd = normalize (p.x * rt + p.y * up + 1.5 * fw);
    vec4 c = shade (ro, rd, 0.5, 12.0);
    gl_FragColor = premul (c.rgb, c.a);
}
)";

constexpr const char* objectRings = R"(
float sceneSdf (vec3 p)
{
    float spacing = 1.2;
    float z = p.z + uTime * 2.0;
    float id = floor (z / spacing);
    vec3 q = vec3 (p.x, p.y, mod (z, spacing) - spacing * 0.5);
    q.xy = rot (id * 0.4 + uTime * 0.2) * q.xy;
    float r = 0.9 + uSize * 1.2 + spec (fract (id * 0.137)) * uReact * 0.5;
    return sdTorus (q.xzy, vec2 (r, 0.03 + uAmount * 0.15));
}
void main()
{
    vec2 p = (uv - uPos) * vec2 (aspect(), 1.0);
    vec4 c = shade (vec3 (0.0, 0.0, 0.0), normalize (vec3 (p, 1.0)), 0.9, 14.0);
    gl_FragColor = premul (c.rgb, c.a);
}
)";

// ---------------------------------------------------------------------------------------------------------------
// Feedback: this layer's previous output moved, faded and kept under the new picture (uSrc), max-blended
// ---------------------------------------------------------------------------------------------------------------
constexpr const char* feedbackMain = R"(
vec2 warpUv (vec2 q);
void main()
{
    vec4 src = texture2D (uSrc, uv);
    vec2 q = warpUv (uv);
    float fade = 0.75 + uAmount * 0.235;
    vec4 prev = (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0) ? vec4 (0.0) : texture2D (uPrev, q) * fade;
    vec4 outc = max (src, prev);
    gl_FragColor = mix (src, outc, uOpacity);
}
)";

constexpr const char* feedbackZoom = R"(
vec2 warpUv (vec2 q)
{
    float z = 1.0 + (0.004 + uSize * 0.04) * (1.0 + drive());
    return (q - uPos) / z + uPos;
}
)";

constexpr const char* feedbackSpin = R"(
vec2 warpUv (vec2 q)
{
    float z = 1.0 + (0.003 + uSize * 0.03) * (1.0 + drive());
    vec2 d = (q - uPos) * vec2 (aspect(), 1.0);
    d = rot ((uSpeed - 0.5) * 0.08) * d / z;
    return d / vec2 (aspect(), 1.0) + uPos;
}
)";

constexpr const char* feedbackKaleido = R"(
vec2 warpUv (vec2 q)
{
    float z = 1.0 + (0.003 + uSize * 0.03) * (1.0 + drive());
    vec2 d = (q - uPos) * vec2 (aspect(), 1.0) / z;
    float n = clamp (floor (uCount / 8.0) + 2.0, 2.0, 12.0);
    float seg = 6.28318 / n;
    float a = mod (atan (d.y, d.x) + uTime * 0.1, seg);
    a = abs (a - seg * 0.5);
    d = length (d) * vec2 (cos (a), sin (a));
    return d / vec2 (aspect(), 1.0) + uPos;
}
)";

constexpr const char* feedbackWarp = R"(
vec2 warpUv (vec2 q)
{
    vec2 k = q * (2.0 + uSize * 8.0);
    vec2 o = vec2 (noise (k + uTime), noise (k - uTime + 7.0)) - 0.5;
    return q + o * 0.012 * (1.0 + drive() * 2.0);
}
)";

constexpr const char* feedbackEcho = R"(
vec2 warpUv (vec2 q) { return q; }
)";

constexpr const char* feedbackDrift = R"(
vec2 warpUv (vec2 q)
{
    float side = (noise (vec2 (q.y * 6.0, uTime)) - 0.5) * 0.004 * (1.0 + uSize * 4.0);
    return q - vec2 (side, 0.002 + uSpeed * 0.01 + drive() * 0.004);
}
)";

// ---------------------------------------------------------------------------------------------------------------
// Post-process: an effect on what the layers below drew
// ---------------------------------------------------------------------------------------------------------------
constexpr const char* postRgbShift = R"(
void main()
{
    vec2 d = uv - uPos;
    float len = length (d);
    vec2 dir = len > 0.0001 ? d / len : vec2 (1.0, 0.0);
    vec2 off = dir * (0.002 + uAmount * 0.02) * (1.0 + drive() * 3.0);
    vec4 src = texture2D (uSrc, uv);
    vec4 c = vec4 (texture2D (uSrc, uv + off).r, src.g, texture2D (uSrc, uv - off).b, 1.0);
    gl_FragColor = mix (src, c, uOpacity);
}
)";

constexpr const char* postPixelate = R"(
void main()
{
    float cell = (2.0 + uSize * 48.0) * (1.0 + drive() * 0.5);
    vec2 q = (floor (uv * uRes / cell) + 0.5) * cell / uRes;
    gl_FragColor = mix (texture2D (uSrc, uv), texture2D (uSrc, q), uOpacity);
}
)";

constexpr const char* postDots = R"(
void main()
{
    float cell = 4.0 + uSize * 26.0;
    vec2 g = uv * uRes / cell;
    vec2 q = (floor (g) + 0.5) * cell / uRes;
    vec4 c = texture2D (uSrc, q);
    float r = sqrt (clamp (luma (c.rgb) * (1.0 + drive()), 0.0, 1.0)) * 0.55;
    float d = length (fract (g) - 0.5);
    float soft = 0.02 + (1.0 - uAmount) * 0.25;
    vec4 dots = vec4 (c.rgb * smoothstep (r, r - soft, d), 1.0);
    gl_FragColor = mix (texture2D (uSrc, uv), dots, uOpacity);
}
)";

constexpr const char* postScanlines = R"(
void main()
{
    vec4 src = texture2D (uSrc, uv);
    float line = 0.5 + 0.5 * sin ((uv.y * uRes.y / (2.0 + uSize * 8.0) + uTime * 6.0) * 3.14159);
    vec3 c = src.rgb * (1.0 - uAmount * 0.8 * (1.0 - line));
    vec2 d = uv - 0.5;
    c *= 1.0 - dot (d, d) * 0.6;
    gl_FragColor = mix (src, vec4 (c, 1.0), uOpacity);
}
)";

constexpr const char* postVignette = R"(
void main()
{
    vec4 src = texture2D (uSrc, uv);
    float d = length ((uv - uPos) * vec2 (aspect(), 1.0));
    float r = 0.3 + uSize * 0.9;
    float v = smoothstep (r, r - 0.5, d);
    gl_FragColor = mix (src, vec4 (src.rgb * mix (1.0, v, uAmount), 1.0), uOpacity);
}
)";

constexpr const char* postColor = R"(
vec3 hueRotate (vec3 c, float a)
{
    const vec3 k = vec3 (0.57735);
    float ca = cos (a);
    return c * ca + cross (k, c) * sin (a) + k * dot (k, c) * (1.0 - ca);
}
void main()
{
    vec4 src = texture2D (uSrc, uv);
    vec3 c = hueRotate (src.rgb, uTime * uSpeed * 0.5);
    float l = luma (c);
    c = mix (vec3 (l), c, uSize * 2.0);
    c = (c - 0.5) * (0.5 + uAmount * 1.5) + 0.5;
    c *= uColA.rgb;
    c *= 1.0 + drive() * 0.3;
    gl_FragColor = mix (src, vec4 (clamp (c, 0.0, 1.0), 1.0), uOpacity);
}
)";

constexpr const char* postKaleido = R"(
void main()
{
    vec2 d = (uv - uPos) * vec2 (aspect(), 1.0);
    float n = clamp (floor (uCount / 8.0) + 2.0, 2.0, 16.0);
    float seg = 6.28318 / n;
    float a = mod (atan (d.y, d.x) + uTime * 0.2, seg);
    a = abs (a - seg * 0.5);
    vec2 q = length (d) * vec2 (cos (a), sin (a)) / vec2 (aspect(), 1.0) + uPos;
    q = abs (q);
    q = 1.0 - abs (1.0 - q);
    gl_FragColor = mix (texture2D (uSrc, uv), texture2D (uSrc, q), uOpacity);
}
)";

constexpr const char* postMirror = R"(
void main()
{
    vec2 q = uv;
    float m = mod (max (uCount, 1.0) - 1.0, 3.0);
    if (m != 1.0) q.x = 0.5 - abs (q.x - 0.5);
    if (m != 0.0) q.y = 0.5 - abs (q.y - 0.5);
    gl_FragColor = mix (texture2D (uSrc, uv), texture2D (uSrc, q), uOpacity);
}
)";

constexpr const char* postEdges = R"(
void main()
{
    vec2 px = 1.0 / uRes;
    float tl = luma (texture2D (uSrc, uv + px * vec2 (-1.0, 1.0)).rgb), t = luma (texture2D (uSrc, uv + px * vec2 (0.0, 1.0)).rgb);
    float tr = luma (texture2D (uSrc, uv + px * vec2 (1.0, 1.0)).rgb), l = luma (texture2D (uSrc, uv + px * vec2 (-1.0, 0.0)).rgb);
    float r = luma (texture2D (uSrc, uv + px * vec2 (1.0, 0.0)).rgb), bl = luma (texture2D (uSrc, uv + px * vec2 (-1.0, -1.0)).rgb);
    float b = luma (texture2D (uSrc, uv + px * vec2 (0.0, -1.0)).rgb), br = luma (texture2D (uSrc, uv + px * vec2 (1.0, -1.0)).rgb);
    float gx = -tl - 2.0 * l - bl + tr + 2.0 * r + br;
    float gy = -tl - 2.0 * t - tr + bl + 2.0 * b + br;
    float e = clamp (sqrt (gx * gx + gy * gy) * (1.0 + uAmount * 3.0), 0.0, 1.0);
    vec4 src = texture2D (uSrc, uv);
    vec3 c = src.rgb * (1.0 - uAmount * 0.85) + uColA.rgb * e * (1.0 + drive());
    gl_FragColor = mix (src, vec4 (c, 1.0), uOpacity);
}
)";

constexpr const char* postShake = R"(
void main()
{
    float k = floor (uAbs * 30.0);
    vec2 o = (vec2 (hash (vec2 (k, 1.0)), hash (vec2 (k, 2.0))) - 0.5) * uAmount * 0.08 * clamp (uBand * uReact * 1.5, 0.0, 1.0);
    vec2 q = clamp (uv + o, vec2 (0.0), vec2 (1.0));
    gl_FragColor = mix (texture2D (uSrc, uv), texture2D (uSrc, q), uOpacity);
}
)";

constexpr const char* postGlitch = R"(
void main()
{
    float k = floor (uAbs * 15.0);
    float block = floor (uv.y * (12.0 + uSize * 30.0));
    float trig = step (1.0 - uAmount * 0.5 - drive() * 0.4, hash (vec2 (block, k)));
    vec2 q = uv + vec2 ((hash (vec2 (block, k + 3.0)) - 0.5) * 0.2 * trig, 0.0);
    vec4 c = vec4 (texture2D (uSrc, q + vec2 (0.006 * trig, 0.0)).r, texture2D (uSrc, q).g, texture2D (uSrc, q - vec2 (0.006 * trig, 0.0)).b, 1.0);
    gl_FragColor = mix (texture2D (uSrc, uv), c, uOpacity);
}
)";

constexpr const char* postGrain = R"(
void main()
{
    vec4 src = texture2D (uSrc, uv);
    float n = hash (uv * uRes + fract (uAbs * 7.13) * 100.0) - 0.5;
    gl_FragColor = mix (src, vec4 (src.rgb + n * uAmount * 0.35, 1.0), uOpacity);
}
)";

constexpr const char* postInvert = R"(
void main()
{
    vec4 src = texture2D (uSrc, uv);
    float k = clamp (uBand * uReact * 1.2, 0.0, 1.0);
    gl_FragColor = mix (src, vec4 (mix (src.rgb, 1.0 - src.rgb, k), 1.0), uOpacity);
}
)";

// blur / bloom: the picture made smaller (bloom: only what is bright), blurred in two passes, then mixed back
constexpr const char* postDown = R"(
uniform float uThreshold;
void main()
{
    vec4 c = texture2D (uSrc, uv);
    float l = luma (c.rgb);
    gl_FragColor = vec4 (c.rgb * smoothstep (uThreshold, uThreshold + 0.25, l), 1.0);
}
)";

constexpr const char* postBlurPass = R"(
void main()
{
    vec4 c = texture2D (uSrc, uv) * 0.227027;
    c += (texture2D (uSrc, uv + uDir * 1.384615) + texture2D (uSrc, uv - uDir * 1.384615)) * 0.316216;
    c += (texture2D (uSrc, uv + uDir * 3.230769) + texture2D (uSrc, uv - uDir * 3.230769)) * 0.070270;
    gl_FragColor = c;
}
)";

constexpr const char* postBloomMix = R"(
void main()
{
    vec4 src = texture2D (uSrc, uv);
    vec4 glow = texture2D (uPrev, uv);
    gl_FragColor = vec4 (src.rgb + glow.rgb * (0.5 + uAmount * 2.5) * (1.0 + drive()) * uOpacity, 1.0);
}
)";

constexpr const char* postBlurMix = R"(
void main()
{
    gl_FragColor = mix (texture2D (uSrc, uv), vec4 (texture2D (uPrev, uv).rgb, 1.0), uOpacity);
}
)";

// ---------------------------------------------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------------------------------------------
constexpr const char* composite = R"(
varying vec2 uv;
uniform sampler2D uSrc;
uniform sampler2D uLayer;
uniform float uOpacity;
uniform float uMode;
vec3 blendColour (vec3 b, vec3 l)
{
    if (uMode < 0.5) return l;
    if (uMode < 1.5) return b + l;
    if (uMode < 2.5) return 1.0 - (1.0 - b) * (1.0 - l);
    if (uMode < 3.5) return b * l;
    if (uMode < 4.5) return mix (2.0 * b * l, 1.0 - 2.0 * (1.0 - b) * (1.0 - l), step (0.5, b));
    if (uMode < 5.5) return abs (b - l);
    if (uMode < 6.5) return max (b, l);
    return min (b, l);
}
void main()
{
    vec4 b = texture2D (uSrc, uv);
    vec4 l = texture2D (uLayer, uv);
    float a = clamp (l.a * uOpacity, 0.0, 1.0);
    vec3 lc = l.a > 0.0001 ? l.rgb / l.a : vec3 (0.0);
    vec3 r = mix (b.rgb, clamp (blendColour (b.rgb, lc), 0.0, 1.0), a);
    gl_FragColor = vec4 (r, 1.0);
}
)";

constexpr const char* finalPass = R"(
varying vec2 uv;
uniform sampler2D uSrc;
uniform float uZoom;
uniform float uBright;
void main()
{
    vec2 q = (uv - 0.5) / uZoom + 0.5;
    vec4 c = (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0) ? vec4 (0.0, 0.0, 0.0, 1.0) : texture2D (uSrc, q);
    gl_FragColor = vec4 (c.rgb * uBright, 1.0);
}
)";

constexpr const char* copy = R"(
varying vec2 uv;
uniform sampler2D uSrc;
void main()
{
    gl_FragColor = vec4 (texture2D (uSrc, uv).rgb, 1.0);
}
)";
} // namespace lynx::glsl
