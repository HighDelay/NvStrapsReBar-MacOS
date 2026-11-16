#!/bin/sh
set -eu

source_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_directory=$(CDPATH= cd -- "$source_directory/../.." && pwd)
output_directory="$repository_directory/build/macos"
run_tests=false
if [ "$#" -gt 1 ]; then
    printf '%s\n' 'Usage: build.sh [--test]' >&2
    exit 2
fi
if [ "${1:-}" = '--test' ]; then
    run_tests=true
elif [ "$#" -ne 0 ]; then
    printf '%s\n' 'Usage: build.sh [--test]' >&2
    exit 2
fi

mkdir -p "$output_directory"
/usr/bin/xcrun clang -arch x86_64 -mmacosx-version-min=10.15 -std=c11 -O2 -Wall -Wextra \
    -I "$repository_directory/ReBarDxe/include" \
    -c "$repository_directory/ReBarDxe/DeviceRegistry.c" -o "$output_directory/DeviceRegistry.o"
/usr/bin/xcrun clang++ -arch x86_64 -mmacosx-version-min=10.15 -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    -I "$repository_directory/ReBarDxe/include" \
    "$source_directory/Config.cpp" "$source_directory/MacPlatform.cpp" "$source_directory/main.cpp" \
    "$output_directory/DeviceRegistry.o" -framework IOKit -framework CoreFoundation \
    -o "$output_directory/NvStrapsReBar"
/usr/bin/codesign --force --sign - "$output_directory/NvStrapsReBar"

if [ "$run_tests" = true ]; then
    /usr/bin/xcrun clang++ -arch x86_64 -mmacosx-version-min=10.15 -std=c++17 -O1 -g \
        -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I "$repository_directory/ReBarDxe/include" \
        "$source_directory/Config.cpp" "$source_directory/tests.cpp" "$output_directory/DeviceRegistry.o" \
        -o "$output_directory/NvStrapsReBarTests"
    "$output_directory/NvStrapsReBarTests"
fi

cp "$source_directory/NvStrapsReBar.command" "$source_directory/README.md" \
    "$source_directory/UPSTREAM-DRIVER.txt" "$repository_directory/LICENSE" "$output_directory/"
chmod +x "$output_directory/NvStrapsReBar.command"

# Bundle the original release asset, never a locally rebuilt or newer DXE driver.
driver_file="$output_directory/NvStrapsReBar.ffs"
driver_sha256=bc05d92b214d663a3b8d637a9f72ca694105151e467e24d18e2b25fbf6ecd3c3
if [ ! -f "$driver_file" ] || [ "$(/usr/bin/shasum -a 256 "$driver_file" | /usr/bin/awk '{print $1}')" != "$driver_sha256" ]; then
    driver_download=$(/usr/bin/mktemp "$output_directory/NvStrapsReBar.ffs.XXXXXX")
    trap 'rm -f "$driver_download"' EXIT HUP INT TERM
    /usr/bin/curl --fail --location --silent --show-error \
        https://github.com/terminatorul/NvStrapsReBar/releases/download/v0.3/NvStrapsReBar.ffs \
        --output "$driver_download"
    if [ "$(/usr/bin/shasum -a 256 "$driver_download" | /usr/bin/awk '{print $1}')" != "$driver_sha256" ]; then
        printf '%s\n' 'Upstream v0.3 driver SHA-256 mismatch; refusing to package it.' >&2
        exit 1
    fi
    mv "$driver_download" "$driver_file"
    trap - EXIT HUP INT TERM
fi
(
    cd "$output_directory"
    /usr/bin/shasum -a 256 NvStrapsReBar NvStrapsReBar.command NvStrapsReBar.ffs \
        README.md LICENSE UPSTREAM-DRIVER.txt > SHA256SUMS
)
archive="$output_directory/NvStrapsReBar-macOS-v0.3.zip"
rm -f "$archive"
/usr/bin/zip -j -q "$archive" "$output_directory/NvStrapsReBar" \
    "$output_directory/NvStrapsReBar.command" "$output_directory/NvStrapsReBar.ffs" \
    "$output_directory/README.md" "$output_directory/LICENSE" \
    "$output_directory/UPSTREAM-DRIVER.txt" "$output_directory/SHA256SUMS"
(
    cd "$output_directory"
    /usr/bin/shasum -a 256 NvStrapsReBar-macOS-v0.3.zip NvStrapsReBar.ffs NvStrapsReBar > SHA256SUMS.txt
)
printf 'Built: %s\nArchive: %s\n' "$output_directory/NvStrapsReBar" "$archive"
