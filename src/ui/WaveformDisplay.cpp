// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "WaveformDisplay.h"

#include "../core/IRMath.h"         // detectLeadingSilence (leading-silence onset)
#include "../core/WaveformMath.h"   // freq<->x, EQ magnitude/curve, peak envelope

#include <cmath>

//==============================================================================
// IR loading

void WaveformDisplay::setFromMemory (const void* data, size_t size, const juce::String& displayName)
{
    load (formatManager.createReaderFor (std::make_unique<juce::MemoryInputStream> (data, size, false)),
          displayName);
}

void WaveformDisplay::setFromFile (const juce::File& file)
{
    load (formatManager.createReaderFor (file), file.getFileName());
}

void WaveformDisplay::clearIR()
{
    peaks.clear();
    metrics.clear();
    trimFraction = 1.0f;
    leadFraction = 0.0f;
    leadMs       = 0.0;
    repaint();
}

//==============================================================================
// State (pushed from the editor / params)

void WaveformDisplay::setTrimInteractive (bool shouldBeInteractive)
{
    trimInteractive = shouldBeInteractive;
    updateCursor();
    repaint();
}

void WaveformDisplay::setTrimFraction (float fraction01)
{
    trimFraction = juce::jlimit (kMinTrim, 1.0f, fraction01);
    repaint();
}

void WaveformDisplay::setEqVisible (bool shouldShow)
{
    eqVisible = shouldShow;
    updateCursor();
    repaint();
}

void WaveformDisplay::setTrimEnabled (bool shouldBeEnabled)
{
    trimEnabled = shouldBeEnabled;
    repaint();
}

void WaveformDisplay::setSpectrum (const std::vector<float>& pre, const std::vector<float>& post)
{
    preSpec  = pre;
    postSpec = post;
    repaint();
}

void WaveformDisplay::setFilters (bool hOn, float hHz, float hMin, float hMax,
                                  bool lOn, float lHz, float lMin, float lMax)
{
    hpfOn = hOn; hpfHz = hHz; hpfMin = hMin; hpfMax = hMax;
    lpfOn = lOn; lpfHz = lHz; lpfMin = lMin; lpfMax = lMax;
    repaint();
}

//==============================================================================
// Geometry — thin wrappers that feed the on-screen rectangle into cab::WaveformMath.

juce::Rectangle<int> WaveformDisplay::contentBounds() const
{
    return getLocalBounds().withTrimmedRight (dwEnabled ? kDwGutter : 0);
}

float WaveformDisplay::xForFreq (float f, juce::Rectangle<float> r) const
{
    return cab::xForFreq (f, r.getX(), r.getWidth(), kFMin, kFMax);
}

float WaveformDisplay::freqForX (float x, juce::Rectangle<float> r) const
{
    return cab::freqForX (x, r.getX(), r.getWidth(), kFMin, kFMax);
}

float WaveformDisplay::magAt (float f, bool hOn, float hHz, bool lOn, float lHz)
{
    return cab::eqMagnitude (f, hOn, hHz, lOn, lHz);
}

float WaveformDisplay::curveY (float mag, juce::Rectangle<float> r) const
{
    return cab::eqCurveY (mag, r.getY(), r.getHeight());
}

//==============================================================================
// Painting

void WaveformDisplay::paint (juce::Graphics& g)
{
    // subtle top-lit background gradient (premium feel)
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff191920), 0.0f, 0.0f,
                                             juce::Colour (0xff101013), 0.0f, (float) getHeight(), false));
    g.fillRect (getLocalBounds());
    auto r = contentBounds().toFloat();        // leave room for the D/W gutter

    if (peaks.empty())
    {
        g.setColour (juce::Colour (0xff3a3a40));
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("No IR / Bypass", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    const float mid   = r.getCentreY();
    const float amp   = r.getHeight() * 0.46f;
    const int   n     = (int) peaks.size();
    const float trimX = r.getX() + r.getWidth() * trimFraction;

    g.setColour (juce::Colour (0x14ffffff));
    g.drawHorizontalLine ((int) mid, r.getX(), r.getRight());
    drawDbGrid (g, r, mid, amp);
    drawSpectrum (g, r);                  // faint pre/post analyser, behind the impulse

    for (int i = 0; i < n; ++i)
    {
        const float x = r.getX() + r.getWidth() * (float) i / (float) n;
        const float h = peaks[(size_t) i] * amp;
        g.setColour (x <= trimX ? accent.withAlpha (0.85f)      // kept (per-side accent)
                                : juce::Colour (0x33ffffff));    // trimmed (faint)
        g.drawLine (x, mid - h, x, mid + h, 1.0f);
    }

    drawHeadIndicator (g, r);             // leading-silence hint / cut

    if (trimInteractive && trimEnabled)
    {
        // dim the trimmed-away region
        if (trimFraction < 0.999f)
        {
            g.setColour (juce::Colour (0x73000000));
            g.fillRect (juce::Rectangle<float> (trimX, r.getY(), r.getRight() - trimX, r.getHeight()));
        }

        // always-visible draggable handle (a grip tab) so TRIM is discoverable —
        // clamped just inside the right edge when at full length.
        const float hx = juce::jlimit (r.getX() + 4.0f, r.getRight() - 4.0f, trimX);
        g.setColour (accent.withAlpha (0.8f));
        g.drawLine (hx, r.getY(), hx, r.getBottom(), 1.5f);
        const juce::Rectangle<float> tab (hx - 6.0f, mid - 20.0f, 12.0f, 40.0f);
        g.setColour (accent);
        g.fillRoundedRectangle (tab, 3.0f);
        g.setColour (juce::Colour (0xcc141417));
        for (int i = -1; i <= 1; ++i)
            g.drawLine (hx + (float) i * 2.5f, mid - 8.0f, hx + (float) i * 2.5f, mid + 8.0f, 1.0f);
    }

    if (eqVisible && (hpfOn || lpfOn))      // only draw the curve when a filter is on
        drawEq (g, r);

    drawReadout (g, r);                     // inplace Hz / ms near the hovered/dragged handle

    if (dwEnabled)
        drawDwFader (g);                    // D/W vertical fader in the right gutter

    // metrics (bottom-left — clear of the EQ curve's left rise)
    g.setColour (juce::Colour (0xffb0b0b0));
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (metrics, getLocalBounds().reduced (8).removeFromBottom (18),
                juce::Justification::bottomLeft, false);

    // muted/empty slot → dim the whole canvas (mouse is already disabled)
    if (! isEnabled())
    {
        g.setColour (juce::Colour (0xa8141417));
        g.fillRect (getLocalBounds());
    }
}

void WaveformDisplay::drawSpectrum (juce::Graphics& g, juce::Rectangle<float> r)
{
    if (! isEnabled())                    // no spectrum on a muted/empty slot
        return;
    const int n = (int) postSpec.size();
    if (n < 2)
        return;

    const float base = r.getBottom();
    const float h    = r.getHeight() * 0.72f;
    auto sx = [&] (int i) { return r.getX() + r.getWidth() * (float) i / (float) (n - 1); };

    // pre (input): ghost filled area
    if ((int) preSpec.size() == n)
    {
        juce::Path pre;
        pre.startNewSubPath (r.getX(), base);
        for (int i = 0; i < n; ++i)
            pre.lineTo (sx (i), base - juce::jlimit (0.0f, 1.0f, preSpec[(size_t) i]) * h);
        pre.lineTo (r.getRight(), base);
        pre.closeSubPath();
        g.setColour (juce::Colour (0x12ffffff));
        g.fillPath (pre);
    }

    // post (output): faint brand-violet line
    juce::Path post;
    for (int i = 0; i < n; ++i)
    {
        const float y = base - juce::jlimit (0.0f, 1.0f, postSpec[(size_t) i]) * h;
        if (i == 0) post.startNewSubPath (sx (i), y);
        else        post.lineTo          (sx (i), y);
    }
    g.setColour (accent.withAlpha (0.45f));
    g.strokePath (post, juce::PathStrokeType (1.3f));
}

// Leading-silence indicator. Always shows the full IR; this marks the
// silent head so the user knows there's pre-delay to trim. HEAD off → faint hint
// ("12 ms" + light band); HEAD on → the region reads as cut (dim + diagonal hatch).
void WaveformDisplay::drawHeadIndicator (juce::Graphics& g, juce::Rectangle<float> r)
{
    if (leadFraction <= 0.0f)
        return;

    const float lx = r.getX() + r.getWidth() * juce::jlimit (0.0f, 1.0f, leadFraction);
    const juce::Rectangle<float> region (r.getX(), r.getY(), lx - r.getX(), r.getHeight());

    if (headEnabled)
    {
        g.setColour (juce::Colour (0x73000000));          // removed (matches TRIM dim)
        g.fillRect (region);
        g.setColour (juce::Colour (0x1effffff));          // diagonal hatch reads as "cut"
        for (float x = region.getX() - region.getHeight(); x < region.getRight(); x += 7.0f)
            g.drawLine (x, region.getBottom(), x + region.getHeight(), region.getY(), 1.0f);
    }
    else
    {
        g.setColour (juce::Colour (0x16ffffff));          // faint hint band
        g.fillRect (region);
    }

    // onset marker
    const juce::Colour mark = headEnabled ? accent : juce::Colour (0xffb0b0b0);
    g.setColour (mark.withAlpha (headEnabled ? 0.8f : 0.6f));
    g.drawLine (lx, r.getY(), lx, r.getBottom(), headEnabled ? 1.5f : 1.0f);

    // label: "−12 ms" when cutting, else "12 ms" hint
    const juce::String text = (headEnabled ? juce::String::fromUTF8 ("\xe2\x88\x92") : juce::String())
                            + juce::String (juce::roundToInt (leadMs)) + " ms";
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    const float w = (float) text.length() * 6.5f + 10.0f;
    juce::Rectangle<float> box (lx + 4.0f, r.getY() + 4.0f, w, 14.0f);
    if (box.getRight() > r.getRight())                    // flip to the left if it'd overflow
        box.setX (lx - 4.0f - w);
    g.setColour (juce::Colour (0xcc0e0e12));
    g.fillRoundedRectangle (box, 3.0f);
    g.setColour (headEnabled ? accent.withAlpha (0.95f) : juce::Colour (0xffc8c8c8));
    g.drawText (text, box, juce::Justification::centred, false);
}

void WaveformDisplay::drawDwFader (juce::Graphics& g)
{
    auto gutter = getLocalBounds().toFloat().removeFromRight ((float) kDwGutter).reduced (3.0f, 5.0f);
    const float cx  = gutter.getCentreX();
    const float top = gutter.getY(), bot = gutter.getBottom();
    const float hy  = top + (1.0f - dryWet) * gutter.getHeight();   // top = 100% wet

    g.setColour (juce::Colour (0x22ffffff));                        // track
    g.fillRoundedRectangle (juce::Rectangle<float> (cx - 1.5f, top, 3.0f, bot - top), 1.5f);
    g.setColour (accent.withAlpha (0.55f));                         // wet fill (top → handle)
    g.fillRoundedRectangle (juce::Rectangle<float> (cx - 1.5f, top, 3.0f, hy - top), 1.5f);
    g.setColour (accent);                                           // handle
    g.fillRoundedRectangle (juce::Rectangle<float> (cx - 6.0f, hy - 3.0f, 12.0f, 6.0f), 2.0f);
}

void WaveformDisplay::drawDbGrid (juce::Graphics& g, juce::Rectangle<float> r, float mid, float amp)
{
    // Faint dBFS amplitude rules (relative to the IR's normalised peak) + tiny labels.
    for (const int db : { -6, -12, -18 })
    {
        const float dy = amp * juce::Decibels::decibelsToGain ((float) db);
        g.setColour (juce::Colour (0x12ffffff));
        g.drawHorizontalLine ((int) (mid - dy), r.getX(), r.getRight());
        g.drawHorizontalLine ((int) (mid + dy), r.getX(), r.getRight());
        g.setColour (juce::Colour (0x55b0b0b0));
        g.setFont (juce::FontOptions (9.0f));
        g.drawText (juce::String (db), juce::Rectangle<float> (r.getRight() - 26.0f, mid - dy - 6.0f, 24.0f, 12.0f),
                    juce::Justification::centredRight, false);
    }
}

void WaveformDisplay::drawEq (juce::Graphics& g, juce::Rectangle<float> r)
{
    juce::Path p;
    const int   w = (int) r.getWidth();
    for (int x = 0; x <= w; ++x)
    {
        const float f = freqForX (r.getX() + (float) x, r);
        const float y = curveY (magAt (f, hpfOn, hpfHz, lpfOn, lpfHz), r);
        if (x == 0) p.startNewSubPath (r.getX() + (float) x, y);
        else        p.lineTo          (r.getX() + (float) x, y);
    }
    // soft glow fill under the curve (premium feel) — accent fading to transparent
    juce::Path fill = p;
    fill.lineTo (r.getRight(), r.getBottom());
    fill.lineTo (r.getX(),     r.getBottom());
    fill.closeSubPath();
    const float gy = r.getY() + r.getHeight() * 0.58f;
    g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.22f), r.getCentreX(), gy,
                                             accent.withAlpha (0.0f),  r.getCentreX(), r.getBottom(), false));
    g.fillPath (fill);

    g.setColour (accent.withAlpha (0.5f));         // ghost EQ line (per-side accent)
    g.strokePath (p, juce::PathStrokeType (1.5f));

    if (hpfOn) drawHandle (g, r, hpfHz, true);    // a point per *enabled* filter only
    if (lpfOn) drawHandle (g, r, lpfHz, true);
}

void WaveformDisplay::drawHandle (juce::Graphics& g, juce::Rectangle<float> r, float f, bool on)
{
    const float x = xForFreq (f, r);
    const float y = curveY (magAt (f, hpfOn, hpfHz, lpfOn, lpfHz), r);
    g.setColour (accent.withAlpha (on ? 0.85f : 0.4f));
    g.fillEllipse (x - 4.5f, y - 4.5f, 9.0f, 9.0f);
    g.setColour (juce::Colour (0x99141417));
    g.drawEllipse (x - 4.5f, y - 4.5f, 9.0f, 9.0f, 1.2f);
}

// Inplace value pill near the hovered/dragged handle: Hz for HPF, kHz for
// LPF, ms for TRIM — so the value is visible without a slider.
void WaveformDisplay::drawReadout (juce::Graphics& g, juce::Rectangle<float> r)
{
    const Drag el = (dragMode != Drag::none) ? dragMode : hoverEl;
    juce::String text;
    float x = 0.0f, y = 0.0f;

    if (el == Drag::hpf && hpfOn && eqVisible)
    {
        text = juce::String (juce::roundToInt (hpfHz)) + " Hz";
        x = xForFreq (hpfHz, r);
        y = curveY (magAt (hpfHz, hpfOn, hpfHz, lpfOn, lpfHz), r);
    }
    else if (el == Drag::lpf && lpfOn && eqVisible)
    {
        text = juce::String (lpfHz / 1000.0f, 1) + " kHz";
        x = xForFreq (lpfHz, r);
        y = curveY (magAt (lpfHz, hpfOn, hpfHz, lpfOn, lpfHz), r);
    }
    else if (el == Drag::trim && trimEnabled)
    {
        text = juce::String (juce::roundToInt (trimFraction * irMs)) + " ms";
        x = juce::jlimit (r.getX() + 4.0f, r.getRight() - 4.0f, r.getX() + r.getWidth() * trimFraction);
        y = r.getCentreY() - 8.0f;
    }
    else if (el == Drag::dwet && dwEnabled)
    {
        text = juce::String (juce::roundToInt (dryWet * 100.0f)) + "%";
        x = r.getRight() - 4.0f;
        y = r.getY() + 5.0f + (1.0f - dryWet) * (r.getHeight() - 10.0f) - 8.0f;
    }
    else
        return;

    const float w = (float) text.length() * 7.0f + 12.0f;
    juce::Rectangle<float> box (0.0f, 0.0f, w, 16.0f);
    box.setCentre (juce::jlimit (r.getX() + w * 0.5f, r.getRight() - w * 0.5f, x),
                   juce::jlimit (r.getY() + 9.0f, r.getBottom() - 9.0f, y - 16.0f));
    g.setColour (juce::Colour (0xdd0e0e12));
    g.fillRoundedRectangle (box, 3.0f);
    g.setColour (accent.withAlpha (0.95f));
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (text, box, juce::Justification::centred, false);
}

//==============================================================================
// Mouse / hit-testing

void WaveformDisplay::mouseDown (const juce::MouseEvent& e)
{
    dragMode = pickMode (e.position);
    applyDrag (e.position);
    repaint();
}

void WaveformDisplay::mouseDrag (const juce::MouseEvent& e) { applyDrag (e.position); repaint(); }

void WaveformDisplay::mouseUp (const juce::MouseEvent& e)
{
    dragMode = Drag::none;
    hoverEl  = pickMode (e.position);
    repaint();
}

void WaveformDisplay::mouseMove (const juce::MouseEvent& e)
{
    const auto h = pickMode (e.position);
    if (h != hoverEl) { hoverEl = h; repaint(); }
}

void WaveformDisplay::mouseExit (const juce::MouseEvent&)
{
    if (hoverEl != Drag::none) { hoverEl = Drag::none; repaint(); }
}

WaveformDisplay::Drag WaveformDisplay::pickMode (juce::Point<float> pos)
{
    if (peaks.empty())
        return Drag::none;

    const auto r = contentBounds().toFloat();
    if (eqVisible)                         // a point is grabbable only when its filter is on
    {
        if (hpfOn && std::abs (pos.x - xForFreq (hpfHz, r)) < kGrabPx) return Drag::hpf;
        if (lpfOn && std::abs (pos.x - xForFreq (lpfHz, r)) < kGrabPx) return Drag::lpf;
    }
    if (dwEnabled && pos.x >= r.getRight())                            return Drag::dwet;   // gutter
    if (trimInteractive && trimEnabled)                               return Drag::trim;
    return Drag::none;
}

void WaveformDisplay::applyDrag (juce::Point<float> pos)
{
    const auto r = contentBounds().toFloat();
    switch (dragMode)
    {
        case Drag::trim: setTrimFromMouse (pos.x); break;
        case Drag::hpf:                              // freq only — the checkbox owns on/off
            hpfHz = juce::jlimit (hpfMin, hpfMax, freqForX (pos.x, r));
            repaint();
            if (onHpfChanged) onHpfChanged (true, hpfHz);
            break;
        case Drag::lpf:
            lpfHz = juce::jlimit (lpfMin, lpfMax, freqForX (pos.x, r));
            repaint();
            if (onLpfChanged) onLpfChanged (true, lpfHz);
            break;
        case Drag::dwet:
        {
            const float top = r.getY() + 5.0f, bot = r.getBottom() - 5.0f;
            dryWet = juce::jlimit (0.0f, 1.0f, 1.0f - (pos.y - top) / juce::jmax (1.0f, bot - top));
            repaint();
            if (onDryWetChanged) onDryWetChanged (dryWet);
            break;
        }
        case Drag::none: break;
    }
}

void WaveformDisplay::setTrimFromMouse (float x)
{
    if (! trimInteractive || peaks.empty())
        return;
    const float w = (float) juce::jmax (1, contentBounds().getWidth());
    const float f = juce::jlimit (kMinTrim, 1.0f, x / w);
    if (std::abs (f - trimFraction) < 1.0e-4f)
        return;
    trimFraction = f;
    repaint();
    if (onTrimChanged) onTrimChanged (f);
}

void WaveformDisplay::updateCursor()
{
    setMouseCursor ((trimInteractive || eqVisible) ? juce::MouseCursor::PointingHandCursor
                                                   : juce::MouseCursor::NormalCursor);
}

//==============================================================================
// Decode (message thread)

void WaveformDisplay::load (juce::AudioFormatReader* rawReader, const juce::String& /*displayName*/)
{
    std::unique_ptr<juce::AudioFormatReader> reader (rawReader);
    if (reader == nullptr || reader->lengthInSamples <= 0)
    {
        clearIR();
        return;
    }

    // Match the processor's load cap (kMaxIRSeconds): show only what's convolved.
    const double srEarly = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
    const int n  = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (20.0 * srEarly));
    const int ch = juce::jmax (1, (int) reader->numChannels);

    juce::AudioBuffer<float> buf (ch, n);
    reader->read (&buf, 0, n, 0, true, true);

    // 512-bucket normalised peak envelope (the drawn waveform).
    peaks = cab::computePeaks (buf, n, 512);

    trimFraction = 1.0f;

    const double sr = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;

    // Leading-silence onset — same detection as the processor
    // (threshold 0.001 x peak, ~0.2 ms pre-roll, ignore < 0.5 ms) so the indicator
    // matches exactly what HEAD trim removes.
    {
        const int lead = cab::detectLeadingSilence (buf, n, sr);
        leadFraction = (lead > 0 && n > 0) ? (float) lead / (float) n : 0.0f;
        leadMs       =  lead > 0 ? lead / sr * 1000.0 : 0.0;
    }

    const double ms = reader->sampleRate > 0.0 ? n / reader->sampleRate * 1000.0 : 0.0;
    irMs = ms;
    metrics = juce::String (juce::roundToInt (ms)) + "ms  |  "
            + juce::String (reader->sampleRate / 1000.0, 1) + "kHz  |  "
            + juce::String (reader->bitsPerSample) + "-bit  |  "
            + (ch == 1 ? "mono" : ch == 2 ? "stereo" : juce::String (ch) + "ch");
    repaint();
}
