// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace cab
{

//==============================================================================
// cab::StreamResampler — streaming arbitrary-ratio resampler (cubic / Catmull-Rom). Used by
// cab::AmpStage to run a rate-locked NAM model at its native 48 kHz on any host rate (host→model
// on the way in, model→host on the way out).
//
// 🔴 Fixed capacity, allocated once in reset(): feed()/produce*() never allocate on the audio
// thread. `buf` is a linear scratch holding `len` valid samples at the front, compacted by memmove;
// `pos` carries the sub-sample phase across blocks. Header-only so it can be unit-tested directly.
//==============================================================================
struct StreamResampler
{
    double inPerOut = 1.0;        // input samples advanced per output (= inRate / outRate)
    double pos      = 1.0;        // fractional read position into buf (>=1: cubic needs buf[i-1])
    std::vector<float> buf;       // capacity fixed in reset(); buf[0..len) valid
    int len = 0;

    void reset (double inRate, double outRate, int capacity)
    {
        inPerOut = inRate / outRate;
        buf.assign ((size_t) capacity + 8, 0.0f);            // ALLOC here (message thread) — never in process
        len = 3;                                             // 3 leading history zeros
        pos = 1.0;
    }

    void feed (const float* in, int n)
    {
        if (len + n > (int) buf.size())                      // backstop: sized so this shouldn't trigger
        {
            const int drop = len + n - (int) buf.size();
            std::memmove (buf.data(), buf.data() + drop, (size_t) (len - drop) * sizeof (float));
            len -= drop; pos -= drop;
        }
        std::copy (in, in + n, buf.data() + len);
        len += n;
    }

    static float catmull (float a, float b, float c, float d, float t)
    {
        const float t2 = t * t, t3 = t2 * t;
        return 0.5f * ((2.0f * b) + (-a + c) * t
                     + (2.0f * a - 5.0f * b + 4.0f * c - d) * t2
                     + (-a + 3.0f * b - 3.0f * c + d) * t3);
    }

    int produceAvailable (float* out, int cap)
    {
        int k = 0;
        while (k < cap)
        {
            const int i = (int) std::floor (pos);
            if (i + 2 >= len) break;                         // need buf[i-1..i+2]
            out[k++] = catmull (buf[i - 1], buf[i], buf[i + 1], buf[i + 2], (float) (pos - i));
            pos += inPerOut;
        }
        const int keep = (int) std::floor (pos) - 1;         // keep one sample before the read head
        if (keep > 0)
        {
            std::memmove (buf.data(), buf.data() + keep, (size_t) (len - keep) * sizeof (float));
            len -= keep; pos -= keep;
        }
        return k;
    }

    void produceExact (float* out, int want)                 // pad with silence on startup underflow
    {
        const int got = produceAvailable (out, want);
        for (int k = got; k < want; ++k) out[k] = 0.0f;
    }
};

} // namespace cab
