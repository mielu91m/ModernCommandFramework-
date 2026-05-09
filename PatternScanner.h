#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <optional>
#include <string_view>

namespace MCF::PatternScanner
{
    // Scan memory region for pattern (bytes with wildcards ??)
    // Pattern format: "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC"
    std::optional<std::uintptr_t> Scan(std::span<const std::byte> a_region, std::string_view a_pattern);
    
    // Scan module's .text section
    std::optional<std::uintptr_t> ScanModule(std::string_view a_pattern);
}
