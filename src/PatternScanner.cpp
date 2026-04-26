#include "PatternScanner.h"
#include <REL/Module.h>
#include <REL/Segment.h>
#include <span>
#include <sstream>
#include <vector>
#include <cctype>
#include <optional>
#include <cstdint>
#include <string_view>

namespace MCF::PatternScanner
{
    static std::vector<std::optional<std::uint8_t>> ParsePattern(std::string_view a_pattern)
    {
        std::vector<std::optional<std::uint8_t>> result;
        
        for (size_t i = 0; i < a_pattern.length(); )
        {
            // Skip whitespace
            while (i < a_pattern.length() && std::isspace(static_cast<unsigned char>(a_pattern[i])))
                ++i;
            
            if (i >= a_pattern.length())
                break;
            
            // Check for wildcard
            if (a_pattern[i] == '?')
            {
                result.push_back(std::nullopt);
                ++i;
                // Skip second ? if present
                if (i < a_pattern.length() && a_pattern[i] == '?')
                    ++i;
            }
            else
            {
                // Parse hex byte
                auto hexChar = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    return -1;
                };
                
                int hi = hexChar(a_pattern[i]);
                if (hi >= 0 && i + 1 < a_pattern.length())
                {
                    int lo = hexChar(a_pattern[i + 1]);
                    if (lo >= 0)
                    {
                        result.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
                        i += 2;
                    }
                    else
                    {
                        ++i;
                    }
                }
                else
                {
                    ++i;
                }
            }
        }
        
        return result;
    }
    
    std::optional<std::uintptr_t> Scan(std::span<const std::byte> a_region, std::string_view a_pattern)
    {
        auto pattern = ParsePattern(a_pattern);
        if (pattern.empty())
            return std::nullopt;
        
        const std::size_t patternLen = pattern.size();
        
        for (std::size_t i = 0; i <= a_region.size() - patternLen; ++i)
        {
            bool match = true;
            for (std::size_t j = 0; j < patternLen; ++j)
            {
                if (pattern[j].has_value() && 
                    static_cast<std::uint8_t>(a_region[i + j]) != pattern[j].value())
                {
                    match = false;
                    break;
                }
            }
            
            if (match)
            {
                return reinterpret_cast<std::uintptr_t>(&a_region[i]);
            }
        }
        
        return std::nullopt;
    }
    
    std::optional<std::uintptr_t> ScanModule(std::string_view a_pattern)
    {
        auto module = REL::Module::GetSingleton();
        auto textSegment = module->segment(REL::Segment::text);
        
        std::span<const std::byte> region(
            reinterpret_cast<const std::byte*>(textSegment.address()),
            textSegment.size()
        );
        
        return Scan(region, a_pattern);
    }
}
