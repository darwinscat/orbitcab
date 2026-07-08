// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

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
    noiseGate.prepare (sampleRate, maxBlock, numChannels);
    noiseGate.seedEnabled (initial.gate.on);   // seed the on/off crossfade from the restored on-state (no fade-in leak)
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
    reverbMixSm.reset (sampleRate, kRampSeconds);
    reverbMixSm.setCurrentAndTargetValue (initial.reverb.type > 0 ? initial.reverb.mix01 : 0.0f);

    autoLeveler.prepare (sampleRate);
    prevRoute = routeCode (initial);   // seed so a session booting in any route isn't seen as a switch on block 1
    prevContext = makeContext (initial);
    lastModelGeneration = modelGeneration.load (std::memory_order_relaxed);
    routeDwellSamples = 0;
    prevAutoLevel = initial.autoLevel;
    for (auto& r : routeLevel) r = RouteLevel {};   // route memory restarts with the stream
    for (auto& row : pairDeltaDb)    for (auto& v : row) v = 0.0f;   // pair deltas too — they are
    for (auto& row : pairDeltaKnown) for (auto& v : row) v = false;  // per-stream learned estimates

    inputGainPrev = juce::Decibels::decibelsToGain (initial.inputGainDb);
    preampVolPrev = juce::Decibels::decibelsToGain (initial.preampVolumeDb);
    inLevel.store (0.0f);
    outLevel.store (0.0f);

    dryBuffer.setSize (numChannels, maxBlock, false, false, true);
    wet[0].setSize    (numChannels, maxBlock, false, false, true);
    wet[1].setSize    (numChannels, maxBlock, false, false, true);

    // Reverb: a MONO convolver (½ the CPU of stereo, no false width — "not a stereo reverb") with the
    // reference-unity RMS normalization DISABLED (the spring IRs are peak-normalized at bundle time). The
    // default 4 s NUPC schedule covers the ≤ 3.5 s spring tails. reverbScratch is the mono send/return buffer.
    reverbConv.prepare (sampleRate, maxBlock, 1, 4.0, /*normalize*/ false);
    reverbScratch.setSize (1, maxBlock, false, false, true);
}

void CabEngine::reset()
{
    preamp.reset();
    preampBypassAlign.reset();
    noiseGate.reset();
    ampEq.reset();
    amp.reset();
    powerAmpRouter.reset();
    reverbConv.reset();
    for (int i = 0; i < 2; ++i)
        slot[i].reset();
    autoLeveler.reset();
}

void CabEngine::seedAutoLevel()
{
    // The convolver normalizes every IR to reference-unity at load (see Convolver.h), so the
    // converged wet->dry makeup is ~0 dB plus a small signal-dependent residual (the program's
    // spectrum vs the reference's). Seed at unity: the first audio starts ~level and the
    // followers only trim the residual — no startup kick (#48), no IR-energy estimate needed
    // (the old ‖ir‖₂ seed modelled the RAW-IR gain the normalization has since removed).
    if (slot[0].hasOriginal())
        autoLeveler.seed (1.0f);
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

    // Amp-section lane count. When the input was folded to a mono source (p.monoAmp — Input =
    // Left/Right), run the NAM preamp / AMP EQ / poweramp on ONE lane: AmpStage has an explicit
    // numChannels==1 branch that runs a single nam::DSP, so this halves the dominant NAM cost. We
    // then duplicate ch0 -> the other channel(s) just before the cab, so the dry stash, both
    // convolution lanes, mix, auto-level and meters keep running full-width (a mono source lands
    // centred; a stereo IR still images L/R from the shared source; convolver tails stay warm).
    // Stereo (monoAmp=false) => frontCh == numCh, i.e. unchanged v1 true-stereo behaviour.
    const int frontCh = (p.monoAmp && numCh > 1) ? 1 : numCh;

    // DSP load meter: wall-clock each stage with a monotonic clock (a cheap userspace read — no
    // alloc/lock, RT-safe). Published as a smoothed % of the block's real-time budget at the end.
    using PerfClock = std::chrono::steady_clock;
    const auto tStart = PerfClock::now();
    double nsPre = 0.0, nsEq = 0.0, nsPwr = 0.0, nsCab = 0.0, nsRev = 0.0;
    auto elapsedNs = [] (PerfClock::time_point a) noexcept
    { return std::chrono::duration<double, std::nano> (PerfClock::now() - a).count(); };

    // --- bypass: clean passthrough (no input trim, no processing) ---
    if (p.bypass)
    {
        inputGainPrev = juce::Decibels::decibelsToGain (p.inputGainDb);   // keep ramp continuity
        const float lvl = buffer.getMagnitude (0, numSamples);
        inLevel.store  (lvl, std::memory_order_relaxed);
        outLevel.store (lvl, std::memory_order_relaxed);
        gateLevel.store (1.0f, std::memory_order_relaxed);   // bypassed → the gate isn't attenuating; keep its LED open
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
      // NOISE GATE — PHASE A (DETECTOR): key off the CLEAN post-trim input on the frontCh lane(s), NOW,
      // before preamp.process overwrites pio in place. The per-sample gain curve is stashed and applied
      // after the EQ (phase B). Keying the clean input gives accurate open/close; the preamp's latency
      // (0 at 48 kHz) between here and the VCA point is a small free lookahead, uncompensated.
      noiseGate.analyse (pio, frontCh, numSamples, p.gate.on, p.gate.thresholdDb);
      if (p.preampOn)
          preamp.process (pio, frontCh, numSamples, /*normalize*/ true);   // frontCh: 1 lane when mono-folded
      else
          for (int ch = 0; ch < numCh; ++ch)
              juce::FloatVectorOperations::copy (pio[ch], preampBypassAlign.delayed (ch), numSamples);
      nsPre = elapsedNs (a); }

    // Preamp VOLUME: post-preamp output gain (block-ramped, zipper-free) — the preamp's own "volume"
    // driving the tone stack / poweramp / cab. On the frontCh lane; ch>frontCh is stale but the mono
    // fold overwrites it. Sits before the dry tap, so the auto-level reference tracks it.
    {
        const float pvTarget = juce::Decibels::decibelsToGain (p.preampVolumeDb);
        buffer.applyGainRamp (0, numSamples, preampVolPrev, pvTarget);
        preampVolPrev = pvTarget;
    }

    { const auto a = PerfClock::now();
      ampEq.process (buffer.getArrayOfWritePointers(), frontCh, numSamples, p.eq);
      // NOISE GATE — PHASE B (VCA): apply the gain curve computed in phase A. Post-EQ (kills the preamp's
      // hiss shaped by the tone stack) and BEFORE the spring-reverb send below, so a closing gate stops
      // feeding the tank while its tail rings out. On the frontCh lane(s); the mono fold carries ch0 across.
      noiseGate.applyGain (buffer.getArrayOfWritePointers(), frontCh, numSamples);
      nsEq = elapsedNs (a); }

    // --- SPRING REVERB (AFTER the EQ, BEFORE the poweramp): a real amp's built-in spring tank. The wet
    // is summed into the amped guitar HERE, so the poweramp distorts it and the cab IR filters it — it
    // mixes with the guitar "in the cabinet", NOT as a clean stereo studio reverb after the amp. Mono
    // SEND/RETURN: sum the front lane(s) to one mono send, convolve ONCE (zero latency), add the single
    // wet tail back to every front lane. One convolution in either the mono-fold (frontCh=1) or true-
    // stereo (frontCh=2) case, and no false stereo width. Off (type 0 / mix 0 / IR not yet loaded) is
    // skipped entirely (zero CPU); a turn-off fades via the ramp instead of cutting. Runs on the frontCh
    // lanes, before the mono fold — the fold then carries the reverbed ch0 across in mono mode. ---
    {
        const auto aRev = PerfClock::now();
        const float revTarget = (p.reverb.type > 0) ? p.reverb.mix01 : 0.0f;
        reverbMixSm.setTargetValue (revTarget);
        if (reverbHasIR() && (revTarget > 0.0f || reverbMixSm.getCurrentValue() > 1.0e-4f))
        {
            float* const* pio = buffer.getArrayOfWritePointers();
            float*        rs  = reverbScratch.getWritePointer (0);
            if (frontCh >= 2)
                for (int n = 0; n < numSamples; ++n) rs[n] = 0.5f * (pio[0][n] + pio[1][n]);   // mono send
            else
                juce::FloatVectorOperations::copy (rs, pio[0], numSamples);

            float* rsPlanes[1] { rs };
            reverbConv.process (rsPlanes, 1, numSamples);   // mono spring tank, in-place, zero latency

            // kReverbWetGain calibrates the return level: a spring IR convolved with a sustained note
            // accumulates a LOT of tail energy, so a raw unity return is ~6-7× too hot (the Mix knob felt
            // maxed by ~15 %). 0.15 spreads a musical range across 0..100 % (100 % ≈ the old ~15 %).
            // scale01 = the TEMP wet-level calibration knob (replaces the old fixed 0.15 while per-reverb
            // levels are dialled in). Read per block — a coarse step when dragged, fine for calibration.
            const float wetScale = p.reverb.scale01;
            for (int n = 0; n < numSamples; ++n)
            {
                const float wet = rs[n] * reverbMixSm.getNextValue() * wetScale;   // parallel return (dry kept)
                for (int ch = 0; ch < frontCh; ++ch) pio[ch][n] += wet;
            }
        }
        else
        {
            // Idle: park the mix ramp at 0 while the IR is still loading (async) so the reverb RAMPS IN from
            // silence when it lands — no jump — else at the target. We deliberately do NOT reset the
            // convolver to clear its tail here: reset() shares state (state_/xfade/slots) with the
            // message-thread IR load (setOperator) and the NUPC contract forbids racing the two, so a
            // mid-stream clear is unsafe. Cost: re-enabling after a disable can briefly reveal the previous
            // decayed tail — inaudible in practice (the 30 ms mix ramp fades it in from silence).
            reverbMixSm.setCurrentAndTargetValue (reverbHasIR() ? revTarget : 0.0f);
        }
        nsRev = elapsedNs (aRev);
    }

    // Preamp-OUT level: the magnitude leaving the preamp section (preamp → volume → EQ → gate → reverb) that
    // feeds the poweramp — the "preamp out, before the poweramp + cab" meter on the right edge of the strip.
    // Over the FRONT lane(s) only: in mono-fold + preamp-on, ch ≥ frontCh is stale until the fold below
    // (~line 329), so an all-channels getMagnitude would read the UNGATED stale lane and ignore the gate.
    float preampMag = 0.0f;
    for (int ch = 0; ch < frontCh; ++ch) preampMag = juce::jmax (preampMag, buffer.getMagnitude (ch, 0, numSamples));
    preampLevel.store (preampMag, std::memory_order_relaxed);
    gateLevel.store  (noiseGate.currentGain(), std::memory_order_relaxed);   // effective gate gain → GR meter (atomic publish)

    { const auto a = PerfClock::now();
      // The poweramp seam: ampOn gates the stage; powerAmpMode picks NAM capture (`amp`, default)
      // vs the white-box tube stage. The router crossfades click-free on a live capture<->tube
      // switch and keeps the NAM path bit-identical to the legacy `if (p.ampOn) amp.process(...)`.
      powerAmpRouter.process (buffer.getArrayOfWritePointers(), frontCh, numSamples,
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

    // Mono amp fold: the front stages ran on ch0 only (frontCh=1) — copy the fully-amped ch0 onto
    // the remaining channel(s) NOW, before the dry stash, so everything downstream (dry reference,
    // both cab lanes, mix, auto-level, meters) sees a full-width post-amp signal. No-op in stereo.
    for (int ch = frontCh; ch < numCh; ++ch)
        buffer.copyFrom (ch, 0, buffer, 0, 0, numSamples);

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

        // Route snap: entering a route whose makeup is REMEMBERED for the current level context →
        // retarget the leveler deterministically (bounded fast glide, synced with the seam's
        // 30 ms crossfade). No fresh memory (first visit / context changed)? Then start NEAR, not
        // from scratch: snap to (current makeup + learned pair delta) — the spectral A-vs-B
        // offset is largely portable across IR/EQ changes, so the residual is ~±1-2 dB instead of
        // the full step. No delta either (very first transition ever)? Open the transition window
        // so the followers converge at their natural 150 ms speed (ceiling 20 dB/s) instead of
        // being slew-starved at 9 dB/s.
        if (routeSnapPending && p.autoLevel)
        {
            const int to = routeIndex (route), from = routeIndex (oldRoute);
            const RouteLevel& rl = routeLevel[to];
            if (rl.valid && rl.gen == contextGen)
                autoLeveler.snapRatioTo (juce::Decibels::decibelsToGain (rl.makeupDb));
            else if (from >= 0 && from < 6 && pairDeltaKnown[to][from])
            {
                // An ESTIMATE (the delta was learned under a possibly different context — e.g. a
                // dry/wet move dilutes the mode difference): land on it instantly, then open the
                // transition window so the followers correct the residual at 20 dB/s, not 9.
                autoLeveler.snapRatioTo (juce::Decibels::decibelsToGain (
                    autoLeveler.currentGainDb() + pairDeltaDb[to][from]));
                autoLeveler.openTransitionWindow();
            }
            else
                autoLeveler.openTransitionWindow();
        }

        // Freeze the leveler while the GATE holds the signal closed. The gate floors the DRY instantly, but
        // the cab-IR convolution TAIL keeps mixMS up (convolution has memory) — so the raw sqrt(dryMS/mixMS)
        // ratio is bogus during a gated pause. Adapting to it droops the makeup (measured up to ~10 dB with a
        // multi-second user IR) and would poison the route-makeup cache on note return. Skipping processBlock
        // holds the followers + target at their pre-close values; the gate re-opens fast so the leveler resumes
        // at once. (The gate stays OUT of LevelContext/routeCode — it is a signal SCALE, not a spectral-context
        // change: dry and wet scale together, so a STATIC gate gain is already ratio-invariant; only this
        // DYNAMIC close-vs-IR-tail case needs the hold. Short cab IRs — both followers free-fall in lockstep —
        // are unaffected either way.) gateGain 1 (open) or gate off ⇒ never holds.
        constexpr float kGateLevelerHoldGain = 0.1f;   // gate ≥ 20 dB down ⇒ dry is effectively gone
        const bool gateHold = p.gate.on && noiseGate.currentGain() < kGateLevelerHoldGain;
        if (! gateHold)
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
        // above the silence floor, no deterministic glide in flight) past the trust threshold,
        // keep its makeup fresh. On live (non-stationary) material the value wobbles ±~1 dB with
        // the program — that is fine for a snap target (the followers correct the residual); the
        // write gate only has to exclude the SNAP GLIDES themselves (a mid-glide value is not a
        // measurement). An earlier settled()-gate required applied==target and on real playing
        // that is ~never true — caches never formed and every mode switch re-faded (field bug).
        if (p.autoLevel && ! powerAmpRouter.isFading() && dryMS > 1.0e-6 && ! autoLeveler.snapGliding())
        {
            routeDwellSamples = juce::jmin (routeDwellSamples + numSamples, 1 << 30);
            if (routeDwellSamples >= (int) (kRouteDwellSeconds * currentSampleRate))
            {
                const int idx = routeIndex (route);
                routeLevel[idx] = { autoLeveler.currentGainDb(), contextGen, true };
                // Learn PAIR DELTAS against every other route measured under the SAME context.
                // Deltas survive context changes (they are spectral A-vs-B offsets, largely
                // IR/EQ-portable) and give the first visit after a change a "start near, not
                // from scratch" snap estimate.
                for (int other = 0; other < 6; ++other)
                    if (other != idx && routeLevel[other].valid && routeLevel[other].gen == contextGen)
                    {
                        pairDeltaDb[idx][other] = routeLevel[idx].makeupDb - routeLevel[other].makeupDb;
                        pairDeltaKnown[idx][other] = true;
                        pairDeltaDb[other][idx] = -pairDeltaDb[idx][other];
                        pairDeltaKnown[other][idx] = true;
                    }
            }
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
            const double pct[6] = { totalNs / budgetNs * 100.0, nsPre / budgetNs * 100.0,
                                    nsEq / budgetNs * 100.0, nsPwr / budgetNs * 100.0, nsCab / budgetNs * 100.0,
                                    nsRev / budgetNs * 100.0 };
            accumulateLoads (pct);
        }
    }
}

void CabEngine::accumulateLoads (const double pct[6]) noexcept
{
    // One-pole EMA — stable at the GUI's 30 Hz, quick enough to track a stage toggling on/off.
    constexpr double a = 0.08;
    for (int i = 0; i < 6; ++i)
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
