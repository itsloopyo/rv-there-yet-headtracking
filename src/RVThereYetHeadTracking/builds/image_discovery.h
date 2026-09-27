#pragma once

#include <cstddef>
#include <cstdint>

namespace RVThereYetHeadTracking::builds
{
    struct DiscoveredAddresses
    {
        std::uint32_t viewBuilder = 0;
        std::uint32_t objects = 0;
        std::uint32_t names = 0;
        const char* error = nullptr;
    };

    std::uint64_t NormalizedCodeHash(const std::uint8_t* code, std::size_t size);
    DiscoveredAddresses DiscoverImage(const std::uint8_t* image, std::size_t size);
}
