// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit-test runner for the cab:: DSP core. Links core/ only — no GUI, no
// host, no MessageManager (the proof that the core is testable in isolation).
// Returns non-zero on any failure so it gates CI.
#include <juce_core/juce_core.h>
#include <cstdio>

int main()
{
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runAllTests();

    int totalPass = 0, totalFail = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* r = runner.getResult (i);
        if (r == nullptr)
            continue;
        totalPass += r->passes;
        totalFail += r->failures;
        std::printf ("[%s] %-28s %d passed, %d failed\n",
                     r->failures == 0 ? "PASS" : "FAIL",
                     r->unitTestName.toRawUTF8(), r->passes, r->failures);
    }
    std::printf ("\n==== CORE TESTS: %d checks, %d failures — %s ====\n",
                 totalPass + totalFail, totalFail, totalFail == 0 ? "ALL PASSED" : "FAILED");
    return totalFail == 0 ? 0 : 1;
}
