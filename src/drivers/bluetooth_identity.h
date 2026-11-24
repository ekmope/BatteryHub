#pragma once
// Bluetooth device identity is its Windows device-container ID, not its display name.
// Keep the legacy name-based fallback only for devices reported by other drivers
// (e.g. vendor drivers), while allowing two Bluetooth controllers with identical names.
#include "../devices.h"

#include <vector>

inline bool IsDuplicateBluetoothDevice(const DeviceStatus& candidate, const std::vector<DeviceStatus>& existing) {
    for (const auto& other : existing) {
        if (other.key == candidate.key) return true;
        // Vendor drivers (AirPods, Galaxy Buds, Rapoo) also say via "Bluetooth", so tell
        // PollBluetooth's own entries apart by their "bt:" key instead.
        if (other.key.rfind(L"bt:", 0) != 0 && other.name == candidate.name) return true;
    }
    return false;
}

// A vendor row can belong to a link that is dead while the device itself is alive: the 2.4 GHz
// receiver stays plugged in when the mouse or keyboard is switched to Bluetooth, so the vendor
// driver reads an all-zero status block and reports the device as asleep with no level. That
// printed picture is not evidence - the Bluetooth reading is the live one, so it replaces the row
// instead of being hidden behind it. A row that still carries a level is left alone.
inline bool StaleVendorRowFor(const DeviceStatus& existing, const DeviceStatus& candidate) {
    return existing.key != candidate.key && existing.key.rfind(L"bt:", 0) != 0 &&
           existing.name == candidate.name && existing.asleep && existing.percent < 0;
}
