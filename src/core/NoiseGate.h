// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

// The in-amp noise gate now lives in the shared, JUCE-free DSP core felitronics-core
// (`felitronics::dynamics::NoiseGate`, in modules/dynamics) so other plugins don't reinvent it — it
// composes the module's ChannelLinker + EnvelopeFollower with a gate-specific Schmitt/hold state machine.
// This header is a thin compat re-export: `cab::NoiseGate` keeps the engine/adapter source unchanged.
#include <felitronics/dynamics/NoiseGate.h>

namespace cab
{
using NoiseGate = felitronics::dynamics::NoiseGate;
}
