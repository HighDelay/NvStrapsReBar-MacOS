#pragma once

#include "Config.hpp"
#include <filesystem>

namespace nvstraps {

class MacPlatform {
    uint32_t options = 0;
public:
    MacPlatform();
    ~MacPlatform();
    MacPlatform(const MacPlatform &) = delete;
    MacPlatform &operator=(const MacPlatform &) = delete;
    std::optional<Bytes> read(const char *name) const;
    std::vector<Device> devices() const;
    void write(const std::optional<Bytes> &expected, const std::optional<Bytes> &value) const;
};

Bytes readFile(const std::filesystem::path &path);
void exportFile(const std::filesystem::path &path, const Bytes &bytes);
std::filesystem::path backupFile(const std::filesystem::path &directory, const Bytes &bytes);

} // namespace nvstraps
