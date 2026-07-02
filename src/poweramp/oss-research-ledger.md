# Orbit power-amp OSS research for an AGPLv3-or-later guitar plugin

Snapshot date: 2026-06-30. Target context: free/open-source C++20/JUCE 8 guitar plugin, licensed GNU AGPLv3-or-later, with a tube-style power-amp stage placed before an existing cabinet IR convolution block.

## 1) LICENSE COMPATIBILITY MATRIX

Legal baseline used for the AGPL verdicts: GNU AGPLv3 is GPLv3 plus an additional network-interaction source-offer term, and AGPLv3 section 13 explicitly permits linking/combining AGPLv3 code with GPLv3 code while keeping the AGPL terms on the AGPL part and GPLv3 terms on the GPL part ([GNU AGPL rationale](https://www.gnu.org/licenses/why-affero-gpl.en.html), [AGPLv3 section 13 text](https://www.gnu.org/licenses/agpl-3.0.en.html)). FSF lists MIT/Expat, BSD-2/3, Apache-2.0, and LGPLv3/LGPLv2.1 as GPL-compatible, with Apache-2.0 compatible with GPLv3 but not GPLv2 ([MIT/Expat](https://www.gnu.org/licenses/license-list.en.html#Expat), [BSD-3/2](https://www.gnu.org/licenses/license-list.en.html#ModifiedBSD), [Apache-2.0](https://www.gnu.org/licenses/license-list.en.html#apache2), [LGPL](https://www.gnu.org/licenses/license-list.en.html#LGPLv3)).

“AGPLv3-OK?” below means “can be incorporated into or linked with an AGPLv3-or-later plugin at all,” not “no obligations.” Permissive code still requires notices; GPLv3 code keeps its GPLv3 terms and makes the combined distribution a copyleft distribution; model files/data are treated separately when their license is not the code license.

| Project | License verified from source | AGPLv3-OK? | Knob-control? | RT-safe? | Reuse value for a power-amp-before-cab stage |
|---|---:|---:|---:|---:|---|
| [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) | MIT ([LICENSE](https://raw.githubusercontent.com/sdatkinson/NeuralAmpModelerCore/main/LICENSE)) | Yes, keep MIT notices | No true amp-knob conditioning in normal NAM captures; fixed model snapshot unless the model format/architecture itself is conditioned ([PANAMA paper notes NAM lacks parametric knob controls](https://arxiv.org/html/2509.26564v1)) | Designed as real-time NAM core and includes `benchmodel`, but model loading and dynamic changes must be kept off the audio thread ([README](https://github.com/sdatkinson/NeuralAmpModelerCore)) | Best permissive fixed-capture engine if you allow user-loaded power-amp captures; not enough for working gain/presence/depth knobs by itself |
| [sdatkinson/neural-amp-modeler trainer](https://github.com/sdatkinson/neural-amp-modeler) | MIT ([LICENSE](https://raw.githubusercontent.com/sdatkinson/neural-amp-modeler/main/LICENSE)) | Yes, keep MIT notices | Standard NAM training is non-parametric; trainer exports `.nam` files ([README](https://github.com/sdatkinson/neural-amp-modeler)) | Offline Python training; not audio-thread code | Useful for training fixed power-amp or full-rig captures; not a drop-in plugin DSP block |
| [NeuralAmpModelerPlugin](https://github.com/sdatkinson/NeuralAmpModelerPlugin) | MIT ([LICENSE](https://raw.githubusercontent.com/sdatkinson/NeuralAmpModelerPlugin/main/LICENSE)) | Yes, keep MIT notices | Fixed model player UI; not a parametric amp-control system ([repo](https://github.com/sdatkinson/NeuralAmpModelerPlugin)) | Public plugin/release reference; audit UI/file loading paths before copying patterns ([releases](https://github.com/sdatkinson/NeuralAmpModelerPlugin/releases)) | Useful as integration reference; iPlug2-oriented, not JUCE |
| [RTNeural](https://github.com/jatinchowdhury18/RTNeural) | BSD-3-Clause ([LICENSE](https://raw.githubusercontent.com/jatinchowdhury18/RTNeural/main/LICENSE)) | Yes, keep BSD notices | Model-dependent; supports layers/activations, not knob semantics by itself ([README](https://github.com/jatinchowdhury18/RTNeural)) | Explicitly designed for real-time neural audio inference; commit history includes RealtimeSanitizer work ([README](https://github.com/jatinchowdhury18/RTNeural), [commits/activity](https://github.com/jatinchowdhury18/RTNeural/commits/main/)) | Best permissive neural inference backend for your own conditioned LSTM/GRU/WaveNet export |
| [RTNeural-NAM](https://github.com/jatinchowdhury18/RTNeural-NAM) | BSD-3-Clause ([LICENSE](https://raw.githubusercontent.com/jatinchowdhury18/RTNeural-NAM/main/LICENSE)) | Yes, keep BSD notices | No; NAM-style WaveNet implementation is fixed-model unless extended ([README](https://github.com/jatinchowdhury18/RTNeural-NAM)) | Small experimental repo; TODOs mention optimization/gated activations ([README](https://github.com/jatinchowdhury18/RTNeural-NAM)) | Educational bridge between NAM-style networks and RTNeural; not a mature product path |
| [NeuralAudio](https://github.com/mikeoliphant/NeuralAudio) | MIT ([LICENSE](https://raw.githubusercontent.com/mikeoliphant/NeuralAudio/main/LICENSE)) | Yes, keep MIT notices | Mostly fixed NAM/AIDA/RTNeural model playback; conditioning depends on loaded model architecture ([README](https://github.com/mikeoliphant/NeuralAudio)) | README exposes `SetMaxAudioBufferSize`/buffer allocation considerations and quality-change RT-safety checks; latest commits shown on 2026-06-27 in commit history ([README](https://github.com/mikeoliphant/NeuralAudio), [commits](https://github.com/mikeoliphant/NeuralAudio/commits/main/)) | Strong permissive loader/runtime if you want NAM plus AIDA/RTNeural JSON support |
| [neural-amp-modeler-lv2](https://github.com/mikeoliphant/neural-amp-modeler-lv2) | GPL-3.0 ([LICENCE.md](https://raw.githubusercontent.com/mikeoliphant/neural-amp-modeler-lv2/main/LICENCE.md)) | Yes-with-obligations under AGPLv3 section 13 | Fixed model file plus input/output/quality controls; cab IR expected after amp-only models ([README](https://github.com/mikeoliphant/neural-amp-modeler-lv2)) | Public LV2 plugin; oversampling is documented as alias-reducing but costly ([README](https://github.com/mikeoliphant/neural-amp-modeler-lv2)) | Good Linux/LV2 reference, especially for model loading and oversampling decisions |
| [NeuralRack](https://github.com/brummer10/NeuralRack) | BSD-3-Clause repo license ([LICENSE](https://raw.githubusercontent.com/brummer10/NeuralRack/main/LICENSE)); dependencies separately | Yes for repo code; verify submodule/dependency licenses | Fixed NAM/JSON/AIDA-X models plus chain controls/IR loading ([README](https://github.com/brummer10/NeuralRack)) | Public plugin; includes buffer mode/resampling notes, but full audio-thread audit required ([README](https://github.com/brummer10/NeuralRack)) | Useful player-chain reference for Linux/CLAP/LV2/VST2 style architecture |
| [AIDA-X](https://github.com/AidaDSP/AIDA-X) | GPL-3.0-or-later reported by repo ([README/license block](https://github.com/AidaDSP/AIDA-X)) | Yes-with-obligations; GPL code keeps GPL terms | Loaded AI model is fixed; plugin adds input, EQ, depth, presence, output controls ([README controls](https://github.com/AidaDSP/AIDA-X)) | Public amp-model player; depends on RTNeural/DPF/FFTConvolver/r8brain ([README](https://github.com/AidaDSP/AIDA-X)) | Useful GPL reference for neural amp player plus post/pre EQ, presence/depth UI; not a power-amp model itself |
| [aidadsp-lv2](https://github.com/AidaDSP/aidadsp-lv2) | GPL-3.0 ([LICENSE](https://raw.githubusercontent.com/AidaDSP/aidadsp-lv2/master/LICENSE)) | Yes-with-obligations | Fixed RTNeural JSON model plus 5-band EQ/input/output controls ([README](https://github.com/AidaDSP/aidadsp-lv2)) | Public LV2 generic model loader; XSIMD/Eigen/RTNeural build options documented ([README](https://github.com/AidaDSP/aidadsp-lv2)) | Simpler GPL reference for generic neural amp/pedal model playback |
| [GuitarML Proteus](https://github.com/GuitarML/Proteus) | GPL-3.0 reported by repo ([repo/license](https://github.com/GuitarML/Proteus)) | Yes-with-obligations | Yes for single drive/tone knob captures or snapshots; knob captures require multiple output WAVs ([README](https://github.com/GuitarML/Proteus)) | Public plugin; open issues include crash/sample-rate/build reports, so audit before reuse ([issues](https://github.com/GuitarML/Proteus/issues)) | Strongest mature GuitarML knob-capture reference; good for “one control works” neural UX |
| [GuitarML ToneLibrary](https://github.com/GuitarML/ToneLibrary) | GPL-3.0 repo license ([repo](https://github.com/GuitarML/ToneLibrary)) | Yes-with-obligations, but provenance of emailed/community models should be checked | Some models are knob captures; GuitarML site marks knob vs snapshot models ([ToneLibrary page](https://guitarml.com/tonelibrary/tonelib-pro.html)) | Data/model files, not code | Useful GPL model corpus; Open-Amp paper reports 59 amp captures, 101 pedal captures, and 65 single-parameter models in the Proteus Tone Packs ([Open-Amp HTML](https://arxiv.org/html/2411.14972v1)) |
| [SmartGuitarAmp](https://github.com/GuitarML/SmartGuitarAmp) | Apache-2.0 ([LICENSE](https://raw.githubusercontent.com/GuitarML/SmartGuitarAmp/main/LICENSE)) | Yes, Apache-2.0 is GPLv3-compatible; keep notices/patent/license terms ([FSF Apache-2.0 compatibility](https://www.gnu.org/licenses/license-list.en.html#apache2)) | Built-in Gain/EQ knobs modulate a WaveNet small-tube-amp model, but custom model loading was removed in v1.3 ([README](https://github.com/GuitarML/SmartGuitarAmp), [v1.3 release](https://github.com/GuitarML/SmartGuitarAmp/releases)) | Public JUCE plugin; older 2022 release line ([releases](https://github.com/GuitarML/SmartGuitarAmp/releases)) | Learning reference for WaveNet/JUCE UI; less attractive than newer model loaders |
| [SmartGuitarPedal](https://github.com/GuitarML/SmartGuitarPedal) | Ambiguous: README/GitHub show Apache-2.0, but repo has both [Apache LICENSE](https://raw.githubusercontent.com/GuitarML/SmartGuitarPedal/main/LICENSE) and [GPL-3.0 LICENSE.txt](https://raw.githubusercontent.com/GuitarML/SmartGuitarPedal/main/LICENSE.txt) | Unclear until file headers/provenance are audited; if Apache-only portions, yes; if GPL portions, yes-with-obligations | Yes for single-parameter conditioned models as of v1.5 ([README](https://github.com/GuitarML/SmartGuitarPedal)) | Public JUCE plugin; older 2022 release line ([repo](https://github.com/GuitarML/SmartGuitarPedal)) | Valuable single-knob conditioning reference, but license ambiguity blocks direct reuse without audit |
| [Chameleon](https://github.com/GuitarML/Chameleon) | GPL-3.0 ([repo/license](https://github.com/GuitarML/Chameleon)) | Yes-with-obligations | Gain/EQ around three core amp-head sounds; not evidence of full parametric capture ([README](https://github.com/GuitarML/Chameleon)) | Public JUCE/RTNeural plugin; release v1.2 in 2022 ([releases](https://github.com/GuitarML/Chameleon/releases)) | LSTM/JUCE/RTNeural reference; not a reusable power-amp block |
| [Automated-GuitarAmpModelling](https://github.com/GuitarML/Automated-GuitarAmpModelling) | GPL-3.0 ([LICENSE](https://raw.githubusercontent.com/GuitarML/Automated-GuitarAmpModelling/master/LICENSE)) | Yes-with-obligations | Supports conditioned and multi-parameter models, but data requirements multiply with knob count ([README](https://github.com/GuitarML/Automated-GuitarAmpModelling)) | Offline Python training, not audio-thread code | Training workflow and dataset examples; good for learning knob-conditioned data design |
| [NeuralSeed](https://github.com/GuitarML/NeuralSeed) | MIT ([LICENSE](https://raw.githubusercontent.com/GuitarML/NeuralSeed/main/LICENSE)) | Yes, keep MIT notices | Yes: up to 3 parameterized knobs for GRU models; controls documented in README ([README controls](https://github.com/GuitarML/NeuralSeed)) | Embedded/Daisy target; README says GRU size 10 or 8 for limited M7 CPU/flash, RTNeural used for fast inference ([README technical info](https://github.com/GuitarML/NeuralSeed)) | Best permissive “multi-knob neural model” example, but tiny embedded models may underfit high-gain or full power-amp behavior |
| [PANAMA](https://github.com/ETH-DISCO/PANAMA) | MIT ([LICENSE](https://raw.githubusercontent.com/ETH-DISCO/PANAMA/main/LICENSE)) | Yes, keep MIT notices | Yes: explicitly parametric/knob-conditioned; active-learning framework for end-to-end parametric amp models ([repo](https://github.com/ETH-DISCO/PANAMA), [PANAMA paper](https://arxiv.org/html/2507.02109v1)) | Research/offline Python; not a production C++ JUCE plugin | Best open research path for real knobs, but needs engineering to export/run in RTNeural/NeuralAudio-style C++ |
| [chowdsp_wdf](https://github.com/Chowdhury-DSP/chowdsp_wdf) | BSD-3-Clause ([LICENSE](https://raw.githubusercontent.com/Chowdhury-DSP/chowdsp_wdf/main/LICENSE)) | Yes, keep BSD notices | White-box knobs are whatever your circuit/topology exposes | Header-only C++ WDF library “for real-time circuit models” ([README](https://github.com/Chowdhury-DSP/chowdsp_wdf)) | Best permissive white-box block for tube/WDF/OT/load experiments |
| [chowdsp_utils](https://github.com/Chowdhury-DSP/chowdsp_utils) | Mixed per module; non-module code GPLv3 ([license note](https://github.com/Chowdhury-DSP/chowdsp_utils)) | Yes only after checking each module license; GPL modules are yes-with-obligations | Utility/parameter-dependent | JUCE modules; source says each module has a unique license and non-module code is GPLv3 ([README license section](https://github.com/Chowdhury-DSP/chowdsp_utils)) | Useful JUCE/DSP utilities if module license matches your distribution plan |
| [BYOD](https://github.com/Chowdhury-DSP/BYOD) | GPLv3 code plus CLA/BSD option for contributors, CC-BY content ([license section](https://github.com/Chowdhury-DSP/BYOD)) | Yes-with-obligations | Many pedal/circuit controls; not a tube power-amp block ([README](https://github.com/Chowdhury-DSP/BYOD)) | Public JUCE/CLAP/VST3/AU/LV2 plugin with releases ([repo](https://github.com/Chowdhury-DSP/BYOD)) | Strong analog-modeling/JUCE/WDF reference; copy only GPL-compatible code paths and keep notices |
| [SwankyAmp](https://github.com/resonantdsp/swankyamp) | GPL-3.0 ([LICENSE](https://raw.githubusercontent.com/resonantdsp/swankyamp/master/LICENSE)) | Yes-with-obligations | Yes: complete amp plugin with sag/power-amp/tone controls in a whole-amp design ([README](https://github.com/resonantdsp/swankyamp)) | Public JUCE/FAUST plugin; changelog explicitly mentions avoiding memory changes in the audio thread ([README/changelog](https://github.com/resonantdsp/swankyamp)) | High-value GPL reference for sag, power-amp dynamics, and crossover behavior; not a library |
| [Faust libraries: tubes/vaeffects/wdmodels](https://github.com/grame-cncm/faustlibraries) | Mixed per-library/function; generated code license follows used libraries/architecture files, not the Faust compiler alone ([Faust FAQ](https://faustdoc.grame.fr/manual/faq/), [libraries repo](https://github.com/grame-cncm/faustlibraries)) | Likely yes-with-obligations for LGPL/GPL-compatible pieces, but exact per-symbol license must be verified | Yes for Faust sliders/topology parameters | Faust-generated DSP can be efficient, but generated code and chosen architecture must be audited | Excellent prototyping source for tube functions and WDF models; license granularity is the gotcha |
| [Airwindows](https://github.com/airwindows/airwindows) | MIT ([LICENSE](https://raw.githubusercontent.com/airwindows/airwindows/master/LICENSE)) | Yes, keep MIT notices | Plugin-specific knobs; not a circuit-parametric power amp ([official site](https://www.airwindows.com/)) | Public plugin code; simple scalar C++ style but no audit completed here | Useful MIT nonlinear/saturation/amp-sim inspiration; not a physical power-amp model |
| [Surge Synth Team `sst-*` DSP libs](https://github.com/surge-synthesizer) | Mostly GPL-3.0 for `sst-waveshapers`, `sst-filters`, `sst-effects`; `sst-basic-blocks` notes some MIT headers ([sst-waveshapers](https://github.com/surge-synthesizer/sst-waveshapers), [sst-filters](https://github.com/surge-synthesizer/sst-filters), [sst-basic-blocks](https://github.com/surge-synthesizer/sst-basic-blocks)) | GPL pieces: yes-with-obligations; MIT headers: yes with notices | DSP primitives only | Header-only/template DSP, but full audio-thread audit is still on you | Good filters/waveshapers/resampling primitives; not tube power-amp-specific |
| [LiveSPICE](https://github.com/dsharlet/LiveSPICE) | MIT ([LICENSE](https://raw.githubusercontent.com/dsharlet/LiveSPICE/master/LICENSE)) | Yes, keep MIT notices | Circuit pots can be adjusted in simulation ([official site](https://www.livespice.org/)) | Real-time SPICE-like app, but C#/.NET and complexity-limited; official site publishes oversampled benchmark examples ([official site](https://www.livespice.org/)) | Excellent prototyping/reference for tube/transformer circuits, not direct C++20/JUCE DSP |
| [TONE3000 model/IR library](https://www.tone3000.com/) | User-generated model/data terms, not a blanket OSS license; uploader retains ownership and redistribution/bulk download is restricted ([Terms](https://www.tone3000.com/terms)) | No blanket bundling/redistribution; yes only for user-downloaded/user-loaded files or individually licensed files | Mostly fixed NAM profiles/IRs; no blanket guarantee of parametric knobs ([site](https://www.tone3000.com/)) | Data/model hosting, not code | Useful ecosystem for user-loaded NAM/IR files; do not bundle models without explicit permission |

## 2) Per-candidate detail sections

### 2.1 NeuralAmpModelerCore

- **Name + repo:** [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) — official C++ core DSP library for Neural Amp Modeler plugins; the NAM website describes NAM as open-source deep learning for modeling guitar amplifiers and pedals ([NAM site](https://www.neuralampmodeler.com/), [NAM “The code” page](https://www.neuralampmodeler.com/the-code)).
- **Exact license:** MIT from the repository license file ([raw LICENSE](https://raw.githubusercontent.com/sdatkinson/NeuralAmpModelerCore/main/LICENSE)). The MIT code license does not automatically license third-party `.nam` model files; TONE3000’s terms say uploaders retain ownership and restrict redistribution/bulk download of hosted files ([TONE3000 Terms](https://www.tone3000.com/terms)).
- **AGPLv3 verdict:** **Yes.** MIT/Expat is GPL-compatible and permissive; preserve copyright/license notices ([FSF MIT/Expat listing](https://www.gnu.org/licenses/license-list.en.html#Expat)).
- **Language/integration cost:** C++ core with Eigen dependency and included tools such as `loadmodel` and `benchmodel`; the README notes Eigen alignment concerns and a benchmark tool for real-time speed testing ([README](https://github.com/sdatkinson/NeuralAmpModelerCore)). Integration into C++20/JUCE is plausible but you must isolate file/model loading and allocation away from `processBlock`.
- **Maturity/maintenance:** The repository page reported hundreds of stars and a 2026 release line in the inspected snapshot, and the NAM site points builders to this core code path ([repo](https://github.com/sdatkinson/NeuralAmpModelerCore), [NAM “The code” page](https://www.neuralampmodeler.com/the-code)).
- **Real-time safety:** The public contract is “real-time playing” core DSP, and the repository provides `benchmodel`; this is not a substitute for an allocator/lock audit in your JUCE build ([README](https://github.com/sdatkinson/NeuralAmpModelerCore)). Keep model loading, JSON parsing, file I/O, sample-rate changes, and buffer resizing outside the audio callback.
- **Knob controllability:** Standard NAM captures are fixed snapshots, not genuine knob-conditioned amp models; the 2025 PANAMA paper explicitly says NAM does not offer parametric knob controls, contrasting it with commercial amp-model products ([PANAMA 2025 HTML](https://arxiv.org/html/2509.26564v1)). Input/output gain around a NAM model is not the same as changing the modeled power-amp bias, feedback, sag, tube type, or depth.
- **Approx CPU cost:** Model-dependent. Use the included `benchmodel` tool per candidate `.nam`; A2 models were still receiving optimized implementation work according to the NAMCore v0.5.0 announcement ([NAMCore v0.5.0 post](https://www.neuralampmodeler.com/post/neuralampmodelercore-v0-5-0-is-released)).
- **Reusable for power amp:** A fixed neural power-amp capture can sit before your cab IR if you have a direct/loadbox capture of a power amp. Gotchas: no physical speaker-impedance interaction unless the capture included that load; no reliable knobs unless trained conditioned; do not redistribute community models without model-file permission.

### 2.2 sdatkinson/neural-amp-modeler trainer

- **Name + repo:** [sdatkinson/neural-amp-modeler](https://github.com/sdatkinson/neural-amp-modeler) — Python trainer/exporter for `.nam` models; the README says it handles training and exporting while real-time playback is handled by the partner plugin/core repos ([README](https://github.com/sdatkinson/neural-amp-modeler)).
- **Exact license:** MIT ([raw LICENSE](https://raw.githubusercontent.com/sdatkinson/neural-amp-modeler/main/LICENSE)). Training data and resulting model files are separate artifacts; the code license does not clear rights in recorded training material or third-party model files.
- **AGPLv3 verdict:** **Yes** for code, with MIT notices ([FSF MIT/Expat listing](https://www.gnu.org/licenses/license-list.en.html#Expat)).
- **Language/integration cost:** Offline Python/PyTorch-style workflow; not C++ runtime code ([repo](https://github.com/sdatkinson/neural-amp-modeler)).
- **Maturity/maintenance:** NAM is the leading open-source neural amp modeling ecosystem according to the PANAMA paper’s survey context, and the official NAM site directs contributors/builders to these repos ([PANAMA 2025 HTML](https://arxiv.org/html/2509.26564v1), [NAM site](https://www.neuralampmodeler.com/the-code)).
- **Real-time safety:** Not applicable to the audio thread; it is an offline trainer.
- **Knob controllability:** Baseline NAM training is non-parametric in the amp-knob sense; use PANAMA/GuitarML-style conditioned training if knobs must work ([PANAMA 2025 HTML](https://arxiv.org/html/2509.26564v1)).
- **Approx CPU cost:** Training cost is offline and architecture/data dependent; runtime cost is determined by the exported model and runtime engine.
- **Reusable for power amp:** Useful for fixed captures of a power amp, output-transformer/loadbox chain, or full rig. Gotcha: a fixed capture cannot expose independent tube type, sag, presence, resonance, or load-impedance controls.

### 2.3 NeuralAmpModelerPlugin

- **Name + repo:** [NeuralAmpModelerPlugin](https://github.com/sdatkinson/NeuralAmpModelerPlugin) — official NAM plugin codebase; the NAM code page describes a plugin repo that integrates core DSP with plugin formats ([NAM “The code” page](https://www.neuralampmodeler.com/the-code), [plugin repo](https://github.com/sdatkinson/NeuralAmpModelerPlugin)).
- **Exact license:** MIT ([raw LICENSE](https://raw.githubusercontent.com/sdatkinson/NeuralAmpModelerPlugin/main/LICENSE)).
- **AGPLv3 verdict:** **Yes** with MIT notices ([FSF MIT/Expat listing](https://www.gnu.org/licenses/license-list.en.html#Expat)).
- **Language/integration cost:** C++ plugin reference, but iPlug2-oriented rather than JUCE ([repo](https://github.com/sdatkinson/NeuralAmpModelerPlugin)).
- **Maturity/maintenance:** Public releases were visible in 2026, including Version 0.7.15 on 2026-06-02 in the releases page snapshot ([releases](https://github.com/sdatkinson/NeuralAmpModelerPlugin/releases)).
- **Real-time safety:** Treat as reference architecture, not a guarantee; inspect model loading, file browser, resampling, and quality changes before copying.
- **Knob controllability:** Same as NAMCore: fixed model player by default.
- **Approx CPU cost:** Model-dependent; use NAMCore `benchmodel` and profile in your host.
- **Reusable for power amp:** Useful for state management and NAM model UX ideas. Gotcha: not a parametric TSM-like power amp, and not JUCE.

### 2.4 RTNeural

- **Name + repo:** [RTNeural](https://github.com/jatinchowdhury18/RTNeural) — lightweight C++ neural-network inference library designed for real-time audio systems ([README](https://github.com/jatinchowdhury18/RTNeural)).
- **Exact license:** BSD-3-Clause ([raw LICENSE](https://raw.githubusercontent.com/jatinchowdhury18/RTNeural/main/LICENSE)).
- **AGPLv3 verdict:** **Yes.** BSD-3-Clause is GPL-compatible; retain notices and disclaimer ([FSF BSD listing](https://www.gnu.org/licenses/license-list.en.html#ModifiedBSD)).
- **Language/integration cost:** C++ library with templated/static and dynamic paths, JSON import/export workflows, and common layers/activations documented in the README ([README](https://github.com/jatinchowdhury18/RTNeural)). Integration cost is moderate if your model export matches supported layers; lower for LSTM/GRU than for arbitrary PyTorch graphs.
- **Maturity/maintenance:** RTNeural is used by projects such as AIDA-X and Chameleon, whose READMEs credit RTNeural ([AIDA-X README](https://github.com/AidaDSP/AIDA-X), [Chameleon README](https://github.com/GuitarML/Chameleon)). The commit page includes 2025 work and RealtimeSanitizer-related history in the inspected snapshot ([commits](https://github.com/jatinchowdhury18/RTNeural/commits/main/)).
- **Real-time safety:** Stronger than most candidates: the stated purpose is real-time systems/audio, but your specific model wrapper must still avoid allocation, locks, JSON parsing, and weight replacement in the audio path ([README](https://github.com/jatinchowdhury18/RTNeural)).
- **Knob controllability:** RTNeural does not define knob semantics. Knobs work only if the network is trained with control inputs or if surrounding DSP maps controls to input/output/EQ.
- **Approx CPU cost:** Depends heavily on architecture and static vs dynamic implementation. Small LSTM/GRU models can be light; deep WaveNet stacks are heavier. Benchmark in your target block size and sample rate.
- **Reusable for power amp:** Good backend for a PANAMA-style conditioned power-amp model or a compact GRU/LSTM trained on power-amp captures. Gotcha: model export/import format and normalization/calibration are your responsibility.

### 2.5 RTNeural-NAM

- **Name + repo:** [RTNeural-NAM](https://github.com/jatinchowdhury18/RTNeural-NAM) — experimental implementation of a NAM-style WaveNet model using RTNeural ([README](https://github.com/jatinchowdhury18/RTNeural-NAM)).
- **Exact license:** BSD-3-Clause ([raw LICENSE](https://raw.githubusercontent.com/jatinchowdhury18/RTNeural-NAM/main/LICENSE)).
- **AGPLv3 verdict:** **Yes** with BSD notices ([FSF BSD listing](https://www.gnu.org/licenses/license-list.en.html#ModifiedBSD)).
- **Language/integration cost:** C++/CMake with submodules; README includes TODOs for more optimization and gated activations, so treat it as experimental ([README](https://github.com/jatinchowdhury18/RTNeural-NAM)).
- **Maturity/maintenance:** Small repo with no release track visible in the inspected repo page; not a shipping plugin ([repo](https://github.com/jatinchowdhury18/RTNeural-NAM)).
- **Real-time safety:** Not production-proven in this scan; it inherits RTNeural design goals but still needs audit.
- **Knob controllability:** No built-in parametric amp-knob control.
- **Approx CPU cost:** WaveNet model dependent; benchmark rather than assume.
- **Reusable for power amp:** A useful learning bridge if you want to understand NAM-like WaveNet inference in RTNeural, but NeuralAmpModelerCore/NeuralAudio are more practical for immediate plugin use.

### 2.6 NeuralAudio

- **Name + repo:** [NeuralAudio](https://github.com/mikeoliphant/NeuralAudio) — C++ library for NAM and other audio neural network models in real-time; README lists support for NAM WaveNet/LSTM A1/A2 and RTNeural Keras LSTM/GRU/JSON models ([README](https://github.com/mikeoliphant/NeuralAudio)).
- **Exact license:** MIT ([raw LICENSE](https://raw.githubusercontent.com/mikeoliphant/NeuralAudio/main/LICENSE)).
- **AGPLv3 verdict:** **Yes** with MIT notices ([FSF MIT/Expat listing](https://www.gnu.org/licenses/license-list.en.html#Expat)).
- **Language/integration cost:** C++ library with internal optimized implementations for some NAM A1 paths and NAMCore use for A2; README documents model support and buffer-size APIs ([README](https://github.com/mikeoliphant/NeuralAudio)). Integration cost is moderate; it may be simpler than integrating several model loaders separately.
- **Maturity/maintenance:** GitHub commit history showed active commits on 2026-06-27, including A2/internal implementation work ([commits](https://github.com/mikeoliphant/NeuralAudio/commits/main/)). It is used by neural-amp-modeler-lv2 and NeuralRack according to their READMEs ([neural-amp-modeler-lv2](https://github.com/mikeoliphant/neural-amp-modeler-lv2), [NeuralRack](https://github.com/brummer10/NeuralRack)).
- **Real-time safety:** README explicitly notes buffer-size behavior, `SetMaxAudioBufferSize`, and quality-change RT-safety checks; that is unusually useful for plugin integration ([README](https://github.com/mikeoliphant/NeuralAudio)). Still keep model loading and quality changes outside audio unless the library reports the change as safe.
- **Knob controllability:** Mostly fixed-model playback unless the loaded JSON/architecture includes control inputs; normal NAM profiles do not expose true amp knobs ([PANAMA 2025 HTML](https://arxiv.org/html/2509.26564v1)).
- **Approx CPU cost:** Model-dependent; some models allocate based on buffer size unless configured, so pre-size for your maximum host block and profile ([README](https://github.com/mikeoliphant/NeuralAudio)).
- **Reusable for power amp:** Strong permissive option if you want user-loadable NAM, RTNeural JSON, and AIDA-X-style models in one runtime. Gotcha: fixed captures remain fixed captures.

### 2.7 neural-amp-modeler-lv2

- **Name + repo:** [neural-amp-modeler-lv2](https://github.com/mikeoliphant/neural-amp-modeler-lv2) — LV2 plugin using NeuralAudio for NAM/AIDA/RTNeural model playback ([README](https://github.com/mikeoliphant/neural-amp-modeler-lv2)).
- **Exact license:** GPL-3.0 ([raw LICENCE.md](https://raw.githubusercontent.com/mikeoliphant/neural-amp-modeler-lv2/main/LICENCE.md)).
- **AGPLv3 verdict:** **Yes-with-obligations.** AGPLv3 section 13 permits combination with GPLv3; GPLv3 parts remain under GPLv3 terms ([AGPLv3 section 13](https://www.gnu.org/licenses/agpl-3.0.en.html)).
- **Language/integration cost:** C++ LV2 plugin; useful as Linux plugin reference but not JUCE/VST/AU architecture ([repo](https://github.com/mikeoliphant/neural-amp-modeler-lv2)).
- **Maturity/maintenance:** Public releases include v0.2.1 on 2026-06-26 in the inspected repo page, and the repo had hundreds of stars in the snapshot ([repo](https://github.com/mikeoliphant/neural-amp-modeler-lv2)).
- **Real-time safety:** Public plugin, but audit is still required. Its README says amp-only models require an IR after the amp and documents oversampling as alias-reducing but performance-costly ([README](https://github.com/mikeoliphant/neural-amp-modeler-lv2)).
- **Knob controllability:** Fixed model plus host controls such as input/output/quality/model file; not true model-parameter control unless the loaded model supports it ([README](https://github.com/mikeoliphant/neural-amp-modeler-lv2)).
- **Approx CPU cost:** NeuralAudio/model-dependent; oversampling may significantly increase CPU ([README](https://github.com/mikeoliphant/neural-amp-modeler-lv2)).
- **Reusable for power amp:** Good reference for where to place amp-only models before IR; not a white-box tube power stage.

### 2.8 NeuralRack

- **Name + repo:** [NeuralRack](https://github.com/brummer10/NeuralRack) — Linux/Windows neural model and IR loader using NeuralAudio, FFTConvolver, and resampling support ([README](https://github.com/brummer10/NeuralRack)).
- **Exact license:** BSD-3-Clause for the repo code ([raw LICENSE](https://raw.githubusercontent.com/brummer10/NeuralRack/main/LICENSE)); dependencies/submodules must be checked separately because the README lists NeuralAudio, FFTConvolver, and zita/r8brain-style resampling components ([README](https://github.com/brummer10/NeuralRack)).
- **AGPLv3 verdict:** **Yes for repo code**, with BSD notices; dependency verification still required ([FSF BSD listing](https://www.gnu.org/licenses/license-list.en.html#ModifiedBSD)).
- **Language/integration cost:** C++ with plugin formats and standalone/LV2/CLAP/VST2 paths; less directly applicable to JUCE 8 than a JUCE module but valuable for model-chain design ([README](https://github.com/brummer10/NeuralRack)).
- **Maturity/maintenance:** Repo page snapshot showed releases through 2026 and roughly hundreds of stars; topic search also showed an updated 2026 line ([repo](https://github.com/brummer10/NeuralRack), [topic search snapshot](https://github.com/topics/neural-amp-modeler)).
- **Real-time safety:** README exposes buffer mode and resampling, which are useful but need audio-thread audit before reuse ([README](https://github.com/brummer10/NeuralRack)).
- **Knob controllability:** Fixed neural model playback plus chain/EQ/IR controls; not a parametric power-amp model.
- **Approx CPU cost:** Model plus IR plus resampling dependent; buffer mode can trade latency/CPU according to README ([README](https://github.com/brummer10/NeuralRack)).
- **Reusable for power amp:** Good reference if you want a model loader plus IR chain; not a physical tube-power block.

### 2.9 AIDA-X

- **Name + repo:** [AIDA-X](https://github.com/AidaDSP/AIDA-X) — amp-model player that loads AI-trained music-gear models and can run chains including amp, cab, distortion, drive, fuzz, boost, and EQ ([README](https://github.com/AidaDSP/AIDA-X)).
- **Exact license:** GPL-3.0-or-later as reported on the repo page ([repo](https://github.com/AidaDSP/AIDA-X)).
- **AGPLv3 verdict:** **Yes-with-obligations.** GPLv3-compatible under AGPLv3 section 13; GPL components keep GPL terms ([AGPLv3 section 13](https://www.gnu.org/licenses/agpl-3.0.en.html)).
- **Language/integration cost:** C++/DPF plugin, not JUCE; README lists dependencies on RTNeural, DPF, FFTConvolver, and r8brain-free-src ([README](https://github.com/AidaDSP/AIDA-X)).
- **Maturity/maintenance:** Public release list includes 1.1.0; inspected activity/issues suggest the project was more active around 2023-2024 than 2026 ([releases](https://github.com/AidaDSP/AIDA-X/releases), [issues](https://github.com/AidaDSP/AIDA-X/issues)).
- **Real-time safety:** Public plugin, but no allocator/lock audit was completed here. Keep model load and file browsing off the audio thread.
- **Knob controllability:** The loaded AI model is effectively fixed; AIDA-X adds controls including input, pre/post EQ, bass/mid/treble, depth, presence, and output ([README controls](https://github.com/AidaDSP/AIDA-X)). Those controls are useful UX, but they are not equivalent to retraining a power-amp model over real amp knob settings.
- **Approx CPU cost:** Model-dependent; RTNeural inference plus EQ/IR/resampling if enabled.
- **Reusable for power amp:** Reference for a practical neural amp player with presence/depth-style controls. Gotcha: GPL, DPF rather than JUCE, and model files are separate licensing artifacts.

### 2.10 aidadsp-lv2

- **Name + repo:** [aidadsp-lv2](https://github.com/AidaDSP/aidadsp-lv2) — LV2 plugin bundle, including a generic RTNeural amp/pedal model loader with EQ and input/output controls ([README](https://github.com/AidaDSP/aidadsp-lv2)).
- **Exact license:** GPL-3.0 ([raw LICENSE](https://raw.githubusercontent.com/AidaDSP/aidadsp-lv2/master/LICENSE)).
- **AGPLv3 verdict:** **Yes-with-obligations** via AGPLv3 section 13 ([AGPLv3 section 13](https://www.gnu.org/licenses/agpl-3.0.en.html)).
- **Language/integration cost:** C++/LV2; README documents RTNeural, AARCH64, xsimd, and eigen build options ([README](https://github.com/AidaDSP/aidadsp-lv2)).
- **Maturity/maintenance:** Latest release v0.94 in 2022 on the inspected repo page; lower maintenance signal than NAMCore/NeuralAudio ([repo](https://github.com/AidaDSP/aidadsp-lv2)).
- **Real-time safety:** Public LV2 audio plugin; still audit dynamic model loading and file I/O.
- **Knob controllability:** Fixed model plus EQ controls; not full knob-conditioned capture.
- **Approx CPU cost:** RTNeural model dependent; likely lighter than full NAM WaveNet for simple JSON models.
- **Reusable for power amp:** Simple neural model loading reference; less attractive than NeuralAudio for a new JUCE plugin.

### 2.11 GuitarML Proteus

- **Name + repo:** [GuitarML Proteus](https://github.com/GuitarML/Proteus) — amp/pedal/plugin capture player/trainer workflow with snapshot captures and knob captures ([README](https://github.com/GuitarML/Proteus)).
- **Exact license:** GPL-3.0 reported by the repo UI and license file listing ([repo](https://github.com/GuitarML/Proteus)).
- **AGPLv3 verdict:** **Yes-with-obligations** via AGPLv3 section 13 ([AGPLv3 section 13](https://www.gnu.org/licenses/agpl-3.0.en.html)).
- **Language/integration cost:** C++/JUCE-ish plugin plus training utilities; direct-out loadbox models need a cabinet IR after them according to the README ([README](https://github.com/GuitarML/Proteus)).
- **Maturity/maintenance:** Public plugin with issue reports through 2026; issues include build/crash/sample-rate reports, so source reuse needs testing in modern JUCE/CMake ([issues](https://github.com/GuitarML/Proteus/issues)).
- **Real-time safety:** Public plugin, but not audited in this pass; model loading/JSON parsing must be off-thread.
- **Knob controllability:** **Yes, but usually one knob.** README says it can capture a drive/tone knob or snapshots, and knob captures require several output WAVs ([README](https://github.com/GuitarML/Proteus)). The GuitarML ToneLibrary page labels “Knob captures” vs “Snapshot captures” ([ToneLibrary page](https://guitarml.com/tonelibrary/tonelib-pro.html)).
- **Approx CPU cost:** Proteus is presented as lower CPU than earlier SmartPedal in third-party coverage, but exact CPU must be profiled on your architecture ([AudioTechnology Proteus page](https://www.audiotechnology.com/free-stuff/guitarml-proteus)).
- **Reusable for power amp:** Valuable reference if you want a “one important knob works” neural power-amp capture, e.g., Push/Drive or Presence. Gotcha: multi-knob control quickly becomes a dataset problem.

### 2.12 GuitarML ToneLibrary

- **Name + repo:** [GuitarML ToneLibrary](https://github.com/GuitarML/ToneLibrary) — JSON tone-model collection for GuitarML plugins ([repo](https://github.com/GuitarML/ToneLibrary), [GuitarML ToneLibrary page](https://guitarml.com/tonelibrary/tonelib-pro.html)).
- **Exact license:** GPL-3.0 reported by the repo UI ([repo](https://github.com/GuitarML/ToneLibrary)). Because contributions can arrive by pull request or email according to the README, provenance of individual model files should be reviewed before bundling ([README](https://github.com/GuitarML/ToneLibrary)).
- **AGPLv3 verdict:** **Yes-with-obligations** for GPL-licensed model files; keep source/model files and notices available. For community/email contributions, verify that the contributor intended GPL distribution.
- **Language/integration cost:** Model/data files, not code.
- **Maturity/maintenance:** The Open-Amp paper reports that the Proteus Tone Packs collection contained 59 guitar amplifier captures and 101 effects pedal captures, with 65 models including a single parameter ([Open-Amp HTML](https://arxiv.org/html/2411.14972v1)).
- **Real-time safety:** Not applicable; model runtime depends on Proteus/BYOD/other loader.
- **Knob controllability:** Some models are single-parameter knob captures; GuitarML’s tone page labels knob captures with an orange tube and snapshot captures with a purple tube ([ToneLibrary page](https://guitarml.com/tonelibrary/tonelib-pro.html)).
- **Approx CPU cost:** Loader/model dependent.
- **Reusable for power amp:** Useful source of GPL-compatible model files for experiments and regression tests. Gotchas: most are full amp/pedal/preamp captures rather than isolated power amps; direct-out vs cab/mic labeling matters; verify individual model provenance before bundling.

### 2.13 SmartGuitarAmp

- **Name + repo:** [SmartGuitarAmp](https://github.com/GuitarML/SmartGuitarAmp) — JUCE plugin using a WaveNet model to emulate a small tube amp at clean and overdriven settings ([README](https://github.com/GuitarML/SmartGuitarAmp)).
- **Exact license:** Apache-2.0 ([raw LICENSE](https://raw.githubusercontent.com/GuitarML/SmartGuitarAmp/main/LICENSE)).
- **AGPLv3 verdict:** **Yes.** Apache-2.0 is GPLv3-compatible but not GPLv2-compatible; keep notices and comply with patent/NOTICE terms ([FSF Apache-2.0 listing](https://www.gnu.org/licenses/license-list.en.html#apache2), [Apache GPL compatibility note](https://www.apache.org/licenses/GPL-compatibility.html)).
- **Language/integration cost:** C++/JUCE, Eigen-era code; release notes say v1.3 updated to JUCE 7/Eigen and removed the custom model load button ([v1.3 release](https://github.com/GuitarML/SmartGuitarAmp/releases)).
- **Maturity/maintenance:** Public plugin but older release line: v1.3 in 2022 ([releases](https://github.com/GuitarML/SmartGuitarAmp/releases)).
- **Real-time safety:** Public plugin but audit required; older JUCE/Eigen code likely needs modernization for JUCE 8/C++20.
- **Knob controllability:** README says Gain and EQ knobs were added to modulate the modeled sound ([README](https://github.com/GuitarML/SmartGuitarAmp)). It is not the same as a current multi-knob conditioned model player.
- **Approx CPU cost:** WaveNet model dependent; older implementation may be heavier than current RTNeural/NeuralAudio paths.
- **Reusable for power amp:** Useful historical Apache-licensed WaveNet/JUCE example. For a new project, prefer RTNeural/NeuralAudio/PANAMA or GuitarML Proteus patterns.

### 2.14 SmartGuitarPedal

- **Name + repo:** [SmartGuitarPedal](https://github.com/GuitarML/SmartGuitarPedal) — JUCE/WaveNet plugin for amp/pedal models, including single-parameter conditioned models in v1.5 ([README](https://github.com/GuitarML/SmartGuitarPedal)).
- **Exact license:** **Ambiguous.** The repo/README show Apache-2.0, but the repository also contains an Apache-2.0 `LICENSE` and a GPL-3.0 `LICENSE.txt` ([Apache LICENSE](https://raw.githubusercontent.com/GuitarML/SmartGuitarPedal/main/LICENSE), [GPL LICENSE.txt](https://raw.githubusercontent.com/GuitarML/SmartGuitarPedal/main/LICENSE.txt), [repo](https://github.com/GuitarML/SmartGuitarPedal)).
- **AGPLv3 verdict:** **Unresolved until audited.** Apache-only files are AGPL-compatible with notices; GPLv3 files are AGPL-compatible with GPL obligations; mixed or copied third-party code must be traced.
- **Language/integration cost:** C++/JUCE; older codebase.
- **Maturity/maintenance:** Public plugin, older 2022-era release line ([repo](https://github.com/GuitarML/SmartGuitarPedal)).
- **Real-time safety:** Public plugin but audit required; especially model load and runtime allocation.
- **Knob controllability:** v1.5 supports models conditioned on a single parameter, and the Drive knob controls that parameter when such a model is loaded ([README](https://github.com/GuitarML/SmartGuitarPedal)).
- **Approx CPU cost:** WaveNet dependent; older implementation likely heavier than Proteus/RTNeural alternatives.
- **Reusable for power amp:** Valuable conceptually for a single power-amp “Push” knob. Gotcha: license ambiguity makes direct code copy risky without file-level review.

### 2.15 Chameleon

- **Name + repo:** [Chameleon](https://github.com/GuitarML/Chameleon) — vintage amp-head neural plugin with three core sounds plus gain/EQ, intended to be used with a cab IR in the chain ([README](https://github.com/GuitarML/Chameleon)).
- **Exact license:** GPL-3.0 as reported by repo/license UI ([repo](https://github.com/GuitarML/Chameleon)).
- **AGPLv3 verdict:** **Yes-with-obligations** under AGPLv3 section 13 ([AGPLv3 section 13](https://www.gnu.org/licenses/agpl-3.0.en.html)).
- **Language/integration cost:** C++/JUCE and RTNeural; release notes say it updated to JUCE 7 and latest RTNeural modules in v1.2 ([v1.2 release](https://github.com/GuitarML/Chameleon/releases)).
- **Maturity/maintenance:** Older but public plugin release line: v1.2 in 2022 ([releases](https://github.com/GuitarML/Chameleon/releases)).
- **Real-time safety:** Public plugin, no full audit here.
- **Knob controllability:** Gain/EQ modify three core sounds, but it is not a general multi-knob conditioned model ([README](https://github.com/GuitarML/Chameleon)).
- **Approx CPU cost:** LSTM/RTNeural dependent; probably lower than older WaveNet paths but must be measured.
- **Reusable for power amp:** Useful for LSTM/JUCE/RTNeural patterns and cab-IR placement, not as an isolated power-stage model.

### 2.16 Automated-GuitarAmpModelling

- **Name + repo:** [GuitarML/Automated-GuitarAmpModelling](https://github.com/GuitarML/Automated-GuitarAmpModelling) — training code forked from Alec Wright’s guitar amp modeling workflow, with conditioned/multi-parameter GuitarML branches ([README](https://github.com/GuitarML/Automated-GuitarAmpModelling)).
- **Exact license:** GPL-3.0 ([raw LICENSE](https://raw.githubusercontent.com/GuitarML/Automated-GuitarAmpModelling/master/LICENSE)).
- **AGPLv3 verdict:** **Yes-with-obligations** under AGPLv3 section 13 ([AGPLv3 section 13](https://www.gnu.org/licenses/agpl-3.0.en.html)).
- **Language/integration cost:** Python/PyTorch/TensorBoard/scipy/numpy/CoreAudioML workflow; not runtime C++ ([README dependencies](https://github.com/GuitarML/Automated-GuitarAmpModelling)).
- **Maturity/maintenance:** Research/training repo; AIDA DSP maintains a related fork updated in 2025 according to GitHub org search, but this specific GuitarML repo is a training base rather than a shipping plugin ([AidaDSP org/repo listing](https://github.com/AidaDSP), [GuitarML repo](https://github.com/GuitarML/Automated-GuitarAmpModelling)).
- **Real-time safety:** Offline only.
- **Knob controllability:** README documents conditioned/multi-parameter workflows and warns that more knobs require multiplicative combinations of training WAVs ([README](https://github.com/GuitarML/Automated-GuitarAmpModelling)).
- **Approx CPU cost:** Training cost can be high; runtime depends on exported network and inference backend.
- **Reusable for power amp:** Good source for capture-set design. Gotcha: multi-knob power-amp data grows quickly if you sweep Presence, Resonance, Master, Sag, and tube/load settings.

### 2.17 NeuralSeed

- **Name + repo:** [GuitarML/NeuralSeed](https://github.com/GuitarML/NeuralSeed) — neural amp/pedal emulation on Daisy Seed/Terrarium hardware, with models trained from amp/pedal recordings and compiled to firmware ([README](https://github.com/GuitarML/NeuralSeed)).
- **Exact license:** MIT ([raw LICENSE](https://raw.githubusercontent.com/GuitarML/NeuralSeed/main/LICENSE)).
- **AGPLv3 verdict:** **Yes** with MIT notices ([FSF MIT/Expat listing](https://www.gnu.org/licenses/license-list.en.html#Expat)).
- **Language/integration cost:** C++ for embedded Daisy; not a JUCE plugin. It uses RTNeural and has submodules for DaisySP/libDaisy/Terrarium that must be checked if copied ([README](https://github.com/GuitarML/NeuralSeed)).
- **Maturity/maintenance:** Public hardware project; repo page snapshot showed 129 stars and NeuralSeed v0.1 release on 2023-04-05 ([repo](https://github.com/GuitarML/NeuralSeed)).
- **Real-time safety:** Designed for embedded real-time operation; README states the models fit in 128 KB flash, use GRU size 10 or 8, and use RTNeural for fast inference ([README technical info](https://github.com/GuitarML/NeuralSeed)).
- **Knob controllability:** **Yes.** Controls 4-6 are Neural Param 1-3, and the README maps LED brightness to snapshot/1-knob/2-knob/3-knob model parameterization ([README controls](https://github.com/GuitarML/NeuralSeed)).
- **Approx CPU cost:** Very low by design for Cortex-M7, but the README warns that such small models may not accurately capture certain devices, especially high-gain amps ([README technical info](https://github.com/GuitarML/NeuralSeed)).
- **Reusable for power amp:** Best permissive example of multi-knob conditioned neural modeling in this scan. Gotcha: its tiny GRU models are deliberately constrained and may be insufficient for a high-fidelity power amp with transformer/load/sag interactions.

### 2.18 PANAMA

- **Name + repo:** [ETH-DISCO/PANAMA](https://github.com/ETH-DISCO/PANAMA) — Parametric Neural Amp Modeling with Active Learning, built around knob-conditioned amp models ([repo](https://github.com/ETH-DISCO/PANAMA), [PANAMA 2025 paper](https://arxiv.org/html/2507.02109v1)).
- **Exact license:** MIT ([raw LICENSE](https://raw.githubusercontent.com/ETH-DISCO/PANAMA/main/LICENSE)).
- **AGPLv3 verdict:** **Yes** with MIT notices ([FSF MIT/Expat listing](https://www.gnu.org/licenses/license-list.en.html#Expat)).
- **Language/integration cost:** Research/Python code, not a C++ plugin. README exposes `global_condition_size` for WaveNet and `conditioning_dim` for LSTM, and an inference `g-vector` for amp settings ([README](https://github.com/ETH-DISCO/PANAMA)).
- **Maturity/maintenance:** Research repo with minimal commit history in the inspected snapshot; do not treat as production runtime ([repo](https://github.com/ETH-DISCO/PANAMA)).
- **Real-time safety:** Not a real-time C++ runtime. Export a trained conditioned model to RTNeural/NeuralAudio or implement a fixed-shape C++ inference path.
- **Knob controllability:** **Yes, this is the main point.** The PANAMA papers define parametric amp models as conditioned on amplifier settings and controllable at inference, and contrast them with non-parametric snapshot captures ([PANAMA 2025 paper](https://arxiv.org/html/2509.26564v1), [PANAMA active-learning paper](https://arxiv.org/html/2507.02109v1)).
- **Approx CPU cost:** Depends on chosen architecture; the paper positions WaveNet-like/LSTM networks as real-time-capable in prior work, but your exported model must be benchmarked on your C++ path ([PANAMA 2025 paper](https://arxiv.org/html/2509.26564v1)).
- **Reusable for power amp:** Best open method for real knobs. Use it to train a parametric power-amp capture with `Master`, `Presence`, `Depth`, `Bias`, and `Load` coordinates. Gotcha: active learning and capture automation are nontrivial; no turnkey JUCE runtime.

### 2.19 chowdsp_wdf

- **Name + repo:** [Chowdhury-DSP/chowdsp_wdf](https://github.com/Chowdhury-DSP/chowdsp_wdf) — header-only C++ Wave Digital Filter library for real-time circuit models ([README](https://github.com/Chowdhury-DSP/chowdsp_wdf)).
- **Exact license:** BSD-3-Clause ([raw LICENSE](https://raw.githubusercontent.com/Chowdhury-DSP/chowdsp_wdf/main/LICENSE)).
- **AGPLv3 verdict:** **Yes** with BSD notices ([FSF BSD listing](https://www.gnu.org/licenses/license-list.en.html#ModifiedBSD)).
- **Language/integration cost:** C++14+ header-only library, optional CMake/XSIMD; straightforward to wrap in C++20/JUCE if you own the circuit model ([README](https://github.com/Chowdhury-DSP/chowdsp_wdf)).
- **Maturity/maintenance:** GitHub commit history showed activity in 2025 and the repo has a focused WDF scope ([commits](https://github.com/Chowdhury-DSP/chowdsp_wdf/commits/main/), [repo](https://github.com/Chowdhury-DSP/chowdsp_wdf)).
- **Real-time safety:** The README explicitly says it is intended for real-time circuit models; still avoid dynamic topology allocation, variable-size containers, locks, and per-sample Newton allocation in your implementation ([README](https://github.com/Chowdhury-DSP/chowdsp_wdf)).
- **Knob controllability:** Excellent: component values and topology parameters become real knobs, as long as you smooth coefficient changes.
- **Approx CPU cost:** Low to moderate for small stages; grows with nonlinear ports, iterative solves, oversampling, and WDF tree size.
- **Reusable for power amp:** Best white-box foundation. Model the phase inverter/power tubes/output transformer/speaker load as simplified WDF subcircuits, then use cab IR for acoustic/mic response. Gotchas: full push-pull pentode stages with transformer hysteresis and global NFB are difficult; start with a simplified PP/SE nonlinear block plus load/NFB filters.

### 2.20 chowdsp_utils

- **Name + repo:** [Chowdhury-DSP/chowdsp_utils](https://github.com/Chowdhury-DSP/chowdsp_utils) — JUCE module collection for audio/plugin development ([README](https://github.com/Chowdhury-DSP/chowdsp_utils)).
- **Exact license:** Mixed per module; README says each module has a unique license and non-module code is GPLv3 ([license section](https://github.com/Chowdhury-DSP/chowdsp_utils)).
- **AGPLv3 verdict:** **Yes only after module-level verification.** GPLv3 modules are yes-with-obligations under AGPLv3 section 13; permissive modules require notices; modules with different terms must be checked ([AGPLv3 section 13](https://www.gnu.org/licenses/agpl-3.0.en.html)).
- **Language/integration cost:** C++17/JUCE modules; README targets JUCE 6+ style module workflows ([README](https://github.com/Chowdhury-DSP/chowdsp_utils)).
- **Maturity/maintenance:** Public releases include v2.4.0 in 2025 in the inspected snapshot ([repo](https://github.com/Chowdhury-DSP/chowdsp_utils)).
- **Real-time safety:** Utilities vary by module; inspect the specific module source before using in `processBlock`.
- **Knob controllability:** Utility-dependent; good for parameter smoothing, UI, presets, oversampling wrappers, and infrastructure rather than modeling itself.
- **Approx CPU cost:** Module-dependent.
- **Reusable for power amp:** Good support code if license-compatible modules are chosen. Gotcha: do not vendor the entire repo assuming one license.

### 2.21 BYOD

- **Name + repo:** [Chowdhury-DSP/BYOD](https://github.com/Chowdhury-DSP/BYOD) — modular guitar distortion plugin with analog-modeled circuits and tone-shaping blocks ([README](https://github.com/Chowdhury-DSP/BYOD)).
- **Exact license:** README license section says plugin code is GPLv3; contributors sign a CLA enabling GPLv3 or BSD-3 license grant for contributed code, and images/factory presets use Creative Commons Attribution 4.0 ([license section](https://github.com/Chowdhury-DSP/BYOD)).
- **AGPLv3 verdict:** **Yes-with-obligations** for GPLv3 code under AGPLv3 section 13; CC-BY content adds attribution obligations if reused ([AGPLv3 section 13](https://www.gnu.org/licenses/agpl-3.0.en.html), [BYOD license section](https://github.com/Chowdhury-DSP/BYOD)).
- **Language/integration cost:** C++/JUCE/CMake plugin supporting multiple formats; README notes CLAP dependencies and build instructions ([README](https://github.com/Chowdhury-DSP/BYOD)).
- **Maturity/maintenance:** Public shipping plugin with releases, including 1.3.0 in 2024 in the inspected snapshot ([repo](https://github.com/Chowdhury-DSP/BYOD)).
- **Real-time safety:** Public audio plugin, but reuse still needs audio-thread audit.
- **Knob controllability:** Strong for pedal/circuit blocks; not a power-amp model.
- **Approx CPU cost:** Varies by chain; modular distortion/tone blocks are generally much lighter than large neural models, but oversampling and WDF nonlinearities add cost.
- **Reusable for power amp:** Great reference for JUCE module organization, WDF-style circuit blocks, parameter smoothing, and plugin UX. Gotcha: GPL and not specifically a tube output stage.

### 2.22 SwankyAmp

- **Name + repo:** [resonantdsp/swankyamp](https://github.com/resonantdsp/swankyamp) — full tube-amp simulator with dynamics and power-amp behavior, implemented mainly through FAUST DSP and JUCE UI ([README](https://github.com/resonantdsp/swankyamp)).
- **Exact license:** GPL-3.0 ([raw LICENSE](https://raw.githubusercontent.com/resonantdsp/swankyamp/master/LICENSE)).
- **AGPLv3 verdict:** **Yes-with-obligations** under AGPLv3 section 13 ([AGPLv3 section 13](https://www.gnu.org/licenses/agpl-3.0.en.html)).
- **Language/integration cost:** C++/JUCE wrapper with FAUST-generated DSP; not a small library, so extracting just the power-amp part takes work ([README](https://github.com/resonantdsp/swankyamp)).
- **Maturity/maintenance:** Public plugin with extensive changelog notes through v1.4.0; repo page showed no GitHub releases in the inspected snapshot, so build/test yourself ([repo](https://github.com/resonantdsp/swankyamp)).
- **Real-time safety:** Changelog explicitly notes “avoid making changes to memory in audio thread” and several sag/power-amp/crossover improvements, which is directly relevant but not a complete audit ([README/changelog](https://github.com/resonantdsp/swankyamp)).
- **Knob controllability:** Yes as a white-box amp plugin: drive, sag, tonestack, and power-amp-like parameters are part of the model’s design ([README](https://github.com/resonantdsp/swankyamp)).
- **Approx CPU cost:** Likely moderate; FAUST-generated nonlinear amp DSP should be cheaper than large neural models but more expensive than a static waveshaper/EQ chain.
- **Reusable for power amp:** High-value GPL learning source for sag, crossover, and output-stage dynamics. Gotcha: do not copy wholesale unless you accept GPL obligations and can disentangle FAUST-generated code/license obligations.

### 2.23 Faust libraries: `tubes.lib`, `vaeffects.lib`, `wdmodels.lib`, `fi.lib`

- **Name + repo:** [grame-cncm/faustlibraries](https://github.com/grame-cncm/faustlibraries) — standard Faust DSP libraries; repo index includes `tubes.lib`, `vaeffects.lib`, `wdmodels.lib`, and many filter/math libraries ([libraries repo](https://github.com/grame-cncm/faustlibraries)).
- **Exact license:** Mixed/per-library/per-function. The Faust FAQ says generated code is not automatically GPL because of the compiler, but generated code’s license depends on the input DSP libraries and architecture files used ([Faust FAQ](https://faustdoc.grame.fr/manual/faq/)). The raw `tubes.lib` header describes Guitarix tube amp emulations and contains tube models such as 12AX7/6V6, but the exact LGPL version should be verified before copying generated code ([raw tubes.lib](https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/tubes.lib)).
- **AGPLv3 verdict:** **Likely yes-with-obligations for GPL/LGPL-compatible functions, but unresolved per symbol.** Do not paste generated Faust code into your AGPL plugin until you have pinned the exact function/library/architecture licenses.
- **Language/integration cost:** Faust-to-C++ generation; easy to prototype, but build-system integration and generated-code ownership/license tracking must be formalized.
- **Maturity/maintenance:** Faust libraries are active standard libraries with online docs for `vaeffects` and `wdmodels` ([vaeffects docs](https://faustlibraries.grame.fr/libs/vaeffects/), [wdmodels docs](https://faustlibraries.grame.fr/libs/wdmodels/)).
- **Real-time safety:** Generated DSP can be real-time friendly, but architecture choices and any dynamic allocation must be inspected.
- **Knob controllability:** Strong for white-box/control-rate sliders; you can expose tube bias, drive, tone, and WDF component values.
- **Approx CPU cost:** Usually low to moderate for tube/waveshaper/filter blocks; WDF and oversampling increase cost.
- **Reusable for power amp:** Good for rapid prototyping of tube transfer curves, WDF networks, and filter topologies. Gotcha: license granularity and exact LGPL version/architecture files.

### 2.24 Airwindows

- **Name + repo:** [airwindows/airwindows](https://github.com/airwindows/airwindows) — large MIT-licensed collection of C++ audio effects, including amp sims, clipping, distortion, dynamics, and saturation categories on the official site ([official site](https://www.airwindows.com/), [repo](https://github.com/airwindows/airwindows)).
- **Exact license:** MIT ([raw LICENSE](https://raw.githubusercontent.com/airwindows/airwindows/master/LICENSE)).
- **AGPLv3 verdict:** **Yes** with MIT notices ([FSF MIT/Expat listing](https://www.gnu.org/licenses/license-list.en.html#Expat)).
- **Language/integration cost:** C++ plugin source; often simple scalar DSP, but not packaged as JUCE modules ([repo](https://github.com/airwindows/airwindows)).
- **Maturity/maintenance:** The official site had active 2026 posts and category counts for amp sims/clipping/distortion/dynamics/saturation in the inspected snapshot ([official site](https://www.airwindows.com/)).
- **Real-time safety:** Airwindows plugins are audio plugins, but this pass did not audit allocation/locking. Many Airwindows DSP algorithms are simple enough to port manually after review.
- **Knob controllability:** Plugin-specific knobs; not physical tube-power topology knobs.
- **Approx CPU cost:** Usually low; most Airwindows effects are lightweight scalar processors.
- **Reusable for power amp:** Useful MIT inspiration for saturation feel, clip texture, filtering, and “amp-like” nonlinear blocks. Gotcha: not analytic WDF/tube/OT modeling.

### 2.25 Surge Synth Team DSP libs

- **Name + repos:** [Surge Synth Team GitHub org](https://github.com/surge-synthesizer) with `sst-waveshapers`, `sst-filters`, `sst-effects`, and `sst-basic-blocks` ([sst-waveshapers](https://github.com/surge-synthesizer/sst-waveshapers), [sst-filters](https://github.com/surge-synthesizer/sst-filters), [sst-effects](https://github.com/surge-synthesizer/sst-effects), [sst-basic-blocks](https://github.com/surge-synthesizer/sst-basic-blocks)).
- **Exact license:** `sst-waveshapers`, `sst-filters`, and `sst-effects` report GPL-3.0; `sst-basic-blocks` says most code is GPLv3 but some listed headers are MIT ([sst-waveshapers repo](https://github.com/surge-synthesizer/sst-waveshapers), [sst-filters repo](https://github.com/surge-synthesizer/sst-filters), [sst-effects repo](https://github.com/surge-synthesizer/sst-effects), [sst-basic-blocks license note](https://github.com/surge-synthesizer/sst-basic-blocks)).
- **AGPLv3 verdict:** GPL-3.0 pieces are **yes-with-obligations** under AGPLv3 section 13; MIT headers are **yes** with notices ([AGPLv3 section 13](https://www.gnu.org/licenses/agpl-3.0.en.html), [FSF MIT/Expat listing](https://www.gnu.org/licenses/license-list.en.html#Expat)).
- **Language/integration cost:** Header-only/template C++ with raw SSE notes in several READMEs; integration is moderate and architecture/SIMD choices matter ([sst-waveshapers](https://github.com/surge-synthesizer/sst-waveshapers), [sst-filters](https://github.com/surge-synthesizer/sst-filters)).
- **Maturity/maintenance:** Small but active Surge-team DSP libs, with 2026 update lines in the inspected search snapshot ([Surge org](https://github.com/surge-synthesizer)).
- **Real-time safety:** DSP primitives are intended for audio use, but audit each function for allocation/SIMD alignment/denormals.
- **Knob controllability:** Primitive-level; you build the amp controls.
- **Approx CPU cost:** Low for waveshapers/filters; oversampling and SIMD layout dominate.
- **Reusable for power amp:** Good source of waveshapers, filters, resamplers, and utility DSP; not a tube power amp model.

### 2.26 LiveSPICE

- **Name + repo:** [LiveSPICE](https://github.com/dsharlet/LiveSPICE) — SPICE-like real-time circuit simulation tool for live audio, including guitar effect/amplifier prototyping ([official site](https://www.livespice.org/), [repo](https://github.com/dsharlet/LiveSPICE)).
- **Exact license:** MIT ([raw LICENSE](https://raw.githubusercontent.com/dsharlet/LiveSPICE/master/LICENSE)).
- **AGPLv3 verdict:** **Yes** with MIT notices ([FSF MIT/Expat listing](https://www.gnu.org/licenses/license-list.en.html#Expat)).
- **Language/integration cost:** C#/.NET rather than C++20/JUCE; direct incorporation is high cost, but circuit/prototype learning value is high ([repo](https://github.com/dsharlet/LiveSPICE)).
- **Maturity/maintenance:** Public app/repo with hundreds of stars in the inspected snapshot; official site documents real-time low-latency simulation, VST plugin, and a component library that includes opamps, transistors, vacuum tubes, and transformers ([official site](https://www.livespice.org/)).
- **Real-time safety:** LiveSPICE is explicitly about real-time SPICE-style audio, but the site warns transient simulation is computationally intensive and imposes complexity limits to run in real time ([official site](https://www.livespice.org/)).
- **Knob controllability:** Circuit potentiometers can be adjusted in the simulated circuit, per official feature list ([official site](https://www.livespice.org/)).
- **Approx CPU cost:** The official benchmark page reports examples on Ryzen 7 5800X, including 8x oversampled circuits and real-time ratios; examples show simple drive circuits can run many times real-time while larger amp circuits are much closer to the edge ([official site benchmark table](https://www.livespice.org/)).
- **Reusable for power amp:** Excellent for validating simplified output-stage circuits and identifying which parts are too expensive. Gotcha: direct port to JUCE would mean reimplementing the solver/model in C++ or using it only as a reference.

### 2.27 TONE3000 model and IR library

- **Name + site:** [TONE3000](https://www.tone3000.com/) — community library for Neural Amp Modeler profiles and impulse responses, with capture/upload workflows ([site](https://www.tone3000.com/), [capture guide](https://www.tone3000.com/capture-guide)).
- **Exact license:** Not a blanket open-source license. Terms state users retain ownership, TONE3000 does not claim ownership/sell uploaded content, TONE3000 receives an internal research/development license, and bulk downloading/redistribution/commercial exploitation without permission is prohibited ([Terms](https://www.tone3000.com/terms)). Some individual model pages may use TONE3000-specific terms; verify each file before use.
- **AGPLv3 verdict:** **No blanket bundling or redistribution.** You can support user-loaded files or ship only files for which you have explicit compatible permission; do not include TONE3000-hosted captures in your repo or installer without permission.
- **Language/integration cost:** Data/model files, not code.
- **Maturity/maintenance:** Large active ecosystem for NAM profiles and IRs according to TONE3000’s own homepage language ([site](https://www.tone3000.com/)).
- **Real-time safety:** Not applicable; runtime depends on NAMCore/NeuralAudio/plugin implementation.
- **Knob controllability:** Mostly fixed profiles/IRs; no blanket guarantee of parametric knobs.
- **Approx CPU cost:** Runtime/model dependent.
- **Reusable for power amp:** Useful for discovery, user import, and manual testing. Gotcha: not an OSS dataset you can redistribute.

### 2.28 Peripheral plugin SDK notes: JUCE 8, VST3, CLAP

These are not power-amp DSP blocks, but they affect your AGPL plugin distribution.

- **JUCE 8:** JUCE’s repository license file says JUCE framework modules are dual-licensed under AGPLv3 and the commercial JUCE license, and JUCE’s get page says you can choose the AGPLv3 terms ([JUCE LICENSE.md](https://github.com/juce-framework/JUCE/blob/master/LICENSE.md), [Get JUCE](https://juce.com/get-juce/)). That matches your AGPLv3-or-later project direction.
- **VST3 SDK:** Steinberg’s VST3 developer portal says the VST3 SDK is under MIT and that the older GPLv3/proprietary licensing is no longer available; it also notes separate trademark/logo usage rules for the VST mark ([VST3 licensing portal](https://steinbergmedia.github.io/vst3_dev_portal/pages/VST%2B3%2BLicensing/VST3%2BLicense.html), [Steinberg VST SDK page](https://www.steinberg.net/developers/vstsdk/)).
- **CLAP:** The CLAP repo describes CLAP as a stable ABI for audio plugins, and u-he’s CLAP page says it is open source under MIT with no fees/memberships/proprietary agreements ([CLAP repo](https://github.com/free-audio/clap), [u-he CLAP page](https://u-he.com/community/clap/)).

## 3) Theory section with citations

### 3.1 What Two Notes publicly says about TSM / TSM-Ai

Two Notes publicly describes TSM as **proprietary Tube Stage Modeling**. The Torpedo C.A.B. M+ manual says the power-amp stage lets the user choose push-pull class AB or single-ended class A topologies and power-tube types such as 6L6, EL34, EL84, and KT88, and that the “Push” parameter increases clipping/compression in the power section ([C.A.B. M+ manual](https://wiki.two-notes.com/doku.php?id=torpedo_cab_m:torpedo_cab_m_user_s_manual)). The OPUS page similarly describes a customizable TSM power amp with tube choices, pentode/triode behavior, push-pull AB and single-ended A topologies, and a Push control for power-amp clipping/compression ([OPUS page](https://www.two-notes.com/en/torpedo-series/opus/)). Two Notes’ GENOME v1.4 announcement says TSM-Ai fuses proprietary AI captures, modeled tonestacks, and engineered power-amp emulation ([GENOME v1.4 news](https://www.two-notes.com/en/news/two-notes-genome-v1-4/)).

The public information does **not** disclose equations, network architectures, training data, component-level models, or source code. A non-cloning implementation should therefore treat TSM as a feature reference only: topology selection, tube-family voicing, Push/compression, presence/depth, and power-amp-before-cab ordering are public-facing ideas; internals remain proprietary.

### 3.2 Why the power amp must see an electrical load, while the cab IR is an acoustic/mic filter

A cabinet IR convolution block is a linear time-invariant approximation of an acoustic/microphone/cabinet chain; Two Notes’ manual explains IR convolution as suited to simulating speaker miking because the system response is assumed linear and time-invariant ([C.A.B. M+ manual](https://wiki.two-notes.com/doku.php?id=torpedo_cab_m:torpedo_cab_m_user_s_manual)). General IR references likewise define IRs as capturing the output of an LTI system and warn that they do not fully capture nonlinear/time-varying distortion or compression ([29a.ch IR creator](https://29a.ch/impulse-response-creator/), [Chaos Audio IR explanation](https://chaosaudio.com/blogs/whats-new/what-are-impulse-responses-irs-and-why-do-they-matter)).

A tube power amp, however, interacts electrically with the speaker/load through the output transformer. Two Notes markets Captor/Reload-style products as reactive loads that simulate the complex impedance of a real speaker, distinguishing that from a simple resistive load ([Torpedo Captor page](https://www.two-notes.com/en/torpedo-series/torpedo-captor/), [OPUS page: Captor X/Reload descriptions](https://www.two-notes.com/en/torpedo-series/opus/)). Aiken explains that an output transformer reflects the speaker/load impedance to the tube plates rather than having a fixed impedance by itself ([Aiken output transformers](https://www.aikenamps.com/index.php/output-transformers-explained)). Speaker impedance is strongly frequency-dependent: it has a low-frequency resonance peak and rises at high frequencies due to voice-coil inductance, so a nominal 8-ohm speaker is not an 8-ohm resistor across the audio band ([Premier Guitar speaker parameters](https://www.premierguitar.com/speaker-parameters), [Aiken technical Q&A](https://aikenamps.com/index.php/technical-q-a)).

Design implication: put a **virtual electrical speaker/load impedance model** inside or before the power-amp block, then put the **cab IR** after the power-amp output. The load model shapes tube current/voltage, feedback, damping, and transformer behavior; the cab IR shapes the acoustic/mic response.

### 3.3 Output transformer: reflected impedance, low-frequency limits, and saturation

The output transformer reflects the secondary load back to the primary, so a speaker impedance peak or inductive rise changes the effective plate load seen by the tubes ([Aiken output transformers](https://www.aikenamps.com/index.php/output-transformers-explained)). Single-ended output transformers need an air gap to prevent core saturation from standing DC current, and that air gap reduces primary inductance, which makes low-frequency performance harder; push-pull output stages cancel DC currents in the transformer and can use smaller ungapped cores for the same power range ([Aiken “Class A” article](https://www.aikenamps.com/index.php/the-last-word-on-class-a), [Aiken glossary](https://www.aikenamps.com/index.php/a-glossary-of-common-amplifier-terms)).

For a plugin power-amp stage, a practical first model is not a full hysteretic transformer. Use a reflected-load filter with a bass resonance peak and high-frequency inductive rise, a transformer band-limit/highpass, and a saturating magnetizing branch that becomes stronger at low frequencies and high drive. Full transformer hysteresis and leakage/capacitance modeling is possible but expensive and needs careful validation.

### 3.4 Negative feedback, presence, resonance/depth

Global negative feedback in many tube guitar amps is taken from an output-transformer secondary tap back to an earlier power-amp/phase-inverter point; Aiken explains that NFB flattens/extends response, reduces distortion, lowers output impedance, and increases damping factor, but excessive loop feedback can become unstable because transformer phase shift turns feedback positive at some frequencies ([Aiken global NFB](https://www.aikenamps.com/index.php/designing-for-global-negative-feedback)). Presence controls are usually implemented by reducing high-frequency feedback, which boosts high-frequency output relative to the feedback-flattened response ([Aiken global NFB](https://www.aikenamps.com/index.php/designing-for-global-negative-feedback)). Depth/resonance controls are the low-frequency analogue: guitar-amp voicing references describe them as frequency-selective changes in the feedback loop that reduce low-frequency feedback, raising bass response and loosening damping ([Rob Robinette amp voicing](https://robrobinette.com/Voicing_an_Amp.htm), [Amp Books SLO feedback analysis](https://www.ampbooks.com/mobile/amp-technology/slo-feedback/)).

Design implication: a convincing power amp should not just add post-EQ. Model an internal feedback path whose feedback amount is frequency-dependent, with Presence and Depth changing the loop filter. Then feed the loop from the **post-output-transformer/load** signal, not from the pre-power signal.

### 3.5 Power-supply sag and time constants

Aiken defines sag as supply-voltage droop when a note/chord is played, perceived as volume drop/compression/touch sensitivity ([Aiken glossary](https://www.aikenamps.com/index.php/a-glossary-of-common-amplifier-terms)). Aiken’s sag article identifies several mechanisms: rectifier internal resistance, power-transformer winding resistance, filter-cap storage/recharge, class-AB current draw increasing at high output, cathode-bias “squish” time constants, and grid-blocking/coupling-cap recovery behavior ([Aiken “What is Sag?”](https://www.aikenamps.com/index.php/what-is-sag)).

Design implication: implement sag as a dynamic state, not a static waveshaper. A minimal model can estimate current draw from rectified tube output/current, low-pass it with attack/release constants representing rectifier/filter behavior, reduce the virtual B+ and/or bias/headroom, and optionally include a slower cathode-bias shift for cathode-biased/SE modes. Smooth parameters and clamp states to prevent zipper noise and unstable feedback.

### 3.6 Push-pull class AB vs single-ended class A harmonic structure

Aiken explains that push-pull output stages use tubes working on opposite half-cycles through a center-tapped transformer, while single-ended stages use one device/path for the whole waveform; push-pull class AB is common for guitar amps, and standing DC cancels in the output transformer ([Aiken push-pull vs single-ended](https://www.aikenamps.com/index.php/what-do-the-terms-push-pull-and-single-ended-mean)). Aiken also states that even-order harmonics generated in the output stage tend to cancel in a balanced push-pull output stage, while single-ended output stages clip more asymmetrically and retain stronger even-order components ([Aiken Class A article](https://www.aikenamps.com/index.php/the-last-word-on-class-a), [Aiken technical Q&A](https://www.aikenamps.com/index.php/technical-q-a)). Class B crossover distortion comes from the nonlinear handoff between devices near zero crossing; class AB reduces it by overlapping conduction, but bias too cold produces audible crossover artifacts ([Aiken technical Q&A](https://www.aikenamps.com/index.php/technical-q-a), [Aiken bias article](https://www.aikenamps.com/index.php/how-to-bias-a-guitar-tube-amp)).

Design implication: expose topology/mode as more than EQ. A single-ended mode should allow asymmetry, DC-related transformer saturation behavior, and softer class-A compression. A push-pull AB mode should include phase-split halves, even-harmonic cancellation when balanced, bias/crossover controls, and stronger odd-harmonic/crossover behavior when driven.

### 3.7 White-box tube/WDF modeling theory and libraries

Wave Digital Filters are a standard route for real-time virtual analog circuit simulation. The `chowdsp_wdf` README explicitly positions the library as header-only C++ for real-time circuit models and cites WDF guitar-distortion and vacuum-tube amplifier work ([chowdsp_wdf README](https://github.com/Chowdhury-DSP/chowdsp_wdf)). Faust’s `wdmodels` documentation describes WD adaptor models for real-time wave-digital audio circuitry and references Kurt Werner’s WDF dissertation work ([Faust wdmodels docs](https://faustlibraries.grame.fr/libs/wdmodels/)). Academic/public abstracts describe WDF simulation of vacuum-tube amplifier stages and output-chain models including power amplifier stage, output transformer, and loudspeaker ([Aalto WDF vacuum-tube amplifier publication](https://research.aalto.fi/en/publications/wave-digital-simulation-of-a-vacuum-tube-amplifier/), [output-chain WDF abstract](https://www.academia.edu/16959066/Real_Time_Model_of_a_Guitar_Amplifier_Output_Stage)).

Design implication: WDF is a strong architecture for component-level parts where the topology is stable and nonlinear ports are few. For a first product version, a hybrid is safer: simplified WDF/load/NFB blocks plus analytic waveshapers and dynamic sag, instead of a full SPICE-equivalent push-pull pentode/output-transformer solver.

### 3.8 Aliasing, ADAA, and oversampling

Nonlinear waveshaping creates harmonics above Nyquist, which fold back as aliasing when processed directly at the sample rate; a DAFx/ResearchGate summary of continuous-time convolution/ADAA work states this problem and notes oversampling as the traditional mitigation ([continuous-time convolution/ADAA summary](https://www.researchgate.net/publication/308020367_Reducing_The_Aliasing_Of_Nonlinear_Waveshaping_Using_Continuous-Time_Convolution)). Jatin Chowdhury’s ADAA repo describes anti-derivative anti-aliasing as a method for reducing aliasing in nonlinear audio processing without oversampling, and his practical write-up credits Parker/Zavalishin/Le Bivic and follow-up work by Bilbao/Esqueda/Parker/Välimäki ([ADAA repo](https://github.com/jatinchowdhury18/ADAA), [practical ADAA article](https://jatinchowdhury18.medium.com/practical-considerations-for-antiderivative-anti-aliasing-d5847167f510)).

Practical guidance for a guitar power amp: use smooth nonlinearities, avoid discontinuous hard clipping, and oversample the nonlinear power-amp core. A defensible v1 starting point is 2x or 4x for mild power-amp saturation, with 8x available for hard clipping/cold-bias crossover/fuzz-like settings; LiveSPICE’s public benchmark examples include several 8x oversampled circuits, showing both feasibility and cost ([LiveSPICE benchmark examples](https://www.livespice.org/)). NeuralAudio/LV2 documentation also notes that oversampling can reduce aliasing but comes with significant performance cost ([neural-amp-modeler-lv2 README](https://github.com/mikeoliphant/neural-amp-modeler-lv2)). ADAA is most straightforward for memoryless waveshapers; stateful feedback/WDF loops need more care, and oversampling remains the safer default for a stateful power-amp block.

### 3.9 Neural/capture theory: fixed snapshots vs knob-conditioned models

A normal capture model learns an input-output mapping for a fixed device setting or fixed rig state. PANAMA’s 2025 paper explicitly distinguishes non-parametric snapshots from parametric models conditioned on amp settings, and states that NAM does not offer parametric knob controls while GuitarML supports single-control parameterization in some efforts ([PANAMA 2025 HTML](https://arxiv.org/html/2509.26564v1)). The Controllable Neural Audio Effects paper shows a broader method: black-box LSTM models conditioned on amplifier control vectors can cover a full range of amp controls and remain feasible in real time, but that paper is research, not a turnkey plugin library ([Controllable Neural Audio Effects HTML](https://arxiv.org/html/2403.08559v1)).

Design implication: if “knobs actually work” is critical, a fixed NAM profile plus gain/EQ wrappers is not enough. You need either a white-box model where parameters affect the circuit state, or a conditioned neural model trained over the relevant power-amp knob/load space.

## 4) Recommended shortlist for an AGPLv3-or-later project

### 4.1 Best white-box building blocks

1. **Use [`chowdsp_wdf`](https://github.com/Chowdhury-DSP/chowdsp_wdf) as the permissive circuit-model backbone.** It is BSD-3-Clause, header-only C++, and explicitly aimed at real-time WDF circuit models ([license](https://raw.githubusercontent.com/Chowdhury-DSP/chowdsp_wdf/main/LICENSE), [README](https://github.com/Chowdhury-DSP/chowdsp_wdf)). For an AGPLv3-or-later plugin, this is the cleanest legal and technical starting point.
2. **Use [`chowdsp_utils`](https://github.com/Chowdhury-DSP/chowdsp_utils) selectively, module by module.** It is useful for JUCE infrastructure, smoothing, presets, and audio utilities, but its README says modules have unique licenses and non-module code is GPLv3, so do not vendor it wholesale without a module license inventory ([license section](https://github.com/Chowdhury-DSP/chowdsp_utils)).
3. **Study [`SwankyAmp`](https://github.com/resonantdsp/swankyamp) for sag/crossover/power-amp dynamics, but treat it as GPL reference code rather than a drop-in library.** It is one of the closest open full-amp references to your desired power-amp feel and has changelog notes about sag and audio-thread memory behavior ([repo](https://github.com/resonantdsp/swankyamp), [license](https://raw.githubusercontent.com/resonantdsp/swankyamp/master/LICENSE)).
4. **Use Faust libraries for prototyping, not blind copying.** `tubes.lib` and `wdmodels.lib` are valuable for sketching tube and WDF ideas, but Faust’s FAQ says generated code license depends on the libraries/architecture files used, so the exact generated-code license must be pinned before production use ([Faust FAQ](https://faustdoc.grame.fr/manual/faq/), [raw tubes.lib](https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/tubes.lib), [wdmodels docs](https://faustlibraries.grame.fr/libs/wdmodels/)).
5. **Use Airwindows/Surge only as supplemental DSP inspiration.** Airwindows is MIT and useful for saturation textures ([Airwindows LICENSE](https://raw.githubusercontent.com/airwindows/airwindows/master/LICENSE)); Surge DSP libs are mostly GPLv3 with some MIT headers and are more primitive/filter/waveshaper-oriented than tube-power-specific ([Surge org](https://github.com/surge-synthesizer)).

### 4.2 Best neural option

- **For fixed captures:** Use **NeuralAudio** or **NeuralAmpModelerCore**. Both are MIT and active enough for a 2026 codebase; NeuralAudio is attractive if you want one runtime for NAM/AIDA/RTNeural JSON model families, while NAMCore is the official NAM core path ([NeuralAudio license](https://raw.githubusercontent.com/mikeoliphant/NeuralAudio/main/LICENSE), [NeuralAudio README](https://github.com/mikeoliphant/NeuralAudio), [NAMCore license](https://raw.githubusercontent.com/sdatkinson/NeuralAmpModelerCore/main/LICENSE), [NAMCore README](https://github.com/sdatkinson/NeuralAmpModelerCore)).
- **For real knobs:** Use **PANAMA** as the research/training direction and **RTNeural** or a fixed-shape NeuralAudio-style C++ inference backend for runtime. PANAMA is MIT and explicitly parametric/knob-conditioned, while RTNeural is BSD-3-Clause and intended for real-time audio inference ([PANAMA repo/license](https://github.com/ETH-DISCO/PANAMA), [PANAMA paper](https://arxiv.org/html/2507.02109v1), [RTNeural README](https://github.com/jatinchowdhury18/RTNeural), [RTNeural license](https://raw.githubusercontent.com/jatinchowdhury18/RTNeural/main/LICENSE)).
- **Do not bundle community models by default.** TONE3000 terms are not a blanket OSS license and restrict redistribution/bulk download ([TONE3000 Terms](https://www.tone3000.com/terms)); GuitarML ToneLibrary is GPL-3.0 but individual provenance should still be reviewed ([ToneLibrary repo](https://github.com/GuitarML/ToneLibrary)).

### 4.3 Minimal credible v1 stack

A credible AGPLv3-or-later v1 power-amp block, before the cab IR, should be white-box/hybrid rather than neural-only:

1. **Oversampled nonlinear core:** 4x internal oversampling by default, switchable to 2x eco and 8x high quality, because nonlinear waveshaping aliases and oversampling is the traditional mitigation ([ADAA/background summary](https://www.researchgate.net/publication/308020367_Reducing_The_Aliasing_Of_Nonlinear_Waveshaping_Using_Continuous-Time_Convolution), [neural-amp-modeler-lv2 oversampling note](https://github.com/mikeoliphant/neural-amp-modeler-lv2)).
2. **Topology modes:** push-pull AB and single-ended A modes, because Two Notes exposes those modes publicly and tube-amp theory gives them different transformer/DC/harmonic/crossover behavior ([Two Notes C.A.B. M+ manual](https://wiki.two-notes.com/doku.php?id=torpedo_cab_m:torpedo_cab_m_user_s_manual), [Aiken push-pull vs SE](https://www.aikenamps.com/index.php/what-do-the-terms-push-pull-and-single-ended-mean)).
3. **Tube-family voicing:** implement 6L6/EL34/EL84/KT88 as different transfer/headroom/transconductance/NFB voicings, not brand claims, because Two Notes publicly exposes those tube choices but not equations ([OPUS page](https://www.two-notes.com/en/torpedo-series/opus/)).
4. **Dynamic sag:** B+ headroom reduction from current/envelope with fast rectifier/filter component and slower bias/cathode recovery, based on Aiken’s sag mechanisms ([Aiken sag article](https://www.aikenamps.com/index.php/what-is-sag)).
5. **Negative-feedback loop:** internal feedback from a post-output/load node with Presence and Depth altering the feedback filter, because NFB/presence/depth are loop behaviors rather than simple post-EQ ([Aiken global NFB](https://www.aikenamps.com/index.php/designing-for-global-negative-feedback), [Rob Robinette depth/resonance explanation](https://robrobinette.com/Voicing_an_Amp.htm)).
6. **Virtual electrical speaker/load:** a resonant LF impedance peak plus HF inductive rise reflected through an output-transformer approximation before the cab IR, because a cab IR is an acoustic/mic LTI response and not the electrical impedance that the power amp drives ([Two Notes IR explanation](https://wiki.two-notes.com/doku.php?id=torpedo_cab_m:torpedo_cab_m_user_s_manual), [Aiken transformer/load reflection](https://www.aikenamps.com/index.php/output-transformers-explained), [Premier Guitar speaker impedance parameters](https://www.premierguitar.com/speaker-parameters)).
7. **Cab IR remains after power amp:** continue using your FFT convolution cab IR after the power-amp output, because direct-out/loadbox amp models need IR after them and cab IRs emulate the cabinet/mic response ([Proteus README direct-out/IR note](https://github.com/GuitarML/Proteus), [neural-amp-modeler-lv2 amp-only IR note](https://github.com/mikeoliphant/neural-amp-modeler-lv2)).

Suggested v1 implementation stack:

- **Core DSP:** own C++20 code + `chowdsp_wdf` for WDF subcircuits where useful ([BSD-3 license](https://raw.githubusercontent.com/Chowdhury-DSP/chowdsp_wdf/main/LICENSE)).
- **Oversampling/filters:** your own JUCE DSP wrappers or module-vetted `chowdsp_utils`; optional Surge/Airwindows ideas only after license/file review ([chowdsp_utils license note](https://github.com/Chowdhury-DSP/chowdsp_utils), [Airwindows MIT](https://raw.githubusercontent.com/airwindows/airwindows/master/LICENSE)).
- **Optional neural slot:** support user-loaded NAM/AIDA/RTNeural JSON models through NeuralAudio or NAMCore, but label this as “capture insert” rather than “knob-controlled power amp” unless the model is conditioned ([NeuralAudio README](https://github.com/mikeoliphant/NeuralAudio), [NAMCore README](https://github.com/sdatkinson/NeuralAmpModelerCore), [PANAMA 2025 HTML](https://arxiv.org/html/2509.26564v1)).
- **Future v2:** train a PANAMA-style parametric power-amp model over `Master`, `Presence`, `Depth`, `Sag`, and load settings, export to an RTNeural-compatible runtime, and compare against the white-box block using null/error metrics and listening tests ([PANAMA repo](https://github.com/ETH-DISCO/PANAMA), [RTNeural README](https://github.com/jatinchowdhury18/RTNeural)).

## 5) Open questions / license ambiguities not fully resolved

1. **SmartGuitarPedal file-level licensing is ambiguous.** The repository has both Apache-2.0 and GPL-3.0 license files, while the README/repo UI emphasize Apache-2.0; direct incorporation needs file-header/provenance review before treating it as Apache-only ([Apache LICENSE](https://raw.githubusercontent.com/GuitarML/SmartGuitarPedal/main/LICENSE), [GPL LICENSE.txt](https://raw.githubusercontent.com/GuitarML/SmartGuitarPedal/main/LICENSE.txt), [repo](https://github.com/GuitarML/SmartGuitarPedal)).
2. **Faust `tubes.lib` exact license/version and generated-code obligations need per-symbol confirmation.** Faust’s FAQ says generated code license depends on the libraries/architecture files used, and the raw tube library should be checked in the exact commit you vendor or generate from ([Faust FAQ](https://faustdoc.grame.fr/manual/faq/), [raw tubes.lib](https://raw.githubusercontent.com/grame-cncm/faustlibraries/master/tubes.lib)).
3. **Model/data licensing is separate from code licensing.** NAMCore, NeuralAudio, RTNeural, and GuitarML runtimes may be permissive/GPL-compatible, but `.nam`, `.json`, `.aidax`, IR, and training WAV files carry their own rights; TONE3000 terms are not a blanket redistributable OSS license ([TONE3000 Terms](https://www.tone3000.com/terms)).
4. **GuitarML ToneLibrary provenance should be reviewed before bundling.** The repo is GPL-3.0, but the README allows contributions by email or pull request, so individual model-file authorship/licensing intent should be checked for a release bundle ([ToneLibrary repo](https://github.com/GuitarML/ToneLibrary)).
5. **AIDA-X exact “GPL-3.0-or-later” notice should be pinned from the commit you vendor.** The repo page reports GPL-3.0-or-later, but you should archive the exact commit/license file in your dependency-review record ([AIDA-X repo](https://github.com/AidaDSP/AIDA-X)).
6. **GPLv3-only code narrows the combined binary’s license posture.** AGPLv3 section 13 permits GPLv3+AGPLv3 combination, but GPLv3-only parts remain GPLv3; your own code can remain AGPLv3-or-later, while the combined distribution must satisfy both GPLv3 and AGPLv3 terms ([AGPLv3 section 13](https://www.gnu.org/licenses/agpl-3.0.en.html)).
7. **No permissive, clearly licensed, isolated “tube power-amp-only capture dataset” was found.** Closest sources were GuitarML ToneLibrary models, TONE3000 user profiles/IRs, and training examples in GuitarML repos, but these are mostly full amps/pedals/preamp/direct-out rigs and/or have redistribution constraints ([GuitarML ToneLibrary](https://github.com/GuitarML/ToneLibrary), [TONE3000 Terms](https://www.tone3000.com/terms), [Automated-GuitarAmpModelling data note](https://github.com/GuitarML/Automated-GuitarAmpModelling)).
8. **Real-time safety is not guaranteed by any license.** Even libraries described as real-time, such as RTNeural and chowdsp_wdf, require integration-level audit for allocation, locks, denormals, file I/O, parameter changes, and sample-rate/block-size changes ([RTNeural README](https://github.com/jatinchowdhury18/RTNeural), [chowdsp_wdf README](https://github.com/Chowdhury-DSP/chowdsp_wdf)).
9. **VST trademark usage is separate from VST3 SDK code license.** Steinberg’s VST3 portal says the SDK is MIT, but VST trademark/logo usage must follow Steinberg’s trademark rules if you display the mark/logo ([VST3 licensing portal](https://steinbergmedia.github.io/vst3_dev_portal/pages/VST%2B3%2BLicensing/VST3%2BLicense.html)).
10. **Two Notes TSM is proprietary and should be treated as a public feature reference only.** Public sources describe topology/tube/Push behavior, but no implementation details are disclosed; avoid names/marketing that imply compatibility or cloning ([C.A.B. M+ manual](https://wiki.two-notes.com/doku.php?id=torpedo_cab_m:torpedo_cab_m_user_s_manual), [OPUS page](https://www.two-notes.com/en/torpedo-series/opus/), [GENOME v1.4 news](https://www.two-notes.com/en/news/two-notes-genome-v1-4/)).
