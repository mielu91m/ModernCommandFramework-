#include "PCH.h"

// Initialize logging
static void InitializeLog()
{
    auto path = SFSE::log::log_directory();
    if (!path) return;
    
    *path /= "ModernCommandFramework.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
    
    log->set_level(spdlog::level::info);
    log->flush_on(spdlog::level::info);
    
    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S.%e] [%t] [%l] %v");
}

// Plugin load
extern "C" __declspec(dllexport) bool SFSEPlugin_Load(const SFSE::LoadInterface* a_sfse)
{
    InitializeLog();
    
    spdlog::info("=====================================");
    spdlog::info("Modern Command Framework v1.0");
    spdlog::info("=====================================");
    
    SFSE::Init(a_sfse, { .trampoline = true, .trampolineSize = 256 });
    
    spdlog::info("MCF: Plugin loaded successfully");
    spdlog::info("MCF: Waiting for command registrations...");
    spdlog::info("=====================================");
    
    return true;
}

// Version info
extern "C" __declspec(dllexport) constinit auto SFSEPlugin_Version = []() {
    SFSE::PluginVersionData v{};
    v.PluginVersion({ 1, 0, 0, 0 });
    v.PluginName("ModernCommandFramework");
    v.AuthorName("mielu91m");
    v.UsesSigScanning(false);
    v.UsesAddressLibrary(true);
    v.HasNoStructUse(true);
    v.IsLayoutDependent(false);
    v.CompatibleVersions({ SFSE::RUNTIME_LATEST });
    return v;
}();
