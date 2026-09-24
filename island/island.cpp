// island.cpp - procedural tropical island flythrough, CPU raymarched.
// Eroded heightfield terrain with jungle canopy, animated ocean with
// depth-based absorption, reef lagoon and shoreline foam, volumetric cumulus
// with cloud shadows, analytic sky and aerial perspective.
//
// build: g++ -O3 -march=native -fopenmp -o island island.cpp
// usage: ./island <outdir> <W> <H> <spp> <firstFrame> <lastFrame> <totalFrames>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <sys/stat.h>

typedef float F;
static const F PI = 3.14159265f;
static const F FPS = 30.f;

struct V3 {
    F x, y, z;
    V3() : x(0), y(0), z(0) {}
    V3(F a) : x(a), y(a), z(a) {}
    V3(F a, F b, F c) : x(a), y(b), z(c) {}
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(const V3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    V3 operator*(F s) const { return {x * s, y * s, z * s}; }
    V3 operator/(F s) const { return {x / s, y / s, z / s}; }
    V3 operator-() const { return {-x, -y, -z}; }
    V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    V3& operator*=(const V3& o) { x *= o.x; y *= o.y; z *= o.z; return *this; }
};
static inline F dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static inline F len(const V3& a) { return std::sqrt(dot(a, a)); }
static inline V3 norm(const V3& a) { return a / len(a); }
static inline F clamp01(F v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline F mix(F a, F b, F t) { return a + (b - a) * t; }
static inline V3 mix(const V3& a, const V3& b, F t) { return a + (b - a) * t; }
static inline F smooth(F a, F b, F x) { F t = clamp01((x - a) / (b - a)); return t * t * (3 - 2 * t); }
static inline V3 vexp(const V3& v) { return {std::exp(v.x), std::exp(v.y), std::exp(v.z)}; }
static inline F lum(const V3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ULL + 1) { next(); }
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
    F u() { return (next() & 0xFFFFFF) * (1.f / 16777216.f); }
};

// ------------------------------------------------------------------ noise
static inline uint32_t ihash(int x, int y) {
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u;
    h = (h ^ (h >> 15)) * 0x2c1b3c6du;
    h = (h ^ (h >> 12)) * 0x297a2d39u;
    return h ^ (h >> 15);
}
static inline F hash2(int x, int y) { return (ihash(x, y) & 0xFFFFFF) * (1.f / 16777215.f); }
static inline F hash3(int x, int y, int z) { return hash2(x + (int)(ihash(z, 911) & 0xFFFF), y ^ (z * 7919)); }

// 2D value noise in [-1,1] with analytic derivatives (x = value, y,z = d/dx, d/dz)
static inline V3 noised(F px, F pz) {
    int ix = (int)std::floor(px), iz = (int)std::floor(pz);
    F fx = px - ix, fz = pz - iz;
    F ux = fx * fx * fx * (fx * (fx * 6 - 15) + 10), uz = fz * fz * fz * (fz * (fz * 6 - 15) + 10);
    F dux = 30 * fx * fx * (fx * (fx - 2) + 1), duz = 30 * fz * fz * (fz * (fz - 2) + 1);
    F a = hash2(ix, iz), b = hash2(ix + 1, iz), c = hash2(ix, iz + 1), d = hash2(ix + 1, iz + 1);
    F k1 = b - a, k2 = c - a, k4 = a - b - c + d;
    F v = a + k1 * ux + k2 * uz + k4 * ux * uz;
    return {2 * v - 1, 2 * dux * (k1 + k4 * uz), 2 * duz * (k2 + k4 * ux)};
}
static inline F noise2(F px, F pz) { return noised(px, pz).x; }
static inline F noise3(const V3& p) {
    int ix = (int)std::floor(p.x), iy = (int)std::floor(p.y), iz = (int)std::floor(p.z);
    F fx = p.x - ix, fy = p.y - iy, fz = p.z - iz;
    F ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy), uz = fz * fz * (3 - 2 * fz);
    F r = 0;
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                F w = (dx ? ux : 1 - ux) * (dy ? uy : 1 - uy) * (dz ? uz : 1 - uz);
                r += w * hash3(ix + dx, iy + dy, iz + dz);
            }
    return r;
}

// ------------------------------------------------------------------ terrain
static const F ISLAND_R = 520.f;
static const F SEA_Y = 0.f;

// low-frequency shape: radial profile (deep sea -> reef -> lagoon -> beach -> hills)
static F islandBase(F x, F z, F& landMask) {
    F wx = x + 90 * noise2(x * 0.0021f + 3.1f, z * 0.0021f) + 35 * noise2(x * 0.006f, z * 0.006f + 7.f);
    F wz = z + 90 * noise2(x * 0.0021f, z * 0.0021f + 11.3f) + 35 * noise2(x * 0.006f + 5.f, z * 0.006f);
    // elongate the island
    F r = std::sqrt((wx * 0.82f) * (wx * 0.82f) + (wz * 1.12f) * (wz * 1.12f)) / ISLAND_R;
    F h;
    if (r > 1.75f) h = -55;
    else if (r > 1.34f) h = mix(-1.2f, -55.f, smooth(1.34f, 1.75f, r));        // outer reef slope
    else if (r > 1.24f) h = -1.2f + 1.0f * std::sin((r - 1.24f) / 0.10f * PI); // reef crest
    else if (r > 1.02f) h = mix(-3.2f, -1.2f, smooth(1.12f, 1.24f, r)) - 1.0f * smooth(1.2f, 1.1f, r) * smooth(1.02f, 1.1f, r); // lagoon
    else h = mix(2.5f, -3.2f, smooth(0.93f, 1.02f, r));                          // beach
    landMask = smooth(0.97f, 0.80f, r);
    h += landMask * 70.f * smooth(0.9f, 0.2f, r);
    return h;
}

// eroded fbm (iq-style derivative damping), octaves limited by lod
static F hills(F x, F z, int oct) {
    F px = x * 0.0045f, pz = z * 0.0045f;
    F a = 0, b = 1, dx = 0, dz = 0;
    for (int i = 0; i < oct; ++i) {
        V3 n = noised(px, pz);
        dx += n.y; dz += n.z;
        a += b * n.x / (1 + dx * dx + dz * dz);
        b *= 0.5f;
        F nx = 1.6f * px - 1.2f * pz, nz = 1.2f * px + 1.6f * pz;
        px = nx + 1.7f; pz = nz + 9.2f;
    }
    return a;
}

static inline int lodOct(F t, int maxo) {
    // drop octaves whose wavelength is below the pixel footprint
    F foot = std::max(t * 0.0012f, 0.05f);
    int o = (int)(std::log2(222.f / foot)) + 1;
    return std::max(3, std::min(maxo, o));
}

struct TInfo { F ground, canopy, veg; };

static F canopyShape(F x, F z, F& treeId) {
    // worley-like dome canopy: each cell holds one tree crown
    const F cs = 9.f;
    F gx = x / cs, gz = z / cs;
    int ix = (int)std::floor(gx), iz = (int)std::floor(gz);
    F best = 0; treeId = 0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            uint32_t h = ihash(ix + i, iz + j);
            F ox = (h & 0xFF) / 255.f, oz = ((h >> 8) & 0xFF) / 255.f, rr = 0.55f + 0.5f * ((h >> 16) & 0xFF) / 255.f;
            F ddx = ix + i + ox - gx, ddz = iz + j + oz - gz;
            F d2 = (ddx * ddx + ddz * ddz) / (rr * rr);
            if (d2 < 1) {
                F v = std::sqrt(1 - d2) * (0.7f + 0.6f * rr);
                if (v > best) { best = v; treeId = ((h >> 24) & 0xFF) / 255.f; }
            }
        }
    return best;
}

static TInfo terrain(F x, F z, F t, bool detail) {
    F land;
    F h = islandBase(x, z, land);
    int oct = lodOct(t, 9);
    F hl = hills(x, z, oct);
    h += land * (95.f * hl + 60.f) * smooth(0.0f, 0.6f, land) - land * 35.f;
    // volcanic peak
    F dx = x - 120, dz = z + 60;
    F pk = std::exp(-(dx * dx + dz * dz) / (2 * 210.f * 210.f));
    h += land * pk * 150.f * (1 + 0.25f * hl);
    // sand beach flattening near the waterline
    F g = h;
    // vegetation mask: above beach, not too steep (approx via hill slope), below bare summit
    F veg = smooth(3.5f, 9.f, h) * (1 - smooth(215.f, 255.f, h));
    veg *= smooth(-0.35f, 0.1f, noise2(x * 0.004f + 2.f, z * 0.004f)) * 0.5f + 0.5f;
    TInfo ti{g, 0, veg};
    if (veg > 0.01f) {
        F tid;
        F c = t < 2600.f ? canopyShape(x, z, tid) : 0.55f;
        if (!detail && t > 1200.f) c = mix(c, 0.55f, smooth(1200.f, 2600.f, t));
        ti.canopy = veg * (6.f + 11.f * c + 6.f * noise2(x * 0.03f, z * 0.03f));
    }
    return ti;
}
static inline F heightAt(F x, F z, F t) { TInfo ti = terrain(x, z, t, false); return ti.ground + ti.canopy; }

static V3 terrainNormal(F x, F z, F t) {
    F e = std::max(0.06f, t * 0.0012f);
    F hx = heightAt(x + e, z, t) - heightAt(x - e, z, t);
    F hz = heightAt(x, z + e, t) - heightAt(x, z - e, t);
    return norm(V3(-hx, 2 * e, -hz));
}

// ------------------------------------------------------------------ sky & clouds
static V3 SUN = norm(V3(-0.55f, 0.42f, -0.72f));
static const V3 SUN_COL = V3(1.0f, 0.90f, 0.76f) * 5.2f;
static const F CL_LO = 700.f, CL_HI = 1500.f;

static V3 skyColor(const V3& d) {
    F y = std::max(d.y, 0.0f);
    V3 zen(0.10f, 0.26f, 0.62f), hor(0.62f, 0.76f, 0.92f);
    V3 c = mix(hor, zen, std::pow(y, 0.45f));
    F s = std::max(0.f, dot(d, SUN));
    c += V3(1.0f, 0.78f, 0.52f) * (0.25f * std::pow(s, 8.f) + 0.6f * std::pow(s, 64.f));
    if (s > 0.99995f) c += V3(40.f, 36.f, 30.f);
    if (d.y < 0) c = mix(c, V3(0.35f, 0.45f, 0.55f), smooth(0.f, -0.2f, d.y));
    return c;
}

static F cloudDensity(const V3& p, F time, int oct) {
    F hN = (p.y - CL_LO) / (CL_HI - CL_LO);
    if (hN < 0 || hN > 1) return 0;
    V3 q = p * 0.0011f + V3(time * 0.004f, 0, time * 0.002f);
    F cov = noise3(V3(q.x * 0.35f, 0.0f, q.z * 0.35f) + V3(3.1f, 0, 1.7f));
    F n = 0, a = 0.5f;
    V3 r = q;
    for (int i = 0; i < oct; ++i) { n += a * noise3(r); r = r * 2.07f + V3(1.3f, 2.1f, 0.7f); a *= 0.5f; }
    F shape = smooth(0.f, 0.15f, hN) * smooth(1.f, 0.35f, hN);
    F d = (n * 1.1f + cov * 0.7f) - 1.13f + 0.25f * (1 - shape);
    return std::max(0.f, d * 3.0f) * shape;
}

// cheap cloud shadow term at ground point (sample density column along sun ray)
static F cloudShadow(const V3& p, F time) {
    F tt = (CL_LO + 250.f - p.y) / SUN.y;
    V3 q = p + SUN * tt;
    F d = cloudDensity(q, time, 3) + cloudDensity(q + SUN * 250.f, time, 3);
    return std::exp(-d * 1.4f);
}

// raymarch cloud layer; returns premultiplied colour, transmittance in T
static V3 clouds(const V3& ro, const V3& rd, F tmax, F time, F& T, Rng& rng) {
    T = 1;
    if (rd.y <= 0.01f && ro.y < CL_LO) return V3(0);
    F t0 = (CL_LO - ro.y) / rd.y, t1 = (CL_HI - ro.y) / rd.y;
    if (t0 > t1) std::swap(t0, t1);
    t0 = std::max(t0, 0.f); t1 = std::min(t1, std::min(tmax, 30000.f));
    if (t0 >= t1) return V3(0);
    const int N = 42;
    F dt = (t1 - t0) / N;
    F t = t0 + dt * rng.u();
    V3 acc(0);
    F mu = dot(rd, SUN);
    F phase = 0.6f * (1 - 0.64f) / (4 * PI * std::pow(1 + 0.64f - 1.6f * mu, 1.5f)) + 0.4f / (4 * PI);
    for (int i = 0; i < N && T > 0.02f; ++i, t += dt) {
        V3 p = ro + rd * t;
        F d = cloudDensity(p, time, 5);
        if (d > 0.001f) {
            F ls = cloudDensity(p + SUN * 60.f, time, 3) + cloudDensity(p + SUN * 180.f, time, 2);
            F sunT = std::exp(-ls * 60.f * 0.018f) * 0.8f + 0.2f * std::exp(-ls * 12.f * 0.018f);
            F hN = (p.y - CL_LO) / (CL_HI - CL_LO);
            V3 amb = mix(V3(0.35f, 0.40f, 0.50f), V3(0.75f, 0.82f, 0.95f), hN);
            V3 Ls = SUN_COL * (sunT * phase * 7.f) + amb * 0.9f;
            F sigma = d * 0.018f;
            F Ts = std::exp(-sigma * dt);
            acc += Ls * (T * (1 - Ts));
            T *= Ts;
        }
    }
    // fade distant clouds into haze
    F fade = std::exp(-t0 * 0.00006f);
    V3 haze = skyColor(rd);
    acc = acc * fade + haze * ((1 - T) * (1 - fade));
    return acc;
}

static V3 aerial(const V3& col, const V3& rd, F t, F yAvg) {
    F dens = 0.00012f * std::exp(-std::max(0.f, yAvg) * 0.0009f);
    F f = 1 - std::exp(-t * dens);
    F s = std::max(0.f, dot(rd, SUN));
    V3 fogc = mix(V3(0.55f, 0.68f, 0.85f), V3(1.0f, 0.85f, 0.65f), std::pow(s, 6.f));
    return mix(col, fogc, f);
}

// ------------------------------------------------------------------ scene tracing
static bool marchTerrain(const V3& ro, const V3& rd, F tmin, F tmax, F& tHit) {
    F t = tmin;
    F lastD = 0, lastT = t;
    for (int i = 0; i < 400 && t < tmax; ++i) {
        V3 p = ro + rd * t;
        if (p.y > 330.f && rd.y >= 0) return false;
        F d = p.y - heightAt(p.x, p.z, t);
        if (d < 0.002f * t + 0.02f) {
            if (i > 0 && d < 0) {  // refine by secant
                F a = lastT, b = t, da = lastD, db = d;
                for (int k = 0; k < 5; ++k) {
                    F m = a + (b - a) * da / (da - db);
                    V3 pm = ro + rd * m;
                    F dm = pm.y - heightAt(pm.x, pm.z, m);
                    if (dm < 0) { b = m; db = dm; } else { a = m; da = dm; }
                }
                t = a;
            }
            tHit = t;
            return true;
        }
        lastD = d; lastT = t;
        t += std::max(0.4f * d, 0.25f + 0.002f * t);
    }
    return false;
}

static F softShadow(const V3& p, F time) {
    F res = 1, t = 1.5f;
    for (int i = 0; i < 48 && t < 900; ++i) {
        V3 q = p + SUN * t;
        if (q.y > 330.f) break;
        F h = q.y - heightAt(q.x, q.z, t + 300.f);
        res = std::min(res, 10.f * h / t);
        if (res < 0.0f) return 0;
        t += std::max(1.2f, h * 0.6f);
    }
    return clamp01(res);
}

static V3 shadeGround(const V3& p, const V3& rd, F t, F time, bool underwater) {
    TInfo ti = terrain(p.x, p.z, t, true);
    V3 n = terrainNormal(p.x, p.z, t);
    // ground normal (without canopy) for slope-based materials
    F e = std::max(0.3f, t * 0.002f);
    F sl;
    {
        F hx = terrain(p.x + e, p.z, t, false).ground - terrain(p.x - e, p.z, t, false).ground;
        F hz = terrain(p.x, p.z + e, t, false).ground - terrain(p.x, p.z - e, t, false).ground;
        sl = norm(V3(-hx, 2 * e, -hz)).y;
    }
    F h = ti.ground;
    F nz = noise2(p.x * 0.05f, p.z * 0.05f) * 0.5f + noise2(p.x * 0.3f, p.z * 0.3f) * 0.25f;
    V3 sand = mix(V3(0.62f, 0.54f, 0.40f), V3(0.72f, 0.64f, 0.50f), 0.5f + nz);
    V3 wetSand = sand * V3(0.55f, 0.52f, 0.48f);
    V3 grass = mix(V3(0.10f, 0.15f, 0.035f), V3(0.20f, 0.20f, 0.06f), 0.5f + nz);
    V3 rock = mix(V3(0.17f, 0.15f, 0.13f), V3(0.28f, 0.25f, 0.21f), 0.5f + nz);
    V3 alb = sand;
    alb = mix(alb, wetSand, smooth(1.2f, 0.2f, h) * smooth(-0.6f, 0.2f, h));
    alb = mix(alb, grass, smooth(2.5f, 5.5f, h));
    alb = mix(alb, rock, smooth(0.78f, 0.62f, sl) * smooth(3.f, 8.f, h));
    alb = mix(alb, rock * 0.8f, smooth(230.f, 270.f, h));
    F ao = 1;
    if (ti.canopy > 0.5f) {
        F tid;
        F c = canopyShape(p.x, p.z, tid);
        V3 leaf = mix(V3(0.045f, 0.095f, 0.022f), V3(0.10f, 0.165f, 0.04f), tid);
        leaf = mix(leaf, V3(0.10f, 0.12f, 0.03f), smooth(0.6f, 1.f, noise2(p.x * 0.02f, p.z * 0.02f)) * 0.5f);
        F crown = smooth(0.f, 0.5f, c);
        F top = clamp01((p.y - ti.ground) / std::max(1.f, ti.canopy));
        alb = mix(alb, leaf * (0.75f + 0.5f * noise2(p.x * 1.3f, p.z * 1.3f)), smooth(0.3f, 0.6f, ti.canopy / 6.f));
        ao = mix(0.35f, 1.f, top) * mix(0.6f, 1.f, crown);
    }
    if (underwater) {
        // seabed: sand with coral patches in the lagoon
        F cn = noise2(p.x * 0.07f + 7.f, p.z * 0.07f) * 0.6f + noise2(p.x * 0.23f, p.z * 0.23f + 3.f) * 0.3f + noise2(p.x * 0.9f, p.z * 0.9f) * 0.1f;
        F coral = smooth(0.2f, 0.55f, cn) * smooth(-0.5f, -2.f, h);
        alb = mix(sand * 1.05f, V3(0.36f, 0.30f, 0.22f) * (0.7f + 0.6f * nz), coral * 0.6f);
    }
    F sh = underwater ? 1.f : softShadow(p + n * 0.5f, time);
    sh *= cloudShadow(p, time);
    F dif = std::max(0.f, dot(n, SUN));
    F skyA = 0.5f + 0.5f * n.y;
    F bounce = clamp01(-n.y * 0.5f + 0.5f) * 0.2f;
    V3 lin = SUN_COL * (dif * sh) + V3(0.30f, 0.42f, 0.62f) * (skyA * ao * 1.1f) + V3(0.25f, 0.22f, 0.15f) * (bounce * ao);
    if (underwater) {
        // animated caustics
        F c1 = noise2(p.x * 0.35f + time * 0.6f, p.z * 0.35f - time * 0.4f);
        F c2 = noise2(p.x * 0.35f - time * 0.5f + 4.f, p.z * 0.35f + time * 0.45f);
        F caus = std::pow(1 - std::fabs(c1 + c2) * 0.5f, 6.f) * 2.2f;
        lin = lin + SUN_COL * (caus * dif * sh * 0.6f);
    }
    return alb * lin;
}

static V3 waterNormal(F x, F z, F t, F time) {
    F e = 0.08f;
    auto wh = [&](F px, F pz) {
        F h = 0;
        h += 0.30f * std::sin(px * 0.080f + pz * 0.050f + time * 1.4f);
        h += 0.22f * std::sin(px * -0.050f + pz * 0.110f + time * 1.7f);
        h += 0.10f * std::sin(px * 0.210f + pz * 0.170f + time * 2.6f);
        int oct = t < 400 ? 4 : (t < 1500 ? 3 : 2);
        F a = 0.08f, f = 0.35f;
        for (int i = 0; i < oct; ++i) { h += a * noise2(px * f + time * 0.5f, pz * f - time * 0.35f); a *= 0.5f; f *= 2.1f; }
        return h;
    };
    F h0 = wh(x, z);
    V3 n = norm(V3(-(wh(x + e, z) - h0), e, -(wh(x, z + e) - h0)));
    // flatten with distance to avoid shimmering
    F k = smooth(300.f, 3000.f, t);
    return norm(mix(n, V3(0, 1, 0), k * 0.8f));
}

static V3 render(const V3& ro, const V3& rd, F time, Rng& rng) {
    F tWater = rd.y < 0 ? (SEA_Y - ro.y) / rd.y : 1e9f;
    F tTer;
    bool hitT = marchTerrain(ro, rd, 0.5f, std::min(tWater, 9000.f), tTer);
    V3 col;
    F tSurf;
    if (hitT) {
        tSurf = tTer;
        V3 p = ro + rd * tTer;
        col = shadeGround(p, rd, tTer, time, false);
    } else if (tWater < 20000.f) {
        tSurf = tWater;
        V3 p = ro + rd * tWater;
        V3 n = waterNormal(p.x, p.z, tWater, time);
        F cosi = std::max(0.02f, -dot(rd, n));
        F fres = 0.02f + 0.98f * std::pow(1 - cosi, 5.f);
        V3 rf = rd - n * (2 * dot(rd, n));
        if (rf.y < 0.02f) rf = norm(V3(rf.x, 0.02f, rf.z));
        V3 refl = skyColor(rf);
        F cT; V3 cl = clouds(p, rf, 1e9f, time, cT, rng);
        refl = refl * cT + cl;
        // refraction (Snell) into water, march to seabed
        F eta = 1.f / 1.33f;
        F k = 1 - eta * eta * (1 - cosi * cosi);
        V3 tr = norm(rd * eta + n * (eta * cosi - std::sqrt(std::max(0.f, k))));
        TInfo bed = terrain(p.x, p.z, tWater, false);
        F depth = std::max(0.f, SEA_Y - bed.ground);
        F path = depth / std::max(0.15f, -tr.y);
        V3 bedCol(0);
        if (depth < 60.f) {
            V3 bp = p + tr * path;
            bp.y = terrain(bp.x, bp.z, tWater, false).ground;
            bedCol = shadeGround(bp, tr, tWater, time, true);
        }
        V3 sigma(0.42f, 0.075f, 0.045f);
        V3 Tw = vexp(-(sigma * (path + depth)));
        V3 deep = V3(0.004f, 0.035f, 0.075f) * (0.4f + SUN.y);
        V3 scatter = V3(0.02f, 0.16f, 0.20f) * (SUN.y * 1.3f);
        V3 below = bedCol * Tw + mix(scatter, deep, clamp01(depth / 40.f)) * (V3(1) - Tw);
        below = below * cloudShadow(p, time);
        col = mix(below, refl, fres);
        // sun glitter
        V3 hv = norm(SUN - rd);
        // normalized Blinn-Phong; lobe widens with distance to stand in for unresolved waves
        F ex = mix(1400.f, 90.f, smooth(60.f, 1800.f, tWater));
        F spec = (ex + 8.f) / (8.f * PI) * std::pow(std::max(0.f, dot(n, hv)), ex);
        col += SUN_COL * (spec * fres * cloudShadow(p, time) * 0.9f);
        // shoreline + reef foam
        F foamBand = smooth(1.2f, 0.0f, depth) + 0.7f * smooth(1.6f, 0.7f, depth) * smooth(-1.6f, -0.9f, -depth) * 0;
        F reef = smooth(1.5f, 0.5f, depth) * smooth(-20.f, -6.f, bed.ground);
        F fn = noise2(p.x * 0.12f + time * 0.3f, p.z * 0.12f) * 0.5f + 0.5f;
        F waves = 0.5f + 0.5f * std::sin(depth * 6.f - time * 2.2f + fn * 4.f);
        F foam = clamp01(foamBand * (0.4f + 0.8f * waves * fn)) + clamp01(reef * fn * 1.4f);
        foam = clamp01(foam) * smooth(3000.f, 800.f, tWater);
        V3 foamCol = (SUN_COL * std::max(0.f, SUN.y) * cloudShadow(p, time) + V3(0.4f, 0.5f, 0.65f)) * 0.8f;
        col = mix(col, foamCol, foam * 0.85f);
    } else {
        tSurf = 1e9f;
        col = skyColor(rd);
    }
    // clouds in front of surface / sky
    F T;
    V3 cl = clouds(ro, rd, tSurf, time, T, rng);
    if (tSurf < 1e8f) {
        V3 p = ro + rd * tSurf;
        col = aerial(col, rd, tSurf, (ro.y + p.y) * 0.5f);
    }
    col = col * T + cl;
    return col;
}

// ------------------------------------------------------------------ camera path
struct Key { F x, y, z; };
static const Key KEYS[] = {
    {-980, 7, 760},   {-800, 6, 610},  {-640, 5, 480},  {-500, 6, 370},
    {-390, 22, 280},  {-300, 95, 170}, {-210, 150, 60}, {-140, 200, -60},
    {-60, 215, -190}, {60, 190, -320}, {200, 120, -430}, {330, 45, -520},
    {470, 22, -600},  {600, 25, -680},
};
static const int NK = sizeof(KEYS) / sizeof(KEYS[0]);

static V3 catmull(F u) {
    F s = u * (NK - 3);
    int i = std::min(NK - 4, (int)s);
    F t = s - i;
    auto P = [&](int k) { return V3(KEYS[k].x, KEYS[k].y, KEYS[k].z); };
    V3 p0 = P(i), p1 = P(i + 1), p2 = P(i + 2), p3 = P(i + 3);
    F t2 = t * t, t3 = t2 * t;
    return (p1 * 2 + (p2 - p0) * t + (p0 * 2 - p1 * 5 + p2 * 4 - p3) * t2 + (p1 * 3 - p0 - p2 * 3 + p3) * t3) * 0.5f;
}

// arc-length reparametrisation table
static std::vector<F> ARC;
static void buildArc() {
    const int N = 4000;
    ARC.resize(N + 1);
    ARC[0] = 0;
    V3 prev = catmull(0);
    for (int i = 1; i <= N; ++i) { V3 c = catmull((F)i / N); ARC[i] = ARC[i - 1] + len(c - prev); prev = c; }
}
static F arcToU(F s) {
    F L = ARC.back() * s;
    int i = (int)(std::lower_bound(ARC.begin(), ARC.end(), L) - ARC.begin());
    i = std::max(1, std::min((int)ARC.size() - 1, i));
    F f = (L - ARC[i - 1]) / std::max(1e-6f, ARC[i] - ARC[i - 1]);
    return ((i - 1) + f) / (ARC.size() - 1);
}

struct Cam { V3 pos, fwd, right, up; F tanHalf; };
static Cam cameraAt(F time, F T) {
    F s = time / T;
    s = 0.08f * s + 0.92f * (s * s * (3 - 2 * s)) * 0 + 0.92f * s;  // near-constant speed
    F u = arcToU(clamp01(s));
    F ua = arcToU(clamp01(s + 0.06f));
    F ub = arcToU(clamp01(s - 0.035f));
    V3 pos = catmull(u), ahead = catmull(ua), behind = catmull(ub);
    // keep clearance above terrain / water
    F g = std::max(heightAt(pos.x, pos.z, 0), 0.f);
    for (int k = 1; k <= 4; ++k) { V3 q = catmull(arcToU(clamp01(s + 0.01f * k))); g = std::max(g, heightAt(q.x, q.z, 0) - 4.f * k); }
    pos.y = std::max(pos.y, g + 28.f);
    V3 tgt = ahead + V3(0, -0.07f * len(ahead - pos) - 2.f, 0);
    Cam c;
    c.pos = pos;
    c.fwd = norm(tgt - pos);
    // bank into turns: lateral curvature
    V3 d1 = norm(ahead - pos), d0 = norm(pos - behind);
    F turn = cross(d0, d1).y;
    F roll = std::max(-0.35f, std::min(0.35f, -turn * 3.5f));
    V3 r0 = norm(cross(c.fwd, V3(0, 1, 0)));
    V3 u0 = cross(r0, c.fwd);
    c.right = r0 * std::cos(roll) + u0 * std::sin(roll);
    c.up = cross(c.right, c.fwd);
    c.tanHalf = std::tan(0.5f * 62.f * PI / 180.f);
    return c;
}

// ------------------------------------------------------------------ post
static inline F aces(F x) { const F a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f; return clamp01((x * (a * x + b)) / (x * (c * x + d) + e)); }
static inline F toSrgb(F c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f; }

static void blur(std::vector<V3>& img, int W, int H, F sigma) {
    int r = (int)std::ceil(sigma * 3);
    std::vector<F> k(2 * r + 1);
    F s = 0;
    for (int i = -r; i <= r; ++i) { k[i + r] = std::exp(-0.5f * i * i / (sigma * sigma)); s += k[i + r]; }
    for (auto& v : k) v /= s;
    std::vector<V3> tmp(W * H);
    #pragma omp parallel for
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { V3 a(0); for (int i = -r; i <= r; ++i) a += img[y * W + std::min(W - 1, std::max(0, x + i))] * k[i + r]; tmp[y * W + x] = a; }
    #pragma omp parallel for
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { V3 a(0); for (int i = -r; i <= r; ++i) a += tmp[std::min(H - 1, std::max(0, y + i)) * W + x] * k[i + r]; img[y * W + x] = a; }
}

int main(int argc, char** argv) {
    if (argc == 2) {  // probe terrain along camera path
        buildArc();
        F mx = -1e9;
        for (int x = -900; x <= 900; x += 5) for (int z = -900; z <= 900; z += 5) mx = std::max(mx, heightAt(x, z, 0));
        printf("max terrain %.1f\n", mx);
        for (int i = 0; i <= 30; ++i) { F u = arcToU(i / 30.f); V3 p = catmull(u); printf("s=%.2f pos=(%.0f %.0f %.0f) ground=%.1f\n", i / 30.f, p.x, p.y, p.z, heightAt(p.x, p.z, 0)); }
        printf("path length %.0f\n", ARC.back());
        return 0;
    }
    if (argc < 8) { fprintf(stderr, "usage: %s outdir W H spp first last total\n", argv[0]); return 1; }
    std::string out = argv[1];
    int W = atoi(argv[2]), H = atoi(argv[3]), spp = atoi(argv[4]), f0 = atoi(argv[5]), f1 = atoi(argv[6]), total = atoi(argv[7]);
    mkdir(out.c_str(), 0755);
    buildArc();
    F T = total / FPS;
    const F exposure = 0.62f;
    for (int fr = f0; fr <= f1; ++fr) {
        char path[512];
        snprintf(path, sizeof path, "%s/f%04d.ppm", out.c_str(), fr);
        if (FILE* e = fopen(path, "rb")) { fclose(e); continue; }
        F time = fr / FPS;
        std::vector<V3> img(W * H);
        #pragma omp parallel for schedule(dynamic, 1)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                Rng rng(((uint64_t)fr * 1315423911u) ^ ((uint64_t)y * 2654435761u + x * 97u + 1));
                V3 acc(0);
                for (int s = 0; s < spp; ++s) {
                    F t = time + (rng.u() * 0.5f) / FPS;  // motion blur
                    Cam c = cameraAt(t, T);
                    F sx, sy;
                    if (spp == 1) { sx = 0.5f; sy = 0.5f; }
                    else { int g = (int)std::ceil(std::sqrt((F)spp)); sx = ((s % g) + rng.u()) / g; sy = ((s / g % g) + rng.u()) / g; }
                    F px = (2 * (x + sx) / W - 1) * c.tanHalf * W / H;
                    F py = (1 - 2 * (y + sy) / H) * c.tanHalf;
                    V3 rd = norm(c.fwd + c.right * px + c.up * py);
                    V3 col = render(c.pos, rd, t, rng);
                    if (!std::isfinite(col.x) || !std::isfinite(col.y) || !std::isfinite(col.z)) col = V3(0);
                    acc += V3(std::min(col.x, 60.f), std::min(col.y, 60.f), std::min(col.z, 60.f));
                }
                img[y * W + x] = acc / (F)spp * exposure;
            }
        std::vector<V3> bl(W * H);
        for (int i = 0; i < W * H; ++i) { F l = lum(img[i]); bl[i] = l > 1.2f ? img[i] * ((l - 1.2f) / l) : V3(0); }
        blur(bl, W, H, 14.f * W / 1280.f);
        std::vector<unsigned char> o(W * H * 3);
        Rng gr(fr * 7 + 3);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int i = y * W + x;
                V3 c = img[i] + bl[i] * 0.25f;
                F dx = (x + 0.5f) / W - 0.5f, dy = ((y + 0.5f) / H - 0.5f) * H / W;
                c = c * (1 - 0.55f * (dx * dx + dy * dy));
                // gentle warm grade
                c = V3(c.x * 1.03f, c.y, c.z * 0.97f);
                F g = (gr.u() - 0.5f) * 0.01f;
                F ch[3] = {c.x, c.y, c.z};
                for (int k = 0; k < 3; ++k) o[i * 3 + k] = (unsigned char)std::lround(clamp01(toSrgb(aces(ch[k])) + g + (gr.u() - 0.5f) / 255.f) * 255);
            }
        std::string tmp = std::string(path) + ".tmp";
        FILE* fp = fopen(tmp.c_str(), "wb");
        fprintf(fp, "P6\n%d %d\n255\n", W, H);
        fwrite(o.data(), 1, o.size(), fp);
        fclose(fp);
        rename(tmp.c_str(), path);
        fprintf(stderr, "frame %d done\n", fr);
    }
}
