#include "Config.hpp"
#include "MacPlatform.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>

using namespace nvstraps;
namespace fs = std::filesystem;

namespace {

void help() {
    std::cout << R"(NvStrapsReBar for Intel macOS — upstream v0.3 firmware format

Usage: NvStrapsReBar [options]
  --show                     Show saved settings and detected NVIDIA GPUs
  --interactive              Open the configuration menu (default in a Terminal)
  --enable                   Enable automatic Turing GPU sizes (upstream E)
  --disable                  Disable all GPU and PCI changes; remove configuration
  --pci-size N               PCI selector: 0..32, 64, or 65 (upstream P)
  --gpu-size INDEX SIZE      GPU selector: 0..10, auto, exclude, or clear
  --gpu-mask INDEX on|off    Override the BAR size mask for one GPU
  --scope pci|subsystem|location   Scope for --gpu-size (default: pci)
  --clear-selectors          Clear all per-GPU selections
  --skip-s3 on|off           Skip GPU programming on S3 resume
  --override-mask on|off     Override the global GPU BAR size mask
  --save                     Save changes to NVRAM; requires sudo
  --export FILE              Export the current variable as a binary backup
  --restore FILE             Preview restoring a binary backup; add --save to apply
  --inspect FILE             Inspect a backup without accessing firmware
  --backup-dir DIR           Automatic backups (default: ./NvStrapsReBar-backups)
  --help                     Show this help
  --version                  Show the supported firmware version

Changes on the command line are previews until --save is supplied.
GPU indices start at 1. GPU selectors: 0=64 MiB, 1=128 MiB, 2=256 MiB,
3=512 MiB, 4=1 GiB, 5=2 GiB, 6=4 GiB, 7=8 GiB, 8=16 GiB,
9=32 GiB, 10=64 GiB. PCI selectors use different values; see the README.

Examples:
  NvStrapsReBar --show
  NvStrapsReBar --enable --pci-size 64
  sudo NvStrapsReBar --enable --pci-size 64 --save

The NvStrapsReBar.ffs v0.3 driver must already be installed in motherboard
firmware. Saved changes take effect after a reboot.
)";
}

unsigned number(const std::string &text, unsigned maximum) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; }))
        throw std::runtime_error("Expected a decimal number, received: " + text);
    unsigned result = 0;
    for (const auto ch : text) {
        if (result > maximum / 10 || (result == maximum / 10 && unsigned(ch - '0') > maximum % 10))
            throw std::runtime_error("Number out of range: " + text);
        result = result * 10 + unsigned(ch - '0');
    }
    return result;
}

bool onOff(const std::string &text) {
    if (text == "on") return true;
    if (text == "off") return false;
    throw std::runtime_error("Expected on or off, received: " + text);
}

std::string memoryText(uint64_t bytes) {
    if (bytes && bytes % (uint64_t(1) << 30) == 0) return std::to_string(bytes >> 30) + " GiB";
    if (bytes && bytes % (uint64_t(1) << 20) == 0) return std::to_string(bytes >> 20) + " MiB";
    return std::to_string(bytes) + " bytes";
}

void showConfig(const Config &config) {
    std::cout << "Automatic Turing sizes: " << (config.enabled() ? "enabled" : "disabled")
              << "\nTarget PCI BAR size: " << unsigned(config.pciSize) << " / " << pciSizeText(config.pciSize)
              << "\nSkip S3 resume: " << ((config.flags & 4) ? "yes" : "no")
              << "\nOverride BAR size mask: " << ((config.flags & 8) ? "yes" : "no")
              << "\nPer-GPU selectors: " << config.selectors.size() << '\n';
    for (const auto &selector : config.selectors) {
        std::cout << "  10DE:" << hex(selector.deviceID, 4);
        if (selector.subsystemVendor != 65535 && selector.subsystemDevice != 65535)
            std::cout << " / " << hex(selector.subsystemVendor, 4) << ':' << hex(selector.subsystemDevice, 4);
        std::cout << " / " << selector.location.text() << " -> " << gpuSizeText(selector.size);
        if (selector.overrideMask) std::cout << " / mask override " << (selector.overrideMask == 255 ? "off" : "on");
        std::cout << '\n';
    }
    std::cout << "Recorded GPUs: " << config.gpus.size() << "; PCI bridges: " << config.bridges.size() << '\n';
    for (const auto &gpu : config.gpus)
        std::cout << "  " << gpu.location.text() << " BAR0 0x" << hex(gpu.base) << "..0x" << hex(gpu.top) << '\n';
}

void showDevices(const Config &config, const std::vector<Device> &devices) {
    if (devices.empty()) std::cout << "No NVIDIA display GPU is visible in macOS.\n";
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto &device = devices[i];
        std::cout << "\nGPU " << i + 1 << ": " << device.name << "\n  PCI " << device.location.text()
                  << " / " << hex(device.vendorID, 4) << ':' << hex(device.deviceID, 4)
                  << " / subsystem " << hex(device.subsystemVendor, 4) << ':' << hex(device.subsystemDevice, 4)
                  << "\n  Largest current memory BAR: " << memoryText(device.currentBARSize)
                  << "\n  Requested GPU BAR: " << gpuSizeText(config.selectedSize(device))
                  << "\n  BAR0: 0x" << hex(device.bar0Base) << " / " << memoryText(device.bar0Length) << '\n';
        if (device.bridge)
            std::cout << "  Upstream bridge: " << device.bridge->location.text() << " / "
                      << hex(device.bridge->vendorID, 4) << ':' << hex(device.bridge->deviceID, 4) << '\n';
        if (!device.discoveryError.empty()) std::cout << "  Configuration unavailable: " << device.discoveryError << '\n';
    }
}

void preview(const Config &config, const std::vector<Device> &devices) {
    const auto candidate = config.prepared(devices);
    std::cout << "\nPreview for next boot (v0.3):\n";
    showConfig(candidate);
    if (!candidate.configured()) std::cout << "Save will remove the NvStrapsReBar configuration variable.\n";
    else std::cout << "Configuration payload: " << candidate.encode().size() << " bytes.\n";
    std::cout << "Firmware has not been changed.\n";
}

void commit(MacPlatform &platform, std::optional<Bytes> &original, const std::optional<Bytes> &value,
            const fs::path &directory) {
    if (original == value) {
        std::cout << "The requested configuration already matches NVRAM.\n";
        return;
    }
    if (geteuid() != 0) throw std::runtime_error("Saving requires root. Run the utility with sudo, or use the .command launcher.");
    if (platform.read(ConfigName) != original)
        throw std::runtime_error("NVRAM changed since this utility loaded it. Reload before saving.");
    const auto backup = backupFile(directory, original.value_or(Bytes{}));
    std::cout << "Backup: " << backup << '\n';
    platform.write(original, value);
    original = value;
    std::cout << "Configuration saved and readback verified. Reboot to apply changes.\n";
}

void save(MacPlatform &platform, std::optional<Bytes> &original, Config &config, const fs::path &directory) {
    const auto candidate = config.prepared(platform.devices());
    const auto value = candidate.configured() ? std::optional<Bytes>(candidate.encode()) : std::nullopt;
    commit(platform, original, value, directory);
    config = candidate;
}

Selector selectorFor(const Device &device, const std::string &scope) {
    Selector selector;
    selector.deviceID = device.deviceID;
    if (scope != "pci") {
        selector.subsystemVendor = device.subsystemVendor;
        selector.subsystemDevice = device.subsystemDevice;
    }
    if (scope == "location") selector.location = device.location;
    if (scope != "pci" && scope != "subsystem" && scope != "location")
        throw std::runtime_error("Scope must be pci, subsystem, or location.");
    return selector;
}

void setGPU(Config &config, const std::vector<Device> &devices, unsigned index,
            const std::string &value, const std::string &scope) {
    if (!index || index > devices.size()) throw std::runtime_error("GPU index is out of range.");
    auto selector = selectorFor(devices[index - 1], scope);
    if (value == "clear") { config.clearSelector(selector); return; }
    // Preserve an existing per-GPU mask override when only changing its size.
    for (const auto &previous : config.selectors)
        if (previous.deviceID == selector.deviceID && previous.subsystemVendor == selector.subsystemVendor
            && previous.subsystemDevice == selector.subsystemDevice && previous.location == selector.location)
            selector.overrideMask = previous.overrideMask;
    selector.size = value == "auto" ? Automatic : value == "exclude" ? Excluded
                                                                    : static_cast<uint8_t>(number(value, 10));
    config.setSelector(selector);
}

void setGPUMask(Config &config, const std::vector<Device> &devices, unsigned index,
                bool overrideMask, const std::string &scope) {
    if (!index || index > devices.size()) throw std::runtime_error("GPU index is out of range.");
    auto selector = selectorFor(devices[index - 1], scope);
    for (const auto &previous : config.selectors)
        if (previous.deviceID == selector.deviceID && previous.subsystemVendor == selector.subsystemVendor
            && previous.subsystemDevice == selector.subsystemDevice && previous.location == selector.location)
            selector.size = previous.size;
    selector.overrideMask = overrideMask ? 1 : 255;
    config.setSelector(selector);
}

std::optional<std::string> prompt(const char *text) {
    std::cout << text << std::flush;
    std::string value;
    if (!std::getline(std::cin, value)) return std::nullopt;
    const auto begin = value.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return std::string{};
    return value.substr(begin, value.find_last_not_of(" \t\r") - begin + 1);
}

void interactive(MacPlatform &platform, std::optional<Bytes> original, Config config,
                 std::vector<Device> devices, const fs::path &directory) {
    std::cout << "\nE enable/toggle automatic sizes | D disable all | P PCI size | G per-GPU size\n"
                 "O per-GPU mask override\n"
                 "C clear GPU selectors | H toggle skip S3 | M toggle mask override\n"
                 "V preview | S save | R reload/discard | Q quit\n";
    while (true) {
        const auto input = prompt("\nNvStrapsReBar> ");
        if (!input) return;
        std::string command = *input;
        std::transform(command.begin(), command.end(), command.begin(), [](unsigned char ch) { return std::toupper(ch); });
        try {
            if (command == "Q") {
                if (config.encode() != Config::decode(original.value_or(Bytes{})).encode())
                    std::cout << "Unsaved changes discarded.\n";
                return;
            }
            if (command == "E") config.setEnabled(!config.enabled());
            else if (command == "D") config = {};
            else if (command == "C") config.selectors.clear();
            else if (command == "H") config.flags ^= 4;
            else if (command == "M") config.flags ^= 8;
            else if (command == "P") {
                std::cout << "PCI selectors: 0=system default, 1..31=maximum 2^N MiB,\n"
                             "32=any supported size, 64=selected GPUs, 65=GPU straps only.\n";
                const auto value = prompt("PCI selector: ");
                if (!value) return;
                const auto size = number(*value, 65);
                if (!validPCISize(size)) throw std::runtime_error("PCI selector must be 0..32, 64, or 65.");
                config.pciSize = static_cast<uint8_t>(size);
            } else if (command == "G" || command == "O") {
                showDevices(config, devices);
                const auto index = prompt("GPU number: ");
                if (!index) return;
                const auto scope = prompt("Scope [pci/subsystem/location]: ");
                if (!scope) return;
                if (command == "O") {
                    const auto value = prompt("GPU mask override [on/off]: ");
                    if (!value) return;
                    setGPUMask(config, devices, number(*index, 8), onOff(*value), scope->empty() ? "pci" : *scope);
                    showConfig(config);
                    continue;
                }
                std::cout << "GPU selectors: 0=64 MiB, 1=128 MiB, 2=256 MiB, 3=512 MiB,\n"
                             "4=1 GiB, 5=2 GiB, 6=4 GiB, 7=8 GiB, 8=16 GiB, 9=32 GiB, 10=64 GiB.\n";
                const auto value = prompt("GPU selector [0..10/auto/exclude/clear]: ");
                if (!value) return;
                setGPU(config, devices, number(*index, 8), *value, scope->empty() ? "pci" : *scope);
            } else if (command == "V") {
                preview(config, devices);
                continue;
            } else if (command == "S") {
                save(platform, original, config, directory);
            } else if (command == "R") {
                const auto reloaded = platform.read(ConfigName);
                const auto parsed = Config::decode(reloaded.value_or(Bytes{}));
                devices = platform.devices();
                original = reloaded;
                config = parsed;
            } else if (!command.empty()) {
                std::cout << "Choose E, D, P, G, O, C, H, M, V, S, R, or Q.\n";
                continue;
            }
            showConfig(config);
            showDevices(config, devices);
        } catch (const std::exception &error) {
            std::cerr << "Error: " << error.what() << '\n';
        }
    }
}

struct Options {
    bool menu = false, apply = false, enable = false, disable = false, clear = false, show = false;
    std::optional<unsigned> pci;
    std::optional<bool> skipS3, overrideMask;
    std::string scope = "pci";
    std::vector<std::pair<unsigned, std::string>> gpuSizes;
    std::vector<std::pair<unsigned, bool>> gpuMasks;
    std::optional<fs::path> exportPath, restorePath, inspectPath;
    fs::path backupDirectory = "NvStrapsReBar-backups";
    bool changes() const {
        return enable || disable || clear || pci.has_value() || skipS3.has_value()
            || overrideMask.has_value() || !gpuSizes.empty() || !gpuMasks.empty();
    }
};

Options parse(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        auto argument = [&]() -> std::string {
            if (++i == argc) throw std::runtime_error("Missing argument for " + flag);
            return argv[i];
        };
        if (flag == "--interactive") options.menu = true;
        else if (flag == "--show") options.show = true;
        else if (flag == "--save") options.apply = true;
        else if (flag == "--enable") options.enable = true;
        else if (flag == "--disable") options.disable = true;
        else if (flag == "--clear-selectors") options.clear = true;
        else if (flag == "--pci-size") {
            const auto value = number(argument(), 65);
            if (!validPCISize(value)) throw std::runtime_error("PCI selector must be 0..32, 64, or 65.");
            options.pci = value;
        } else if (flag == "--gpu-size") {
            const auto index = number(argument(), 8);
            const auto value = argument();
            if (value != "auto" && value != "exclude" && value != "clear") number(value, 10);
            options.gpuSizes.emplace_back(index, value);
        } else if (flag == "--gpu-mask") {
            const auto index = number(argument(), 8);
            const auto value = onOff(argument());
            options.gpuMasks.emplace_back(index, value);
        } else if (flag == "--scope") {
            options.scope = argument();
            if (options.scope != "pci" && options.scope != "subsystem" && options.scope != "location")
                throw std::runtime_error("Scope must be pci, subsystem, or location.");
        } else if (flag == "--skip-s3") options.skipS3 = onOff(argument());
        else if (flag == "--override-mask") options.overrideMask = onOff(argument());
        else if (flag == "--export") options.exportPath = argument();
        else if (flag == "--restore") options.restorePath = argument();
        else if (flag == "--inspect") options.inspectPath = argument();
        else if (flag == "--backup-dir") options.backupDirectory = argument();
        else throw std::runtime_error("Unknown option: " + flag + ". Use --help.");
    }
    if (options.disable && (options.enable || options.pci || options.clear || options.skipS3
                           || options.overrideMask || !options.gpuSizes.empty() || !options.gpuMasks.empty()))
        throw std::runtime_error("--disable cannot be combined with other setting changes.");
    if (options.restorePath && (options.changes() || options.menu || options.exportPath || options.inspectPath))
        throw std::runtime_error("--restore cannot be combined with other operations except --save and --backup-dir.");
    if (options.inspectPath && (options.changes() || options.menu || options.apply || options.exportPath))
        throw std::runtime_error("--inspect cannot be combined with firmware operations.");
    if (options.menu && (options.changes() || options.apply || options.exportPath))
        throw std::runtime_error("Use --interactive on its own, optionally with --backup-dir.");
    if (options.apply && !options.changes() && !options.restorePath)
        throw std::runtime_error("--save requires setting changes or --restore.");
    if (argc == 1 && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) options.menu = true;
    return options;
}

int run(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") { help(); return 0; }
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "NvStrapsReBar macOS port v0.3-macos.1 (upstream v0.3 firmware format, Intel x86_64)\n";
        return 0;
    }
    const auto options = parse(argc, argv);
    if (options.inspectPath) {
        const auto bytes = readFile(*options.inspectPath);
        if (bytes.empty()) std::cout << "Backup represents an absent configuration variable.\n";
        else showConfig(Config::decode(bytes));
        return 0;
    }
    MacPlatform platform;
    auto original = platform.read(ConfigName);
    auto config = Config::decode(original.value_or(Bytes{}));
    auto devices = platform.devices();
    std::cout << "NvStrapsReBar for Intel macOS / v0.3 firmware format\nDriver: "
              << describeStatus(platform.read(StatusName)) << '\n';
    if (!original) std::cout << "No saved NvStrapsReBar configuration.\n";

    if (options.exportPath) {
        exportFile(*options.exportPath, original.value_or(Bytes{}));
        std::cout << "Exported current variable to " << *options.exportPath << '\n';
    }
    if (options.restorePath) {
        const auto bytes = readFile(*options.restorePath);
        const auto restored = Config::decode(bytes);
        if (!bytes.empty() && restored.prepared(devices).encode() != bytes)
            throw std::runtime_error("Backup GPU/bridge addresses differ from the current hardware. Reconfigure using the menu.");
        showConfig(restored);
        const auto value = bytes.empty() ? std::nullopt : std::optional<Bytes>(bytes);
        if (options.apply) commit(platform, original, value, options.backupDirectory);
        else std::cout << "Restore preview only. Add --save to apply.\n";
        return 0;
    }

    if (options.disable) config = {};
    if (options.enable) config.setEnabled(true);
    if (options.clear) config.selectors.clear();
    if (options.pci) config.pciSize = static_cast<uint8_t>(*options.pci);
    if (options.skipS3) config.flags = (config.flags & ~4u) | (*options.skipS3 ? 4 : 0);
    if (options.overrideMask) config.flags = (config.flags & ~8u) | (*options.overrideMask ? 8 : 0);
    for (const auto &[index, value] : options.gpuSizes) setGPU(config, devices, index, value, options.scope);
    for (const auto &[index, value] : options.gpuMasks) setGPUMask(config, devices, index, value, options.scope);

    showConfig(config);
    showDevices(config, devices);
    if (options.changes()) {
        if (options.apply) save(platform, original, config, options.backupDirectory);
        else preview(config, devices);
    }
    if (options.menu) interactive(platform, original, config, devices, options.backupDirectory);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    try { return run(argc, argv); }
    catch (const std::exception &error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
