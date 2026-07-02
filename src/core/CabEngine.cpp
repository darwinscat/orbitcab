// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "CabEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>   // AudioBuffer, Decibels, FloatVectorOperations
#include <chrono>
#include <cmath>

namespace cab
{

namespace
{
    constexpr double kRampSeconds = 0.03;          // live tweaks / automation glide

    // Encode the poweramp/preamp signal ROUTE as a small int; any change means the spectrum feeding
    // the cab (hence the leveler's wet/dry match) stepped. preamp on/off shifts it too (it's upstream).
    int routeCode (const Params& p) noexcept
    {
        return (p.preampOn ? 100 : 0)
             + (! p.ampOn ? 0 : (p.powerAmpMode == PowerAmpMode::tube ? 2 : 1));
    }
}

//==============================================================================
void CabEngine::prepare (double sampleRate, int maxBlock, int numChannels, const Params& initial)
{
    currentSampleRate = sampleRate;

    preamp.prepare (sampleRate, maxBlock);
    // Dry-alignment for the preamp bypass: 256-sample capacity covers any NAM rate-match latency
    // (≈ ceil(3·hostSR/modelSR)+3, ≤ ~27 even at 384 kHz) with wide margin — matches the router.
    preampBypassAlign.prepare (numChannels, maxBlock, 256);
    ampEq.prepare (sampleRate, maxBlock, numChannels);
    amp.prepare (sampleRate, maxBlock);
    powerAmpRouter.prepare (sampleRate, maxBlock, numChannels);

    for (int i = 0; i < 2; ++i)
        slot[i].prepare (sampleRate, maxBlock, numChannels);

    // Seed smoothers from the initial parameter values so a restored session doesn't ramp
    // from zero on the first block.
    for (int i = 0; i < 2; ++i)
    {
        mixSm[i].reset (sampleRate, kRampSeconds);
        mixSm[i].setCurrentAndTargetValue (initial.slot[i].dryWet01);
        phaseSm[i].reset (sampleRate, kRampSeconds);
        phaseSm[i].setCurrentAndTargetValue (initial.slot[i].phase ? -1.0f : 1.0f);
    }
    gainSmoothed.reset  (sampleRate, kRampSeconds);
    gainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (initial.outputGainDb));
    mixABSmoothed.reset (sampleRate, kRampSeconds);
    mixABSmoothed.setCurrentAndTargetValue (initial.bLoaded ? initial.mixAB01 : 0.0f);
    muteGateSmoothed.reset (sampleRate, kRampSeconds);
    muteGateSmoothed.setCurrentAndTargetValue (1.0f);

    autoLeveler.prepare (sampleRate);
    prevRoute = routeCode (initial);   // seed so a session booting in any route isn't seen as a switch on block 1
    prevContext = makeContext (initial);
    lastModelGeneration = modelGeneration.load (std::memory_order_relaxed);
    routeDwellSamples = 0;
    prevAutoLevel = initial.autoLevel;
    for (auto& r : routeLevel) r = RouteLevel {};   // route memory restarts with the stream

    inputGainPrev = juce::Decibels::decibelsToGain (initial.inputGainDb);
    inLevel.store (0.0f);
    outLevel.store (0.0f);

    dryBuffer.setSize (numChannels, maxBlock, false, false, true);
    wet[0].setSize    (numChannels, maxBlock, false, false, true);
    wet[1].setSize    (numChannels, maxBlock, false, false, true);
}

void CabEngine::reset()
{
    preamp.reset();
    preampBypassAlign.reset();
    ampEq.reset();
    amp.reset();
    powerAmpRouter.reset();
    for (int i = 0; i < 2; ++i)
        slot[i].reset();
    autoLeveler.reset();
}

void CabEngine::seedAutoLevel()
{
    // Estimate the convolution's broadband (white-noise) RMS gain from the loaded IR and
    // seed the leveler's makeup to ~1/gain, so the first audio starts at the converged level
    // instead of kicking while the follower crawls in (#48). The convolver is Normalise::no,
    // so that gain ≈ ‖ir‖₂ at the processing rate ≈ sqrt(SRhost / SRir) · ‖ir_native‖₂.
    auto slotGain = [this] (int s) -> double
    {
        if (! slot[s].hasOriginal())
            return 0.0;
        const auto& ir = slot[s].originalBuffer();
        const int n = ir.getNumSamples();
        if (n <= 0)
            return 0.0;
        double e = 0.0;
        for (int c = 0; c < ir.getNumChannels(); ++c)
        {
            const float* d = ir.getReadPointer (c);
            for (int i = 0; i < n; ++i)
                e += (double) d[i] * d[i];
        }
        e /= juce::jmax (1, ir.getNumChannels());            // per-channel energy
        const double srRatio = currentSampleRate / juce::jmax (1.0, slot[s].originalSampleRate());
        return std::sqrt (e * srRatio);
    };

    const double g = slotGain (0);                           // slot A — the ballpark
    if (g > 1.0e-6)
        autoLeveler.seed ((float) (1.0 / g));
}

//==============================================================================
void CabEngine::process (float* const* io, int numChannels, int numSamples,
                         const Params& p, bool nonRealtime)
{
    // Guard the prepared scratch size. A well-behaved host honours prepare()'s channel
    // count + maxBlock, but the core is now standalone (a direct/WASM/embedded caller may
    // not), so clamp to what wet[]/dryBuffer can hold — process() must never read or write
    // past the preallocated scratch. In the normal case (in==out, n<=maxBlock) this is a
    // no-op.
    numChannels = juce::jmin (numChannels, dryBuffer.getNumChannels());
    numSamples  = juce::jmin (numSamples,  dryBuffer.getNumSamples());

    // Wrap the host's planar buffers so the body reads like the original processBlock.
    juce::AudioBuffer<float> buffer (io, numChannels, numSamples);
    const int numCh = numChannels;

    // DSP load meter: wall-clock each stage with a monotonic clock (a cheap userspace read — no
    // alloc/lock, RT-safe). Published as a smoothed % of the block's real-time budget at the end.
    using PerfClock = std::chrono::steady_clock;
    const auto tStart = PerfClock::now();
    double nsPre = 0.0, nsEq = 0.0, nsPwr = 0.0, nsCab = 0.0;
    auto elapsedNs = [] (PerfClock::time_point a) noexcept
    { return std::chrono::duration<double, std::nano> (PerfClock::now() - a).count(); };

    // --- bypass: clean passthrough (no input trim, no processing) ---
    if (p.bypass)
    {
        inputGainPrev = juce::Decibels::decibelsToGain (p.inputGainDb);   // keep ramp continuity
        const float lvl = buffer.getMagnitude (0, numSamples);
        inLevel.store  (lvl, std::memory_order_relaxed);
        outLevel.store (lvl, std::memory_order_relaxed);
        const double z[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };   // decay the load meter toward 0 while bypassed
        accumulateLoads (z);
        return;
    }

    // --- input trim (block-ramped so it doesn't zipper) + input meter ---
    {
        const float igTarget = juce::Decibels::decibelsToGain (p.inputGainDb);
        buffer.applyGainRamp (0, numSamples, inputGainPrev, igTarget);
        inputGainPrev = igTarget;
        inLevel.store (buffer.getMagnitude (0, numSamples), std::memory_order_relaxed);
    }

    // --- map parameters onto the live smoothers ---
    const bool aLoaded  = p.aLoaded;   // false = slot A empty (no cab) → A contributes the dry signal
    const bool bLoaded  = p.bLoaded;
    const bool hpfOn[2] = { p.slot[0].hpfOn, p.slot[1].hpfOn };
    const bool lpfOn[2] = { p.slot[0].lpfOn, p.slot[1].lpfOn };
    for (int i = 0; i < 2; ++i)
    {
        mixSm[i].setTargetValue   (p.slot[i].dryWet01);
        phaseSm[i].setTargetValue (p.slot[i].phase ? -1.0f : 1.0f);
    }
    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (p.outputGainDb));

    // Track the poweramp/preamp signal ROUTE (off<->capture<->tube, preamp on/off) so a switch can
    // be handled deterministically downstream (the per-route makeup snap). A route change is a
    // discrete step in the thing being levelled; the makeup slew-limit keeps the leveler gentle.
    const int  route = routeCode (p);
    const int  oldRoute = prevRoute;
    const bool routeChanged = (route != prevRoute);
    prevRoute = route;
    if (routeChanged)
        routeDwellSamples = 0;           // the new route must dwell before its makeup is trusted

    // LEVEL-CONTEXT tracking: any change to what shapes the wet/dry spectral ratio (EQ, filters,
    // dry/wet, tube knobs, input trim — or an IR/model load signalled via modelGeneration) makes
    // every remembered route makeup stale. A stale snap would be a deterministic-looking jump to a
    // WRONG level (the pump G2 forbids), so staleness must invalidate, not "snap then glide".
    {
        const juce::uint32 mg = modelGeneration.load (std::memory_order_relaxed);
        const LevelContext ctx = makeContext (p);
        if (mg != lastModelGeneration || ! (ctx == prevContext))
        {
            lastModelGeneration = mg;
            prevContext = ctx;
            ++contextGen;
            routeDwellSamples = 0;
        }
    }

    // MUTE: muting a slot solos the other (overrides MIX); both muted => the dry
    // signal passes through (bypass), not silence (smoothed gate, click-free).
    const bool aOn = ! p.slot[0].mute;
    const bool bOn = bLoaded && ! p.slot[1].mute;
    float abTarget = bLoaded ? p.mixAB01 : 0.0f;
    if      (! aOn) abTarget = 1.0f;     // A muted => full B
    else if (! bOn) abTarget = 0.0f;     // B muted/absent => full A
    mixABSmoothed.setTargetValue (abTarget);
    muteGateSmoothed.setTargetValue ((aOn || bOn) ? 1.0f : 0.0f);

    // --- front stages (PREAMP → AMP EQ → POWERAMP): two nonlinear NAM stages plus the tone
    // stack between them, in signal order. They run on the signal BEFORE the dry tap, so their
    // output becomes the "dry" reference for both the per-slot Dry/Wet blend and the auto-level
    // match — i.e. we cab the amp, not the clean DI. Each NAM stage is a no-op passthrough when
    // off or no model is loaded; the EQ is a bit-exact passthrough when eq.on is false. The EQ
    // sits between the stages so its cuts shape what the poweramp distorts. ---
    { const auto a = PerfClock::now();
      // Latency-aligned preamp gate. The preamp has host-rate latency when it rate-matches (0 at
      // 48 kHz; a handful of samples when resampling). Keep the dry aligned to that PDC EVERY block
      // (warm), so that whether the preamp is ON (its inherent latency) or OFF (dry delayed to the
      // same amount) the plugin's reported latency never changes on the power toggle → no host
      // re-sync gap. advance() reads the raw input first (before preamp.process overwrites it).
      float* const* pio = buffer.getArrayOfWritePointers();
      preampBypassAlign.advance (pio, numCh, numSamples, preamp.latencySamples());
      if (p.preampOn)
          preamp.process (pio, numCh, numSamples, /*normalize*/ true);
      else
          for (int ch = 0; ch < numCh; ++ch)
              juce::FloatVectorOperations::copy (pio[ch], preampBypassAlign.delayed (ch), numSamples);
      nsPre = elapsedNs (a); }
    { const auto a = PerfClock::now();
      ampEq.process (buffer.getArrayOfWritePointers(), numCh, numSamples, p.eq);
      nsEq = elapsedNs (a); }
    { const auto a = PerfClock::now();
      // The poweramp seam: ampOn gates the stage; powerAmpMode picks NAM capture (`amp`, default)
      // vs the white-box tube stage. The router crossfades click-free on a live capture<->tube
      // switch and keeps the NAM path bit-identical to the legacy `if (p.ampOn) amp.process(...)`.
      powerAmpRouter.process (buffer.getArrayOfWritePointers(), numCh, numSamples,
                              p.ampOn, p.powerAmpMode, p.tube, amp);
      nsPwr = elapsedNs (a); }

    // The leveler route-snap triggers on ROUTER-ACCEPTED transitions (a change arriving mid-fade
    // is deferred to the fade end — keying off raw params would desync the snap from the audio),
    // plus the preamp power flip, which is an instant latency-aligned gate with no router fade —
    // but not while a seam fade is still running (the poweramp part of the new route may itself
    // be deferred; snapping early would retarget ahead of the audio).
    const bool routeSnapPending = powerAmpRouter.fadeJustStarted()
                               || (routeChanged && route / 100 != oldRoute / 100
                                   && ! powerAmpRouter.isFading());

    // --- stash the dry (now post-amp) signal for the per-slot Dry/Wet blend, before the filters ---
    for (int ch = 0; ch < numCh; ++ch)
        dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    // --- per-slot wet path: HPF -> LPF -> Convolution (into wet[0] / wet[1]) ---
    // Skip a slot's convolution when it's empty — its branch falls back to the dry signal
    // below (so an empty slot = "no cab", a clean passthrough, not silence).
    { const auto a = PerfClock::now();
      if (aLoaded)
          slot[0].processWet (wet[0], buffer, numCh, numSamples, hpfOn[0], p.slot[0].hpfHz, lpfOn[0], p.slot[0].lpfHz);
      if (bLoaded)
          slot[1].processWet (wet[1], buffer, numCh, numSamples, hpfOn[1], p.slot[1].hpfHz, lpfOn[1], p.slot[1].lpfHz);
      nsCab = elapsedNs (a); }

    // --- per-slot Phase + Dry/Wet, then A<->B crossfade -> buffer (the mix) ---
    {
        const auto* const* dry = dryBuffer.getArrayOfReadPointers();
        const auto* const* wa  = wet[0].getArrayOfReadPointers();
        const auto* const* wb  = wet[1].getArrayOfReadPointers();
        auto* const*       out = buffer.getArrayOfWritePointers();
        for (int n = 0; n < numSamples; ++n)
        {
            const float phA = phaseSm[0].getNextValue(), mA = mixSm[0].getNextValue();
            const float phB = phaseSm[1].getNextValue(), mB = mixSm[1].getNextValue();
            const float ab   = mixABSmoothed.getNextValue();
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float d  = dry[ch][n];
                const float sA = aLoaded ? d * (1.0f - mA) + wa[ch][n] * phA * mA : d;   // empty A → dry
                const float sB = bLoaded ? d * (1.0f - mB) + wb[ch][n] * phB * mB : sA;
                // MUTE gate is applied AFTER auto-level (#45) so the leveler measures the
                // *ungated* wet — its makeup stays put through a toggle instead of lagging
                // ~1 s behind the gate (that lag caused the mute dip + un-mute overshoot).
                out[ch][n] = bLoaded ? (sA * (1.0f - ab) + sB * ab) : sA;
            }
        }
    }

    // --- auto-level (global on the final mix): measure the dry + mixed mean-square,
    // hand it to the follower, which returns the per-sample makeup gain. ---
    {
        double dMS = 0.0, mMS = 0.0;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float dr = dryBuffer.getRMSLevel (ch, 0, numSamples);
            const float mr = buffer.getRMSLevel (ch, 0, numSamples);
            dMS += (double) dr * dr;
            mMS += (double) mr * mr;
        }
        const int chs = juce::jmax (1, numCh);
        const double dryMS = dMS / chs;

        // Route snap: entering a route whose converged makeup is REMEMBERED for the current level
        // context → retarget the leveler deterministically (bounded fast glide, synced with the
        // seam's 30 ms crossfade) instead of letting the followers crawl there over ~0.5 s. An
        // unvisited/stale route just converges normally (bounded by the 9 dB/s slew).
        if (routeSnapPending && p.autoLevel)
        {
            const RouteLevel& rl = routeLevel[routeIndex (route)];
            if (rl.valid && rl.gen == contextGen)
                autoLeveler.snapRatioTo (juce::Decibels::decibelsToGain (rl.makeupDb));
        }

        autoLeveler.processBlock (dryMS, mMS / chs, p.autoLevel, numSamples);

        // An autoLevel flip starts the leveler's own 0.35 s glide — the dwell must restart so a
        // mid-glide gain can't be trusted (review finding: off→on then a fast A/B snapped to a
        // half-glided ~unity value).
        if (p.autoLevel != prevAutoLevel)
        {
            prevAutoLevel = p.autoLevel;
            routeDwellSamples = 0;
        }

        // Route-makeup memory: while the current route DWELLS (leveler on, not fading, signal
        // above the silence floor, gain LANDED — never a mid-glide value) past the trust
        // threshold, keep its converged makeup fresh.
        if (p.autoLevel && ! powerAmpRouter.isFading() && dryMS > 1.0e-6 && autoLeveler.settled())
        {
            routeDwellSamples = juce::jmin (routeDwellSamples + numSamples, 1 << 30);
            if (routeDwellSamples >= (int) (kRouteDwellSeconds * currentSampleRate))
                routeLevel[routeIndex (route)] = { autoLeveler.currentGainDb(), contextGen, true };
        }
    }

    // --- auto-level makeup (wet only), then the MUTE gate (→ dry), then master gain ---
    // The leveler measured the ungated wet above, so its makeup is steady across a mute.
    // Here the wet is leveled and crossfaded to the raw (un-leveled) dry by the gate: the
    // dry passes clean (like bypass) and the leveled wet is already matched → no dip / poof.
    {
        const auto* const* dry = dryBuffer.getArrayOfReadPointers();
        auto* const*       out = buffer.getArrayOfWritePointers();
        for (int n = 0; n < numSamples; ++n)
        {
            const float g    = gainSmoothed.getNextValue();
            const float mg   = autoLeveler.getNextGain();
            const float gate = muteGateSmoothed.getNextValue();
            for (int ch = 0; ch < numCh; ++ch)
                out[ch][n] = (out[ch][n] * mg * gate + dry[ch][n] * (1.0f - gate)) * g;
        }
    }

    // --- output meter (post everything) ---
    outLevel.store (buffer.getMagnitude (0, numSamples), std::memory_order_relaxed);

    // --- feed the spectrum taps (pre = dry input, post = output) ---
    if (spectrumActive.load (std::memory_order_relaxed) && ! nonRealtime)
    {
        const auto* const* dry = dryBuffer.getArrayOfReadPointers();
        const auto* const* out = buffer.getArrayOfReadPointers();
        const float inv = 1.0f / (float) juce::jmax (1, numCh);
        for (int n = 0; n < numSamples; ++n)
        {
            float pre = 0.0f, post = 0.0f;
            for (int ch = 0; ch < numCh; ++ch) { pre += dry[ch][n]; post += out[ch][n]; }
            preTap.push  (pre  * inv);
            postTap.push (post * inv);
        }
    }

    // --- DSP load meter: each stage's wall-clock as a % of this block's real-time budget ---
    {
        const double totalNs  = elapsedNs (tStart);
        const double budgetNs = currentSampleRate > 0.0 ? (double) numSamples / currentSampleRate * 1.0e9 : 0.0;
        if (budgetNs > 0.0)
        {
            const double pct[5] = { totalNs / budgetNs * 100.0, nsPre / budgetNs * 100.0,
                                    nsEq / budgetNs * 100.0, nsPwr / budgetNs * 100.0, nsCab / budgetNs * 100.0 };
            accumulateLoads (pct);
        }
    }
}

void CabEngine::accumulateLoads (const double pct[5]) noexcept
{
    // One-pole EMA — stable at the GUI's 30 Hz, quick enough to track a stage toggling on/off.
    constexpr double a = 0.08;
    for (int i = 0; i < 5; ++i)
    {
        cpuSm[i] += a * (pct[i] - cpuSm[i]);
        cpuPct[i].store ((float) cpuSm[i], std::memory_order_relaxed);
    }
}

//==============================================================================
void CabEngine::setSlotOriginalIR (int s, const float* const* samples, int numChannels,
                                   int numSamples, double irSampleRate)
{
    slot[s].setOriginalIR (samples, numChannels, numSamples, irSampleRate);
    bumpLevelContext();
}

double CabEngine::slotApplyTrim (int s, bool trimOn, float trimFraction01, bool headOn)
{
    const double r = slot[s].applyTrim (trimOn, trimFraction01, headOn);
    bumpLevelContext();
    return r;
}

void CabEngine::slotLoadBytesFallback (int s, const void* data, size_t size)
{
    slot[s].loadBytesFallback (data, size);
    bumpLevelContext();
}

void CabEngine::clearSlotOriginal (int s)           { slot[s].clearOriginal(); bumpLevelContext(); }
bool CabEngine::slotHasOriginal   (int s) const     { return slot[s].hasOriginal(); }
double CabEngine::slotTrimmedSeconds (int s) const  { return slot[s].trimmedLengthSeconds(); }

const juce::AudioBuffer<float>& CabEngine::slotOriginal (int s) const { return slot[s].originalBuffer(); }
double CabEngine::slotOriginalSampleRate (int s) const               { return slot[s].originalSampleRate(); }

bool CabEngine::pullSpectrum (bool pre, float* destFftSize)
{
    // The SPSC ready-handshake lives in felitronics::analysis::SpectrumTap::tryPull: it copies the
    // latest ready frame into destFftSize[fftSize] and re-arms, or returns false if none is ready.
    return (pre ? preTap : postTap).tryPull (destFftSize);
}

} // namespace cab
