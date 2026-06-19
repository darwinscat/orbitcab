// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
// The plugin's APVTS parameter layout — one source of truth for the
// host-automatable parameters and their on-screen + host readout formatters.
//
// 🔒 The parameter IDs and `kParamVersion` are FROZEN: v1 DAW sessions and automation
// lanes key off them, so they must not change (state back-compat).
//==============================================================================
namespace orbitcab
{

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

} // namespace orbitcab
