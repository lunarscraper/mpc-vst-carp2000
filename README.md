# carp 2000

Repo: `mpc-vst-carp2000`

Semi-modularer Synthesizer nach dem Vorbild des ARP 2600 als VST2-Plugin für Akai Force und MPC (Standalone), gebaut auf dem mpc-vst-plugins-Gerüst (DPF).

**Status:** Alle drei Phasen des Fahrplans sind umgesetzt, siehe [Stand](#stand).

## Idee

Der 2600 ist ein vorverdrahteter Modularsynth: Ohne Kabel gibt es einen festen Signalweg, mit Kabeln lässt sich alles umstecken. Freies Patchen ist auf dem Force-Display kaum bedienbar. Dieses Plugin übernimmt deshalb

- die **feste Vorverdrahtung** des Originals: jede normalisierte Verbindung ist ein Regler, 0 bedeutet „kein Kabel",
- eine **Modulationsmatrix mit 6 Slots** als Ersatz für die Patchkabel.

Ziel ist der typische Klangcharakter und die typischen Patches, keine schaltungsgenaue Emulation.

## Signalfluss

```
VCO 1 ─┐
VCO 2 ─┤
VCO 3 ─┼─► Filtermixer ─► VCF ─► VCA ─► Federhall ─► Stereo Out
Noise ─┤                          ▲
Ringmod┘──────────────────────────┘ (direkt auf VCA, optional)

Ringmod = VCO 1 Sägezahn × VCO 2 Sinus
S&H     = Rauschen (oder VCO), getaktet von interner Clock
```

## Parameter

Acht Seiten zu je acht Parametern, passend zu den acht Knobs der Force.

### Seite 1 – VCO 1

| Nr. | Parameter | Bereich | Hinweis |
|---|---|---|---|
| 1 | Coarse | 10 Hz – 10 kHz | im LF-Modus 0,03 – 30 Hz |
| 2 | Fine | ± 1 Halbton | |
| 3 | Audio / LF | Schalter | LF = als LFO nutzbar |
| 4 | FM S&H | 0 – 100 % | |
| 5 | FM ADSR | 0 – 100 % | |
| 6 | FM VCO 2 Sinus | 0 – 100 % | |
| 7 | Keyboard | an / aus | aus = Drone / fester LFO |
| 8 | Portamento | 0 – 3 s | wirkt auf alle VCOs |

Wellenformen: Sägezahn, Rechteck.

### Seite 2 – VCO 2

| Nr. | Parameter | Bereich | Hinweis |
|---|---|---|---|
| 1 | Coarse | 10 Hz – 10 kHz | |
| 2 | Fine | ± 1 Halbton | |
| 3 | Audio / LF | Schalter | |
| 4 | Pulsbreite | 10 – 90 % | |
| 5 | PWM Rauschen | 0 – 100 % | |
| 6 | FM S&H | 0 – 100 % | |
| 7 | FM ADSR | 0 – 100 % | |
| 8 | FM VCO 1 Rechteck | 0 – 100 % | |

Wellenformen: Sinus, Dreieck, Sägezahn, Puls.

### Seite 3 – VCO 3

| Nr. | Parameter | Bereich | Hinweis |
|---|---|---|---|
| 1 | Coarse | 10 Hz – 10 kHz | |
| 2 | Fine | ± 1 Halbton | |
| 3 | Audio / LF | Schalter | |
| 4 | Pulsbreite | 10 – 90 % | nur manuell |
| 5 | FM Rauschen | 0 – 100 % | |
| 6 | FM ADSR | 0 – 100 % | |
| 7 | FM VCO 2 Sinus | 0 – 100 % | |
| 8 | Keyboard | an / aus | |

Wellenformen: Sägezahn, Puls.

### Seite 4 – Filtermixer

| Nr. | Parameter | Hinweis |
|---|---|---|
| 1 | Ringmodulator | |
| 2 | VCO 1 Rechteck | |
| 3 | VCO 2 Puls | |
| 4 | VCO 3 Sägezahn | |
| 5 | Rauschen | |
| 6 | Rauschfarbe | weiß → rosa → rot |
| 7 | VCO 1 Sägezahn | ersetzt typischen Umsteck-Patch |
| 8 | VCO 2 Dreieck | ersetzt typischen Umsteck-Patch |

### Seite 5 – VCF

| Nr. | Parameter | Hinweis |
|---|---|---|
| 1 | Cutoff | 10 Hz – 10 kHz |
| 2 | Resonanz | bis Selbstoszillation |
| 3 | Keyboard-Tracking | 0 – 100 % |
| 4 | ADSR-Anteil | |
| 5 | FM VCO 2 Sinus | |
| 6 | Drive | Eingangspegel in die Ladder |
| 7 | Filtertyp | 4012 (früh) / 4072 (spät) |
| 8 | Velocity → Cutoff | Erweiterung gegenüber dem Original |

### Seite 6 – Hüllkurven

| Nr. | Parameter | Hinweis |
|---|---|---|
| 1 | ADSR Attack | |
| 2 | ADSR Decay | |
| 3 | ADSR Sustain | |
| 4 | ADSR Release | |
| 5 | AR Attack | |
| 6 | AR Release | |
| 7 | Trigger-Modus | single / multiple |
| 8 | Repeat | Hüllkurven vom LFO neu auslösen |

### Seite 7 – VCA und Ausgang

| Nr. | Parameter | Hinweis |
|---|---|---|
| 1 | Initial Gain | VCA dauerhaft offen (Drones) |
| 2 | AR-Anteil | linear |
| 3 | ADSR-Anteil | exponentiell |
| 4 | Ringmod direkt | am Filter vorbei |
| 5 | Pan | |
| 6 | Hall-Anteil | |
| 7 | Hall-Länge | |
| 8 | Volume | |

### Seite 8 – LFO, S&H, Stimmen

| Nr. | Parameter | Hinweis |
|---|---|---|
| 1 | LFO-Rate | |
| 2 | LFO-Form | Sinus / Dreieck / Rechteck |
| 3 | Vibrato-Tiefe | |
| 4 | Vibrato-Delay | |
| 5 | S&H-Rate | interne Clock |
| 6 | S&H-Quelle | Rauschen / VCO 1 / VCO 2 |
| 7 | S&H-Lag | Glättung |
| 8 | Stimmenmodus | mono / duophon / poly 4 |

## Modulationsmatrix

Sechs Slots, je **Quelle**, **Ziel**, **Stärke** (−100 … +100 %). Die negative Stärke ersetzt den Inverter des Originals.

**Quellen**

- VCO 1, VCO 2, VCO 3 (auch im Audiobereich)
- Rauschen
- S&H
- ADSR, AR
- LFO
- Keyboard-CV
- Velocity, Aftertouch, Modwheel

**Ziele**

- Tonhöhe VCO 1, VCO 2, VCO 3
- Pulsbreite VCO 2, VCO 3
- Cutoff, Resonanz
- VCA-Pegel, Pan
- Hall-Anteil
- LFO-Rate, S&H-Rate

**Beispiel-Patches**

| Klang | Slot |
|---|---|
| Audio-FM auf den Filter | VCO 1 → Cutoff |
| Cross-Modulation | VCO 3 → Tonhöhe VCO 1 |
| S&H-Blubbern | S&H → Cutoff, Repeat an |
| Percussion / Snare | Rauschen → Tonhöhe VCO 1, kurze ADSR |
| Aftertouch-Filter | Aftertouch → Cutoff |

## DSP

| Baustein | Ansatz |
|---|---|
| Oszillatoren | PolyBLEP, Sinus per Phasor |
| Filter | nichtlineare 4-Pol-Ladder, 2× Oversampling nur im Filter |
| Ringmodulator | Multiplikation, AC/DC-Schalter |
| Rauschen | weiß, per Tiefpass zu rosa/rot überblendet |
| Federhall | kleines Allpass-/Delay-Netz, keine Faltung |
| Hüllkurven | exponentielle Segmente |

### Performance-Regeln

- durchgehend `float`, kein `double`
- `tanh` in der Ladder als schnelle Näherung, nicht aus der libm
- langsame Modulationen (Hüllkurven, LFO, S&H, Regler) im Kontrolltakt, alle 16 Samples, mit Glättung
- Matrix-Slots nur dann in Audiorate rechnen, wenn die Quelle ein VCO oder Rauschen ist
- Denormals abfangen (Flush-to-zero bzw. kleiner DC-Offset)
- Build mit `-O3`, NEON und `-ffast-math`
- inaktive Stimmen und stumme Mixerkanäle überspringen
- Federhall einmal pro Plugin, nicht pro Stimme

## Fahrplan

1. **Grundstimme (mono):** 3 VCOs, Filtermixer, VCF, ADSR/AR, VCA. CPU-Last auf der Force messen.
2. **Modulation:** LFO, S&H, Ringmodulator, Rauschfarbe, Matrix.
3. **Ausbau:** Federhall, Duophonie, Poly 4, Presets.

## Stand

Code in `vst/`: `carp_core.h` (die Stimme), `carp_vst.cpp` (Plugin, MIDI, Projekt-Chunk), `host_test.cpp` (Offline-Test und Benchmark). Bauen über GitHub Actions („VST release (draft)") oder lokal mit `vst/build.sh`, danach `vst/test.sh`.

Umgesetzt sind die Seiten 1 bis 8, die Matrix, der Federhall, die drei Stimmenmodi und 13 Presets (eigener Tab PRESET).

Festlegungen, die das Konzept offen ließ:

- **Coarse** rastet in Halbtönen. Mit Keyboard zeigt der Regler die Transposition zur gespielten Taste (`+12 st`), ohne Keyboard oder im LF-Modus die Frequenz.
- **LF-Modus** trennt wie beim Original die Tastatur ab. VCO 2 hat keinen eigenen Keyboard-Schalter und folgt im Audio-Modus immer der Tastatur.
- **Tastatur (mono):** eine Stimme, die zuletzt gedrückte Taste klingt. Trigger „single" löst bei gebundenem Spiel nicht neu aus, „multiple" bei jeder Taste. Pitchbend ± 2 Halbtöne.
- **Filtertyp:** 4012 reicht bis 18 kHz, 4072 endet wie das Original bei etwa 11 kHz. Weitere Unterschiede der beiden Schaltungen sind nicht modelliert.
- **Dreieck von VCO 2** ist nicht bandbegrenzt (Obertöne fallen mit 1/n², Aliasing bleibt gering).
- **Repeat** hat drei Stellungen: OFF, KEY (das LFO-Rechteck ist das Gate, solange eine Taste gehalten wird) und AUTO (auch ohne Taste). Der Takt kommt wie im Konzept vom LFO, nicht von der S&H-Clock.
- **LFO** 0,05 – 50 Hz, **S&H-Clock** 0,1 – 100 Hz. Vibrato bis ± 2 Halbtöne auf alle VCOs, die der Tastatur folgen; nach dem Delay wird es eingeblendet.
- **Rauschfarbe** wirkt auf alles, was Rauschen nutzt (Mixer, FM, PWM, S&H, Matrix).
- **Ringmodulator** ohne AC/DC-Schalter: Auf den acht Seiten ist kein Platz dafür, und bei Sägezahn × Sinus ist der Unterschied klein.
- **Matrix-Quellen:** VCO 1 liefert den Sägezahn, VCO 2 den Sinus, VCO 3 den Puls (dafür ist dessen Pulsbreite da). Diese drei und das Rauschen modulieren in Audiorate, alle anderen im Kontrolltakt. LFO-Rate und S&H-Rate nehmen auch Audioquellen nur im Kontrolltakt an.
- **Matrix-Stärke** quadratisch (feinfühlig um 0). ± 100 % entsprechen ± 4 Oktaven Tonhöhe, ± 5 Oktaven Cutoff, ± 40 % Pulsbreite, der vollen Resonanz, dem vollen VCA-Pegel, voll links/rechts und ± 4 Oktaven Rate.
- **Duophon:** wie bei der 3620-Tastatur. Die tiefste gehaltene Taste spielt VCO 1, VCO 3 und das Filter-Tracking, die höchste VCO 2. Filter, Hüllkurven und VCA gibt es weiter nur einmal.
- **Poly 4:** vier vollständige Stimmen, jede mit eigenem LFO und S&H. Die fünfte Taste übernimmt die älteste Stimme. Jede Taste löst neu aus, der Trigger-Schalter wirkt nur mono und duophon. Der Summenpegel ist um 3 dB abgesenkt. Initial Gain und Repeat AUTO wirken nur auf die erste Stimme, sonst klängen ohne Taste vier Stimmen.
- **Federhall:** einmal pro Plugin. Zwei Federn (links/rechts), jede eine Verzögerung mit gedämpfter Rückkopplung und einer Kette gestreckter Allpässe für das typische Zwitschern. Hall-Anteil ist der Send hinter dem VCA, das Direktsignal bleibt unverändert. Länge 0,4 – 4 s.
- **Presets** sind ein Parameter: Auswählen überschreibt alle Regler. Projekte speichern die Regler selbst, ein nach dem Laden verändertes Preset bleibt also erhalten. VST-Programme nutzt das Plugin nicht.
- Neue Parameter werden hinten angehängt. Projekte speichern nach Schlüssel und bleiben ladbar.

CPU-Last im Offline-Benchmark auf einem x86-Kern: etwa 0,5 % für das Startpatch, 0,8 % im ungünstigsten Mono-Fall (alle Quellen, sechs Matrix-Slots in Audiorate) und 3,2 % für denselben Fall mit vier Stimmen und Hall.

## Nicht enthalten

- Vorverstärker und Envelope Follower (brauchen einen Audioeingang). Denkbar als eigenes Effekt-Plugin.
- Freies Patchfeld
- Elektronischer Schalter, Voltage Processors (durch die Matrix abgedeckt)

## Lizenz und Quellen

- Bristol (GPL) dient als Vergleich für Kennlinien. Wird Code daraus übernommen, muss das Plugin unter GPL stehen.
- „ARP" und „2600" sind Marken ihrer jeweiligen Inhaber. Dieses Projekt steht in keiner Verbindung zu ihnen.
