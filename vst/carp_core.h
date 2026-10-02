/* =============================================================================
 * carp_core.h - carp 2000: the voice of a semi-modular synthesizer in the manner of the
 * ARP 2600, and its spring reverb. One Voice = 3 VCOs, ring modulator, noise -> filter mixer ->
 * 4-pole ladder -> VCA, with ADSR, AR, LFO, sample & hold and a 6-slot modulation matrix in
 * place of the patch cords. The plug-in around it (carp_vst.cpp) runs one voice (mono, or
 * duophonic: VCO 2 follows the upper key) or four (poly) into one Reverb. MIT license.
 *
 *   VCO 1 (saw, square)  <- FM: S&H, ADSR, VCO 2 sine
 *   VCO 2 (sine, triangle, pulse)  <- FM: S&H, ADSR, VCO 1 square; PWM: noise
 *   VCO 3 (saw, pulse)  <- FM: noise, ADSR, VCO 2 sine
 *   ring modulator = VCO 1 saw x VCO 2 sine; noise white -> pink -> red
 *   mixer: ring, VCO 1 square, VCO 2 pulse, VCO 3 saw, noise, VCO 1 saw, VCO 2 triangle
 *   VCF: cutoff <- keyboard, ADSR, velocity, VCO 2 sine (audio rate)
 *   VCA: initial gain + AR (linear) + ADSR (exponential); ring modulator direct, past the filter
 *   LFO (vibrato with delay; its square is the gate in REPEAT), S&H (own clock, lag)
 *   matrix: VCO 1 saw, VCO 2 sine, VCO 3 pulse and noise modulate at audio rate, the other
 *   sources at control rate
 *
 * Performance rules (README): float only, no libm in the audio loop, slow modulation every
 * CTL samples with linear ramps in between, audio-rate maths only where a VCO or noise is the
 * source, silent mixer channels and an idle voice are skipped, 2x oversampling in the filter only.
 * Voice holds no global state; render() adds into the output and the reverb send.
 * ========================================================================== */
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

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

enum { MIX_V1SQ, MIX_V2PULSE, MIX_V3SAW, MIX_NOISE, MIX_V1SAW, MIX_V2TRI, MIX_RING, NMIX };
enum { LFO_SINE, LFO_TRI, LFO_SQUARE };
enum { SH_NOISE, SH_VCO1, SH_VCO2 };
enum { REPEAT_OFF, REPEAT_KEY, REPEAT_AUTO };
/* matrix sources; the first four are audio rate (order matters: SRC_VCO1..SRC_NOISE = 1..4) */
enum { SRC_OFF, SRC_VCO1, SRC_VCO2, SRC_VCO3, SRC_NOISE, SRC_SH, SRC_ADSR, SRC_AR, SRC_LFO, SRC_KBD, SRC_VEL, SRC_AT, SRC_WHEEL, NSRC };
enum { DST_P1, DST_P2, DST_P3, DST_PW2, DST_PW3, DST_CUTOFF, DST_RESO, DST_VCA, DST_PAN, DST_REVERB, DST_LFO_RATE, DST_SH_RATE, NDST };
static const int NSLOTS = 6;
/* what +-100 % of a slot means at each destination: octaves, pulse width, resonance, gain, pan */
static const float DST_RANGE[NDST] = {4, 4, 4, 0.4f, 0.4f, 5, 1, 1, 1, 1, 4, 4};

struct Slot { int src = SRC_OFF, dst = DST_P1; float amt = 0; };   /* amt -1..1 */

/* the panel, in engineering units (carp_vst.cpp maps the parameters onto it) */
struct Patch {
    float semis[3] = {60, 60, 60};  /* coarse + fine as a MIDI note number: the pitch at middle C */
    bool lf[3] = {false, false, false};   /* LF mode: 1/333 of the frequency, keyboard disconnected */
    bool kbd[3] = {true, true, true};
    float fm_adsr[3] = {0, 0, 0};   /* octaves at ADSR = 1 */
    float fm_sh[3] = {0, 0, 0};     /* octaves at S&H = 1 (VCO 1, VCO 2) */
    float fm_x[3] = {0, 0, 0};      /* audio-rate FM, octaves: VCO1 <- VCO2 sine, VCO2 <- VCO1 square, VCO3 <- VCO2 sine */
    float fm_noise3 = 0;            /* octaves */
    float pw2 = 0.5f, pwm_noise2 = 0, pw3 = 0.5f;
    float porta = 0;                /* seconds */
    float lvl[NMIX] = {0, 0, 0, 0, 0, 0, 0};
    float noise_color = 0;          /* 0 white, 0.5 pink, 1 red */
    float cutoff = 7;               /* octaves above 10 Hz */
    float reso = 0;                 /* 0..1, self-oscillation near the top */
    float f_kbd = 0;                /* 0..1 */
    float f_adsr = 0;               /* octaves at ADSR = 1 */
    float f_vel = 0;                /* octaves at full velocity */
    float f_fm = 0;                 /* octaves, VCO 2 sine */
    float drive = 0.5f;             /* gain into the ladder */
    int f_type = 0;                 /* 0 = 4012, 1 = 4072 (cutoff stops near 11 kHz) */
    float att = 0.001f, dec = 0.3f, sus = 0.6f, rel = 0.2f, ar_att = 0.001f, ar_rel = 0.2f;   /* seconds */
    int repeat = REPEAT_OFF;        /* KEY: the LFO square gates the envelopes while a key is held; AUTO: always */
    float gain = 0, vca_ar = 0, vca_adsr = 1, vca_ring = 0;
    float pan = 0;                  /* -1..1 */
    float rev = 0;                  /* send into the spring, 0..1 */
    float volume = 0.6f;
    float lfo_rate = 5;             /* Hz */
    int lfo_shape = LFO_SINE;
    float vib = 0, vib_delay = 0;   /* semitones, seconds */
    float sh_rate = 8;              /* Hz */
    int sh_src = SH_NOISE;
    float sh_lag = 0;               /* seconds */
    Slot slot[NSLOTS];
};

struct Voice {
    void init(float rate) {
        sr = rate; fs2 = 2 * rate;
        ph[0] = 0; ph[1] = 0.37f; ph[2] = 0.71f;
        s1 = s2 = s3 = s4 = 0; xprev = 0; hp = 0;
        sq1 = saw1 = sin2 = pul3 = noise = 0; nb0 = nb1 = nb2 = nred = 0;
        note2 = pitch2 = 60; pitch_set = false; rsend = 0; vol = 0;
        adsr = ar = 0; key = false; egate = false; att_stage = false;
        lfo_ph = 0; lfo = 0; sh_ph = 1; sh_raw = sh = 0; vib_t = 0;
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
        c_lag = p.sh_lag < 0.002f ? 1.0f : 1 - std::exp(-t / (p.sh_lag / 4));
        c_smooth = 1 - std::exp(-t / 0.004f);
        /* the matrix, sorted into audio-rate coefficients per destination and control-rate slots */
        for (int d = 0; d < NDST; d++) { a_on[d] = false; for (int j = 0; j < 4; j++) a_c[d][j] = 0; }
        a_src[0] = a_src[1] = a_src[2] = a_src[3] = false;
        nctl = 0;
        for (int i = 0; i < NSLOTS; i++) {
            const Slot &s = p.slot[i];
            if (s.src <= SRC_OFF || s.src >= NSRC || s.dst < 0 || s.dst >= NDST || s.amt == 0) continue;
            const float depth = s.amt * std::fabs(s.amt) * DST_RANGE[s.dst];
            if (s.src <= SRC_NOISE && s.dst <= DST_PAN) {
                a_c[s.dst][s.src - 1] += depth; a_on[s.dst] = true; a_src[s.src - 1] = true;
            } else {
                if (s.src <= SRC_NOISE) a_src[s.src - 1] = true;   /* sampled once per tick */
                c_src[nctl] = s.src; c_dst[nctl] = s.dst; c_amt[nctl] = depth; nctl++;
            }
        }
    }
    void note_on(int n, float velocity, bool retrigger) {
        note = note2 = (float)n;
        if (!pitch_set) { pitch = pitch2 = note; pitch_set = true; }
        if (!key || retrigger) { vel = velocity; vib_t = 0; if (pt.repeat == REPEAT_OFF) att_stage = true; }
        key = true;
    }
    void set_upper(int n) { note2 = (float)n; }      /* duophonic: the upper key, for VCO 2 (after note_on) */
    void note_off() { key = false; }
    void set_bend(float semitones) { bend = semitones; }
    void set_wheel(float v) { wheel = v; }
    void set_aftertouch(float v) { touch = v; }
    bool idle() const { return is_idle; }

    /* adds n samples to L, R and to the reverb send S */
    void render(float *L, float *R, float *S, int n) {
        int i = 0;
        while (i < n) {
            if (left == 0) { tick(); left = CTL; }
            const int m = left < n - i ? left : n - i;
            if (!is_idle) run(L + i, R + i, S + i, m);
            left -= m; i += m;
        }
    }

private:
    Patch pt;
    float sr = 44100, fs2 = 88200;
    /* oscillators, noise */
    float ph[3], inc[3] = {0, 0, 0}, dinc[3] = {0, 0, 0};
    float sq1 = 0, saw1 = 0, sin2 = 0, pul3 = 0, noise = 0;   /* last sample: the modulation sources */
    float nb0 = 0, nb1 = 0, nb2 = 0, nred = 0;
    uint32_t rng = 0x2600u;
    /* filter */
    float s1, s2, s3, s4, xprev, hp, hp_c = 0;
    float G = 0, dG = 0, fc = 100, dfc = 0, fmax = 18000;
    /* envelopes, keyboard, LFO, S&H */
    float adsr, ar, vel = 1, note = 60, pitch = 60, note2 = 60, pitch2 = 60, bend = 0, wheel = 0, touch = 0;
    bool key, egate, att_stage, pitch_set = false, is_idle = true, snap;
    float lfo_ph, lfo, sh_ph, sh_raw, sh, vib_t;
    float c_att = 1, c_dec = 1, c_rel = 1, c_aatt = 1, c_arel = 1, c_glide = 1, c_lag = 1, c_smooth = 1;
    /* matrix */
    float a_c[NDST][4];
    bool a_on[NDST], a_src[4];
    int nctl = 0, c_src[NSLOTS], c_dst[NSLOTS];
    float c_amt[NSLOTS], cm[NDST] = {0};
    /* smoothed controls */
    float lvl[NMIX] = {0, 0, 0, 0, 0, 0, 0}, cut = 7, k = 0, kbase = 0, pw = 0.5f, pw3 = 0.5f, drv = 0.5f, gl = 0, gr = 0, gain0 = 0;
    float ring_direct = 0, color = 0, rsend = 0, vol = 0;
    float vgain, dvgain = 0;
    int left;

    /* control rate: LFO, S&H, envelopes, glide, smoothing, the ramp targets for the next CTL samples */
    void tick() {
        const Patch &p = pt;
        const float dt = (float)CTL / sr;

        /* matrix, control-rate slots (LFO and S&H rate see the values of the previous tick) */
        for (int d = 0; d < NDST; d++) cm[d] = 0;
        for (int i = 0; i < nctl; i++) {
            float v = 0;
            switch (c_src[i]) {
            case SRC_VCO1: v = saw1; break;
            case SRC_VCO2: v = sin2; break;
            case SRC_VCO3: v = pul3; break;
            case SRC_NOISE: v = noise; break;
            case SRC_SH: v = sh; break;
            case SRC_ADSR: v = adsr; break;
            case SRC_AR: v = ar; break;
            case SRC_LFO: v = lfo; break;
            case SRC_KBD: v = (pitch - 60) * (1.0f / 24.0f); break;
            case SRC_VEL: v = vel; break;
            case SRC_AT: v = touch; break;
            case SRC_WHEEL: v = wheel; break;
            }
            cm[c_dst[i]] += c_amt[i] * v;
        }

        /* LFO; a new cycle starts with the positive half */
        lfo_ph += clampf(p.lfo_rate * fast_exp2(cm[DST_LFO_RATE]), 0.01f, 200.0f) * dt;
        if (lfo_ph >= 1) lfo_ph -= (float)(int)lfo_ph;
        lfo = p.lfo_shape == LFO_SINE ? fast_sin(lfo_ph)
            : p.lfo_shape == LFO_TRI ? (lfo_ph < 0.25f ? 4 * lfo_ph : lfo_ph < 0.75f ? 2 - 4 * lfo_ph : 4 * lfo_ph - 4)
            : (lfo_ph < 0.5f ? 1.0f : -1.0f);

        /* sample & hold on its own clock */
        sh_ph += clampf(p.sh_rate * fast_exp2(cm[DST_SH_RATE]), 0.01f, 500.0f) * dt;
        if (sh_ph >= 1) {
            sh_ph -= (float)(int)sh_ph;
            if (p.sh_src == SH_NOISE) {                       /* its own draw: works while the voice idles */
                rng = rng * 1664525u + 1013904223u;
                sh_raw = (float)(int32_t)rng * (1.0f / 2147483648.0f);
            } else sh_raw = p.sh_src == SH_VCO1 ? saw1 : sin2;
        }
        sh += (sh_raw - sh) * c_lag;

        /* gate: the key, or in REPEAT the LFO square */
        const bool g = p.repeat == REPEAT_OFF ? key : (lfo_ph < 0.5f && (key || p.repeat == REPEAT_AUTO));
        if (g && !egate && p.repeat != REPEAT_OFF) att_stage = true;
        egate = g;
        if (egate) {
            if (att_stage) { adsr += c_att * (1.3f - adsr); if (adsr >= 1) { adsr = 1; att_stage = false; } }
            else adsr += c_dec * (p.sus - adsr);
            ar += c_aatt * (1.3f - ar); if (ar > 1) ar = 1;
        } else {
            adsr -= c_rel * adsr; if (adsr < 1e-4f) adsr = 0;
            ar -= c_arel * ar; if (ar < 1e-4f) ar = 0;
        }
        pitch += (note - pitch) * c_glide;
        pitch2 += (note2 - pitch2) * c_glide;

        /* vibrato, faded in after the delay */
        vib_t += dt;
        float vib = 0;
        if (p.vib > 0) vib = p.vib * lfo * clampf((vib_t - p.vib_delay) / (0.5f * p.vib_delay + 0.01f), 0.0f, 1.0f);

        const float cs = snap ? 1.0f : c_smooth;
        for (int j = 0; j < NMIX; j++) lvl[j] += (p.lvl[j] - lvl[j]) * cs;
        cut += (p.cutoff - cut) * cs;
        kbase += (p.reso * 4.3f - kbase) * cs;
        k = clampf(kbase + 4.3f * cm[DST_RESO], 0.0f, 4.3f);
        pw += (clampf(p.pw2 + cm[DST_PW2], 0.05f, 0.95f) - pw) * cs;
        pw3 += (clampf(p.pw3 + cm[DST_PW3], 0.05f, 0.95f) - pw3) * cs;
        drv += (p.drive - drv) * cs;
        gain0 += (p.gain - gain0) * cs;
        ring_direct += (p.vca_ring - ring_direct) * cs;
        color += (p.noise_color - color) * cs;
        vol += (p.volume - vol) * cs;
        rsend += (clampf(p.rev + cm[DST_REVERB], 0.0f, 1.0f) - rsend) * cs;
        const float a = (clampf(p.pan + cm[DST_PAN], -1.0f, 1.0f) + 1) * 0.125f;   /* 0..0.25 = 0..90 degrees, constant power */
        gl += (p.volume * fast_sin(0.25f + a) - gl) * cs;          /* cos */
        gr += (p.volume * fast_sin(a) - gr) * cs;

        const float kv = pitch - 60 + bend + vib, kv2 = pitch2 - 60 + bend + vib;
        for (int v = 0; v < 3; v++) {
            const bool track = p.kbd[v] && !p.lf[v];
            const float oct = (p.semis[v] + (track ? (v == 1 ? kv2 : kv) : 0.0f) - 69.0f) * (1.0f / 12.0f)
                            + p.fm_adsr[v] * adsr + p.fm_sh[v] * sh + cm[DST_P1 + v];
            float f = 440.0f * fast_exp2(oct);
            if (p.lf[v]) f *= 0.003f;
            const float target = clampf(f / sr, 0.0f, 0.45f);
            if (snap) inc[v] = target;
            dinc[v] = (target - inc[v]) * (1.0f / CTL);
        }
        fmax = p.f_type ? 11000.0f : 18000.0f;
        if (fmax > 0.2f * fs2) fmax = 0.2f * fs2;
        const float foct = cut + p.f_kbd * (pitch - 60) * (1.0f / 12.0f) + p.f_adsr * adsr + p.f_vel * vel + cm[DST_CUTOFF];
        const float ft = clampf(10.0f * fast_exp2(foct), 10.0f, fmax);
        const float gt = fast_tan(3.14159265f * ft / fs2), Gt = gt / (1 + gt);
        if (snap) { fc = ft; G = Gt; }
        dfc = (ft - fc) * (1.0f / CTL);
        dG = (Gt - G) * (1.0f / CTL);

        const float vt = clampf(gain0 + p.vca_ar * ar + p.vca_adsr * adsr * adsr + cm[DST_VCA], 0.0f, 1.0f);
        if (snap) vgain = vt;
        dvgain = (vt - vgain) * (1.0f / CTL);

        if (not_finite(s4) || not_finite(hp) || not_finite(nb0 + nred)) { s1 = s2 = s3 = s4 = 0; xprev = 0; hp = 0; nb0 = nb1 = nb2 = nred = 0; }
        is_idle = !egate && adsr == 0 && ar == 0 && p.gain <= 0 && vt < 1e-5f && vgain < 1e-5f && !a_on[DST_VCA];
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
    inline float amod(int d) const { return a_c[d][0] * saw1 + a_c[d][1] * sin2 + a_c[d][2] * pul3 + a_c[d][3] * noise; }

    void run(float *L, float *R, float *S, int m) {
        const Patch &p = pt;
        const float eps = 1e-4f;
        bool on[NMIX];
        for (int j = 0; j < NMIX; j++) on[j] = lvl[j] > eps || p.lvl[j] > eps;
        const bool ring_vca = ring_direct > eps || p.vca_ring > eps;
        const bool ring = on[MIX_RING] || ring_vca;
        const bool aP1 = a_on[DST_P1], aP2 = a_on[DST_P2], aP3 = a_on[DST_P3], aPW2 = a_on[DST_PW2], aPW3 = a_on[DST_PW3];
        const bool aCut = a_on[DST_CUTOFF], aRes = a_on[DST_RESO], aVca = a_on[DST_VCA], aPan = a_on[DST_PAN];
        const bool fm1 = p.fm_x[0] > 0 || aP1, fm2 = p.fm_x[1] > 0 || aP2, fm3 = p.fm_x[2] > 0 || p.fm_noise3 > 0 || aP3;
        const bool ffm = p.f_fm > 0 || aCut, pwm = p.pwm_noise2 > 0 || aPW2;
        const bool need_sq1 = on[MIX_V1SQ] || p.fm_x[1] > 0;
        const bool need_saw1 = on[MIX_V1SAW] || ring || a_src[0] || p.sh_src == SH_VCO1;
        const bool need_sin2 = p.fm_x[0] > 0 || p.fm_x[2] > 0 || p.f_fm > 0 || ring || a_src[1] || p.sh_src == SH_VCO2;
        const bool need_pul3 = a_src[2];
        const bool colored = color > eps || p.noise_color > eps;
        const float cw = color < 0.5f ? 1 - 2 * color : 0.0f, cr = color > 0.5f ? 2 * color - 1 : 0.0f, cp = 1 - cw - cr;
        const float makeup = 1.0f / clampf(drv, 0.35f, 1.0f);
        const float pi_fs2 = 3.14159265f / fs2;
        const bool send = rsend > eps || p.rev > eps;
        const float sg = rsend * vol;
        float p1 = ph[0], p2 = ph[1], p3 = ph[2];

        for (int i = 0; i < m; i++) {
            rng = rng * 1664525u + 1013904223u;
            const float white = (float)(int32_t)rng * (1.0f / 2147483648.0f);
            float nz = white;
            if (colored) {                       /* pink: Kellet's 3-pole filter; red: leaky integrator */
                nb0 = 0.99765f * nb0 + white * 0.0990460f;
                nb1 = 0.96300f * nb1 + white * 0.2965164f;
                nb2 = 0.57000f * nb2 + white * 1.0526913f;
                nred = 0.995f * nred + white;
                nz = cw * white + cp * 0.21f * (nb0 + nb1 + nb2 + white * 0.1848f) + cr * 0.057f * nred;   /* about equally loud */
            }

            /* the modulation sources are the previous sample's outputs */
            float d1 = inc[0], d2 = inc[1], d3 = inc[2];
            if (fm1) { d1 *= fast_exp2(p.fm_x[0] * sin2 + (aP1 ? amod(DST_P1) : 0.0f)); if (d1 > 0.45f) d1 = 0.45f; }
            if (fm2) { d2 *= fast_exp2(p.fm_x[1] * sq1 + (aP2 ? amod(DST_P2) : 0.0f)); if (d2 > 0.45f) d2 = 0.45f; }
            if (fm3) { d3 *= fast_exp2(p.fm_x[2] * sin2 + p.fm_noise3 * noise + (aP3 ? amod(DST_P3) : 0.0f)); if (d3 > 0.45f) d3 = 0.45f; }
            float w2 = pw, w3 = pw3, res = k;
            if (pwm) w2 = clampf(w2 + 0.4f * p.pwm_noise2 * noise + (aPW2 ? amod(DST_PW2) : 0.0f), 0.05f, 0.95f);
            if (aPW3) w3 = clampf(w3 + amod(DST_PW3), 0.05f, 0.95f);
            if (aRes) res = clampf(res + 4.3f * amod(DST_RESO), 0.0f, 4.3f);
            float g = G;
            if (ffm) {
                const float f = clampf(fc * fast_exp2(p.f_fm * sin2 + (aCut ? amod(DST_CUTOFF) : 0.0f)), 10.0f, fmax);
                const float t = fast_tan(pi_fs2 * f);
                g = t / (1 + t);
            }
            float va = vgain, pl = gl, pr = gr;
            if (aVca) va = clampf(va + amod(DST_VCA), 0.0f, 1.0f);
            if (aPan) { const float q = clampf(amod(DST_PAN), -1.0f, 1.0f); pl *= 1 - q; pr *= 1 + q; }

            p1 += d1; if (p1 >= 1) p1 -= 1;
            p2 += d2; if (p2 >= 1) p2 -= 1;
            p3 += d3; if (p3 >= 1) p3 -= 1;
            noise = nz;

            float x = 1e-5f * white;                  /* seed for self-oscillation */
            if (need_sq1) {
                float q = p1 + 0.5f; if (q >= 1) q -= 1;
                sq1 = (p1 < 0.5f ? 1.0f : -1.0f) + blep(p1, d1) - blep(q, d1);
                x += lvl[MIX_V1SQ] * sq1;
            }
            if (need_saw1) { saw1 = 2 * p1 - 1 - blep(p1, d1); x += lvl[MIX_V1SAW] * saw1; }
            if (need_sin2) sin2 = fast_sin(p2);
            if (on[MIX_V2PULSE]) {
                float q = p2 + 1 - w2; if (q >= 1) q -= 1;
                x += lvl[MIX_V2PULSE] * ((p2 < w2 ? 1.0f : -1.0f) + blep(p2, d2) - blep(q, d2));
            }
            if (on[MIX_V2TRI]) x += lvl[MIX_V2TRI] * (1 - 4 * std::fabs(p2 - 0.5f));
            if (on[MIX_V3SAW]) x += lvl[MIX_V3SAW] * (2 * p3 - 1 - blep(p3, d3));
            if (need_pul3) {
                float q = p3 + 1 - w3; if (q >= 1) q -= 1;
                pul3 = (p3 < w3 ? 1.0f : -1.0f) + blep(p3, d3) - blep(q, d3);
            }
            if (on[MIX_NOISE]) x += lvl[MIX_NOISE] * nz;
            float rm = 0;
            if (ring) { rm = saw1 * sin2; x += lvl[MIX_RING] * rm; }
            x *= drv * (1 + 0.25f * res);

            const float ya = ladder(0.5f * (xprev + x), g, res);   /* 2x: linear interpolation in, */
            const float yb = ladder(x, g, res);                    /* two-point average out */
            xprev = x;
            float y = 0.5f * (ya + yb) * makeup;
            if (ring_vca) y += ring_direct * rm;                   /* past the filter, into the VCA */
            hp += hp_c * (y - hp); y -= hp;                        /* DC (asymmetric pulse) before the VCA */
            y *= va;
            L[i] += y * pl; R[i] += y * pr;
            if (send) S[i] += y * sg;

            inc[0] += dinc[0]; inc[1] += dinc[1]; inc[2] += dinc[2];
            G += dG; fc += dfc; vgain += dvgain;
        }
        ph[0] = p1; ph[1] = p2; ph[2] = p3;
    }
};

/* Spring reverb, once per plug-in: two springs, each a delay line in a damped feedback loop with
 * a chain of stretched allpasses (z^-K), which delays the treble more than the bass on every
 * pass - the spring's chirp. Mono in, spring A left, spring B right. No convolution. */
struct Reverb {
    void init(float rate) {
        sr = rate;
        K = (int)(rate / 11025.0f + 0.5f); if (K < 1) K = 1; if (K > KMAX) K = KMAX;
        sp[0].init((int)(0.0331f * rate), 0.62f);
        sp[1].init((int)(0.0413f * rate), 0.58f);
        ki = 0; in1 = in2 = inhp = 0; awake = false; quiet = 0;
        c_in = 1 - std::exp(-2 * 3.14159265f * 4500 / rate);
        c_hp = 1 - std::exp(-2 * 3.14159265f * 120 / rate);
        c_out = 1 - std::exp(-2 * 3.14159265f * 5000 / rate);
        set_length(len);
    }
    void set_length(float seconds) {               /* decay time to -60 dB */
        len = seconds;
        for (int s = 0; s < 2; s++) sp[s].fb = std::pow(10.0f, -3.0f * (float)sp[s].n / (sr * seconds));
    }
    bool active() const { return awake; }
    /* reads the send S, adds the wet signal to L and R */
    void process(const float *S, float *L, float *R, int n) {
        float pk = 0;
        for (int i = 0; i < n; i++) { const float a = std::fabs(S[i]); if (a > pk) pk = a; }
        if (!awake) { if (pk < 1e-6f) return; awake = true; quiet = 0; }
        float opk = 0;
        for (int i = 0; i < n; i++) {
            in1 += c_in * (S[i] - in1); in2 += c_in * (in1 - in2);
            inhp += c_hp * (in2 - inhp);
            const float x = in2 - inhp;
            const float a = sp[0].step(x, K, ki, c_out), b = sp[1].step(x, K, ki, c_out);
            if (++ki >= K) ki = 0;
            L[i] += 0.6f * (a + 0.25f * b); R[i] += 0.6f * (b + 0.25f * a);
            const float m = std::fabs(a) + std::fabs(b); if (m > opk) opk = m;
        }
        if (pk < 1e-6f && opk < 1e-6f) { quiet += n; if (quiet > (int)sr) { awake = false; for (int s = 0; s < 2; s++) sp[s].clear(); in1 = in2 = inhp = 0; } }
        else quiet = 0;
        if (not_finite(opk)) { for (int s = 0; s < 2; s++) sp[s].clear(); in1 = in2 = inhp = 0; }
    }
private:
    enum { NAP = 24, KMAX = 20 };
    struct Spring {
        std::vector<float> buf;
        int n = 1, pos = 0;
        float a = 0.6f, fb = 0.5f, damp = 0, out = 0, ap[NAP][KMAX];
        void init(int length, float coef) { n = length < 8 ? 8 : length; a = coef; buf.assign((size_t)n, 0.0f); clear(); }
        void clear() { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; damp = out = 0; std::memset(ap, 0, sizeof ap); }
        inline float step(float in, int, int ki, float c_out) {
            const float y = buf[(size_t)pos];
            damp += 0.45f * (y - damp);                       /* loop damping */
            float x = in + fb * damp;
            for (int j = 0; j < NAP; j++) { const float o = a * x + ap[j][ki]; ap[j][ki] = x - a * o; x = o; }
            buf[(size_t)pos] = x; if (++pos >= n) pos = 0;
            out += c_out * (y - out);
            return out;
        }
    };
    Spring sp[2];
    float sr = 44100, len = 2, in1 = 0, in2 = 0, inhp = 0, c_in = 0.5f, c_hp = 0.02f, c_out = 0.5f;
    int K = 4, ki = 0, quiet = 0;
    bool awake = false;
};

}   // namespace carp
