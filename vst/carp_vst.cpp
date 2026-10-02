/* =============================================================================
 * carp_vst.cpp - carp 2000: a semi-modular synthesizer in the manner of the ARP 2600 as a VST2
 * instrument for the MPC OS plugin host (Force, MPC Live/One/X/Key), armhf. Phases 1 + 2 of the
 * README's roadmap: the monophonic voice (carp_core.h) - 3 VCOs, ring modulator, noise, filter
 * mixer, VCF, ADSR/AR, VCA, LFO, S&H, modulation matrix. This file is the plug-in around it:
 * parameters, MIDI (sample-accurate, last-note priority, single/multiple trigger, pitch bend
 * +-2, mod wheel, aftertouch), project chunk.
 * MIT license (see ../LICENSE). "ARP" and "2600" are trademarks of their owners; no affiliation.
 * ========================================================================== */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "carp_core.h"

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;

enum {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10, kVstTimeSigValid = 1 << 13 };
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };
enum { kVstMidiType = 1 };

/* the parameters, by key (module.json); IDX[] is their position in the generated PARAMS[] */
enum {
    V1_COARSE, V1_FINE, V1_LF, V1_FM_ADSR, V1_FM_VCO2, V1_KBD, PORTA,
    V2_COARSE, V2_FINE, V2_LF, V2_PW, V2_PWM_NOISE, V2_FM_ADSR, V2_FM_VCO1,
    V3_COARSE, V3_FINE, V3_LF, V3_FM_NOISE, V3_FM_ADSR, V3_FM_VCO2, V3_KBD,
    MIX_V1SQ, MIX_V2PULSE, MIX_V3SAW, MIX_NOISE, MIX_V1SAW, MIX_V2TRI,
    CUTOFF, RESO, F_KBD, F_ADSR, F_FM_VCO2, DRIVE, F_TYPE, F_VEL,
    ATTACK, DECAY, SUSTAIN, RELEASE, AR_ATTACK, AR_RELEASE, TRIG,
    VCA_GAIN, VCA_AR, VCA_ADSR, PAN, VOLUME,
    /* phase 2, appended */
    V1_FM_SH, V2_FM_SH, V3_PW, MIX_RING, NOISE_COLOR, REPEAT, VCA_RING,
    LFO_RATE, LFO_SHAPE, VIB_DEPTH, VIB_DELAY, SH_RATE, SH_SOURCE, SH_LAG,
    M1_SRC, M1_DST, M1_AMT, M2_SRC, M2_DST, M2_AMT, M3_SRC, M3_DST, M3_AMT,
    M4_SRC, M4_DST, M4_AMT, M5_SRC, M5_DST, M5_AMT, M6_SRC, M6_DST, M6_AMT, NKEYS
};
static const char *const KEYS[NKEYS] = {
    "v1_coarse", "v1_fine", "v1_lf", "v1_fm_adsr", "v1_fm_vco2", "v1_kbd", "porta",
    "v2_coarse", "v2_fine", "v2_lf", "v2_pw", "v2_pwm_noise", "v2_fm_adsr", "v2_fm_vco1",
    "v3_coarse", "v3_fine", "v3_lf", "v3_fm_noise", "v3_fm_adsr", "v3_fm_vco2", "v3_kbd",
    "mix_v1sq", "mix_v2pulse", "mix_v3saw", "mix_noise", "mix_v1saw", "mix_v2tri",
    "cutoff", "reso", "f_kbd", "f_adsr", "f_fm_vco2", "drive", "f_type", "f_vel",
    "attack", "decay", "sustain", "release", "ar_attack", "ar_release", "trig",
    "vca_gain", "vca_ar", "vca_adsr", "pan", "volume",
    "v1_fm_sh", "v2_fm_sh", "v3_pw", "mix_ring", "noise_color", "repeat", "vca_ring",
    "lfo_rate", "lfo_shape", "vib_depth", "vib_delay", "sh_rate", "sh_source", "sh_lag",
    "m1_src", "m1_dst", "m1_amt", "m2_src", "m2_dst", "m2_amt", "m3_src", "m3_dst", "m3_amt",
    "m4_src", "m4_dst", "m4_amt", "m5_src", "m5_dst", "m5_amt", "m6_src", "m6_dst", "m6_amt",
};
static int IDX[NKEYS];
static int KEY_OF[NPARAMS];   /* PARAMS[] position -> key enum, -1 = not ours */

struct MidiEv { int32_t frame; uint8_t d[3]; };

struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::atomic<float> cache[NPARAMS];
    std::atomic<int> notify[NPARAMS];
    float open[NPARAMS] = {0};
    volatile int release[NPARAMS] = {0};
    std::atomic<bool> dirty{true};
    carp::Voice voice;
    carp::Patch patch;
    float sr = 44100;
    bool multi = false;
    MidiEv ev[256];
    int nev = 0;
    uint8_t held[32];             /* held keys, oldest first */
    int nheld = 0;
    std::vector<uint8_t> chunk;
};

static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}
/* a knob is used at full resolution (the integer range only names its ends), a switch as its index */
static float val_at(Plugin *w, int i) {
    const param_t *p = &PARAMS[i];
    const float n = w->cache[i].load();
    if (p->nopts) return (float)norm_to_ui(p, n);
    return p->min + (p->max - p->min) * clamp01(n);
}
static float val(Plugin *w, int k) { return IDX[k] < 0 ? 0.0f : val_at(w, IDX[k]); }
static float pct(Plugin *w, int k) { return val(w, k) * 0.01f; }
static bool sw(Plugin *w, int k) { return val(w, k) > 0.5f; }
static void set_val(Plugin *w, int i, double v) {
    if (i < 0) return;
    w->cache[i].store(ui_to_norm(&PARAMS[i], v));
    w->notify[i].store(1);
}
static void start(Plugin *w, int k, double v) { set_val(w, IDX[k], v); }

struct NoDenormals {
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t old = 0;
    NoDenormals() { asm volatile("vmrs %0, fpscr" : "=r"(old)); asm volatile("vmsr fpscr, %0" : : "r"(old | (1u << 24))); }
    ~NoDenormals() { asm volatile("vmsr fpscr, %0" : : "r"(old)); }
#elif defined(__x86_64__) || defined(__i386__)
    unsigned old = __builtin_ia32_stmxcsr();
    NoDenormals() { __builtin_ia32_ldmxcsr(old | 0x8040); }
    ~NoDenormals() { __builtin_ia32_ldmxcsr(old); }
#endif
};

/* knob laws, shared by configure() and the value display */
static float sq(float x) { return x * x; }
static float law_semis(Plugin *w, int coarse, int fine) { return std::round(val(w, coarse)) + pct(w, fine); }
static float law_time(float x, float lo, float ratio) { return lo * std::pow(ratio, x); }
static float law_cutoff_oct(float x) { return x * 9.9657843f; }          /* 10 Hz .. 10 kHz */
static float law_lfo_hz(float x) { return 0.05f * std::pow(1000.0f, x); }   /* 0.05 .. 50 Hz */
static float law_sh_hz(float x) { return 0.1f * std::pow(1000.0f, x); }     /* 0.1 .. 100 Hz */
static float vco_hz(float semis, bool lf) { return 440.0f * std::pow(2.0f, (semis - 69) / 12) * (lf ? 0.003f : 1.0f); }

static void configure(Plugin *w) {
    carp::Patch &p = w->patch;
    static const int C[3] = {V1_COARSE, V2_COARSE, V3_COARSE}, F[3] = {V1_FINE, V2_FINE, V3_FINE};
    static const int LF[3] = {V1_LF, V2_LF, V3_LF}, FA[3] = {V1_FM_ADSR, V2_FM_ADSR, V3_FM_ADSR};
    static const int FX[3] = {V1_FM_VCO2, V2_FM_VCO1, V3_FM_VCO2};
    for (int v = 0; v < 3; v++) {
        p.semis[v] = law_semis(w, C[v], F[v]);
        p.lf[v] = sw(w, LF[v]);
        p.fm_adsr[v] = 5 * sq(pct(w, FA[v]));
        p.fm_x[v] = 3 * sq(pct(w, FX[v]));
    }
    p.kbd[0] = sw(w, V1_KBD); p.kbd[1] = true; p.kbd[2] = sw(w, V3_KBD);
    p.fm_noise3 = 3 * sq(pct(w, V3_FM_NOISE));
    p.fm_sh[0] = 3 * sq(pct(w, V1_FM_SH)); p.fm_sh[1] = 3 * sq(pct(w, V2_FM_SH)); p.fm_sh[2] = 0;
    p.pw3 = pct(w, V3_PW);
    p.pw2 = pct(w, V2_PW);
    p.pwm_noise2 = pct(w, V2_PWM_NOISE);
    p.porta = 3 * sq(pct(w, PORTA));
    static const int M[carp::NMIX] = {MIX_V1SQ, MIX_V2PULSE, MIX_V3SAW, MIX_NOISE, MIX_V1SAW, MIX_V2TRI, MIX_RING};
    for (int j = 0; j < carp::NMIX; j++) p.lvl[j] = pct(w, M[j]);
    p.cutoff = law_cutoff_oct(pct(w, CUTOFF));
    p.reso = pct(w, RESO);
    p.f_kbd = pct(w, F_KBD);
    p.f_adsr = 9 * pct(w, F_ADSR);
    p.f_vel = 4 * pct(w, F_VEL);
    p.f_fm = 3 * sq(pct(w, F_FM_VCO2));
    p.drive = 0.25f * std::pow(2.0f, 4 * pct(w, DRIVE));
    p.f_type = sw(w, F_TYPE) ? 1 : 0;
    p.att = law_time(pct(w, ATTACK), 0.001f, 5000);
    p.dec = law_time(pct(w, DECAY), 0.005f, 2000);
    p.sus = pct(w, SUSTAIN);
    p.rel = law_time(pct(w, RELEASE), 0.005f, 2000);
    p.ar_att = law_time(pct(w, AR_ATTACK), 0.001f, 5000);
    p.ar_rel = law_time(pct(w, AR_RELEASE), 0.005f, 2000);
    p.gain = pct(w, VCA_GAIN);
    p.vca_ar = pct(w, VCA_AR);
    p.vca_adsr = pct(w, VCA_ADSR);
    p.pan = carp::clampf(val(w, PAN) / 50.0f, -1, 1);
    p.volume = sq(pct(w, VOLUME));
    p.noise_color = pct(w, NOISE_COLOR);
    p.repeat = clampi((int)val(w, REPEAT), 0, 2);
    p.vca_ring = pct(w, VCA_RING);
    p.lfo_rate = law_lfo_hz(pct(w, LFO_RATE));
    p.lfo_shape = clampi((int)val(w, LFO_SHAPE), 0, 2);
    p.vib = 2 * sq(pct(w, VIB_DEPTH));
    p.vib_delay = 3 * sq(pct(w, VIB_DELAY));
    p.sh_rate = law_sh_hz(pct(w, SH_RATE));
    p.sh_src = clampi((int)val(w, SH_SOURCE), 0, 2);
    p.sh_lag = sq(pct(w, SH_LAG));
    for (int i = 0; i < carp::NSLOTS; i++) {
        p.slot[i].src = clampi((int)val(w, M1_SRC + 3 * i), 0, carp::NSRC - 1);
        p.slot[i].dst = clampi((int)val(w, M1_DST + 3 * i), 0, carp::NDST - 1);
        p.slot[i].amt = carp::clampf(val(w, M1_AMT + 3 * i) / 100.0f, -1, 1);
    }
    w->multi = sw(w, TRIG);
    w->voice.set_patch(p);
}

/* ---- MIDI: one voice, the newest held key sounds --------------------------- */
static void all_off(Plugin *w) { w->nheld = 0; w->voice.note_off(); }
static void midi(Plugin *w, const uint8_t *d) {
    const int st = d[0] & 0xf0, n = d[1] & 0x7f;
    if (st == 0x90 && d[2] > 0) {
        int k = 0;
        for (int i = 0; i < w->nheld; i++) if (w->held[i] != n) w->held[k++] = w->held[i];
        if (k == 32) { std::memmove(w->held, w->held + 1, 31); k = 31; }
        const bool legato = k > 0;
        w->held[k++] = (uint8_t)n;
        w->nheld = k;
        w->voice.note_on(n, d[2] / 127.0f, !legato || w->multi);
    } else if (st == 0x80 || st == 0x90) {
        if (!w->nheld) return;
        const bool top = w->held[w->nheld - 1] == n;
        int k = 0;
        for (int i = 0; i < w->nheld; i++) if (w->held[i] != n) w->held[k++] = w->held[i];
        if (k == w->nheld) return;
        w->nheld = k;
        if (!k) w->voice.note_off();
        else if (top) w->voice.note_on(w->held[k - 1], 1.0f, false);   /* back to the key still held */
    } else if (st == 0xe0) {
        w->voice.set_bend(((((int)d[2] & 0x7f) << 7 | (d[1] & 0x7f)) - 8192) * (2.0f / 8192.0f));
    } else if (st == 0xd0) {
        w->voice.set_aftertouch(n / 127.0f);
    } else if (st == 0xb0 && n == 1) {
        w->voice.set_wheel((d[2] & 0x7f) / 127.0f);
    } else if (st == 0xb0 && (n == 120 || n == 123)) {
        all_off(w);
    }
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
    NoDenormals nd;
    if (w->dirty.exchange(false)) configure(w);
    float *L = out[0], *R = out[1];
    int pos = 0;
    for (int k = 0; k < w->nev; k++) {
        const int f = clampi(w->ev[k].frame, pos, n);
        if (f > pos) { w->voice.render(L + pos, R + pos, f - pos); pos = f; }
        midi(w, w->ev[k].d);
    }
    w->nev = 0;
    if (n > pos) w->voice.render(L + pos, R + pos, n - pos);
    if (!w->voice.idle()) {
        for (int c = 0; c < 2; c++) {
            float *y = out[c];
            for (int i = 0; i < n; i++) {
                const float a = std::fabs(y[i]);              /* output safety above -3 dBFS, ceiling 0.98 */
                if (a > 0.7f) {
                    float t = std::min((a - 0.7f) / 0.3f, 3.0f), t2 = t * t;
                    y[i] = std::copysign(0.7f + 0.28f * t * (27 + t2) / (27 + 9 * t2), y[i]);
                }
            }
        }
    }
    bool any = false;
    for (int i = 0; i < NPARAMS; i++) {
        if (w->release[i]) { w->release[i] = 0; any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
        if (!w->notify[i].exchange(0)) continue;
        any = true;
        w->master(&w->fx, audioMasterAutomate, i, 0, 0, w->cache[i].load());
    }
    if (any) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

static intptr_t process_events(Plugin *w, const VstEvents *evs) {
    if (!evs) return 0;
    for (int i = 0; i < evs->numEvents; i++) {
        const VstEvent *ev = evs->events[i];
        if (!ev || ev->type != kVstMidiType || w->nev >= 256) continue;
        const VstMidiEvent *m = (const VstMidiEvent *)ev;
        MidiEv &q = w->ev[w->nev++];
        q.frame = m->deltaFrames;
        q.d[0] = m->midiData[0]; q.d[1] = m->midiData[1]; q.d[2] = m->midiData[2];
    }
    std::stable_sort(w->ev, w->ev + w->nev, [](const MidiEv &a, const MidiEv &b) { return a.frame < b.frame; });
    return 1;
}

static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    bool nudge = false;
    if (p->nopts > 1) {
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            n = (float)clampi((int)std::lround(cur) + (pos > cur ? 1 : -1), 0, p->nopts - 1) / (p->nopts - 1);
            nudge = true;
        }
    }
    w->cache[i].store(clamp01(n));
    w->dirty.store(true);
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    if (popup_is(i)) return w->open[i];
    return w->cache[i].load();
}

/* project chunk: "CARP1;key=value;..." - by key, so parameters added in later phases keep
 * old projects loading (missing keys stay at their start values, unknown ones are skipped) */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string t = "CARP1;";
    char buf[96];
    for (int i = 0; i < NPARAMS; i++) { std::snprintf(buf, sizeof buf, "%s=%.3f;", PARAMS[i].key, (double)val_at(w, i)); t += buf; }
    w->chunk.assign(t.begin(), t.end());
    *ptr = w->chunk.data();
    return (intptr_t)w->chunk.size();
}
static intptr_t set_chunk(Plugin *w, const void *data, intptr_t len) {
    std::string t((const char *)data, (size_t)len);
    if (t.compare(0, 6, "CARP1;")) return 0;
    for (size_t pos = 6; pos < t.size();) {
        size_t semi = t.find(';', pos);
        if (semi == std::string::npos) break;
        std::string kv = t.substr(pos, semi - pos);
        pos = semi + 1;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        int i = param_index(kv.substr(0, eq).c_str());
        if (i >= 0) set_val(w, i, std::atof(kv.c_str() + eq + 1));
    }
    w->dirty.store(true);
    return 1;
}

static void fmt_hz(char *b, size_t n, float hz) {
    if (hz >= 1000) std::snprintf(b, n, "%.2f kHz", hz / 1000);
    else if (hz >= 100) std::snprintf(b, n, "%.0f Hz", hz);
    else if (hz >= 1) std::snprintf(b, n, "%.1f Hz", hz);
    else std::snprintf(b, n, "%.2f Hz", hz);
}
static void fmt_time(char *b, size_t n, float s) {
    if (s < 1) std::snprintf(b, n, "%.0f ms", s * 1000);
    else std::snprintf(b, n, "%.2f s", s);
}
static void display(Plugin *w, int idx, char *buf, size_t n) {
    const param_t *pp = &PARAMS[idx];
    if (popup_is(idx)) {
        const int u = norm_to_ui(pp, w->open[idx]);
        if (pp->nopts) std::snprintf(buf, n, "%s", pp->opts[u]); else std::snprintf(buf, n, "%d", u);
        return;
    }
    const int u = norm_to_ui(pp, w->cache[idx].load());
    if (pp->nopts) { std::snprintf(buf, n, "%s", pp->opts[u]); return; }
    const float x = (val_at(w, idx) - pp->min) / (pp->max > pp->min ? pp->max - pp->min : 1);
    switch (KEY_OF[idx]) {
    case V1_COARSE: case V2_COARSE: case V3_COARSE: {
        const int k = KEY_OF[idx];
        const bool lf = sw(w, k == V1_COARSE ? V1_LF : k == V2_COARSE ? V2_LF : V3_LF);
        const bool kbd = k == V2_COARSE || sw(w, k == V1_COARSE ? V1_KBD : V3_KBD);
        if (kbd && !lf) std::snprintf(buf, n, "%+d st", u - 60);     /* relative to the key played */
        else fmt_hz(buf, n, vco_hz((float)u, lf));
        break;
    }
    case V1_FINE: case V2_FINE: case V3_FINE: std::snprintf(buf, n, "%+d ct", u); break;
    case PORTA: fmt_time(buf, n, 3 * sq(x)); break;
    case CUTOFF: fmt_hz(buf, n, 10 * std::pow(2.0f, law_cutoff_oct(x))); break;
    case ATTACK: case AR_ATTACK: fmt_time(buf, n, law_time(x, 0.001f, 5000)); break;
    case DECAY: case RELEASE: case AR_RELEASE: fmt_time(buf, n, law_time(x, 0.005f, 2000)); break;
    case VIB_DELAY: fmt_time(buf, n, 3 * sq(x)); break;
    case SH_LAG: fmt_time(buf, n, sq(x)); break;
    case LFO_RATE: fmt_hz(buf, n, law_lfo_hz(x)); break;
    case SH_RATE: fmt_hz(buf, n, law_sh_hz(x)); break;
    case M1_AMT: case M2_AMT: case M3_AMT: case M4_AMT: case M5_AMT: case M6_AMT: std::snprintf(buf, n, "%+d %%", u); break;
    case PAN: if (u == 0) std::snprintf(buf, n, "C"); else std::snprintf(buf, n, "%c%d", u < 0 ? 'L' : 'R', std::abs(u)); break;
    default: std::snprintf(buf, n, "%d %%", u); break;
    }
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: delete w; return 1;
    case effGetPlugCategory: return 2;   /* kPlugCategSynth */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS;
    case effGetParamName: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8); return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        char buf[32];
        display(w, idx, buf, sizeof buf);
        copy_str(p, buf, 24);
        return 1;
    }
    case effSetSampleRate:
        if (o > 0) { w->sr = o; w->voice.init(o); w->nheld = 0; w->dirty.store(true); }
        return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (!v) all_off(w); return 1;
    case effProcessEvents: return process_events(w, (const VstEvents *)p);
    case effCanDo:
        if (p && (!std::strcmp((const char *)p, "receiveVstEvents") || !std::strcmp((const char *)p, "receiveVstMidiEvent"))) return 1;
        return -1;
    case effGetChunk: return get_chunk(w, (void **)p);
    case effSetChunk: return set_chunk(w, p, v);
    default: return 0;
    }
}

/* the start patch: two detuned VCOs an octave apart into a half-open filter, a plucked ADSR */
static void start_values(Plugin *w) {
    start(w, V1_COARSE, 60); start(w, V1_FINE, 0); start(w, V1_LF, 0); start(w, V1_FM_ADSR, 0); start(w, V1_FM_VCO2, 0);
    start(w, V1_KBD, 1); start(w, PORTA, 0);
    start(w, V2_COARSE, 48); start(w, V2_FINE, 7); start(w, V2_LF, 0); start(w, V2_PW, 50); start(w, V2_PWM_NOISE, 0);
    start(w, V2_FM_ADSR, 0); start(w, V2_FM_VCO1, 0);
    start(w, V3_COARSE, 60); start(w, V3_FINE, -7); start(w, V3_LF, 0); start(w, V3_FM_NOISE, 0); start(w, V3_FM_ADSR, 0);
    start(w, V3_FM_VCO2, 0); start(w, V3_KBD, 1);
    start(w, MIX_V1SQ, 0); start(w, MIX_V2PULSE, 60); start(w, MIX_V3SAW, 0); start(w, MIX_NOISE, 0);
    start(w, MIX_V1SAW, 80); start(w, MIX_V2TRI, 0);
    start(w, CUTOFF, 55); start(w, RESO, 25); start(w, F_KBD, 50); start(w, F_ADSR, 40); start(w, F_FM_VCO2, 0);
    start(w, DRIVE, 25); start(w, F_TYPE, 0); start(w, F_VEL, 0);
    start(w, ATTACK, 0); start(w, DECAY, 55); start(w, SUSTAIN, 60); start(w, RELEASE, 40);
    start(w, AR_ATTACK, 0); start(w, AR_RELEASE, 40); start(w, TRIG, 0);
    start(w, V1_FM_SH, 0); start(w, V2_FM_SH, 0); start(w, V3_PW, 50); start(w, MIX_RING, 0); start(w, NOISE_COLOR, 0);
    start(w, REPEAT, 0); start(w, VCA_RING, 0);
    start(w, LFO_RATE, 67); start(w, LFO_SHAPE, 0); start(w, VIB_DEPTH, 0); start(w, VIB_DELAY, 0);
    start(w, SH_RATE, 63); start(w, SH_SOURCE, 0); start(w, SH_LAG, 0);
    for (int i = 0; i < carp::NSLOTS; i++) { start(w, M1_SRC + 3 * i, 0); start(w, M1_DST + 3 * i, carp::DST_CUTOFF); start(w, M1_AMT + 3 * i, 0); }
    start(w, VCA_GAIN, 0); start(w, VCA_AR, 0); start(w, VCA_ADSR, 100); start(w, PAN, 0); start(w, VOLUME, 80);
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < NPARAMS; i++) KEY_OF[i] = -1;
        for (int k = 0; k < NKEYS; k++) { IDX[k] = param_index(KEYS[k]); if (IDX[k] >= 0) KEY_OF[IDX[k]] = k; }
    });
    Plugin *w = new Plugin();
    w->master = master;
    for (int i = 0; i < NPARAMS; i++) { w->cache[i].store(PARAMS[i].def); w->notify[i].store(0); }
    start_values(w);
    w->voice.init(w->sr);
    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450;
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks | effFlagsIsSynth;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    return e;
}
