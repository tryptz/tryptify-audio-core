// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the driver's no-hardware paths. It cannot exercise the iso
// pump without a DAC; what it does prove is that the driver and the vendored
// libusb link as a standalone library and fail cleanly with nothing plugged
// in, which is the state every consumer starts in.

#include <chrono>
#include <cstdio>

#include "libusb_uac_driver.h"

using monotrypt::usb::LibusbUacDriver;
using monotrypt::usb::StartError;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
}  // namespace

int main() {
    LibusbUacDriver d;
    check(!d.isOpen(), "fresh driver is not open");
    check(!d.start(48000, 16, 2), "start() without open() fails");
    check(d.lastError() == StartError::NoDevice, "  ...reporting NoDevice");
    check(!d.isStreaming(), "  ...and is not streaming");
    check(d.playedFrames() == 0, "played clock starts at zero");

    // Position accounting. The iso pump needs a DAC, so what is provable
    // here is the shape of the reported figures before any stream exists:
    // every clock at zero, and audibleFrames() never negative. That last
    // one matters because it is playedFrames() minus two other terms, and
    // a consumer feeding a negative frame count into a media timeline
    // gets a seek to garbage rather than a visible error.
    check(d.silenceFrames() == 0, "underrun padding clock starts at zero");
    check(d.inflightFrames() == 0, "queue depth is zero before start()");
    check(d.audibleFrames() == 0, "audible clock starts at zero");
    check(d.audibleFrames() >= 0, "  ...and is never negative");
    check(d.ringFrames() == 0, "ring capacity is zero before start()");

    // waitWritable() must not park a producer for its full timeout when
    // there is no pump to free space. It also must not claim success:
    // its predicate fires on teardown as well as on space becoming
    // available, so the return value is re-derived from the ring.
    {
        const auto t0 = std::chrono::steady_clock::now();
        const bool got = d.waitWritable(1024, 2000);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        check(!got, "waitWritable() reports no space on an idle driver");
        check(elapsed < 500, "  ...and returns without burning its timeout");
    }

    check(d.waitWritable(0, 0), "waitWritable(0) is trivially satisfied");

    d.stop();  // documented idempotent from any state
    check(!d.isStreaming(), "stop() on an idle driver is a no-op");
    check(d.audibleFrames() == 0, "clocks stay at zero after a no-op stop");

    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
