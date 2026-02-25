set_project("ModernCommandFramework")
set_version("2.0.0")
set_xmakever("2.8.2")

set_languages("c++23")
add_rules("mode.debug", "mode.release", "mode.releasedbg")

if is_plat("windows") then
    set_arch("x64")
    add_defines("UNICODE", "_UNICODE", "WIN32_LEAN_AND_MEAN")
end

-- 1. Ładujemy CommonLibSF (musi zawierać własny plik xmake.lua)
includes("extern/commonlibsf")

target("ModernCommandFramework")
    set_kind("shared")
    
    -- 2. KLUCZOWE: Łączymy Twój plugin z biblioteką CommonLibSF
    -- To naprawi błędy LNK2019 (nierozpoznane symbole REL, SFSE, REX)
    add_deps("commonlibsf") 

    add_files("src/*.cpp")
    add_headerfiles("src/*.h")
    
    -- 3. Ścieżki do nagłówków
    add_includedirs("src")
    add_includedirs("extern/spdlog/include", "extern/fmt/include", { public = true })
    add_includedirs("extern/commonlibsf/include", "extern/commonlibsf/lib/commonlib-shared/include", { public = true })
    
    set_pcxxheader("src/PCH.h")

    if is_plat("windows") then
        -- 4. Dodajemy biblioteki systemowe wymagane przez REX::W32
        add_syslinks("User32", "Shell32", "Advapi32", "Ole32")
        
        add_cxflags("/MP", "/permissive-", "/utf-8", "/Zc:__cplusplus", "/wd4200", {force = true})
        
        -- Flagi linkera dla SFSE
        add_ldflags("/EXPORT:SFSEPlugin_Version", "/EXPORT:SFSEPlugin_Load", "/EXPORT:RegisterCommand", {force = true})
    end

    -- Opcjonalnie: automatyczne kopiowanie po kompilacji
    after_build(function (target)
        local plugins_dir = "C:/Program Files (x86)/Steam/steamapps/common/Starfield/Data/SFSE/Plugins"
        if os.isdir(plugins_dir) then
            os.cp(target:targetfile(), plugins_dir)
            print(">> MCF: Plugin skopiowany do folderu Starfielda.")
        end
    end)
target_end()