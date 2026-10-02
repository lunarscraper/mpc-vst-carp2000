/* =============================================================================
 * carp_core.h - carp 2000, phase 1: the monophonic base voice of a semi-modular synthesizer
 * in the manner of the ARP 2600. One Voice = 3 VCOs -> filter mixer -> 4-pole ladder -> VCA,
 * with ADSR and AR. The plug-in around it is carp_vst.cpp. MIT license.
 *
 *   VCO 1 (saw, square)  <- FM: ADSR, VCO 2 sine
 *   VCO 2 (sine, triangle, pulse)  <- FM: ADSR, VCO 1 square; PWM: noise
 *   VCO 3 (saw)  <- FM: ADSR, VCO 2 sine, noise
 *   mixer: VCO 1 square, VCO 2 pulse, VCO 3 saw, noise, VCO 1 saw, VCO 2 triangle
 *   VCF: cutoff <- keyboard, ADSR, velocity, VCO 2 sine (audio rate)
 *   VCA: initial gain + AR (linear) + ADSR (exponential)
 *
 * Performance rules (README): float only, no libm in the audio loop, slow modulation every
 * CTL samples with linear ramps in between, audio-rate maths only where a VCO or noise is the
 * source, silent mixer channels and an idle voice are skipped, 2x oversampling in the filter only.
 * Voice holds no global state, so phase 3 can run four of them.
 * ========================================================================== */
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace carp {

static const int CTL = 16;   /* control rate: every 16 samples */

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/* tanh, rational approximation, exact +-1 beyond |x| = 3 */
static inline float fast_tanh(float x) {
    x = clampf(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
/* 2^x, |error| < 0.01 cent */
static inline float fast_exp2(float x) {
    x = clampf(x, -40.0f, 40.0f);
    const int i = (int)(x + 64.5f);                       /* round(x) + 64 */
    const float f = (x - (float)(i - 64)) * 0.69314718f;  /* +-0.5 * ln 2 */
    const float p = 1.0f + f * (1.0f + f * (0.5f + f * (0.16666667f + f * (0.041666667f + f * 0.0083333333f))));
    union { uint32_t u; float fl; } s;
    s.u = (uint32_t)(i - 64 + 127) << 23;
    return p * s.fl;
}
/* tan(x) for 0 <= x <= 0.75 */
static inline float fast_tan(float x) {
    const float x2 = x * x;
    return x * (1.0f + x2 * (0.33333333f + x2 * (0.13333333f + x2 * (0.053968254f + x2 * 0.021869489f))));
}
/* sin(2 pi p), p in [0,1) */
static inline float fast_sin(float p) {
    const float t = p - 0.5f;
    float y = 8.0f * t - 16.0f * t * std::fabs(t);
    y = 0.225f * (y * std::fabs(y) - y) + y;
    return -y;
}
/* PolyBLEP residual for a step at phase 0, phase increment dt */
static inline float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
static inline bool not_finite(float v) {   /* works under -ffast-math */
    uint32_t u; std::memcpy(&u, &v, 4);
    return (u & 0x7f800000u) == 0x7f800000u;
}

enum { MIX_V1SQ, MIX_V2PULSE, MIX_V3SAW, MIX_NOISE, MIX_V1SAW, MIX_V2TRI, NMIX };

/* the panel, in engineering units (carp_vst.cpp maps the parameters onto it) */
struct Patch {
    float semis[3] = {60, 60, 60};  /* coarse + fine as a MIDI note number: the pitch at middle C */
    bool lf[3] = {false, false, false};   /* LF mode: 1/333 of the frequency, keyboard disconnected */
    bool kbd[3] = {true, true, true};
    float fm_adsr[3] = {0, 0, 0};   /* octaves at ADSR = 1 */
    float fm_x[3] = {0, 0, 0};      /* audio-rate FM, octaves: VCO1 <- VCO2 sine, VCO2 <- VCO1 square, VCO3 <- VCO2 sine */
    float fm_noise3 = 0;            /* octaves */
    float pw2 = 0.5f, pwm_noise2 = 0;
    float porta = 0;                /* seconds */
    float lvl[NMIX] = {0, 0, 0, 0, 0, 0};
    float cutoff = 7;               /* octaves above 10 Hz */
    float reso = 0;                 /* 0..1, self-oscillation near the top */
    float f_kbd = 0;                /* 0..1 */
    float f_adsr = 0;               /* octaves at ADSR = 1 */
    float f_vel = 0;                /* octaves at full velocity */
    float f_fm = 0;                 /* octaves, VCO 2 sine */
    float drive = 0.5f;             /* gain into the ladder */
    int f_type = 0;                 /* 0 = 4012, 1 = 4072 (cutoff stops near 11 kHz) */
    float att = 0.001f, dec = 0.3f, sus = 0.6f, rel = 0.2f, ar_att = 0.001f, ar_rel = 0.2f;   /* seconds */
    float gain = 0, vca_ar = 0, vca_adsr = 1;
    float pan = 0;                  /* -1..1 */
    float volume = 0.6f;
};

struct Voice {
    void init(float rate) {
        sr = rate; fs2 = 2 * rate;
        ph[0] = 0; ph[1] = 0.37f; ph[2] = 0.71f;
        s1 = s2 = s3 = s4 = 0; xprev = 0; hp = 0;
        sq1 = sin2 = 0; adsr = ar = 0; gate = false; att_stage = false;
        left = 0; snap = true; vgain = 0;
        hp_c = 1.0f - std::exp(-2.0f * 3.14159265f * 8.0f / sr);
        set_patch(pt);
    }
    void set_patch(const Patch &p) {
        pt = p;
        const float t = (float)CTL / sr;
        c_att = 1 - std::exp(-t / (p.att / 1.47f));
        c_dec = 1 - std::exp(-t / (p.dec / 4));
        c_rel = 1 - std::exp(-t / (p.rel / 4));
        c_aatt = 1 - std::exp(-t / (p.ar_att / 1.47f));
        c_arel = 1 - std::exp(-t / (p.ar_rel / 4));
        c_glide = p.porta < 0.002f ? 1.0f : 1 - std::exp(-t / (p.porta / 4));
        c_smooth = 1 - std::exp(-t / 0.004f);
    }
    void note_on(int n, float velocity, bool retrigger) {
        note = (float)n;
        if (!pitch_set) { pitch = note; pitch_set = true; }
        if (!gate || retrigger) { att_stage = true; vel = velocity; }
        gate = true;
    }
    void note_off() { gate = false; }
    void set_bend(float semitones) { bend = semitones; }
    bool idle() const { return is_idle; }

    void render(float *L, float *R, int n) {
        int i = 0;
        while (i < n) {
            if (left == 0) { tick(); left = CTL; }
            const int m = left < n - i ? left : n - i;
            if (is_idle) { for (int k = 0; k < m; k++) L[i + k] = R[i + k] = 0; }
            else run(L + i, R + i, m);
            left -= m; i += m;
        }
    }

private:
    Patch pt;
    float sr = 44100, fs2 = 88200;
    /* oscillators */
    float ph[3], inc[3] = {0, 0, 0}, dinc[3] = {0, 0, 0};
    float sq1 = 0, sin2 = 0;         /* last sample, the FM sources */
    uint32_t rng = 0x2600u;
    /* filter */
    float s1, s2, s3, s4, xprev, hp, hp_c = 0;
    float G = 0, dG = 0, fc = 100, dfc = 0, fmax = 18000;
    /* envelopes, keyboard */
    float adsr, ar, vel = 1, note = 60, pitch = 60, bend = 0;
    bool gate, att_stage, pitch_set = false, is_idle = true, snap;
    float c_att = 1, c_dec = 1, c_rel = 1, c_aatt = 1, c_arel = 1, c_glide = 1, c_smooth = 1;
    /* smoothed controls */
    float lvl[NMIX] = {0, 0, 0, 0, 0, 0}, cut = 7, k = 0, pw = 0.5f, drv = 0.5f, gl = 0, gr = 0, gain0 = 0;
    float vgain, dvgain = 0;
    int left;

    /* control rate: envelopes, glide, smoothing, the ramp targets for the next CTL samples */
    void tick() {
        const Patch &p = pt;
        if (gate) {
            if (att_stage) { adsr += c_att * (1.3f - adsr); if (adsr >= 1) { adsr = 1; att_stage = false; } }
            else adsr += c_dec * (p.sus - adsr);
            ar += c_aatt * (1.3f - ar); if (ar > 1) ar = 1;
        } else {
            adsr -= c_rel * adsr; if (adsr < 1e-4f) adsr = 0;
            ar -= c_arel * ar; if (ar < 1e-4f) ar = 0;
        }
        pitch += (note - pitch) * c_glide;

        const float cs = snap ? 1.0f : c_smooth;
        for (int j = 0; j < NMIX; j++) lvl[j] += (p.lvl[j] - lvl[j]) * cs;
        cut += (p.cutoff - cut) * cs;
        k += (p.reso * 4.3f - k) * cs;
        pw += (p.pw2 - pw) * cs;
        drv += (p.drive - drv) * cs;
        gain0 += (p.gain - gain0) * cs;
        const float a = (p.pan + 1) * 0.125f;                      /* 0..0.25 = 0..90 degrees, constant power */
        gl += (p.volume * fast_sin(0.25f + a) - gl) * cs;          /* cos */
        gr += (p.volume * fast_sin(a) - gr) * cs;

        const float key = pitch - 60 + bend;
        for (int v = 0; v < 3; v++) {
            const bool track = p.kbd[v] && !p.lf[v];
            const float oct = (p.semis[v] + (track ? key : 0.0f) - 69.0f) * (1.0f / 12.0f) + p.fm_adsr[v] * adsr;
            float f = 440.0f * fast_exp2(oct);
            if (p.lf[v]) f *= 0.003f;
            const float target = clampf(f / sr, 0.0f, 0.45f);
            if (snap) inc[v] = target;
            dinc[v] = (target - inc[v]) * (1.0f / CTL);
        }
        fmax = p.f_type ? 11000.0f : 18000.0f;
        if (fmax > 0.2f * fs2) fmax = 0.2f * fs2;
        const float foct = cut + p.f_kbd * (pitch - 60) * (1.0f / 12.0f) + p.f_adsr * adsr + p.f_vel * vel;
        const float ft = clampf(10.0f * fast_exp2(foct), 10.0f, fmax);
        const float g = fast_tan(3.14159265f * ft / fs2), Gt = g / (1 + g);
        if (snap) { fc = ft; G = Gt; }
        dfc = (ft - fc) * (1.0f / CTL);
        dG = (Gt - G) * (1.0f / CTL);

        float vt = gain0 + p.vca_ar * ar + p.vca_adsr * adsr * adsr;
        if (vt > 1) vt = 1;
        if (snap) vgain = vt;
        dvgain = (vt - vgain) * (1.0f / CTL);

        if (not_finite(s4) || not_finite(hp)) { s1 = s2 = s3 = s4 = 0; xprev = 0; hp = 0; }
        is_idle = !gate && adsr == 0 && ar == 0 && p.gain <= 0 && vt < 1e-5f && vgain < 1e-5f;
        snap = false;
    }

    inline float ladder(float in, float g, float res) {
        /* 4 one-pole TPT stages; the feedback loop is solved for the linear case and the
         * summing node saturates (so resonance and self-oscillation stay bounded and in tune) */
        const float G2 = g * g, G4 = G2 * G2;
        const float S = (1 - g) * (G2 * g * s1 + G2 * s2 + g * s3 + s4);
        const float y4 = (G4 * in + S) / (1 + res * G4);
        const float u = fast_tanh(in - res * y4);
        float v, y;
        v = (u - s1) * g; y = v + s1; s1 = y + v;
        v = (y - s2) * g; y = v + s2; s2 = y + v;
        v = (y - s3) * g; y = v + s3; s3 = y + v;
        v = (y - s4) * g; y = v + s4; s4 = y + v;
        return y;
    }

    void run(float *L, float *R, int m) {
        const Patch &p = pt;
        const float eps = 1e-4f;
        bool on[NMIX];
        for (int j = 0; j < NMIX; j++) on[j] = lvl[j] > eps || p.lvl[j] > eps;
        const bool fm1 = p.fm_x[0] > 0, fm2 = p.fm_x[1] > 0, fm3 = p.fm_x[2] > 0 || p.fm_noise3 > 0;
        const bool ffm = p.f_fm > 0, pwm = p.pwm_noise2 > 0;
        const bool need_sq1 = on[MIX_V1SQ] || fm2;
        const bool need_sin2 = fm1 || p.fm_x[2] > 0 || ffm;
        const float in_gain = drv * (1 + 0.25f * k);
        const float makeup = 1.0f / clampf(drv, 0.35f, 1.0f);
        const float pi_fs2 = 3.14159265f / fs2;
        float p1 = ph[0], p2 = ph[1], p3 = ph[2];

        for (int i = 0; i < m; i++) {
            rng = rng * 1664525u + 1013904223u;
            const float nz = (float)(int32_t)rng * (1.0f / 2147483648.0f);

            float d1 = inc[0], d2 = inc[1], d3 = inc[2];
            if (fm1) { d1 *= fast_exp2(p.fm_x[0] * sin2); if (d1 > 0.45f) d1 = 0.45f; }
            if (fm2) { d2 *= fast_exp2(p.fm_x[1] * sq1); if (d2 > 0.45f) d2 = 0.45f; }
            if (fm3) { d3 *= fast_exp2(p.fm_x[2] * sin2 + p.fm_noise3 * nz); if (d3 > 0.45f) d3 = 0.45f; }
            p1 += d1; if (p1 >= 1) p1 -= 1;
            p2 += d2; if (p2 >= 1) p2 -= 1;
            p3 += d3; if (p3 >= 1) p3 -= 1;

            float x = 1e-5f * nz;                     /* seed for self-oscillation */
            if (need_sq1) {
                float q = p1 + 0.5f; if (q >= 1) q -= 1;
                sq1 = (p1 < 0.5f ? 1.0f : -1.0f) + blep(p1, d1) - blep(q, d1);
                x += lvl[MIX_V1SQ] * sq1;
            }
            if (on[MIX_V1SAW]) x += lvl[MIX_V1SAW] * (2 * p1 - 1 - blep(p1, d1));
            if (need_sin2) sin2 = fast_sin(p2);
            if (on[MIX_V2PULSE]) {
                float w = pw;
                if (pwm) w = clampf(w + 0.4f * p.pwm_noise2 * nz, 0.05f, 0.95f);
                float q = p2 + 1 - w; if (q >= 1) q -= 1;
                x += lvl[MIX_V2PULSE] * ((p2 < w ? 1.0f : -1.0f) + blep(p2, d2) - blep(q, d2));
            }
            if (on[MIX_V2TRI]) x += lvl[MIX_V2TRI] * (1 - 4 * std::fabs(p2 - 0.5f));
            if (on[MIX_V3SAW]) x += lvl[MIX_V3SAW] * (2 * p3 - 1 - blep(p3, d3));
            if (on[MIX_NOISE]) x += lvl[MIX_NOISE] * nz;
            x *= in_gain;

            float g = G;
            if (ffm) {
                const float f = clampf(fc * fast_exp2(p.f_fm * sin2), 10.0f, fmax);
                const float t = fast_tan(pi_fs2 * f);
                g = t / (1 + t);
            }
            const float ya = ladder(0.5f * (xprev + x), g, k);   /* 2x: linear interpolation in, */
            const float yb = ladder(x, g, k);                    /* two-point average out */
            xprev = x;
            float y = 0.5f * (ya + yb) * makeup;
            hp += hp_c * (y - hp); y -= hp;                      /* DC (asymmetric pulse) before the VCA */
            y *= vgain;
            L[i] = y * gl; R[i] = y * gr;

            inc[0] += dinc[0]; inc[1] += dinc[1]; inc[2] += dinc[2];
            G += dG; fc += dfc; vgain += dvgain;
        }
        ph[0] = p1; ph[1] = p2; ph[2] = p3;
    }
};

}   // namespace carp
