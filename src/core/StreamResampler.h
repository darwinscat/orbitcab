// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

// Moved to felitronics-core v0.13.0; this alias keeps the cab:: seam stable.
#include <felitronics/core/StreamResampler.h>

namespace cab
{
using StreamResampler = felitronics::core::StreamResampler;
}
