#pragma once
// Extra sections for `--dump`: HID collections that could not be opened, every USB device (including
// ones without a HID interface, e.g. audio-only or WinUSB dongles and serial ports) and the Bluetooth
// devices Windows knows about.
#include <windows.h>

#include <string>

namespace report {

std::wstring DumpExtras();

}  // namespace report
