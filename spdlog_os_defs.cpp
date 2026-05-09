// Definitions for spdlog os functions
#include <string>
#include <string_view>
#include <chrono>
#include <spdlog/details/windows_include.h>

// Forward declarations
namespace spdlog {
    namespace level {
        enum level_enum { trace, debug, info, warn, err, critical, off, n_levels };
    }
    
    struct source_loc {
        const char* filename{nullptr};
        int line{0};
        const char* funcname{nullptr};
    };
    
    namespace details {
        struct log_msg {
            std::string logger_name;
            level::level_enum level{level::level_enum::off};
            std::chrono::system_clock::time_point time;
            size_t thread_id{0};
            mutable size_t color_range_start{0};
            mutable size_t color_range_end{0};
            source_loc source;
            std::string payload;
            
            // Constructor that commonlibsf.lib expects
            log_msg(source_loc loc, std::string_view logger_name, level::level_enum lvl, std::string_view msg);
        };
    }
}

// Define missing log_msg constructor
namespace spdlog::details {
    log_msg::log_msg(source_loc loc, std::string_view logger_name, level::level_enum lvl, std::string_view msg)
        : logger_name(logger_name), level(lvl), payload(msg), source(loc) {
        time = std::chrono::system_clock::now();
    }
}

// Define missing os functions with exact types from commonlibsf.lib
namespace spdlog::details::os {
    // Use std:: types as expected by commonlibsf.lib
    void wstr_to_utf8buf(std::wstring_view wstr, std::string& target) {
        if (wstr.size() == 0) {
            target.clear();
            return;
        }
        
        int wstr_size = static_cast<int>(wstr.size());
        int buf_size = ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), wstr_size, NULL, 0, NULL, NULL);
        
        if (buf_size > 0) {
            target.resize(static_cast<size_t>(buf_size));
            ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), wstr_size, &target[0], buf_size, NULL, NULL);
        }
    }
    
    void utf8_to_wstrbuf(std::string_view str, std::wstring& target) {
        if (str.size() == 0) {
            target.clear();
            return;
        }
        
        int str_size = static_cast<int>(str.size());
        int buf_size = ::MultiByteToWideChar(CP_UTF8, 0, str.data(), str_size, NULL, 0);
        
        if (buf_size > 0) {
            target.resize(static_cast<size_t>(buf_size));
            ::MultiByteToWideChar(CP_UTF8, 0, str.data(), str_size, &target[0], buf_size);
        }
    }
}
