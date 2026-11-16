#include "Config.hpp"
#include "DeviceRegistry.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace nvstraps {
namespace {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void append(Bytes &bytes, uint64_t value, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        bytes.push_back(static_cast<uint8_t>(value));
        value >>= 8;
    }
}

void appendLocation(Bytes &bytes, const Location &location) {
    require(location.wildcard() || (location.device < 32 && location.function < 8),
            "Invalid PCI location.");
    append(bytes, location.bus, 1);
    append(bytes, (location.device << 3) | (location.function & 7), 1);
}

bool sameSelector(const Selector &left, const Selector &right) {
    return left.deviceID == right.deviceID && left.subsystemVendor == right.subsystemVendor
        && left.subsystemDevice == right.subsystemDevice && left.location == right.location;
}

class Reader {
    const Bytes &bytes;
    size_t position = 0;
public:
    explicit Reader(const Bytes &data) : bytes(data) {}
    uint64_t take(size_t length) {
        auto value = readLE(bytes, position, length);
        position += length;
        return value;
    }
    uint8_t byte() { return static_cast<uint8_t>(take(1)); }
    uint16_t word() { return static_cast<uint16_t>(take(2)); }
    Location location(bool wildcardAllowed) {
        const auto bus = byte(), positionByte = byte();
        if (wildcardAllowed && bus == 255 && positionByte == 255) return {};
        return {bus, static_cast<uint8_t>(positionByte >> 3), static_cast<uint8_t>(positionByte & 7)};
    }
    size_t count(size_t maximum) {
        const auto count = byte();
        require(count <= maximum, "Configuration record count exceeds the v0.3 limit.");
        return count;
    }
    bool done() const { return position == bytes.size(); }
};

} // namespace

std::string hex(uint64_t value, unsigned width) {
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << value;
    return stream.str();
}

uint64_t readLE(const Bytes &bytes, size_t offset, size_t length) {
    require(length <= 8 && offset <= bytes.size() && length <= bytes.size() - offset,
            "Truncated configuration or PCI property.");
    uint64_t value = 0;
    for (size_t i = 0; i < length; ++i) value |= uint64_t(bytes[offset + i]) << (8 * i);
    return value;
}

bool Location::operator==(const Location &other) const {
    return bus == other.bus && device == other.device && function == other.function;
}

bool Location::wildcard() const { return bus == 255 && device == 255 && function == 255; }

std::string Location::text() const {
    return wildcard() ? "any location" : hex(bus, 2) + ":" + hex(device, 2) + "." + hex(function);
}

bool Bridge::operator==(const Bridge &other) const {
    return vendorID == other.vendorID && deviceID == other.deviceID
        && location == other.location && secondaryBus == other.secondaryBus;
}

bool validPCISize(unsigned value) { return value <= 32 || value == 64 || value == 65; }

std::string gpuSizeText(unsigned value) {
    if (value == Automatic) return "automatic / no explicit size";
    if (value == Excluded) return "excluded";
    if (value > 10) return "invalid";
    const auto mib = uint64_t(64) << value;
    return mib < 1024 ? std::to_string(mib) + " MiB" : std::to_string(mib / 1024) + " GiB";
}

std::string pciSizeText(unsigned value) {
    if (value == 0) return "system default";
    if (value == 32) return "any supported BAR size";
    if (value == 64) return "selected GPUs only";
    if (value == 65) return "GPU straps only; leave PCI BAR configuration to firmware";
    if (!validPCISize(value)) return "invalid";
    const char *units[] = {"MiB", "GiB", "TiB", "PiB"};
    return "maximum " + std::to_string(1u << (value % 10)) + " " + units[value / 10];
}

bool Config::enabled() const { return (flags & 3) != 0; }
bool Config::configured() const { return pciSize || enabled() || !selectors.empty(); }
void Config::setEnabled(bool enabled) { flags = (flags & ~3u) | (enabled ? 2 : 0); }

uint8_t Config::selectedSize(const Device &device) const {
    if (device.vendorID != 0x10DE) return Automatic;
    uint8_t result = Automatic;
    unsigned priority = 0;
    for (const auto &selector : selectors) {
        if (selector.deviceID != device.deviceID || selector.size == Automatic) continue;
        const bool subsystem = selector.subsystemVendor != 65535 && selector.subsystemDevice != 65535;
        if (subsystem && (selector.subsystemVendor != device.subsystemVendor
                         || selector.subsystemDevice != device.subsystemDevice)) continue;
        // v0.3 considers a location only when both subsystem IDs are specified.
        const bool location = subsystem && !selector.location.wildcard();
        if (location && !(selector.location == device.location)) continue;
        const unsigned candidate = location ? 5 : subsystem ? 4 : 3;
        if (candidate >= priority) { result = selector.size; priority = candidate; }
        if (location) return result;
    }
    if (priority || !enabled()) return result;
    const auto registered = lookupBarSizeInRegistry(device.deviceID);
    if (registered != BarSizeSelector_None) return static_cast<uint8_t>(registered);
    return (flags & 3) > 1 && isTuringGPU(device.deviceID) ? 5 : Automatic;
}

void Config::setSelector(const Selector &selector) {
    auto found = std::find_if(selectors.begin(), selectors.end(), [&](const auto &value) {
        return sameSelector(value, selector);
    });
    if (found != selectors.end()) *found = selector;
    else {
        require(selectors.size() < 8, "At most eight GPU selectors are supported.");
        selectors.push_back(selector);
    }
}

void Config::clearSelector(const Selector &selector) {
    selectors.erase(std::remove_if(selectors.begin(), selectors.end(), [&](const auto &value) {
        return sameSelector(value, selector);
    }), selectors.end());
}

Config Config::prepared(const std::vector<Device> &devices) const {
    Config result = *this;
    result.gpus.clear();
    result.bridges.clear();
    for (const auto &device : devices) {
        const auto size = selectedSize(device);
        if (size >= Excluded) continue;
        const auto prefix = device.name + " (" + device.location.text() + "): ";
        require(device.discoveryError.empty(), prefix + device.discoveryError);
        require(device.bridge.has_value(), prefix + "Cannot identify the upstream PCI bridge.");
        const auto base = device.bar0Base, length = device.bar0Length;
        require(base && length && base <= UINT64_MAX - (length - 1), prefix + "Cannot identify BAR0.");
        const auto top = base + length - 1;
        require(base < UINT32_MAX && top < UINT32_MAX,
                prefix + "The v0.3 driver does not support BAR0 at a 64-bit address.");
        require(!(base & 15) && !(length & (length - 1)) && base % length == 0,
                prefix + "BAR0 is not naturally aligned.");
        require(device.location.device < 32 && device.location.function < 8,
                prefix + "Invalid GPU PCI location.");
        require(device.bridge->secondaryBus == device.location.bus,
                prefix + "The bridge secondary bus does not match the GPU bus.");
        require(result.gpus.size() < 8, "At most eight GPUs are supported.");
        for (const auto &previous : result.gpus)
            require(!(previous.location == device.location), "Ambiguous GPU PCI location (multiple PCI segments?).");
        result.gpus.push_back({device.deviceID, device.subsystemVendor, device.subsystemDevice,
                              device.location, base, top});
        const auto found = std::find_if(result.bridges.begin(), result.bridges.end(), [&](const auto &bridge) {
            return bridge.secondaryBus == device.location.bus || bridge.location == device.bridge->location;
        });
        if (found == result.bridges.end()) result.bridges.push_back(*device.bridge);
        else require(*found == *device.bridge, prefix + "Ambiguous PCI bridge configuration.");
    }
    // Never silently discard active records for a GPU hidden by the bootloader.
    for (const auto &previous : gpus) {
        Device identity;
        identity.vendorID = 0x10DE;
        identity.deviceID = previous.deviceID;
        identity.subsystemVendor = previous.subsystemVendor;
        identity.subsystemDevice = previous.subsystemDevice;
        identity.location = previous.location;
        if (selectedSize(identity) >= Excluded) continue;
        const auto visible = std::find_if(devices.begin(), devices.end(), [&](const auto &device) {
            return device.vendorID == identity.vendorID && device.deviceID == identity.deviceID
                && device.subsystemVendor == identity.subsystemVendor
                && device.subsystemDevice == identity.subsystemDevice && device.location == identity.location;
        });
        require(visible != devices.end(), "A previously configured GPU is missing or moved. Disable all changes before reconfiguring hardware.");
    }
    const bool selecting = enabled() || std::any_of(selectors.begin(), selectors.end(), [](const auto &s) {
        return s.size <= 10;
    });
    // Explicit exclusions may legitimately leave no active GPU records.
    require(!selecting || !devices.empty(),
            "No selected NVIDIA GPU is visible in macOS. Check whether the bootloader hides it.");
    return result;
}

Bytes Config::encode() const {
    require(validPCISize(pciSize), "Invalid PCI BAR size selector.");
    require((flags & 0xF0) == 0, "Unsupported flags; this utility uses the v0.3 format.");
    require(selectors.size() <= 8 && gpus.size() <= 8 && bridges.size() <= 10,
            "Too many configuration records.");
    Bytes bytes;
    // v0.3 has a two-byte header. v0.4 uses an incompatible eleven-byte header.
    append(bytes, pciSize, 1);
    append(bytes, flags, 1);
    append(bytes, selectors.size(), 1);
    for (const auto &selector : selectors) {
        require(selector.size <= 10 || selector.size >= Excluded, "Invalid GPU BAR size selector.");
        append(bytes, selector.deviceID, 2);
        append(bytes, selector.subsystemVendor, 2);
        append(bytes, selector.subsystemDevice, 2);
        appendLocation(bytes, selector.location);
        append(bytes, selector.size, 1);
        append(bytes, selector.overrideMask, 1);
    }
    append(bytes, gpus.size(), 1);
    for (const auto &gpu : gpus) {
        append(bytes, gpu.deviceID, 2);
        append(bytes, gpu.subsystemVendor, 2);
        append(bytes, gpu.subsystemDevice, 2);
        appendLocation(bytes, gpu.location);
        append(bytes, gpu.base, 8);
        append(bytes, gpu.top, 8);
    }
    append(bytes, bridges.size(), 1);
    for (const auto &bridge : bridges) {
        append(bytes, bridge.vendorID, 2);
        append(bytes, bridge.deviceID, 2);
        appendLocation(bytes, bridge.location);
        append(bytes, bridge.secondaryBus, 1);
    }
    return bytes;
}

Config Config::decode(const Bytes &bytes) {
    if (bytes.empty()) return {};
    Reader reader(bytes);
    Config result;
    result.pciSize = reader.byte();
    result.flags = reader.byte();
    const auto selectors = reader.count(8);
    for (size_t i = 0; i < selectors; ++i) {
        Selector selector;
        selector.deviceID = reader.word();
        selector.subsystemVendor = reader.word();
        selector.subsystemDevice = reader.word();
        selector.location = reader.location(true);
        selector.size = reader.byte();
        selector.overrideMask = reader.byte();
        result.selectors.push_back(selector);
    }
    const auto gpus = reader.count(8);
    for (size_t i = 0; i < gpus; ++i) {
        GPUConfig gpu;
        gpu.deviceID = reader.word();
        gpu.subsystemVendor = reader.word();
        gpu.subsystemDevice = reader.word();
        gpu.location = reader.location(false);
        gpu.base = reader.take(8);
        gpu.top = reader.take(8);
        result.gpus.push_back(gpu);
    }
    const auto bridges = reader.count(10);
    for (size_t i = 0; i < bridges; ++i) {
        Bridge bridge;
        bridge.vendorID = reader.word();
        bridge.deviceID = reader.word();
        bridge.location = reader.location(true);
        bridge.secondaryBus = reader.byte();
        result.bridges.push_back(bridge);
    }
    require(reader.done(), "Unrecognized configuration length. Expected v0.3; refusing to reinterpret a newer format.");
    require(result.encode() == bytes, "Configuration does not use the canonical v0.3 format.");
    return result;
}

void parseAssignedAddresses(Device &device, const Bytes &bytes) {
    require(!bytes.empty() && bytes.size() % 20 == 0, "Missing or malformed assigned-addresses property.");
    device.bar0Base = device.bar0Length = device.currentBARSize = 0;
    bool found = false;
    // Intel macOS stores IOPCIPhysicalAddress cells in native little-endian order.
    for (size_t i = 0; i < bytes.size(); i += 20) {
        const auto high = readLE(bytes, i, 4);
        const Location location{static_cast<uint8_t>(high >> 16),
                                static_cast<uint8_t>((high >> 11) & 31),
                                static_cast<uint8_t>((high >> 8) & 7)};
        require(location == device.location, "PCI resource location does not match the GPU.");
        const auto space = (high >> 24) & 3;
        if (space != 2 && space != 3) continue;
        const auto base = (readLE(bytes, i + 4, 4) << 32) | readLE(bytes, i + 8, 4);
        const auto length = (readLE(bytes, i + 12, 4) << 32) | readLE(bytes, i + 16, 4);
        const auto bar = high & 255;
        if (bar >= 0x10 && bar <= 0x24) device.currentBARSize = std::max(device.currentBARSize, length);
        if (bar == 0x10) {
            require(!found, "Multiple BAR0 resources were found.");
            device.bar0Base = base;
            device.bar0Length = length;
            found = true;
        }
    }
    require(found && device.bar0Base && device.bar0Length, "No usable memory BAR0 was found.");
}

std::string describeStatus(const std::optional<Bytes> &bytes) {
    if (!bytes) return "status variable unavailable (driver not loaded, or not exposed by macOS)";
    require(bytes->size() == 8, "The driver status variable must contain eight bytes.");
    const auto value = readLE(*bytes, 0, 8);
    const auto code = static_cast<uint32_t>(value);
    const char *description = "unknown status";
    switch (code) {
        case 10: description = "not loaded"; break;
        case 20: description = "configured"; break;
        case 30: description = "GPU unconfigured"; break;
        case 40: description = "unconfigured"; break;
        case 50: description = "configuration cleared"; break;
        case 60: description = "PCI bridge found"; break;
        case 70: description = "GPU found"; break;
        case 80: description = "GPU straps configured"; break;
        case 90: description = "GPU straps preconfigured"; break;
        case 100: description = "GPU straps confirmed"; break;
        case 110: description = "GPU delay elapsed"; break;
        case 120: description = "GPU Resizable BAR configured"; break;
        case 130: description = "GPU straps not confirmed"; break;
        case 135: description = "GPU BAR size mask overridden"; break;
        case 140: description = "GPU has no Resizable BAR capability"; break;
        case 150: description = "GPU excluded"; break;
        case 159: description = "missing bridge configuration"; break;
        case 160: description = "bad bridge configuration"; break;
        case 161: description = "bridge not enumerated"; break;
        case 162: description = "missing GPU configuration"; break;
        case 163: description = "bad GPU configuration"; break;
        case 170: description = "EFI allocation error"; break;
        case 180: description = "internal EFI error"; break;
        case 190: description = "firmware variable API error"; break;
        case 200: description = "configuration parse error"; break;
    }
    std::string result = std::string(description) + " (0x" + hex(value, 16) + ")";
    if (code == 180) result += "; EFI error " + std::to_string((value >> 32) & 255)
        + ", v0.3 error location " + std::to_string((value >> 40) & 255);
    return result;
}

} // namespace nvstraps
