/* Offline x86 test of carp_vst.cpp (test.sh builds it with ASan/UBSan): silence without a key,
 * tuning (VCO, coarse, pitch bend), the filter (darkening, keyboard-tracked self-oscillation,
 * 4072 ceiling), ADSR/AR/VCA (sustain, release, initial gain), mono key handling (last note,
 * single/multiple trigger), LF mode, cross FM, pan, ring modulator, noise colour, LFO vibrato
 * with delay, S&H, REPEAT, the modulation matrix at control and audio rate, the spring reverb,
 * duophonic and four-voice modes, every preset, output ceiling,
 * chunk restore, a random parameter/MIDI stress run, NaN/denormal-free output.
 * With "bench" as the second argument it only measures CPU load. Prints PASSED/FAILED. */
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>

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
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4]; char detune, noteOffVelocity, reserved1, reserved2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstMidiEvent *events[2]; } VstEvents;

static intptr_t master(AEffect *, int32_t, int32_t, intptr_t, void *, float) { return 0; }
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)
static const float SR = 44100;
static const int BS = 256;
static const int NPRESETS = 13;
static bool bad = false;
typedef std::vector<float> Buf;

static int param(AEffect *e, const char *name) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { buf[0] = 0; e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, name)) return i; }
    std::printf("FAIL: no parameter %s\n", name); fails++; return 0;
}
static std::string display(AEffect *e, const char *name) { char buf[64] = {0}; e->dispatcher(e, 7, param(e, name), 0, buf, 0); return buf; }
/* set a parameter in the units of module.json (0..100 unless listed; switches 0/1) */
static void set(AEffect *e, const char *name, double v) {
    double lo = 0, hi = 100;
    const std::string s = name;
    auto ends = [&](const char *x) { std::string t = x; return s.size() >= t.size() && !s.compare(s.size() - t.size(), t.size(), t); };
    if (ends("Coarse")) { lo = 4; hi = 123; }
    else if (ends("Fine")) { lo = -100; hi = 100; }
    else if (ends("Pulse Width")) { lo = 10; hi = 90; }
    else if (s == "Pan") { lo = -50; hi = 50; }
    else if (ends("Amount")) { lo = -100; hi = 100; }
    else if (ends("Source") && s != "S&H Source") { hi = 12; }
    else if (ends("Dest")) { hi = 11; }
    else if (s == "LFO Shape" || s == "S&H Source" || s == "Repeat" || s == "Voice Mode") { hi = 2; }
    else if (s == "Preset") { hi = NPRESETS - 1; }
    else if (ends(" LF") || (ends("Keyboard") && s != "VCF Keyboard") || s == "VCF Type" || s == "Trigger") { hi = 1; }
    e->setParameter(e, param(e, name), (float)((v - lo) / (hi - lo)));
}
static void send(AEffect *e, int a, int b, int c, int frame = 0) {
    VstMidiEvent m; std::memset(&m, 0, sizeof m);
    m.type = 1; m.byteSize = sizeof m; m.deltaFrames = frame;
    m.midiData[0] = (unsigned char)a; m.midiData[1] = (unsigned char)b; m.midiData[2] = (unsigned char)c;
    VstEvents ev; ev.numEvents = 1; ev.reserved = 0; ev.events[0] = &m; ev.events[1] = nullptr;
    e->dispatcher(e, 25, 0, 0, &ev, 0);
}
static void on(AEffect *e, int n, int v = 100) { send(e, 0x90, n, v); }
static void off(AEffect *e, int n) { send(e, 0x80, n, 0); }
/* render; returns the left channel (right too if asked) */
static Buf run(AEffect *e, double sec, Buf *right = nullptr) {
    Buf outL, ol(BS), orr(BS);
    float *out[2] = {ol.data(), orr.data()};
    for (int b = 0; b < (int)(sec * SR / BS); b++) {
        e->processReplacing(e, nullptr, out, BS);
        for (int i = 0; i < BS; i++) {
            for (float s : {ol[i], orr[i]}) if (!std::isfinite(s) || std::fpclassify(s) == FP_SUBNORMAL) bad = true;
            outL.push_back(ol[i]);
            if (right) right->push_back(orr[i]);
        }
    }
    return outL;
}
static double goertzel(const Buf &a, double f, double t0 = 0.1) {
    size_t i0 = (size_t)(t0 * SR), i1 = a.size();
    double w = 2 * M_PI * f / SR, c = std::cos(w), s1 = 0, s2 = 0;
    for (size_t i = i0; i < i1; i++) { double s0 = a[i] + 2 * c * s1 - s2; s2 = s1; s1 = s0; }
    return 2 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - 2 * c * s1 * s2)) / (i1 - i0);
}
static double rms(const Buf &a, double t0 = 0.1, double t1 = -1) {
    double s = 0; size_t i0 = (size_t)(t0 * SR), i1 = t1 < 0 ? a.size() : (size_t)(t1 * SR);
    for (size_t i = i0; i < i1; i++) s += (double)a[i] * a[i];
    return std::sqrt(s / (i1 - i0));
}
static double peak(const Buf &a) { double p = 0; for (float s : a) p = std::max(p, (double)std::fabs(s)); return p; }
/* the strongest frequency near f0 (+-6 %), by a Goertzel scan */
static double freq(const Buf &a, double f0) {
    double best = 0, bf = f0;
    for (double f = f0 * 0.94; f <= f0 * 1.06; f *= 1.0005) { double g = goertzel(a, f, 0.2); if (g > best) { best = g; bf = f; } }
    return bf;
}
static double cents(double f, double ref) { return 1200 * std::log2(f / ref); }
static double db(double x) { return 20 * std::log10(x + 1e-12); }

/* a plain test patch: VCO 1 saw only, filter open, organ envelope */
static void plain(AEffect *e) {
    for (const char *k : {"Mix VCO1 Square", "Mix VCO2 Pulse", "Mix VCO3 Saw", "Mix Noise", "Mix VCO2 Triangle", "Resonance",
                          "VCF Keyboard", "VCF ADSR", "VCF FM VCO2", "VCF Velocity", "VCO1 FM ADSR", "VCO1 FM VCO2", "VCO2 FM ADSR",
                          "VCO2 FM VCO1", "VCO2 PWM Noise", "VCO3 FM Noise", "VCO3 FM ADSR", "VCO3 FM VCO2", "Portamento",
                          "ADSR Attack", "ADSR Release", "AR Attack", "AR Release", "VCA Initial Gain", "VCA AR", "VCF Drive",
                          "VCO1 FM S&H", "VCO2 FM S&H", "Mix Ring Mod", "Noise Color", "VCA Ring Mod", "Vibrato Depth", "Vibrato Delay", "S&H Lag",
                          "Repeat", "LFO Shape", "S&H Source", "Reverb", "Voice Mode"}) set(e, k, 0);
    set(e, "Reverb Length", 50);
    for (int i = 1; i <= 6; i++) { char b[32]; std::snprintf(b, sizeof b, "Mod %d Source", i); set(e, b, 0); std::snprintf(b, sizeof b, "Mod %d Amount", i); set(e, b, 0); }
    set(e, "LFO Rate", 67); set(e, "S&H Rate", 63); set(e, "VCO3 Pulse Width", 50);
    for (const char *k : {"VCO1 LF", "VCO2 LF", "VCO3 LF", "VCF Type", "Trigger"}) set(e, k, 0);
    for (const char *k : {"VCO1 Keyboard", "VCO3 Keyboard"}) set(e, k, 1);
    for (const char *k : {"VCO1 Coarse", "VCO2 Coarse", "VCO3 Coarse"}) set(e, k, 60);
    for (const char *k : {"VCO1 Fine", "VCO2 Fine", "VCO3 Fine", "Pan"}) set(e, k, 0);
    set(e, "Mix VCO1 Saw", 100); set(e, "Cutoff", 100); set(e, "ADSR Sustain", 100); set(e, "ADSR Decay", 50);
    set(e, "VCA ADSR", 100); set(e, "Volume", 80); set(e, "VCO2 Pulse Width", 50);
    send(e, 0xb0, 123, 0); send(e, 0xe0, 0, 64); send(e, 0xb0, 1, 0); send(e, 0xd0, 0, 0);
    run(e, 0.3);
}

int main(int argc, char **argv) {
    void *h = dlopen(argc > 1 ? argv[1] : "./carp2000.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::printf("FAILED: dlopen %s\n", dlerror()); return 1; }
    auto mainf = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    AEffect *e = mainf(master);
    CHECK(e->numInputs == 0 && e->numOutputs == 2 && (e->flags & (1 << 8)), "not a stereo instrument");
    e->dispatcher(e, 0, 0, 0, nullptr, 0);
    e->dispatcher(e, 10, 0, 0, nullptr, SR);
    if (argc > 2 && !std::strcmp(argv[2], "bench")) {
        struct { const char *what; int mode; } B[] = {{"idle (no key)", 0}, {"start patch, key held", 1}, {"worst case: all sources, all FM, 6 audio-rate slots", 2}, {"the same, four voices + reverb", 3}};
        for (auto &b : B) {
            if (b.mode == 3) { set(e, "Voice Mode", 2); set(e, "Reverb", 60); run(e, 0.1); on(e, 52); on(e, 57); on(e, 60); }
            if (b.mode >= 1) on(e, 45);
            if (b.mode == 2) {
                for (const char *k : {"Mix VCO1 Square", "Mix VCO2 Pulse", "Mix VCO3 Saw", "Mix Noise", "Mix VCO1 Saw", "Mix VCO2 Triangle"}) set(e, k, 70);
                for (const char *k : {"VCO1 FM VCO2", "VCO2 FM VCO1", "VCO3 FM VCO2", "VCO3 FM Noise", "VCF FM VCO2", "VCO2 PWM Noise", "Resonance", "Mix Ring Mod", "Noise Color", "VCO1 FM S&H"}) set(e, k, 40);
                for (int i = 1; i <= 6; i++) {   /* six audio-rate slots: VCO 1..3 and noise onto pitch, pulse width, cutoff, resonance, VCA, pan */
                    char b[32]; static const int D[6] = {0, 4, 5, 6, 7, 8};
                    std::snprintf(b, sizeof b, "Mod %d Source", i); set(e, b, 1 + i % 4);
                    std::snprintf(b, sizeof b, "Mod %d Dest", i); set(e, b, D[i - 1]);
                    std::snprintf(b, sizeof b, "Mod %d Amount", i); set(e, b, 30);
                }
            }
            auto t0 = std::chrono::steady_clock::now();
            run(e, 20.0);
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("bench (this CPU, one core): %-46s %.2f %%\n", b.what, 100 * s / 20.0);
        }
        return 0;
    }

    /* 0. start patch: silent without a key, sounds with one, silent again after the release */
    {
        double q = rms(run(e, 0.3), 0);
        on(e, 57); double s = rms(run(e, 0.5)); off(e, 57);
        double tail = rms(run(e, 3.0), 2.5);
        std::printf("start patch: idle %.6f, key %.1f dBFS, after release %.6f\n", q, db(s), tail);
        CHECK(q == 0, "not silent without a key"); CHECK(s > 0.03, "start patch too quiet"); CHECK(tail < 1e-4, "does not fall silent");
    }

    /* 1. tuning: A4 = 440 Hz, coarse +12, fine, pitch bend, keyboard off */
    plain(e);
    {
        on(e, 69); double f = freq(run(e, 1.0), 440);
        set(e, "VCO1 Coarse", 72); double f12 = freq(run(e, 1.0), 880);
        std::string disp = display(e, "VCO1 Coarse");
        set(e, "VCO1 Coarse", 60); set(e, "VCO1 Fine", 50); double ff = freq(run(e, 1.0), 440);
        set(e, "VCO1 Fine", 0); send(e, 0xe0, 127, 127); double fb = freq(run(e, 1.0), 493.88);
        send(e, 0xe0, 0, 64);
        set(e, "VCO1 Keyboard", 0); double fk = freq(run(e, 1.0), 261.63);
        std::string dispk = display(e, "VCO1 Coarse");
        set(e, "VCO1 Keyboard", 1); off(e, 69);
        std::printf("  tuning: A4 %.2f Hz, +12 %.2f (%s), fine +50 ct %+.1f ct, bend up %+.1f ct, keyboard off %.2f Hz (%s)\n",
                    f, f12, disp.c_str(), cents(ff, 440), cents(fb, 440), fk, dispk.c_str());
        CHECK(std::fabs(cents(f, 440)) < 3, "A4 out of tune"); CHECK(std::fabs(cents(f12, 880)) < 3, "coarse +12 wrong");
        CHECK(std::fabs(cents(ff, 440) - 50) < 4, "fine wrong"); CHECK(std::fabs(cents(fb, 440) - 200) < 5, "bend wrong");
        CHECK(std::fabs(cents(fk, 261.63)) < 3, "keyboard off: not at middle C");
        CHECK(disp == "+12 st", "coarse display %s", disp.c_str());
    }

    /* 2. filter: closing it removes the 10th harmonic; resonance lifts the harmonic at the cutoff */
    plain(e);
    {
        on(e, 45);   /* 110 Hz */
        double hi[2];
        for (int k = 0; k < 2; k++) { set(e, "Cutoff", k ? 50 : 100); auto a = run(e, 0.8); hi[k] = goertzel(a, 1100) / goertzel(a, 110); }
        std::printf("  filter: 10th harmonic %.1f dB open, %.1f dB at %s\n", db(hi[0]), db(hi[1]), display(e, "Cutoff").c_str());
        CHECK(hi[1] < hi[0] * 0.1, "cutoff does not darken");
        set(e, "Cutoff", 66.67); double r[2];   /* 1 kHz */
        for (int k = 0; k < 2; k++) { set(e, "Resonance", k ? 80 : 0); auto a = run(e, 0.8); r[k] = goertzel(a, 990) / goertzel(a, 110); }
        std::printf("  resonance 80: 9th harmonic %+.1f dB\n", db(r[1] / r[0]));
        CHECK(r[1] > r[0] * 2, "resonance does not peak");
        off(e, 45);
    }

    /* 3. self-oscillation, tracked by the keyboard: in tune over two octaves */
    plain(e);
    {
        set(e, "Mix VCO1 Saw", 0); set(e, "Resonance", 100); set(e, "VCF Keyboard", 100);
        set(e, "Cutoff", 100.0 * std::log2(26.163) / std::log2(1000.0));   /* 261.63 Hz at middle C */
        double f[3]; int notes[3] = {48, 60, 84};
        for (int k = 0; k < 3; k++) { on(e, notes[k]); auto a = run(e, 1.0); f[k] = freq(a, 261.63 * std::pow(2.0, (notes[k] - 60) / 12.0)); if (k == 1) CHECK(rms(a, 0.5) > 0.02, "filter does not self-oscillate"); off(e, notes[k]); run(e, 0.2); }
        std::printf("  self-oscillation: C3 %.1f Hz (%+.0f ct), C4 %.1f Hz (%+.0f ct), C6 %.1f Hz (%+.0f ct)\n",
                    f[0], cents(f[0], 130.81), f[1], cents(f[1], 261.63), f[2], cents(f[2], 1046.5));
        for (int k = 0; k < 3; k++) CHECK(std::fabs(cents(f[k], 261.63 * std::pow(2.0, (notes[k] - 60) / 12.0))) < 15, "self-oscillation off pitch at note %d", notes[k]);
        /* 4072: the cutoff stops near 11 kHz */
        set(e, "VCF Keyboard", 0); set(e, "Cutoff", 100); set(e, "VCF ADSR", 30);
        double ft[2];
        for (int k = 0; k < 2; k++) {
            set(e, "VCF Type", k); on(e, 60); auto a = run(e, 0.6); off(e, 60); run(e, 0.2);
            double best = 0; ft[k] = 0;
            for (double x = 8000; x < 20000; x += 50) { double g = goertzel(a, x, 0.3); if (g > best) { best = g; ft[k] = x; } }
        }
        std::printf("  filter ceiling: 4012 %.0f Hz, 4072 %.0f Hz\n", ft[0], ft[1]);
        CHECK(ft[0] > 15000 && std::fabs(ft[1] - 11000) < 600, "filter type ceilings wrong");
    }

    /* 4. envelopes and VCA */
    plain(e);
    {
        set(e, "ADSR Sustain", 0); set(e, "ADSR Decay", 40);
        on(e, 57); auto a = run(e, 2.0); off(e, 57);
        std::printf("  ADSR sustain 0, decay %s: %.1f dBFS in the first 12 ms, %.1f dBFS at 1.5 s\n", display(e, "ADSR Decay").c_str(), db(rms(a, 0.002, 0.012)), db(rms(a, 1.5)));
        CHECK(rms(a, 0.002, 0.012) > 0.05 && rms(a, 1.5) < 1e-3, "decay to sustain 0 fails");
        set(e, "ADSR Sustain", 100); set(e, "ADSR Attack", 70);
        on(e, 57); a = run(e, 1.5); off(e, 57); run(e, 0.3);
        std::printf("  attack %s: %.1f dBFS at 30 ms, %.1f dBFS at 1.4 s\n", display(e, "ADSR Attack").c_str(), db(rms(a, 0.02, 0.04)), db(rms(a, 1.3)));
        CHECK(rms(a, 0.02, 0.04) < 0.1 * rms(a, 1.3), "attack not slow");
        set(e, "ADSR Attack", 0); set(e, "VCA ADSR", 0); set(e, "VCA AR", 100); set(e, "AR Release", 60);
        on(e, 57); run(e, 0.3); off(e, 57); a = run(e, 1.5);
        std::printf("  AR release %s: %.1f dBFS after 50 ms, %.1f dBFS after 1.4 s\n", display(e, "AR Release").c_str(), db(rms(a, 0.04, 0.06)), db(rms(a, 1.4)));
        CHECK(rms(a, 0.04, 0.06) > 0.03 && rms(a, 1.4) < 3e-3, "AR release wrong");
        set(e, "VCA AR", 0); set(e, "VCA Initial Gain", 100); a = run(e, 0.5);
        set(e, "VCA Initial Gain", 0); auto z = run(e, 0.5);
        std::printf("  initial gain: drone %.1f dBFS, closed again %.6f\n", db(rms(a, 0.2)), rms(z, 0.3));
        CHECK(rms(a, 0.2) > 0.05 && rms(z, 0.3) < 1e-5, "initial gain wrong");
    }

    /* 5. one voice: the newest key sounds, releasing it returns to the held one; trigger modes */
    plain(e);
    {
        on(e, 57); run(e, 0.3); on(e, 69); double f2 = freq(run(e, 0.6), 440);
        off(e, 69); double f1 = freq(run(e, 0.6), 220); off(e, 57);
        double tail = rms(run(e, 0.3), 0.2);
        std::printf("  mono: second key %.1f Hz, back to first %.1f Hz, all up %.6f\n", f2, f1, tail);
        CHECK(std::fabs(cents(f2, 440)) < 5 && std::fabs(cents(f1, 220)) < 5 && tail < 1e-4, "key handling wrong");
        set(e, "ADSR Sustain", 0); set(e, "ADSR Decay", 50);
        double lv[2];
        for (int k = 0; k < 2; k++) {
            set(e, "Trigger", k); on(e, 57); run(e, 2.0); on(e, 69); lv[k] = rms(run(e, 0.05), 0.005); off(e, 69); off(e, 57); run(e, 0.3);
        }
        std::printf("  legato second key: single %.5f, multiple %.4f\n", lv[0], lv[1]);
        CHECK(lv[0] < 0.01 && lv[1] > 0.05, "trigger mode wrong");
        set(e, "Trigger", 0);
    }

    /* 6. portamento glides; LF mode gives a sub-audio VCO; cross FM adds sidebands; pan */
    plain(e);
    {
        set(e, "Portamento", 60);
        on(e, 45); run(e, 3.0); on(e, 69); auto a = run(e, 0.15); double mid = goertzel(a, 440, 0.05) / (rms(a, 0.05) + 1e-9);
        run(e, 4.0); double fe = freq(run(e, 0.6), 440); off(e, 69); off(e, 45);
        std::printf("  portamento %s: 440 Hz share early %.2f, later %.1f Hz\n", display(e, "Portamento").c_str(), mid, fe);
        CHECK(mid < 0.5 && std::fabs(cents(fe, 440)) < 5, "portamento wrong");
        set(e, "Portamento", 0);
        /* VCO 2 in LF mode modulating VCO 1: vibrato -> the 440 Hz line spreads */
        on(e, 69); run(e, 0.3); double c0 = goertzel(run(e, 1.0), 440);
        set(e, "VCO2 LF", 1); set(e, "VCO2 Coarse", 100); set(e, "VCO1 FM VCO2", 40);
        std::string lfd = display(e, "VCO2 Coarse");
        double c1 = goertzel(run(e, 1.0), 440);
        std::printf("  LF VCO 2 at %s as vibrato: 440 Hz line %.1f dB\n", lfd.c_str(), db(c1 / c0));
        CHECK(c1 < 0.7 * c0, "LF FM has no effect");
        set(e, "VCO2 LF", 0); set(e, "VCO2 Coarse", 67); set(e, "VCO1 FM VCO2", 50);
        auto b = run(e, 1.0);
        std::printf("  audio FM: 440 Hz line %.1f dB\n", db(goertzel(b, 440) / c0));
        CHECK(goertzel(b, 440) < 0.8 * c0, "audio FM has no effect");
        set(e, "VCO1 FM VCO2", 0);
        Buf R; set(e, "Pan", -50); auto L = run(e, 0.4, &R);
        double l = rms(L, 0.2); double r = rms(R, 0.2);
        Buf R2; set(e, "Pan", 50); auto L2 = run(e, 0.4, &R2);
        std::printf("  pan: left %.4f/%.6f, right %.6f/%.4f\n", l, r, rms(L2, 0.2), rms(R2, 0.2));
        CHECK(l > 0.05 && r < 1e-3 && rms(R2, 0.2) > 0.05 && rms(L2, 0.2) < 1e-3, "pan wrong");
        off(e, 69);
    }

    /* P2.1 ring modulator = VCO 1 saw x VCO 2 sine: sum and difference, no carrier; in the mixer and direct */
    plain(e);
    {
        enum { SRC_VCO3 = 3, SRC_NOISE = 4, SRC_SH = 5, SRC_ADSR = 6, SRC_LFO = 8, SRC_VEL = 10, SRC_AT = 11, SRC_WHEEL = 12 };
        enum { DST_P1 = 0, DST_PW3 = 4, DST_CUTOFF = 5, DST_VCA = 7, DST_PAN = 8, DST_LFO_RATE = 10 };
        (void)SRC_NOISE; (void)DST_PW3;
        set(e, "Mix VCO1 Saw", 0); set(e, "VCO2 Coarse", 48);      /* VCO 1 440 Hz, VCO 2 220 Hz at A4 */
        double c[2], sb[2];
        for (int k = 0; k < 2; k++) {
            set(e, "Mix Ring Mod", k ? 0 : 100); set(e, "VCA Ring Mod", k ? 100 : 0);
            on(e, 69); auto a = run(e, 0.8); off(e, 69); run(e, 0.2);
            c[k] = goertzel(a, 440); sb[k] = goertzel(a, 660);
            /* saw harmonic n*440 +- 220: all odd multiples of 220, none of the even ones */
            std::printf("  ring %s: 660 Hz %.1f dBFS, 440 Hz %.1f dBFS\n", k ? "direct" : "in mixer", db(sb[k]), db(c[k]));
            CHECK(sb[k] > 0.02 && c[k] < 0.05 * sb[k], "ring modulator wrong");
        }
        set(e, "VCA Ring Mod", 0);

        /* P2.2 noise colour: red has far less treble than white */
        set(e, "Mix Noise", 100); double hf[3];
        for (int k = 0; k < 3; k++) {
            set(e, "Noise Color", 50 * k); on(e, 60); auto a = run(e, 1.0); off(e, 60); run(e, 0.2);
            double d = 0; for (size_t i = 4410; i + 1 < a.size(); i++) d += std::fabs(a[i + 1] - a[i]);
            hf[k] = d / (a.size() - 4411) / rms(a);
            std::printf("  noise colour %3d: %.1f dBFS, treble measure %.3f\n", 50 * k, db(rms(a)), hf[k]);
        }
        CHECK(hf[1] < 0.8 * hf[0] && hf[2] < 0.4 * hf[1], "noise colour does not darken");
    }

    /* P2.3 LFO vibrato and its delay; S&H steps the pitch */
    plain(e);
    {
        on(e, 69); run(e, 0.3); double c0 = goertzel(run(e, 1.0), 440); off(e, 69); run(e, 0.2);
        set(e, "Vibrato Depth", 80); set(e, "Vibrato Delay", 60);
        on(e, 69); auto a = run(e, 0.9); auto b = run(e, 2.0); off(e, 69); run(e, 0.2);
        double early = goertzel(a, 440), late = goertzel(b, 440, 1.0);
        std::printf("  vibrato, delay %s: 440 Hz line %.1f dB early, %.1f dB late (LFO %s)\n", display(e, "Vibrato Delay").c_str(), db(early / c0), db(late / c0), display(e, "LFO Rate").c_str());
        CHECK(early > 0.9 * c0 && late < 0.7 * c0, "vibrato or its delay wrong");
        set(e, "Vibrato Depth", 0);
        set(e, "VCO1 FM S&H", 60); on(e, 69); a = run(e, 2.0); off(e, 69); run(e, 0.2);
        std::printf("  S&H (%s) on VCO 1: 440 Hz line %.1f dB\n", display(e, "S&H Rate").c_str(), db(goertzel(a, 440) / c0));
        CHECK(goertzel(a, 440) < 0.5 * c0, "S&H FM has no effect");
        /* with full lag at a slow clock the steps turn into slow drift: successive short windows change little */
        set(e, "VCO1 FM S&H", 0);
    }

    /* P2.4 REPEAT: the LFO square gates the envelopes; KEY needs a key, AUTO does not */
    plain(e);
    {
        set(e, "LFO Rate", 67); set(e, "ADSR Sustain", 0); set(e, "ADSR Decay", 30);   /* ~5 Hz, short blips */
        set(e, "Repeat", 1);
        double q = rms(run(e, 1.0), 0);
        on(e, 57); auto a = run(e, 2.0); off(e, 57); run(e, 0.5);
        int bursts = 0; bool hi = false;
        for (size_t i = 0; i + 441 < a.size(); i += 441) { double r = 0; for (int j = 0; j < 441; j++) r += std::fabs(a[i + j]); r /= 441; if (r > 0.05 && !hi) { hi = true; bursts++; } else if (r < 0.01) hi = false; }
        set(e, "Repeat", 2); double au = rms(run(e, 1.0), 0);
        set(e, "Repeat", 0); double z = rms(run(e, 1.0), 0.6);
        std::printf("  repeat KEY: silent without key %.6f, %d blips in 2 s with key; AUTO without key %.4f; off %.6f\n", q, bursts, au, z);
        CHECK(q == 0 && bursts >= 8 && bursts <= 12 && au > 0.01 && z < 1e-4, "repeat wrong");
    }

    /* P2.5 matrix */
    plain(e);
    {
        enum { SRC_VCO3 = 3, SRC_ADSR = 6, SRC_LFO = 8, SRC_VEL = 10, SRC_AT = 11, SRC_WHEEL = 12 };
        enum { DST_P1 = 0, DST_CUTOFF = 5, DST_VCA = 7, DST_PAN = 8 };
        on(e, 69); run(e, 0.3); auto ref = run(e, 1.0); double c0 = goertzel(ref, 440), h0 = goertzel(ref, 4400);
        /* a slot with a source but amount 0, or an amount but source OFF, does nothing */
        set(e, "Mod 1 Source", SRC_LFO); set(e, "Mod 1 Dest", DST_P1);
        double n1 = goertzel(run(e, 1.0), 440);
        set(e, "Mod 1 Source", 0); set(e, "Mod 1 Amount", 100);
        double n2 = goertzel(run(e, 1.0), 440);
        CHECK(std::fabs(n1 / c0 - 1) < 0.01 && std::fabs(n2 / c0 - 1) < 0.01, "an empty slot changes the sound");
        /* control rate: mod wheel closes the filter (negative amount = the inverter) */
        set(e, "Mod 1 Source", SRC_WHEEL); set(e, "Mod 1 Dest", DST_CUTOFF); set(e, "Mod 1 Amount", -100);
        double w0 = goertzel(run(e, 0.6), 4400); send(e, 0xb0, 1, 127); double w1 = goertzel(run(e, 0.6), 4400); send(e, 0xb0, 1, 0);
        std::printf("  matrix: mod wheel -> cutoff -100 %%: 10th harmonic %.1f dB -> %.1f dB\n", db(w0 / h0), db(w1 / h0));
        CHECK(w0 > 0.9 * h0 && w1 < 0.05 * h0, "mod wheel -> cutoff wrong");
        /* aftertouch opens the VCA further / LFO pans */
        set(e, "Mod 1 Source", SRC_AT); set(e, "Mod 1 Dest", DST_VCA); set(e, "Mod 1 Amount", -100);
        send(e, 0xd0, 127, 0); double at = rms(run(e, 0.5), 0.3); send(e, 0xd0, 0, 0);
        std::printf("  matrix: aftertouch -> VCA -100 %%: %.6f\n", at);
        CHECK(at < 1e-4, "aftertouch -> VCA wrong");
        set(e, "Mod 1 Source", SRC_LFO); set(e, "Mod 1 Dest", DST_PAN); set(e, "Mod 1 Amount", 100); set(e, "LFO Shape", 2); set(e, "LFO Rate", 43);   /* square, ~1 Hz */
        Buf R; auto L = run(e, 3.0, &R); double lmax = 0, rmax = 0, both = 0;
        for (size_t i = 0; i + 2205 < L.size(); i += 2205) { double l = 0, r = 0; for (int j = 0; j < 2205; j++) { l += std::fabs(L[i + j]); r += std::fabs(R[i + j]); } lmax = std::max(lmax, l / (l + r + 1e-9)); rmax = std::max(rmax, r / (l + r + 1e-9)); both += 0; }
        std::printf("  matrix: LFO square -> pan: left share up to %.2f, right share up to %.2f\n", lmax, rmax);
        CHECK(lmax > 0.95 && rmax > 0.95, "LFO -> pan wrong");
        /* ADSR -> pitch, negative: the note starts low and rises to pitch */
        set(e, "Mod 1 Source", SRC_ADSR); set(e, "Mod 1 Dest", DST_P1); set(e, "Mod 1 Amount", -50);   /* -1 octave at ADSR = 1 */
        off(e, 69); run(e, 0.3); on(e, 69); double fa = freq(run(e, 1.0), 220);
        std::printf("  matrix: ADSR -> VCO 1 pitch -50 %%: %.1f Hz\n", fa);
        CHECK(std::fabs(cents(fa, 220)) < 10, "ADSR -> pitch wrong");
        /* audio rate: VCO 3 pulse -> VCO 1 pitch (cross modulation) and -> cutoff */
        set(e, "Mod 1 Source", SRC_VCO3); set(e, "Mod 1 Dest", DST_P1); set(e, "Mod 1 Amount", 20); set(e, "VCO3 Coarse", 67);   /* +-0.16 octaves */
        auto xa = run(e, 1.0); double x1 = goertzel(xa, 440), f50 = freq(xa, 440);
        /* at pulse width 20 % the pulse is negative on average (-0.6): VCO 1 sits 0.096 octaves lower */
        set(e, "VCO3 Pulse Width", 20); run(e, 0.2); double f20 = freq(run(e, 1.0), 440 * std::pow(2.0, -0.096));
        std::printf("  matrix: VCO 3 pulse -> VCO 1 pitch: 440 Hz line %.1f dB; centre %+.0f ct, at pulse width 20 %% %+.0f ct\n", db(x1 / c0), cents(f50, 440), cents(f20, 440));
        CHECK(x1 < 0.8 * c0 && std::fabs(cents(f50, 440)) < 40 && std::fabs(cents(f20, 440) + 115) < 40, "cross modulation or VCO 3 pulse width wrong");
        set(e, "Mod 1 Amount", 50);
        set(e, "Mod 1 Dest", DST_CUTOFF); set(e, "Cutoff", 60);
        auto fm = run(e, 1.0); set(e, "Mod 1 Amount", 0); auto nf = run(e, 1.0);
        double sbm = goertzel(fm, 440 + 659.26), sbn = goertzel(nf, 440 + 659.26);
        std::printf("  matrix: VCO 3 -> cutoff: sideband at 1099 Hz %.1f dBFS (without %.1f dBFS)\n", db(sbm), db(sbn));
        CHECK(sbm > 5 * sbn && sbm > 1e-3, "audio-rate filter FM wrong");
        off(e, 69);
    }

    /* P3.1 spring reverb: a tail after the note, longer with LENGTH, stereo, silent again; matrix -> reverb */
    plain(e);
    {
        double tail[3], late[2];
        for (int k = 0; k < 3; k++) {
            set(e, "Reverb", k ? 70 : 0); set(e, "Reverb Length", k == 2 ? 100 : 0);
            on(e, 57); run(e, 0.3); off(e, 57); Buf R; auto L = run(e, 3.0, &R);
            tail[k] = rms(L, 0.1, 0.3); if (k) late[k - 1] = rms(L, 1.5, 2.5);
            if (k == 2) { double d = 0; for (size_t i = 4410; i < 13230; i++) d += std::fabs(L[i] - R[i]); CHECK(d / 8820 > 0.3 * tail[2], "reverb is mono"); }
            run(e, 8.0);
        }
        double z = rms(run(e, 1.0), 0);
        std::printf("  reverb: tail %.6f dry, %.1f dBFS short (%s), %.1f dBFS long; 2 s later %.1f / %.1f dBFS; after 11 s %.6f\n",
                    tail[0], db(tail[1]), "0.4 s", db(tail[2]), db(late[0]), db(late[1]), z);
        CHECK(tail[0] < 1e-5 && tail[1] > 1e-3 && tail[2] > 1e-3 && late[1] > 10 * late[0] && late[1] > 1e-4 && z == 0, "reverb wrong");
        set(e, "Reverb", 0); set(e, "Mod 1 Source", 12); set(e, "Mod 1 Dest", 9); set(e, "Mod 1 Amount", 100); send(e, 0xb0, 1, 127);
        on(e, 57); run(e, 0.3); off(e, 57); double mt = rms(run(e, 1.0), 0.1, 0.3); send(e, 0xb0, 1, 0); run(e, 10.0);
        std::printf("  matrix: mod wheel -> reverb: tail %.1f dBFS\n", db(mt));
        CHECK(mt > 1e-3, "matrix -> reverb wrong");
    }

    /* P3.2 duophonic: VCO 1 on the lower key, VCO 2 on the upper; mono plays both on the newest key */
    plain(e);
    {
        set(e, "Mix VCO2 Triangle", 100);
        double lo[2], hi[2];
        for (int k = 0; k < 2; k++) {
            set(e, "Voice Mode", k); run(e, 0.1);
            on(e, 57); on(e, 64); auto a = run(e, 0.8);
            lo[k] = goertzel(a, 220); hi[k] = goertzel(a, 329.63);
            if (k) { off(e, 64); auto b = run(e, 0.8); CHECK(goertzel(b, 329.63, 0.3) < 0.05 * hi[1] && goertzel(b, 440, 0.3) > 0.02, "duophonic: upper key released, VCO 2 does not come down"); }
            off(e, 64); off(e, 57); run(e, 0.3);
        }
        std::printf("  mono, keys A3+E4: 220 Hz %.4f, 330 Hz %.4f; duophonic: 220 Hz %.4f, 330 Hz %.4f\n", lo[0], hi[0], lo[1], hi[1]);
        CHECK(lo[0] < 0.01 * hi[0] && lo[1] > 0.05 && hi[1] > 0.05, "duophonic mode wrong");
    }

    /* P3.3 poly: four voices, the fifth key takes the oldest */
    plain(e);
    {
        set(e, "Voice Mode", 2); run(e, 0.1);
        on(e, 57); run(e, 0.05); on(e, 61); run(e, 0.05); on(e, 64); auto a = run(e, 0.8);
        double c3[3] = {goertzel(a, 220, 0.3), goertzel(a, 277.18, 0.3), goertzel(a, 329.63, 0.3)};
        on(e, 68); run(e, 0.05); on(e, 71); auto b = run(e, 0.8);
        double first = goertzel(b, 220, 0.3), fifth = goertzel(b, 493.88, 0.3), pk = peak(b);
        for (int n : {57, 61, 64, 68, 71}) off(e, n);
        double z = rms(run(e, 0.5), 0.3);
        std::printf("  poly: chord %.4f / %.4f / %.4f; five keys: first %.5f, fifth %.4f, peak %.3f; released %.6f\n", c3[0], c3[1], c3[2], first, fifth, pk, z);
        CHECK(c3[0] > 0.03 && c3[1] > 0.03 && c3[2] > 0.03 && first < 0.1 * c3[0] && fifth > 0.03 && z < 1e-4, "poly mode wrong");
        set(e, "Voice Mode", 0); run(e, 0.1);
    }

    /* P3.4 presets: each one sounds and stays in range; INIT restores the start patch; a chunk keeps edits */
    {
        AEffect *fresh = mainf(master);
        fresh->dispatcher(fresh, 10, 0, 0, nullptr, SR);
        for (int k = 1; k < NPRESETS; k++) {
            set(e, "Preset", k);
            std::string name = display(e, "Preset");
            on(e, 48); run(e, 0.02); on(e, 55); auto a = run(e, 1.5); off(e, 55); off(e, 48); auto tl = run(e, 2.0);
            send(e, 0xb0, 123, 0);
            std::printf("  preset %-12s first 150 ms %6.1f dBFS, 1.5 s %6.1f dBFS, peak %.3f, 1-2 s after release %6.1f dBFS\n", name.c_str(), db(rms(a, 0, 0.15)), db(rms(a, 0)), peak(a), db(rms(tl, 1.0)));
            CHECK(rms(a, 0) > 3e-3 && peak(a) <= 0.981, "preset %s silent or too hot", name.c_str());
        }
        set(e, "Preset", 1); set(e, "Cutoff", 77);
        void *chunk = nullptr; intptr_t len = e->dispatcher(e, 23, 0, 0, &chunk, 0);
        std::vector<uint8_t> copy((uint8_t *)chunk, (uint8_t *)chunk + len);
        AEffect *e2 = mainf(master);
        e2->dispatcher(e2, 24, 0, (intptr_t)copy.size(), copy.data(), 0);
        CHECK(display(e2, "Preset") == "BASS" && display(e2, "Cutoff") == display(e, "Cutoff") && display(e2, "VCO2 Coarse") == display(e, "VCO2 Coarse"), "chunk after a preset loses the edit");
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
        set(e, "Preset", 0);
        int diff = 0; char b1[64], b2[64];
        for (int i = 0; i < e->numParams; i++) { b1[0] = b2[0] = 0; e->dispatcher(e, 7, i, 0, b1, 0); fresh->dispatcher(fresh, 7, i, 0, b2, 0); if (std::strcmp(b1, b2)) diff++; }
        CHECK(diff == 0, "INIT differs from the start patch in %d parameters", diff);
        fresh->dispatcher(fresh, 1, 0, 0, nullptr, 0);
        run(e, 12.0);
    }

    /* 7. everything up: stays below 0 dBFS */
    plain(e);
    {
        for (const char *k : {"Mix VCO1 Square", "Mix VCO2 Pulse", "Mix VCO3 Saw", "Mix Noise", "Mix VCO1 Saw", "Mix VCO2 Triangle", "VCF Drive", "Volume", "Resonance"}) set(e, k, 100);
        on(e, 40, 127); double pk = peak(run(e, 1.0)); off(e, 40);
        std::printf("  hot: peak %.3f\n", pk);
        CHECK(pk < 1.0, "output above 0 dBFS");
    }

    /* 8. chunk */
    set(e, "VCF Type", 1); set(e, "Trigger", 1); set(e, "Cutoff", 37.5); set(e, "VCO2 Coarse", 55); set(e, "VCO3 Fine", -33); set(e, "Pan", -20);
    {
        void *chunk = nullptr;
        intptr_t len = e->dispatcher(e, 23, 0, 0, &chunk, 0);
        std::vector<uint8_t> copy((uint8_t *)chunk, (uint8_t *)chunk + len);
        AEffect *e2 = mainf(master);
        e2->dispatcher(e2, 10, 0, 0, nullptr, SR);
        e2->dispatcher(e2, 24, 0, (intptr_t)copy.size(), copy.data(), 0);
        for (const char *k : {"VCF Type", "Trigger", "Cutoff", "VCO2 Coarse", "VCO3 Fine", "Pan", "Volume"})
            CHECK(display(e2, k) == display(e, k), "chunk: %s %s vs %s", k, display(e2, k).c_str(), display(e, k).c_str());
        CHECK(std::fabs(e2->getParameter(e2, param(e2, "Cutoff")) - 0.375f) < 1e-4, "chunk loses knob resolution");
        std::printf("  chunk %ld bytes: %s, %s, %s, %s, %s, %s\n", (long)len, display(e2, "VCF Type").c_str(), display(e2, "Trigger").c_str(),
                    display(e2, "Cutoff").c_str(), display(e2, "VCO2 Coarse").c_str(), display(e2, "VCO3 Fine").c_str(), display(e2, "Pan").c_str());
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
    }

    /* 9. stress: random parameters, keys, bends and sample offsets; also 48 and 96 kHz */
    {
        std::srand(2600);
        double pk = 0;
        for (float sr : {44100.0f, 48000.0f, 96000.0f}) {
            e->dispatcher(e, 10, 0, 0, nullptr, sr);
            for (int it = 0; it < 400; it++) {
                for (int j = 0; j < 6; j++) e->setParameter(e, std::rand() % e->numParams, (float)std::rand() / RAND_MAX);
                int r = std::rand() % 4, n = 20 + std::rand() % 90;
                if (r == 0) send(e, 0x90, n, 1 + std::rand() % 127, std::rand() % BS);
                else if (r == 1) send(e, 0x80, n, 0, std::rand() % BS);
                else if (r == 2) send(e, 0xe0, std::rand() % 128, std::rand() % 128, std::rand() % BS);
                pk = std::max(pk, peak(run(e, 0.03)));
            }
            send(e, 0xb0, 123, 0);
        }
        std::printf("  stress: peak %.3f\n", pk);
        CHECK(pk < 1.0, "stress run above 0 dBFS");
    }

    CHECK(!bad, "NaN, Inf or denormals in the output");
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
