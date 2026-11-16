#include "Config.hpp"

#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

using namespace nvstraps;

namespace {

unsigned checks = 0;

void check(bool value, const char *message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

void rejects(const std::function<void()> &operation, const char *message) {
    try { operation(); }
    catch (const std::exception &) { ++checks; return; }
    throw std::runtime_error(message);
}

Bytes bytes(const std::string &text) {
    std::istringstream stream(text);
    unsigned value;
    Bytes result;
    while (stream >> std::hex >> value) result.push_back(static_cast<uint8_t>(value));
    return result;
}

Device turing() {
    Device device;
    device.vendorID = 0x10DE;
    device.deviceID = 0x1F06;
    device.subsystemVendor = 0x1043;
    device.subsystemDevice = 0x86AE;
    device.location = {1, 0, 0};
    device.name = "Test RTX 2060 SUPER";
    device.bar0Base = 0xA0000000;
    device.bar0Length = 0x1000000;
    device.bridge = Bridge{0x8086, 0x4C01, {0, 1, 0}, 1};
    return device;
}

void goldenProtocol() {
    // Independently laid out using v0.3 NvStrapsConfig_Save/GPUConfig_pack/BridgeConfig_pack.
    const auto fixture = bytes(
        "00 02 00 01 06 1F 43 10 AE 86 01 00 "
        "00 00 00 A0 00 00 00 00 FF FF FF A0 00 00 00 00 "
        "01 86 80 01 4C 00 08 01");
    Config config;
    config.setEnabled(true);
    const auto prepared = config.prepared({turing()});
    check(fixture.size() == 36, "Golden fixture length");
    check(prepared.encode() == fixture, "v0.3 serialization differs from golden bytes");
    const auto decoded = Config::decode(fixture);
    check(decoded.encode() == fixture, "v0.3 fixture did not roundtrip");
    check(decoded.gpus.at(0).base == 0xA0000000 && decoded.gpus.at(0).top == 0xA0FFFFFF,
          "BAR0 address unpacking");
    check(decoded.bridges.at(0).location == Location{0, 1, 0}, "Bridge BDF unpacking");
    check(decoded.flags == 2 && decoded.selectedSize(turing()) == 7, "Upstream E behavior");

    Config selection;
    selection.setSelector({0x1F06, 65535, 65535, {}, 7, 1});
    const auto selectorFixture = bytes("00 00 01 06 1F FF FF FF FF FF FF 07 01 00 00");
    check(selection.encode() == selectorFixture, "Wildcard selector serialization");
    check(Config::decode(selectorFixture).selectors.at(0).location.wildcard(), "Wildcard selector decoding");
    check(Config{}.encode() == bytes("00 00 00 00 00"), "Empty configuration header");
    check(!Config{}.configured() && !Config::decode({}).configured(), "Missing configuration is disabled");

    for (size_t length = 1; length < fixture.size(); ++length)
        rejects([&] { Config::decode(Bytes(fixture.begin(), fixture.begin() + length)); }, "Truncated payload accepted");
    auto trailing = fixture;
    trailing.push_back(0);
    rejects([&] { Config::decode(trailing); }, "Trailing bytes accepted");
    rejects([&] { Config::decode(Bytes(14, 0)); }, "v0.4 header accepted as v0.3");
    rejects([&] { Config::decode(bytes("00 02 09 00 00")); }, "Oversized selector count accepted");
    rejects([&] { Config::decode(bytes("00 02 00 09 00")); }, "Oversized GPU count accepted");
    rejects([&] { Config::decode(bytes("00 02 00 00 0B")); }, "Oversized bridge count accepted");
    rejects([&] { Config::decode(bytes("21 02 00 00 00")); }, "Invalid PCI size accepted");
    rejects([&] { Config::decode(bytes("00 82 00 00 00")); }, "Unknown format flags accepted");
}

void selectionPriority() {
    auto device = turing();
    Config config;
    check(config.selectedSize(device) == Automatic, "Disabled GPU has an implicit size");
    config.setEnabled(true);
    check(config.selectedSize(device) == 7, "Turing device registry lookup");
    device.deviceID = 0x1F7F;
    check(config.selectedSize(device) == 5, "Unknown Turing GPU fallback must be 2 GiB");
    device.deviceID = 0x1E30;
    check(config.selectedSize(device) == Excluded, "Upstream excluded GPU must stay excluded");
    device.deviceID = 0x2206;
    check(config.selectedSize(device) == Automatic, "Non-Turing GPU auto-configured");
    device = turing();
    config.setSelector({device.deviceID, 65535, 65535, {}, 5, 0});
    check(config.selectedSize(device) == 5, "PCI ID selection");
    config.setSelector({device.deviceID, device.subsystemVendor, device.subsystemDevice, {}, 6, 0});
    check(config.selectedSize(device) == 6, "Subsystem must override device ID");
    config.setSelector({device.deviceID, device.subsystemVendor, device.subsystemDevice, device.location, 8, 0});
    check(config.selectedSize(device) == 8, "Location must override subsystem");
    auto other = device;
    other.location.bus = 2;
    check(config.selectedSize(other) == 6, "Location selector applied to a different GPU");
    config.setSelector({device.deviceID, device.subsystemVendor, device.subsystemDevice, device.location, Excluded, 255});
    check(config.selectedSize(device) == Excluded, "Per-GPU exclusion lost");
    config.clearSelector({device.deviceID, device.subsystemVendor, device.subsystemDevice, device.location});
    check(config.selectedSize(device) == 6, "Clearing location selector should reveal subsystem selection");
    config.flags = 12;
    config.setEnabled(true);
    check(config.flags == 14, "Enable must preserve skip-S3 and override flags");
    config.setEnabled(false);
    check(config.flags == 12, "Disable auto must preserve other flags");
    device.vendorID = 0x1002;
    check(config.selectedSize(device) == Automatic, "Non-NVIDIA GPU selected");
}

void hardwareValidation() {
    Config config;
    config.setEnabled(true);
    const auto device = turing();
    auto invalid = device;
    invalid.bar0Base = 0x100000000;
    rejects([&] { config.prepared({invalid}); }, "64-bit BAR0 accepted");
    invalid = device;
    invalid.bar0Length = 0;
    rejects([&] { config.prepared({invalid}); }, "Zero-length BAR0 accepted");
    invalid = device;
    invalid.bar0Length = 0x3000000;
    rejects([&] { config.prepared({invalid}); }, "Non-power-of-two BAR0 accepted");
    invalid = device;
    invalid.bar0Base += 16;
    rejects([&] { config.prepared({invalid}); }, "Misaligned BAR0 accepted");
    invalid = device;
    invalid.bar0Base = UINT64_MAX;
    rejects([&] { config.prepared({invalid}); }, "BAR0 arithmetic overflow accepted");
    invalid = device;
    invalid.bridge.reset();
    rejects([&] { config.prepared({invalid}); }, "Missing bridge accepted");
    invalid = device;
    invalid.bridge->secondaryBus = 2;
    rejects([&] { config.prepared({invalid}); }, "Incorrect secondary bus accepted");
    invalid = device;
    invalid.discoveryError = "Incomplete hardware data";
    rejects([&] { config.prepared({invalid}); }, "Hardware discovery failure ignored");
    rejects([&] { config.prepared({}); }, "Enabling a hidden GPU accepted");
    rejects([&] { config.prepared({device, device}); }, "Duplicate GPU BDF accepted");
    auto second = device;
    second.location.device = 1;
    check(config.prepared({device, second}).bridges.size() == 1, "Shared PCI bridge not deduplicated");
    second.bridge->deviceID++;
    rejects([&] { config.prepared({device, second}); }, "Conflicting bridge identities accepted");
    const auto old = config.prepared({device});
    auto excluded = old;
    excluded.setSelector({device.deviceID, 65535, 65535, {}, Excluded, 0});
    check(excluded.prepared({device}).gpus.empty(), "Excluding every GPU should remove its active records");
    auto moved = device;
    moved.location.bus = 2;
    moved.bridge->secondaryBus = 2;
    rejects([&] { old.prepared({moved}); }, "A moved or hidden configured GPU was silently dropped");
    check(Config{}.prepared({}).gpus.empty(), "Disabling should work without a visible GPU");
    auto stale = old;
    stale.gpus[0].base = 0xB0000000;
    check(stale.prepared({device}).gpus[0].base == device.bar0Base, "Stale BAR0 was not refreshed");

    auto parsed = device;
    const auto addresses = bytes(
        "10 00 01 82 00 00 00 00 00 00 00 A0 00 00 00 00 00 00 00 01 "
        "14 00 01 C2 7C 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00 "
        "24 00 01 81 00 00 00 00 00 30 00 00 00 00 00 00 80 00 00 00");
    parseAssignedAddresses(parsed, addresses);
    check(parsed.bar0Base == 0xA0000000 && parsed.bar0Length == 0x1000000, "Intel PCI cells decoded incorrectly");
    check(parsed.currentBARSize == uint64_t(8) << 30, "64-bit BAR length decoding");
    rejects([&] { parseAssignedAddresses(parsed, {}); }, "Missing PCI resources accepted");
    rejects([&] { parseAssignedAddresses(parsed, Bytes{0}); }, "Partial PCI record accepted");
    auto wrong = addresses;
    wrong[2] = 2;
    rejects([&] { parseAssignedAddresses(parsed, wrong); }, "Wrong GPU resource BDF accepted");
    auto duplicate = addresses;
    duplicate.insert(duplicate.end(), addresses.begin(), addresses.begin() + 20);
    rejects([&] { parseAssignedAddresses(parsed, duplicate); }, "Duplicate BAR0 accepted");
}

void limitsAndMalformedInput() {
    Config maximum;
    maximum.setEnabled(true);
    for (unsigned i = 0; i < 8; ++i) {
        maximum.selectors.push_back({static_cast<uint16_t>(i), 65535, 65535, {}, Automatic, 0});
        maximum.gpus.push_back({static_cast<uint16_t>(i), 1, 2, {1, static_cast<uint8_t>(i), 0}, 0xA0000000, 0xA0FFFFFF});
    }
    for (unsigned i = 0; i < 10; ++i)
        maximum.bridges.push_back({0x8086, 1, {0, static_cast<uint8_t>(i), 0}, 1});
    check(maximum.encode().size() == 347, "Maximum v0.3 payload size");
    check(Config::decode(maximum.encode()).encode() == maximum.encode(), "Maximum payload roundtrip");
    rejects([&] { maximum.setSelector({99, 65535, 65535, {}, Automatic, 0}); }, "Ninth selector accepted");
    check(validPCISize(0) && validPCISize(32) && validPCISize(64) && validPCISize(65), "Valid PCI selector rejected");
    check(!validPCISize(33) && !validPCISize(63) && !validPCISize(66), "Invalid PCI selector allowed");
    check(pciSizeText(12) == "maximum 4 GiB", "PCI selector conversion");
    check(gpuSizeText(6) == "4 GiB", "GPU selector conversion");
    check(describeStatus(bytes("64 00 00 00 00 00 00 01")).find("GPU straps confirmed") != std::string::npos,
          "Driver status decoding");
    rejects([&] { describeStatus(Bytes{100}); }, "Short status accepted");

    std::mt19937 random(0x4E565242);
    for (unsigned i = 0; i < 10000; ++i) {
        Bytes sample(random() % 360);
        for (auto &value : sample) value = static_cast<uint8_t>(random());
        std::optional<Config> decoded;
        try { decoded = Config::decode(sample); }
        catch (const std::runtime_error &) { ++checks; continue; }
        check(sample.empty() || decoded->encode() == sample, "Accepted payload did not roundtrip");
    }
}

} // namespace

int main() {
    try {
        goldenProtocol();
        selectionPriority();
        hardwareValidation();
        limitsAndMalformedInput();
        std::cout << checks << " configuration, hardware, and malformed-input checks passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
