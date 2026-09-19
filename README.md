# tryptify-audio-core

Native audio code shared by [Tryptify](https://github.com/tryptz/monotrypt.android)
and the StepMania Android port.

| Target    | What it is |
|-----------|------------|
| `tac_usb` | libusb UAC1/UAC2 isochronous output driver. Takes a `UsbDeviceConnection` fd, owns the streaming interface, paces from the async feedback endpoint. |
| `tac_dsp` | Mix-bus DSP engine: four buses plus master, snap-in processor chains, oversampling. |

No JNI. JNI symbol names encode the calling class, so each app keeps its own
bridge and links these static libraries into its own `.so`.

## Use

```cmake
add_subdirectory(third_party/tryptify-audio-core)
target_link_libraries(my_native_lib tac_usb tac_dsp)
```

Clone with `--recursive`, or run `git submodule update --init --recursive`
(libusb is a submodule).

## Test on the host

```
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

Logging goes to logcat on Android and stderr elsewhere (`common/tac_log.h`),
which is what lets the library build and test without a device.

## Known behaviour

- An empty `DspEngine` is **-6.02 dB**, not unity: the equal-power pan law
  (0.707 at center) is applied at the bus stage and again at master.
  `dsp_engine_test` prints the measured figure.
- `DspEngine::process()` takes `chainMutex_`. Fine behind a ring buffer, which
  both consumers have; not safe to call straight from a hardware callback.
- `LibusbUacDriver::playedFrames()` counts frames handed to the iso pump,
  underrun silence included. It leads what the DAC is playing by the frames
  in flight.

## License

GPL-3.0-or-later.
