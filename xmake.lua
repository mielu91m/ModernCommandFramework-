-- Modern Command Framework (MCF) - xmake build configuration
-- Built with libxe's updated CommonLibSF

set_project("ModernCommandFramework")
set_version("2.0.0")
set_xmakever("2.7.0")

-- Set C++ standard
set_languages("c++23")

-- Add rules
add_rules("mode.debug", "mode.release", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- Platform configuration
if is_plat("windows") then
    set_arch("x64")
    add_defines("UNICODE", "_UNICODE")
end

-- Include directories
 add_includedirs(
        "include",
        "extern/fmt/include",
        "extern/commonlibsf/include",
        "extern/commonlibsf/lib/commonlib-shared/include",
        { public = true }
		)

-- Main target
target("ModernCommandFramework")
    set_kind("shared")
    
    -- Source files
    add_files("src/*.cpp")
    add_headerfiles("src/*.h")
    
    -- Include directories
    add_includedirs(
        "src",
        "$(builddir)/include",
        { public = false }
    )
    
    -- Dependencies
    -- Build with CommonLibSF and add spdlog source files to resolve linking
    -- Link with CommonLibSF (libxe's version)
    add_linkdirs("extern/commonlibsf/build/windows/x64/release")
    add_links("commonlib-shared", "commonlibsf", "spdlog")
    
    -- Using manually built spdlog.lib from extern/spdlog_build
    add_includedirs("extern/spdlog_build/include")
    
    -- Add all spdlog source files for compiled library
    add_defines("SPDLOG_COMPILED_LIB")
    add_files("extern/spdlog_build/src/*.cpp")
    
    add_syslinks("user32", "kernel32", "shell32", "version", "ws2_32", "advapi32", "bcrypt", "ole32", "oleaut32", "dxgi", "d3dcompiler", "dbghelp", "d3d11", "psapi")
    
    -- Compiler options for MSVC
    if is_plat("windows") and is_mode("release") then
        add_cxflags(
            "/MP",      -- Multi-processor compilation
            "/await",   -- Enable coroutines
            "/W0",      -- Warning level 0 (suppress warnings)
            "/WX",      -- Treat warnings as errors
            "/permissive-",  -- Standards conformance
            "/utf-8",   -- UTF-8 source and execution character sets
            -- "/DSPDLOG_HEADER_ONLY",  -- Removed, using compiled lib instead
            "/Zc:alignedNew",
            "/Zc:auto",
            "/Zc:__cplusplus",
            "/Zc:externC",
            "/Zc:externConstexpr",
            "/Zc:forScope",
            "/Zc:hiddenFriend",
            "/Zc:implicitNoexcept",
            "/Zc:lambda",
            "/Zc:noexceptTypes",
            "/Zc:preprocessor",
            "/Zc:referenceBinding",
            "/Zc:rvalueCast",
            "/Zc:sizedDealloc",
            "/Zc:strictStrings",
            "/Zc:ternary",
            "/Zc:threadSafeInit",
            "/Zc:trigraphs",
            "/Zc:wchar_t",
            "/wd4200",  -- Suppress: nonstandard extension used : zero-sized array in struct/union
            { force = true }
        )
    end
    
    -- Linker options
    add_ldflags(
        "/EXPORT:SFSEPlugin_Version",
        "/EXPORT:SFSEPlugin_Load",
        "/EXPORT:RegisterCommand",
        { force = true }
    )
    
    -- Precompiled header
    -- Precompiled header (use absolute path for xmake)
set_pcheader("src/PCH.h")
    
    -- Generate Plugin.h from template
    before_build(function (target)
        import("core.project.config")
        
        local plugin_h_in = path.join("cmake", "Plugin.h.in")
        local plugin_h_out = path.join("$(builddir)", "include", "Plugin.h")
        
        if os.isfile(plugin_h_in) then
            os.mkdir(path.directory(plugin_h_out))
            
            local content = io.readfile(plugin_h_in)
            content = content:gsub("@PROJECT_NAME@", "ModernCommandFramework")
            content = content:gsub("@PROJECT_VERSION@", "2.0.0")
            content = content:gsub("@PROJECT_VERSION_MAJOR@", "2")
            content = content:gsub("@PROJECT_VERSION_MINOR@", "0")
            content = content:gsub("@PROJECT_VERSION_PATCH@", "0")
            
            io.writefile(plugin_h_out, content)
            print("Generated Plugin.h")
        end
    end)
    
    -- Post-build: Copy to Starfield directory
    after_build(function (target)
        local starfield_path = "C:/Program Files (x86)/Steam/steamapps/common/Starfield/Data/SFSE/Plugins"
        
        if os.isdir(starfield_path) then
            local dll_path = target:targetfile()
            local pdb_path = path.join(path.directory(dll_path), path.basename(dll_path) .. ".pdb")
            
            os.cp(dll_path, starfield_path)
            print("Copied " .. path.filename(dll_path) .. " to Starfield directory")
            
            if os.isfile(pdb_path) then
                os.cp(pdb_path, starfield_path)
                print("Copied " .. path.filename(pdb_path) .. " to Starfield directory")
            end
        else
            print("Warning: Starfield directory not found, skipping copy")
        end
    end)
    
    -- Version resource
   -- add_files("cmake/version.rc")
    
target_end()

-- spdlog is now managed via add_requires above

-- Tasks

-- Clean task
task("clean")
    set_menu({
        usage = "xmake clean",
        description = "Clean build files"
    })
    on_run(function ()
        os.rm("build")
        os.rm(".xmake")
        print("Cleaned build files")
    end)
task_end()

-- Install task
task("install")
    set_menu({
        usage = "xmake install",
        description = "Install to Starfield directory"
    })
    on_run(function ()
        import("core.project.task")
        task.run("build")
        
        local starfield_path = "C:/Program Files (x86)/Steam/steamapps/common/Starfield/Data/SFSE/Plugins"
        if os.isdir(starfield_path) then
            local dll = "build/windows/x64/release/ModernCommandFramework.dll"
            local pdb = "build/windows/x64/release/ModernCommandFramework.pdb"
            
            if os.isfile(dll) then
                os.cp(dll, starfield_path)
                print("Installed ModernCommandFramework.dll")
            end
            
            if os.isfile(pdb) then
                os.cp(pdb, starfield_path)
                print("Installed ModernCommandFramework.pdb")
            end
        else
            print("Error: Starfield directory not found")
            os.exit(1)
        end
    end)
task_end()

-- Package task
task("package")
    set_menu({
        usage = "xmake package",
        description = "Create distribution package"
    })
    on_run(function ()
        import("core.project.task")
        task.run("build")
        
        local package_name = "MCF-v2.0.0"
        local package_dir = path.join("build", "package", package_name)
        
        os.mkdir(package_dir)
        
        -- Copy DLL
        local dll = "build/windows/x64/release/ModernCommandFramework.dll"
        if os.isfile(dll) then
            os.cp(dll, package_dir)
        end
        
        -- Copy documentation
        local docs = {"README.md", "LICENSE", "CHANGELOG.md"}
        for _, doc in ipairs(docs) do
            if os.isfile(doc) then
                os.cp(doc, package_dir)
            end
        end
        
        -- Copy API header
        os.cp("src/MCF_API.h", package_dir)
        
        -- Create archive
        os.cd("build/package")
        os.exec("7z a -tzip " .. package_name .. ".zip " .. package_name)
        os.cd("../..")
        
        print("Created package: build/package/" .. package_name .. ".zip")
    end)
task_end()

-- Development helper tasks

task("dev:watch")
    set_menu({
        usage = "xmake dev:watch",
        description = "Watch for changes and rebuild"
    })
    on_run(function ()
        print("Watching for file changes...")
        while true do
            os.exec("xmake build")
            os.sleep(2000)
        end
    end)
task_end()

task("dev:test")
    set_menu({
        usage = "xmake dev:test",
        description = "Run tests"
    })
    on_run(function ()
        print("Running tests...")
        -- Add test execution here
    end)
task_end()