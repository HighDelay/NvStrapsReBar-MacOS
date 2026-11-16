#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nvstraps {

using Bytes = std::vector<uint8_t>;
inline constexpr const char *VariableGUID = "e3ee4a27-e2a2-4435-bba3-184ccad935a8";
inline constexpr const char *ConfigName = "NvStrapsReBar";
inline constexpr const char *StatusName = "NvStrapsReBarStatus";
inline constexpr uint8_t Excluded = 254, Automatic = 255;

struct Location {
    uint8_t bus = 255, device = 255, function = 255;
    bool operator==(const Location &other) const;
    bool wildcard() const;
    std::string text() const;
};

struct Selector {
    uint16_t deviceID = 0, subsystemVendor = 65535, subsystemDevice = 65535;
    Location location;
    uint8_t size = Automatic, overrideMask = 0;
};

struct GPUConfig {
    uint16_t deviceID = 0, subsystemVendor = 0, subsystemDevice = 0;
    Location location;
    uint64_t base = 0, top = 0; // BAR0; top is inclusive, as in the Windows utility.
};

struct Bridge {
    uint16_t vendorID = 0, deviceID = 0;
    Location location;
    uint8_t secondaryBus = 0;
    bool operator==(const Bridge &other) const;
};

struct Device {
    uint16_t vendorID = 0, deviceID = 0, subsystemVendor = 0, subsystemDevice = 0;
    Location location;
    std::string name, discoveryError;
    uint64_t bar0Base = 0, bar0Length = 0, currentBARSize = 0;
    std::optional<Bridge> bridge;
};

struct Config {
    uint8_t pciSize = 0, flags = 0;
    std::vector<Selector> selectors;
    std::vector<GPUConfig> gpus;
    std::vector<Bridge> bridges;

    bool enabled() const;
    bool configured() const;
    void setEnabled(bool enabled);
    uint8_t selectedSize(const Device &device) const;
    void setSelector(const Selector &selector);
    void clearSelector(const Selector &selector);
    Config prepared(const std::vector<Device> &devices) const;
    Bytes encode() const;
    static Config decode(const Bytes &bytes);
};

bool validPCISize(unsigned value);
std::string gpuSizeText(unsigned value);
std::string pciSizeText(unsigned value);
std::string hex(uint64_t value, unsigned width = 0);
uint64_t readLE(const Bytes &bytes, size_t offset, size_t length);
void parseAssignedAddresses(Device &device, const Bytes &bytes);
std::string describeStatus(const std::optional<Bytes> &bytes);

} // namespace nvstraps
