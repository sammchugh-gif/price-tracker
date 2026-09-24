// cradle.cpp - physically based Monte Carlo path tracer rendering an animated
// Newton's cradle. GGX microfacet BSDFs, MIS next-event estimation on area
// lights, thin-lens depth of field, shutter motion blur, bloom, ACES tonemap.
//
// build: g++ -O3 -march=native -fopenmp -o cradle cradle.cpp  (no -ffast-math: it breaks isfinite)
// usage: ./cradle <outdir> <width> <height> <spp> <firstFrame> <lastFrame>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <sys/stat.h>

static const double PI = 3.14159265358979323846;
static const double FPS = 30.0;

struct V3 {
    double x, y, z;
    V3() : x(0), y(0), z(0) {}
    V3(double a) : x(a), y(a), z(a) {}
    V3(double a, double b, double c) : x(a), y(b), z(c) {}
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(const V3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
    V3 operator/(double s) const { return {x / s, y / s, z / s}; }
    V3 operator-() const { return {-x, -y, -z}; }
    V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    V3& operator*=(const V3& o) { x *= o.x; y *= o.y; z *= o.z; return *this; }
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
static inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static inline double len(const V3& a) { return std::sqrt(dot(a, a)); }
static inline V3 norm(const V3& a) { return a / len(a); }
static inline double lum(const V3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }
static inline double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline V3 lerp(const V3& a, const V3& b, double t) { return a * (1 - t) + b * t; }
static inline double smooth(double a, double b, double x) { double t = clamp01((x - a) / (b - a)); return t * t * (3 - 2 * t); }
static inline V3 vmax(const V3& a, double m) { return {std::max(a.x, m), std::max(a.y, m), std::max(a.z, m)}; }
static inline V3 vabs(const V3& a) { return {std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}; }

// ---------------------------------------------------------------- RNG
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL) { next(); next(); }
    uint32_t next() {
        uint64_t old = s;
        s = old * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t xs = (uint32_t)(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = (uint32_t)(old >> 59u);
        return (xs >> rot) | (xs << ((-rot) & 31));
    }
    double u() { return (next() >> 8) * (1.0 / 16777216.0); }
};

// ---------------------------------------------------------------- noise
static inline double hash3(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) / 16777215.0;
}
static double vnoise(const V3& p) {
    int ix = (int)std::floor(p.x), iy = (int)std::floor(p.y), iz = (int)std::floor(p.z);
    double fx = p.x - ix, fy = p.y - iy, fz = p.z - iz;
    double ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy), uz = fz * fz * (3 - 2 * fz);
    double r = 0;
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                double w = (dx ? ux : 1 - ux) * (dy ? uy : 1 - uy) * (dz ? uz : 1 - uz);
                r += w * hash3(ix + dx, iy + dy, iz + dz);
            }
    return r;
}
static double fbm(V3 p, int oct) {
    double a = 0.5, s = 0;
    for (int i = 0; i < oct; ++i) { s += a * vnoise(p); p = p * 2.03 + V3(17.1, 9.2, 3.7); a *= 0.5; }
    return s;
}

// ---------------------------------------------------------------- scene
enum MatId { M_BALL, M_FRAME, M_STRING, M_WOOD, M_FLOOR, M_LIGHT };

struct Hit {
    double t = 1e30;
    V3 p, n;
    int mat = -1;
    int light = -1;
};

struct Light {
    V3 c0, eu, ev, n, center, Le;
    double area;
};

struct Capsule { V3 a, b; double r; int mat; };

// Geometry constants (scene units ~ 10 cm)
static const double R_BALL = 0.25;
static const double Y_BASE = 0.25;
static const double Y_TOP = 2.75;
static const double Z_RAIL = 0.70;
static const double X_POST = 1.60;
static const double L_PEND = 1.70;
static const V3 BASE_C(0, Y_BASE * 0.5, 0), BASE_B(1.85, Y_BASE * 0.5, 0.95);
static const double BASE_R = 0.05;

struct Scene {
    std::vector<Light> lights;
    std::vector<double> lightCdf;
    std::vector<Capsule> frame;
};

static Light makeLight(V3 center, V3 target, double w, double h, V3 Le) {
    Light L;
    V3 n = norm(target - center);
    V3 up = std::fabs(n.y) > 0.99 ? V3(0, 0, 1) : V3(0, 1, 0);
    V3 u = norm(cross(up, n));
    V3 v = cross(n, u);
    L.eu = u * w; L.ev = v * h; L.n = n; L.center = center;
    L.c0 = center - L.eu * 0.5 - L.ev * 0.5;
    L.area = w * h; L.Le = Le;
    return L;
}

static Scene buildScene() {
    Scene s;
    V3 tgt(0, 1.2, 0);
    s.lights.push_back(makeLight(V3(-3.6, 5.8, 4.2), tgt, 3.6, 2.6, V3(1.0, 0.94, 0.86) * 7.0));   // key softbox
    s.lights.push_back(makeLight(V3(3.2, 6.6, -5.2), tgt, 4.5, 1.2, V3(0.80, 0.90, 1.0) * 11.0));  // cool rim strip
    s.lights.push_back(makeLight(V3(5.2, 2.2, 2.2), tgt, 0.8, 4.2, V3(1.0, 0.97, 0.93) * 6.0));    // side strip
    s.lights.push_back(makeLight(V3(0.0, 8.0, 0.5), V3(0, 0, 0.5), 7.0, 5.0, V3(1.0) * 0.9));      // overhead
    double acc = 0;
    for (auto& l : s.lights) { acc += lum(l.Le) * l.area; s.lightCdf.push_back(acc); }
    for (auto& c : s.lightCdf) c /= acc;

    double rf = 0.045;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2)
            s.frame.push_back({V3(sx * X_POST, Y_BASE - 0.02, sz * Z_RAIL), V3(sx * X_POST, Y_TOP, sz * Z_RAIL), rf, M_FRAME});
    for (int sz = -1; sz <= 1; sz += 2)
        s.frame.push_back({V3(-X_POST, Y_TOP, sz * Z_RAIL), V3(X_POST, Y_TOP, sz * Z_RAIL), rf, M_FRAME});
    return s;
}

// Animated state at time t
struct Dyn { V3 ball[5]; };

static Dyn animate(double t) {
    Dyn d;
    const double period = 1.9;          // slightly slowed from real time for drama
    double w = 2 * PI / period;
    double A = (34.0 * PI / 180.0) * std::exp(-t * 0.015);
    double s = A * std::sin(w * t + 1.3 * PI);
    double th[5] = {0, 0, 0, 0, 0};
    if (s < 0) { th[0] = s; th[1] = 0.035 * s; }
    else { th[4] = s; th[3] = 0.035 * s; }
    for (int i = 0; i < 5; ++i) {
        double x = (i - 2) * (2 * R_BALL + 0.002);
        d.ball[i] = V3(x + L_PEND * std::sin(th[i]), Y_TOP - L_PEND * std::cos(th[i]), 0);
    }
    return d;
}

// ---------------------------------------------------------------- intersection
static inline double isectSphere(const V3& ro, const V3& rd, const V3& c, double r) {
    V3 oc = ro - c;
    double b = dot(oc, rd), cc = dot(oc, oc) - r * r, h = b * b - cc;
    if (h < 0) return -1;
    h = std::sqrt(h);
    double t = -b - h;
    if (t > 1e-6) return t;
    t = -b + h;
    return t > 1e-6 ? t : -1;
}

static inline double isectCapsule(const V3& ro, const V3& rd, const V3& pa, const V3& pb, double r) {
    V3 ba = pb - pa, oa = ro - pa;
    double baba = dot(ba, ba), bard = dot(ba, rd), baoa = dot(ba, oa), rdoa = dot(rd, oa), oaoa = dot(oa, oa);
    double a = baba - bard * bard, b = baba * rdoa - baoa * bard, c = baba * oaoa - baoa * baoa - r * r * baba;
    double h = b * b - a * c;
    if (h >= 0.0) {
        double t = (-b - std::sqrt(h)) / a;
        double y = baoa + t * bard;
        if (y > 0.0 && y < baba) return t;
        V3 oc = (y <= 0.0) ? oa : ro - pb;
        b = dot(rd, oc);
        c = dot(oc, oc) - r * r;
        h = b * b - c;
        if (h > 0.0) return -b - std::sqrt(h);
    }
    return -1;
}
static inline V3 capsuleNormal(const V3& p, const V3& a, const V3& b, double r) {
    V3 ba = b - a, pa = p - a;
    double h = clamp01(dot(pa, ba) / dot(ba, ba));
    (void)r;
    return norm(pa - ba * h);
}

static inline bool slab(const V3& ro, const V3& rd, const V3& lo, const V3& hi, double& t0, double& t1) {
    t0 = 0; t1 = 1e30;
    for (int i = 0; i < 3; ++i) {
        double inv = 1.0 / rd[i];
        double a = (lo[i] - ro[i]) * inv, b = (hi[i] - ro[i]) * inv;
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a); t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    return true;
}

static inline double sdBase(const V3& p) {
    V3 q = vabs(p - BASE_C) - BASE_B + V3(BASE_R);
    return len(vmax(q, 0.0)) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0) - BASE_R;
}

static bool intersect(const Scene& S, const Dyn& D, const V3& ro, const V3& rd, double tmax, Hit& h, bool anyHit) {
    h.t = tmax;
    bool found = false;
    // floor
    if (rd.y < 0) {
        double t = -ro.y / rd.y;
        if (t > 1e-6 && t < h.t) { h.t = t; h.n = V3(0, 1, 0); h.mat = M_FLOOR; h.light = -1; found = true; if (anyHit) return true; }
    }
    // wooden base (rounded box, sphere-traced inside its bounds)
    double t0, t1;
    if (slab(ro, rd, BASE_C - BASE_B, BASE_C + BASE_B, t0, t1) && t0 < h.t) {
        double t = std::max(t0, 1e-6);
        for (int i = 0; i < 96 && t < std::min(t1, h.t); ++i) {
            double d = sdBase(ro + rd * t);
            if (d < 1e-6) {
                V3 p = ro + rd * t;
                const double e = 1e-5;
                V3 n(sdBase(p + V3(e, 0, 0)) - sdBase(p - V3(e, 0, 0)),
                     sdBase(p + V3(0, e, 0)) - sdBase(p - V3(0, e, 0)),
                     sdBase(p + V3(0, 0, e)) - sdBase(p - V3(0, 0, e)));
                h.t = t; h.n = norm(n); h.mat = M_WOOD; h.light = -1; found = true;
                if (anyHit) return true;
                break;
            }
            t += d;
        }
    }
    // cradle bounds
    if (slab(ro, rd, V3(-2.3, 0.2, -0.8), V3(2.3, 2.85, 0.8), t0, t1) && t0 < h.t) {
        for (int i = 0; i < 5; ++i) {
            double t = isectSphere(ro, rd, D.ball[i], R_BALL);
            if (t > 0 && t < h.t) { h.t = t; h.n = norm(ro + rd * t - D.ball[i]); h.mat = M_BALL; h.light = -1; found = true; if (anyHit) return true; }
        }
        for (const Capsule& c : S.frame) {
            double t = isectCapsule(ro, rd, c.a, c.b, c.r);
            if (t > 1e-6 && t < h.t) { h.t = t; h.n = capsuleNormal(ro + rd * t, c.a, c.b, c.r); h.mat = c.mat; h.light = -1; found = true; if (anyHit) return true; }
        }
        for (int i = 0; i < 5; ++i) {
            double x0 = (i - 2) * (2 * R_BALL + 0.002);
            for (int sz = -1; sz <= 1; sz += 2) {
                V3 a(x0, Y_TOP, sz * Z_RAIL);
                const double rs = 0.0065;
                double t = isectCapsule(ro, rd, a, D.ball[i], rs);
                if (t > 1e-6 && t < h.t) { h.t = t; h.n = capsuleNormal(ro + rd * t, a, D.ball[i], rs); h.mat = M_STRING; h.light = -1; found = true; if (anyHit) return true; }
            }
        }
    }
    // area lights (two-sided geometry, emit from front only)
    for (int i = 0; i < (int)S.lights.size(); ++i) {
        const Light& L = S.lights[i];
        double dn = dot(rd, L.n);
        if (std::fabs(dn) < 1e-9) continue;
        double t = dot(L.center - ro, L.n) / dn;
        if (t < 1e-6 || t >= h.t) continue;
        V3 d = ro + rd * t - L.c0;
        double a = dot(d, L.eu) / dot(L.eu, L.eu), b = dot(d, L.ev) / dot(L.ev, L.ev);
        if (a < 0 || a > 1 || b < 0 || b > 1) continue;
        h.t = t; h.n = L.n; h.mat = M_LIGHT; h.light = i; found = true;
        if (anyHit) return true;
    }
    if (found) h.p = ro + rd * h.t;
    return found;
}

// ---------------------------------------------------------------- materials
struct Mat {
    bool metal;
    V3 base;       // albedo (dielectric) or F0 (metal)
    double alpha;  // GGX roughness (alpha = r^2)
};

static V3 woodColor(const V3& p) {
    V3 q = p * V3(1.0, 1.0, 1.0);
    double warp = fbm(q * V3(0.6, 2.5, 2.5), 4) * 1.6;
    double r = std::sqrt((q.y + 1.8) * (q.y + 1.8) + (q.z - 0.7) * (q.z - 0.7)) * 9.0 + warp * 2.2;
    double ring = r - std::floor(r);
    ring = smooth(0.0, 0.75, ring) * (1 - smooth(0.85, 1.0, ring));
    double grain = fbm(V3(q.x * 1.5, q.y * 70, q.z * 70), 3);
    V3 light(0.215, 0.098, 0.042), dark(0.072, 0.030, 0.013);
    V3 c = lerp(dark, light, 0.35 + 0.55 * ring);
    c = c * (0.8 + 0.4 * grain);
    return c;
}

static Mat material(int id, const V3& p) {
    switch (id) {
        case M_BALL: return {true, V3(0.62, 0.62, 0.63), 0.012};
        case M_FRAME: return {true, V3(0.56, 0.57, 0.58), 0.09};
        case M_STRING: return {false, V3(0.02, 0.02, 0.022), 0.35};
        case M_WOOD: return {false, woodColor(p), 0.035};
        case M_FLOOR: {
            double smudge = fbm(p * 1.3, 4);
            return {false, V3(0.006, 0.006, 0.007), 0.012 + 0.10 * smooth(0.45, 0.8, smudge)};
        }
    }
    return {false, V3(0.5), 0.5};
}

static inline V3 fresnelSchlick(const V3& F0, double c) {
    double m = std::pow(1 - clamp01(c), 5);
    return F0 + (V3(1) - F0) * m;
}
static inline double D_ggx(double noh, double a2) {
    double d = noh * noh * (a2 - 1) + 1;
    return a2 / (PI * d * d);
}
static inline double G1(double nov, double a2) {
    return 2 * nov / (nov + std::sqrt(a2 + (1 - a2) * nov * nov));
}

struct Frame {
    V3 t, b, n;
    explicit Frame(const V3& n_) : n(n_) {
        double s = n.z >= 0 ? 1.0 : -1.0;
        double a = -1.0 / (s + n.z), b0 = n.x * n.y * a;
        t = V3(1 + s * n.x * n.x * a, s * b0, -s * n.x);
        b = V3(b0, s + n.y * n.y * a, -n.y);
    }
    V3 toLocal(const V3& v) const { return {dot(v, t), dot(v, b), dot(v, n)}; }
    V3 toWorld(const V3& v) const { return t * v.x + b * v.y + n * v.z; }
};

// probability of choosing the specular lobe for dielectrics
static inline double specProb(const Mat& m, double nov) {
    double F = 0.04 + 0.96 * std::pow(1 - nov, 5);
    double ws = F, wd = lum(m.base) * (1 - F);
    return std::min(0.95, std::max(0.15, ws / (ws + wd)));
}

// evaluate f*cos and pdf (local frame, v and l in upper hemisphere)
static V3 evalBsdf(const Mat& m, const V3& v, const V3& l, double& pdf) {
    pdf = 0;
    if (v.z <= 0 || l.z <= 0) return V3(0);
    V3 h = norm(v + l);
    double a2 = m.alpha * m.alpha;
    double D = D_ggx(h.z, a2), g1v = G1(v.z, a2), g1l = G1(l.z, a2);
    double voh = std::max(1e-9, dot(v, h));
    double pdfSpec = g1v * D / (4 * v.z);
    if (m.metal) {
        V3 F = fresnelSchlick(m.base, voh);
        pdf = pdfSpec;
        return F * (D * g1v * g1l / (4 * v.z));
    }
    double F = 0.04 + 0.96 * std::pow(1 - voh, 5);
    double ps = specProb(m, v.z);
    pdf = ps * pdfSpec + (1 - ps) * l.z / PI;
    double Fv = 0.04 + 0.96 * std::pow(1 - v.z, 5), Fl = 0.04 + 0.96 * std::pow(1 - l.z, 5);
    V3 diff = m.base * ((1 - Fv) * (1 - Fl) / PI * l.z);
    return diff + V3(F * D * g1v * g1l / (4 * v.z));
}

static V3 sampleVndf(const V3& v, double alpha, double u1, double u2) {
    V3 vh = norm(V3(alpha * v.x, alpha * v.y, v.z));
    double lensq = vh.x * vh.x + vh.y * vh.y;
    V3 T1 = lensq > 0 ? V3(-vh.y, vh.x, 0) / std::sqrt(lensq) : V3(1, 0, 0);
    V3 T2 = cross(vh, T1);
    double r = std::sqrt(u1), phi = 2 * PI * u2;
    double t1 = r * std::cos(phi), t2 = r * std::sin(phi);
    double s = 0.5 * (1 + vh.z);
    t2 = (1 - s) * std::sqrt(std::max(0.0, 1 - t1 * t1)) + s * t2;
    V3 nh = T1 * t1 + T2 * t2 + vh * std::sqrt(std::max(0.0, 1 - t1 * t1 - t2 * t2));
    return norm(V3(alpha * nh.x, alpha * nh.y, std::max(1e-6, nh.z)));
}

static bool sampleBsdf(const Mat& m, const V3& v, Rng& rng, V3& l) {
    bool spec = m.metal || rng.u() < specProb(m, v.z);
    if (spec) {
        V3 h = sampleVndf(v, m.alpha, rng.u(), rng.u());
        l = h * (2 * dot(v, h)) - v;
    } else {
        double r = std::sqrt(rng.u()), ph = 2 * PI * rng.u();
        l = V3(r * std::cos(ph), r * std::sin(ph), std::sqrt(std::max(0.0, 1 - r * r)));
    }
    return l.z > 0;
}

// ---------------------------------------------------------------- lights & env
static V3 envRadiance(const V3& d) {
    // dark studio: faint warm falloff behind, near-black above
    double up = clamp01(d.y * 0.5 + 0.5);
    V3 c = lerp(V3(0.010, 0.009, 0.008), V3(0.002, 0.002, 0.0025), up);
    // soft warm backdrop glow behind the subject
    V3 g = norm(V3(-0.25, 0.12, -1));
    double k = std::max(0.0, dot(d, g));
    c += V3(0.030, 0.021, 0.015) * std::pow(k, 8.0) * smooth(0.0, 0.35, d.y);
    return c;
}

static double lightPdf(const Scene& S, int i, const V3& from, const V3& p) {
    const Light& L = S.lights[i];
    V3 d = p - from;
    double d2 = dot(d, d);
    double cosl = -dot(norm(d), L.n);
    if (cosl <= 0) return 0;
    double pSel = S.lightCdf[i] - (i ? S.lightCdf[i - 1] : 0);
    return pSel * d2 / (cosl * L.area);
}

// ---------------------------------------------------------------- integrator
static V3 trace(const Scene& S, const Dyn& D, V3 ro, V3 rd, Rng& rng) {
    V3 L(0), thr(1);
    double lastPdf = 0;
    V3 lastP = ro;
    for (int depth = 0; depth < 10; ++depth) {
        Hit h;
        if (!intersect(S, D, ro, rd, 1e30, h, false)) {
            L += thr * envRadiance(rd);
            break;
        }
        if (h.mat == M_LIGHT) {
            const Light& Lt = S.lights[h.light];
            if (dot(rd, Lt.n) < 0) {
                double w = 1;
                if (depth > 0) {
                    double pl = lightPdf(S, h.light, lastP, h.p);
                    w = lastPdf * lastPdf / (lastPdf * lastPdf + pl * pl);
                }
                V3 c = thr * Lt.Le * w;
                if (depth > 1) { double m = lum(c); if (m > 6) c = c * (6 / m); }
                L += c;
            }
            break;
        }
        V3 n = norm(h.n);
        if (dot(n, rd) > 0) n = -n;
        Mat m = material(h.mat, h.p);
        Frame F(n);
        V3 v = F.toLocal(-rd);
        if (v.z <= 1e-6) break;
        V3 P = h.p + n * 2e-5;

        // next-event estimation
        {
            double u = rng.u();
            int li = 0;
            while (li < (int)S.lights.size() - 1 && u > S.lightCdf[li]) ++li;
            const Light& Lt = S.lights[li];
            V3 lp = Lt.c0 + Lt.eu * rng.u() + Lt.ev * rng.u();
            V3 dl = lp - P;
            double dist = len(dl);
            dl = dl / dist;
            double cosl = -dot(dl, Lt.n);
            V3 ll = F.toLocal(dl);
            if (cosl > 0 && ll.z > 0) {
                double pb;
                V3 f = evalBsdf(m, v, ll, pb);
                if (lum(f) > 0) {
                    Hit sh;
                    if (!intersect(S, D, P, dl, dist * (1 - 1e-5), sh, true)) {
                        double pl = lightPdf(S, li, P, lp);
                        double w = pl * pl / (pl * pl + pb * pb);
                        V3 c = thr * f * Lt.Le * (w / pl);
                        if (depth > 0) { double mm = lum(c); if (mm > 6) c = c * (6 / mm); }
                        L += c;
                    }
                }
            }
        }

        V3 ll;
        if (!sampleBsdf(m, v, rng, ll)) break;
        double pdf;
        V3 f = evalBsdf(m, v, ll, pdf);
        if (pdf <= 0 || lum(f) <= 0) break;
        thr *= f / pdf;
        lastPdf = pdf;
        lastP = P;
        ro = P;
        rd = F.toWorld(ll);
        if (depth >= 3) {
            double q = std::min(0.95, lum(thr));
            if (rng.u() > q) break;
            thr = thr / q;
        }
    }
    return L;
}

// ---------------------------------------------------------------- camera
struct Cam { V3 pos, fwd, right, up; double tanHalf, aperture, focus; };

static Cam cameraAt(double t, double T) {
    double u = smooth(0, 1, t / T);
    V3 tgt(0.05, 1.42 - 0.06 * u, 0);
    double az = (-40 + 66 * u) * PI / 180.0;
    double R = 8.0 - 1.0 * u;
    double el = 0.85 - 0.55 * u;
    Cam c;
    c.pos = tgt + V3(R * std::sin(az), el, R * std::cos(az));
    c.fwd = norm(tgt - c.pos);
    c.right = norm(cross(c.fwd, V3(0, 1, 0)));
    c.up = cross(c.right, c.fwd);
    c.tanHalf = std::tan(0.5 * 30.0 * PI / 180.0);
    c.aperture = 0.045;
    c.focus = len(V3(0, 1.05, 0) - c.pos) - 0.1;
    return c;
}

// ---------------------------------------------------------------- post
static inline double aces(double x) {
    const double a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp01((x * (a * x + b)) / (x * (c * x + d) + e));
}
static inline double toSrgb(double c) {
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

static void blurH(const std::vector<V3>& in, std::vector<V3>& out, int W, int H, const std::vector<double>& k) {
    int r = (int)k.size() / 2;
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            V3 s(0);
            for (int i = -r; i <= r; ++i) { int xx = std::min(W - 1, std::max(0, x + i)); s += in[y * W + xx] * k[i + r]; }
            out[y * W + x] = s;
        }
}
static void blurV(const std::vector<V3>& in, std::vector<V3>& out, int W, int H, const std::vector<double>& k) {
    int r = (int)k.size() / 2;
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            V3 s(0);
            for (int i = -r; i <= r; ++i) { int yy = std::min(H - 1, std::max(0, y + i)); s += in[yy * W + x] * k[i + r]; }
            out[y * W + x] = s;
        }
}
static std::vector<double> gauss(double sigma) {
    int r = (int)std::ceil(sigma * 3);
    std::vector<double> k(2 * r + 1);
    double s = 0;
    for (int i = -r; i <= r; ++i) { k[i + r] = std::exp(-0.5 * i * i / (sigma * sigma)); s += k[i + r]; }
    for (auto& v : k) v /= s;
    return k;
}

int main(int argc, char** argv) {
    if (argc < 7) { fprintf(stderr, "usage: %s outdir W H spp first last [totalFrames]\n", argv[0]); return 1; }
    std::string outdir = argv[1];
    int W = atoi(argv[2]), H = atoi(argv[3]), spp = atoi(argv[4]);
    int f0 = atoi(argv[5]), f1 = atoi(argv[6]);
    int total = argc > 7 ? atoi(argv[7]) : 240;
    double T = total / FPS;
    mkdir(outdir.c_str(), 0755);
    Scene S = buildScene();
    const double exposure = 1.9;
    const double shutter = 0.5 / FPS;  // 180-degree shutter

    for (int fr = f0; fr <= f1; ++fr) {
        char path[512];
        snprintf(path, sizeof path, "%s/f%04d.ppm", outdir.c_str(), fr);
        if (FILE* ex = fopen(path, "rb")) { fclose(ex); continue; }
        double tFrame = fr / FPS;
        Cam cam = cameraAt(tFrame, T);
        std::vector<V3> img(W * H);
        #pragma omp parallel for schedule(dynamic, 1)
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                Rng rng(((uint64_t)fr * 7919 + y) * 104729 + x);
                V3 acc(0);
                for (int s = 0; s < spp; ++s) {
                    double t = tFrame + shutter * rng.u();
                    Dyn D = animate(t);
                    double px = (2 * (x + rng.u()) / W - 1) * cam.tanHalf * W / H;
                    double py = (1 - 2 * (y + rng.u()) / H) * cam.tanHalf;
                    V3 dir = norm(cam.fwd + cam.right * px + cam.up * py);
                    V3 fp = cam.pos + dir * (cam.focus / dot(dir, cam.fwd));
                    double r = cam.aperture * std::sqrt(rng.u()), ph = 2 * PI * rng.u();
                    V3 o = cam.pos + cam.right * (r * std::cos(ph)) + cam.up * (r * std::sin(ph));
                    V3 c = trace(S, D, o, norm(fp - o), rng);
                    if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z)) c = V3(0);
                    acc += c;
                }
                img[y * W + x] = acc / spp;
            }
        }
        // bloom: two gaussian scales of the bright pass
        std::vector<V3> bright(W * H), tmp(W * H), b1(W * H), b2(W * H);
        for (int i = 0; i < W * H; ++i) { V3 c = img[i] * exposure; double l = lum(c); bright[i] = l > 0.9 ? c * ((l - 0.9) / l) : V3(0); }
        double sc = W / 1280.0;
        auto k1 = gauss(4 * sc), k2 = gauss(22 * sc);
        blurH(bright, tmp, W, H, k1); blurV(tmp, b1, W, H, k1);
        blurH(bright, tmp, W, H, k2); blurV(tmp, b2, W, H, k2);

        std::vector<unsigned char> out(W * H * 3);
        Rng grain(fr * 31 + 7);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int i = y * W + x;
                V3 c = img[i] * exposure + b1[i] * 0.12 + b2[i] * 0.10;
                double dx = (x + 0.5) / W - 0.5, dy = ((y + 0.5) / H - 0.5) * H / W;
                double vig = 1.0 - 0.9 * (dx * dx + dy * dy);
                c = c * vig;
                double g = (grain.u() - 0.5) * 0.012;
                double ch[3] = {c.x, c.y, c.z};
                for (int k = 0; k < 3; ++k) {
                    double v = toSrgb(aces(ch[k])) + g + (grain.u() - 0.5) / 255.0;
                    out[i * 3 + k] = (unsigned char)std::lround(clamp01(v) * 255);
                }
            }
        std::string tmpPath = std::string(path) + ".tmp";
        FILE* fp = fopen(tmpPath.c_str(), "wb");
        fprintf(fp, "P6\n%d %d\n255\n", W, H);
        fwrite(out.data(), 1, out.size(), fp);
        fclose(fp);
        rename(tmpPath.c_str(), path);
        fprintf(stderr, "frame %d done\n", fr);
    }
    return 0;
}
