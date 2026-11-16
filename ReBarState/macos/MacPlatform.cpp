#include "MacPlatform.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_error.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

#if !defined(__x86_64__)
#error NvStrapsReBar requires Intel macOS and an NVIDIA PCI GPU.
#endif

namespace nvstraps {
namespace {

class IOObject {
public:
    io_object_t value = IO_OBJECT_NULL;
    explicit IOObject(io_object_t object = IO_OBJECT_NULL) : value(object) {}
    ~IOObject() { if (value) IOObjectRelease(value); }
    IOObject(const IOObject &) = delete;
    IOObject &operator=(const IOObject &) = delete;
};

class CFObject {
public:
    CFTypeRef value;
    explicit CFObject(CFTypeRef object) : value(object) {}
    ~CFObject() { if (value) CFRelease(value); }
    CFObject(const CFObject &) = delete;
    CFObject &operator=(const CFObject &) = delete;
};

CFStringRef makeString(const std::string &value) {
    const auto result = CFStringCreateWithCString(kCFAllocatorDefault, value.c_str(), kCFStringEncodingUTF8);
    if (!result) throw std::runtime_error("Cannot allocate a firmware property name.");
    return result;
}

std::optional<Bytes> dataProperty(io_registry_entry_t entry, CFStringRef key) {
    CFObject value(IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0));
    if (!value.value) return std::nullopt;
    if (CFGetTypeID(value.value) != CFDataGetTypeID())
        throw std::runtime_error("Expected a binary firmware or PCI property.");
    const auto data = static_cast<CFDataRef>(value.value);
    const auto length = CFDataGetLength(data);
    if (length > 65536) throw std::runtime_error("Unexpectedly large firmware or PCI property.");
    if (length == 0) return Bytes{};
    const auto begin = CFDataGetBytePtr(data);
    return Bytes(begin, begin + length);
}

uint32_t integerProperty(io_registry_entry_t entry, CFStringRef key) {
    const auto data = dataProperty(entry, key);
    if (!data || data->size() != 4) throw std::runtime_error("Missing or malformed PCI identity property.");
    return static_cast<uint32_t>(readLE(*data, 0, 4));
}

Location locationProperty(io_registry_entry_t entry) {
    const auto data = dataProperty(entry, CFSTR("reg"));
    if (!data || data->size() < 20 || data->size() % 20)
        throw std::runtime_error("Missing or malformed PCI reg property.");
    const auto address = readLE(*data, 0, 4);
    if ((address & 0x030000FF) != 0) throw std::runtime_error("Unexpected PCI configuration address.");
    return {static_cast<uint8_t>(address >> 16), static_cast<uint8_t>((address >> 11) & 31),
            static_cast<uint8_t>((address >> 8) & 7)};
}

std::string deviceName(io_registry_entry_t entry, uint16_t deviceID) {
    const auto data = dataProperty(entry, CFSTR("model"));
    if (data && !data->empty()) {
        std::string name(data->begin(), data->end());
        name.erase(std::find(name.begin(), name.end(), '\0'), name.end());
        for (auto &ch : name) if (static_cast<unsigned char>(ch) < 32 || ch == 127) ch = '?';
        if (!name.empty()) return name;
    }
    return "NVIDIA GPU 10DE:" + hex(deviceID, 4);
}

std::optional<Bridge> parentBridge(io_registry_entry_t entry, uint8_t secondaryBus) {
    io_registry_entry_t current;
    if (IORegistryEntryGetParentEntry(entry, kIOServicePlane, &current) != KERN_SUCCESS) return std::nullopt;
    for (unsigned depth = 0; current && depth < 64; ++depth) {
        IOObject parent(current);
        if (IOObjectConformsTo(current, "IOPCIDevice")) {
            if ((integerProperty(current, CFSTR("class-code")) >> 8) != 0x0604)
                throw std::runtime_error("The nearest parent PCI device is not a PCI bridge.");
            return Bridge{static_cast<uint16_t>(integerProperty(current, CFSTR("vendor-id"))),
                          static_cast<uint16_t>(integerProperty(current, CFSTR("device-id"))),
                          locationProperty(current), secondaryBus};
        }
        io_registry_entry_t next;
        if (IORegistryEntryGetParentEntry(current, kIOServicePlane, &next) != KERN_SUCCESS) return std::nullopt;
        current = next;
    }
    if (current) IOObjectRelease(current);
    return std::nullopt;
}

void memoryRanges(CFTypeRef value, std::vector<std::pair<uint64_t, uint64_t>> &ranges, unsigned depth = 0) {
    if (!value || depth > 8) return;
    if (CFGetTypeID(value) == CFArrayGetTypeID()) {
        const auto array = static_cast<CFArrayRef>(value);
        for (CFIndex i = 0; i < CFArrayGetCount(array); ++i)
            memoryRanges(CFArrayGetValueAtIndex(array, i), ranges, depth + 1);
    } else if (CFGetTypeID(value) == CFDictionaryGetTypeID()) {
        const auto dictionary = static_cast<CFDictionaryRef>(value);
        const auto address = CFDictionaryGetValue(dictionary, CFSTR("address"));
        const auto length = CFDictionaryGetValue(dictionary, CFSTR("length"));
        int64_t base = 0, size = 0;
        if (address && length && CFGetTypeID(address) == CFNumberGetTypeID()
            && CFGetTypeID(length) == CFNumberGetTypeID()
            && CFNumberGetValue(static_cast<CFNumberRef>(address), kCFNumberSInt64Type, &base)
            && CFNumberGetValue(static_cast<CFNumberRef>(length), kCFNumberSInt64Type, &size)
            && base >= 0 && size > 0)
            ranges.emplace_back(static_cast<uint64_t>(base), static_cast<uint64_t>(size));
    }
}

void refreshMemoryResources(io_registry_entry_t entry, Device &device) {
    CFObject memory(IORegistryEntryCreateCFProperty(entry, CFSTR("IODeviceMemory"), kCFAllocatorDefault, 0));
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    memoryRanges(memory.value, ranges);
    if (ranges.empty()) return; // Unbound devices may expose only assigned-addresses.
    bool foundBAR0 = false;
    uint64_t largest = 0;
    for (const auto &[base, length] : ranges) {
        largest = std::max(largest, length);
        if (base == device.bar0Base) {
            if (foundBAR0) throw std::runtime_error("Ambiguous BAR0 in current IOKit memory resources.");
            device.bar0Length = length;
            foundBAR0 = true;
        }
    }
    if (!foundBAR0) throw std::runtime_error("Current IOKit resources do not match the firmware BAR0 address.");
    // A GPU driver can resize BAR1 after boot, leaving assigned-addresses stale.
    device.currentBARSize = largest;
}

void checkIO(kern_return_t result, const std::string &operation) {
    if (result == KERN_SUCCESS) return;
    throw std::runtime_error(operation + ": " + mach_error_string(result) + " (0x"
        + hex(static_cast<uint32_t>(result)) + "). macOS/bootloader NVRAM permissions must allow this GUID.");
}

void writeDescriptor(int descriptor, const Bytes &bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "Writing backup");
        offset += static_cast<size_t>(count);
    }
    if (fsync(descriptor) != 0) throw std::system_error(errno, std::generic_category(), "Flushing backup");
}

void finishFile(int descriptor, const std::filesystem::path &path, const Bytes &bytes) {
    try {
        writeDescriptor(descriptor, bytes);
    } catch (...) {
        close(descriptor);
        unlink(path.c_str());
        throw;
    }
    if (close(descriptor) != 0) throw std::system_error(errno, std::generic_category(), "Closing backup");
}

} // namespace

MacPlatform::MacPlatform() {
    options = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/options");
    if (!options) throw std::runtime_error("macOS does not expose an EFI NVRAM options service.");
}

MacPlatform::~MacPlatform() { if (options) IOObjectRelease(options); }

std::optional<Bytes> MacPlatform::read(const char *name) const {
    CFObject key(makeString(std::string(VariableGUID) + ":" + name));
    return dataProperty(options, static_cast<CFStringRef>(key.value));
}

std::vector<Device> MacPlatform::devices() const {
    io_iterator_t iterator;
    checkIO(IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOPCIDevice"), &iterator),
            "Listing PCI devices");
    IOObject iteratorGuard(iterator);
    std::vector<Device> result;
    while (const auto entry = IOIteratorNext(iterator)) {
        IOObject entryGuard(entry);
        const auto vendor = integerProperty(entry, CFSTR("vendor-id"));
        const auto classCode = integerProperty(entry, CFSTR("class-code"));
        if (vendor != 0x10DE || (classCode >> 16) != 3) continue;
        Device device;
        device.vendorID = static_cast<uint16_t>(vendor);
        device.deviceID = static_cast<uint16_t>(integerProperty(entry, CFSTR("device-id")));
        device.name = deviceName(entry, device.deviceID);
        try {
            device.subsystemVendor = static_cast<uint16_t>(integerProperty(entry, CFSTR("subsystem-vendor-id")));
            device.subsystemDevice = static_cast<uint16_t>(integerProperty(entry, CFSTR("subsystem-id")));
            device.location = locationProperty(entry);
            const auto assigned = dataProperty(entry, CFSTR("assigned-addresses"));
            parseAssignedAddresses(device, assigned.value_or(Bytes{}));
            refreshMemoryResources(entry, device);
            device.bridge = parentBridge(entry, device.location.bus);
        } catch (const std::exception &error) {
            device.discoveryError = error.what();
        }
        result.push_back(device);
    }
    if (!IOIteratorIsValid(iterator)) throw std::runtime_error("PCI devices changed during discovery; try again.");
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.location.text() < right.location.text();
    });
    return result;
}

void MacPlatform::write(const std::optional<Bytes> &expected, const std::optional<Bytes> &value) const {
    if (geteuid() != 0) throw std::runtime_error("Saving requires root. Run this command again with sudo.");
    if (read(ConfigName) != expected)
        throw std::runtime_error("The firmware configuration changed since it was loaded. Reload before saving.");
    CFObject key(makeString(std::string(VariableGUID) + ":" + ConfigName));
    if (value) {
        CFObject data(CFDataCreate(kCFAllocatorDefault, value->data(), static_cast<CFIndex>(value->size())));
        if (!data.value) throw std::runtime_error("Cannot allocate firmware configuration data.");
        checkIO(IORegistryEntrySetCFProperty(options, static_cast<CFStringRef>(key.value), data.value),
                "Saving NvStrapsReBar");
    } else if (expected) {
        checkIO(IORegistryEntrySetCFProperty(options, CFSTR("IONVRAM-DELETE-PROPERTY"), key.value),
                "Deleting NvStrapsReBar configuration");
    }
    if (read(ConfigName) != value)
        throw std::runtime_error("Firmware readback did not match the requested data. Keep the backup and inspect NVRAM before rebooting.");
    // Same sync request used by Apple's nvram command; some firmware backends do not implement it.
    const auto sync = IORegistryEntrySetCFProperty(options, CFSTR("IONVRAM-SYNCNOW-PROPERTY"),
                                                 CFSTR("IONVRAM-SYNCNOW-PROPERTY"));
    if (sync != KERN_SUCCESS)
        std::cerr << "NVRAM readback verified; sync request returned " << mach_error_string(sync)
                  << ". Persistence across reboot is not verified.\n";
}

Bytes readFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot open " + path.string());
    const auto length = stream.tellg();
    if (length < 0 || length > 347) throw std::runtime_error("Not a v0.3 configuration backup (maximum 347 bytes).");
    Bytes bytes(static_cast<size_t>(length));
    stream.seekg(0);
    if (length && !stream.read(reinterpret_cast<char *>(bytes.data()), length))
        throw std::runtime_error("Cannot read " + path.string());
    return bytes;
}

void exportFile(const std::filesystem::path &path, const Bytes &bytes) {
    const auto descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "Creating " + path.string());
    finishFile(descriptor, path, bytes);
}

std::filesystem::path backupFile(const std::filesystem::path &directory, const Bytes &bytes) {
    std::filesystem::create_directories(directory);
    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char timestamp[32];
    if (!std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local))
        throw std::runtime_error("Cannot generate backup filename.");
    auto pattern = (std::filesystem::absolute(directory)
        / (std::string("NvStrapsReBar-v0.3-") + timestamp + "-XXXXXX.bin")).string();
    const auto descriptor = mkstemps(pattern.data(), 4);
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "Creating firmware backup");
    finishFile(descriptor, pattern, bytes);
    return pattern;
}

} // namespace nvstraps
