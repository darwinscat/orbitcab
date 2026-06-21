// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <vector>

//==============================================================================
// IR waveform view (Genome-style; look mirrors the web cabinet-ir-utility).
// Decodes the IR on the message thread — independent of the processor's audio-side
// load — so there's no cross-thread data sharing. It hosts the direct-manipulation
// controls:
//   • TRIM  — drag anywhere to truncate the IR; cut region dims + a marker tracks it.
//   • EQ    — an HPF/LPF response curve with two draggable corner points (per slot —
//             each slot has its own HPF/LPF). Drag a point = cutoff; park it past the
//             edge = filter off. Curve shape matches the web `drawEqCurve`.
// Changes are reported via callbacks so the editor can write the parameters.
//
// The geometry/envelope math (freq<->x, EQ magnitude/curve, peak buckets) lives in
// core/WaveformMath.h so it's unit-tested headless; this class is the GUI shell that
// feeds it the on-screen rectangle and draws the result.
//==============================================================================
class WaveformDisplay final : public juce::Component
{
public:
    WaveformDisplay() { formatManager.registerBasicFormats(); }

    std::function<void (float)>        onTrimChanged;    // fraction to keep, (0,1]
    std::function<void (bool, float)>  onHpfChanged;     // (on, Hz)
    std::function<void (bool, float)>  onLpfChanged;     // (on, Hz)

    void setFromMemory (const void* data, size_t size, const juce::String& displayName);
    void setFromFile (const juce::File& file);
    void clearIR();

    void setTrimInteractive (bool shouldBeInteractive);
    void setTrimFraction (float fraction01);
    float getTrimFraction() const { return trimFraction; }

    void setEqVisible (bool shouldShow);

    // The TRIM handle (and trimming) only show/work when the slot's TRIM is enabled.
    void setTrimEnabled (bool shouldBeEnabled);

    // HEAD trim: when on, the detected leading-silence region reads as "cut".
    // The waveform always shows the full IR; this only changes the indicator's look.
    void setHeadEnabled (bool shouldBeEnabled) { headEnabled = shouldBeEnabled; repaint(); }
    bool  hasLeadingSilence() const { return leadFraction > 0.0f; }

    // Live pre/post spectrum (normalised 0..1 magnitude bins, log-freq), drawn faint
    // behind the impulse as ambient animation. Fed by the editor's analyser timer.
    void setSpectrum (const std::vector<float>& pre, const std::vector<float>& post);

    // Per-side accent (Slot A = violet, Slot B = orange) — tints the impulse, EQ,
    // spectrum and trim handle (left-violet, right-orange).
    void setAccent (juce::Colour c) { accent = c; repaint(); }

    // Pushed from the editor (params or host automation) so the curve stays in sync.
    void setFilters (bool hOn, float hHz, float hMin, float hMax,
                     bool lOn, float lHz, float lMin, float lMax);

    void paint (juce::Graphics& g) override;

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

private:
    enum class Drag { none, trim, hpf, lpf };

    static constexpr float kMinTrim  = 0.02f;
    static constexpr float kFMin     = 20.0f;
    static constexpr float kFMax     = 20000.0f;
    static constexpr float kGrabPx   = 14.0f;

    // The drawable/interactive content area (the D/W fader no longer lives here —
    // it's a horizontal slider under the slot's checkbox row now).
    juce::Rectangle<int> contentBounds() const;

    // --- thin wrappers over core/WaveformMath.h, feeding in the current rectangle ---
    float xForFreq (float f, juce::Rectangle<float> r) const;
    float freqForX (float x, juce::Rectangle<float> r) const;
    static float magAt (float f, bool hOn, float hHz, bool lOn, float lHz);
    float curveY (float mag, juce::Rectangle<float> r) const;

    void drawSpectrum (juce::Graphics& g, juce::Rectangle<float> r);
    void drawHeadIndicator (juce::Graphics& g, juce::Rectangle<float> r);
    void drawDbGrid (juce::Graphics& g, juce::Rectangle<float> r, float mid, float amp);
    void drawEq (juce::Graphics& g, juce::Rectangle<float> r);
    void drawHandle (juce::Graphics& g, juce::Rectangle<float> r, float f, bool on);
    void drawReadout (juce::Graphics& g, juce::Rectangle<float> r);

    Drag pickMode (juce::Point<float> pos);
    void applyDrag (juce::Point<float> pos);
    void setTrimFromMouse (float x);
    void updateCursor();

    void load (juce::AudioFormatReader* rawReader, const juce::String& displayName);

    juce::AudioFormatManager formatManager;
    std::vector<float>       peaks;
    std::vector<float>       preSpec, postSpec;
    juce::Colour             accent { 0xff7c4dff };   // per-side tint (set by the editor)
    juce::String             metrics;
    float                    trimFraction    = 1.0f;
    bool                     trimInteractive = false;
    bool                     trimEnabled     = false;
    float                    leadFraction    = 0.0f;    // leading-silence width (0..1 of IR)
    double                   leadMs          = 0.0;     // leading-silence duration (ms)
    bool                     headEnabled     = false;   // HEAD trim on → region reads as cut
    double                   irMs            = 0.0;     // full IR length (for TRIM ms readout)
    Drag                     dragMode        = Drag::none;
    Drag                     hoverEl         = Drag::none;  // element under the cursor (inplace readout)

    // EQ overlay state (mirrors the params; set via setFilters)
    bool  eqVisible = false;
    bool  hpfOn = false, lpfOn = false;
    float hpfHz = 80.0f,  hpfMin = 30.0f,   hpfMax = 180.0f;
    float lpfHz = 7000.0f, lpfMin = 4000.0f, lpfMax = 12000.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
};
