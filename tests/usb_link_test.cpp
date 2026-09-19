// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the driver's no-hardware paths. It cannot exercise the iso
// pump without a DAC; what it does prove is that the driver and the vendored
// libusb link as a standalone library and fail cleanly with nothing plugged
// in, which is the state every consumer starts in.

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
    d.stop();  // documented idempotent from any state
    check(!d.isStreaming(), "stop() on an idle driver is a no-op");

    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
