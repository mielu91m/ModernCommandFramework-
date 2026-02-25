#include "PCH.h"
#include "MCF_API.h"
#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Util
{
	static std::vector<MCF::simple_string_view> sv_vec_to_ssv_vec(const std::vector<std::string_view>& a_sourceVec)
	{
		std::vector<MCF::simple_string_view> result;
		result.reserve(a_sourceVec.size());
		for (auto& s : a_sourceVec) {
			result.push_back(MCF::simple_string_view(s.data(), s.size()));
		}
		return result;
	}

	static std::vector<std::string_view> str_split(const std::string_view& s, const std::string_view& delimiter, const std::optional<char>& escapeChar)
	{
		std::vector<std::string_view> substrings;
		size_t start = 0;
		size_t end = 0;
		bool escaped = false;

		while (end < s.length()) {
			if (escapeChar.has_value() && s[end] == escapeChar.value()) {
				escaped = !escaped;
			} else if (!escaped && s.substr(end, delimiter.length()) == delimiter) {
				substrings.push_back(s.substr(start, end - start));
				start = end + delimiter.length();
				end = start - 1;
			}
			end++;
		}

		if (start < s.length()) {
			substrings.push_back(s.substr(start));
		}

		if (escapeChar.has_value()) {
			for (auto& substring : substrings) {
				if (substring.size() > 1 && substring.front() == escapeChar.value() && substring.back() == escapeChar.value()) {
					substring.remove_prefix(1);
					substring.remove_suffix(1);
				}
			}
		}

		return substrings;
	}

	static std::string str_tolower(const std::string_view s)
	{
		std::string result(s);
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return result;
	}
}

namespace Commands
{
	static bool g_hookInstalled = false;
	static thread_local bool g_inHook = false;
	static std::mutex g_regLock;
	static std::unordered_map<std::string, MCF::CommandCallback> g_registrations;

	// Jak w CCF: pobierz referencję z uchwytu
	static RE::NiPointer<RE::TESObjectREFR> GetRefrFromHandle(uint32_t handle)
	{
		RE::NiPointer<RE::TESObjectREFR> result;
		REL::Relocation<void(RE::NiPointer<RE::TESObjectREFR>&, uint32_t*)> func(REL::ID(72399));
		func(result, &handle);
		return result;
	}

	// Jak w CCF: wskaźnik do managera konsoli pod adresem z REL::ID(879512)
	static RE::NiPointer<RE::TESObjectREFR> GetConsoleRefr()
	{
		REL::Relocation<std::uintptr_t> consoleReferencesManager(REL::ID(879512));
		uintptr_t addrOfSlot = consoleReferencesManager.address();
		void* managerPtr = reinterpret_cast<void*>(*reinterpret_cast<std::uintptr_t*>(addrOfSlot));

		REL::Relocation<void(void*, uint32_t*)> GetConsoleHandle(REL::ID(166314));
		uint32_t outId = 0;
		GetConsoleHandle(managerPtr, &outId);
		return GetRefrFromHandle(outId);
	}

	class ConsoleImpl : public MCF::ConsoleInterface
	{
	public:
		RE::NiPointer<RE::TESObjectREFR> GetSelectedReference() override
		{
			return GetConsoleRefr();
		}

		RE::TESForm* HexStrToForm(const MCF::simple_string_view& a_str) override
		{
			if (!a_str.data || a_str.size == 0) return nullptr;
			try {
				std::string str(a_str.data, a_str.size);
				RE::TESFormID id = static_cast<RE::TESFormID>(std::stoul(str, nullptr, 16));
				return RE::TESForm::LookupByID(id);
			} catch (...) {
				return nullptr;
			}
		}

		void PrintLn(const MCF::simple_string_view& a_txt) override
		{
			if (!a_txt.data) return;
			if (!printedDefault) {
				PrintDefault();
			}
			if (log) {
				std::string line(a_txt.data, a_txt.size);
				log->PrintLine("%s", line.c_str());
			}
		}

		void PreventDefaultPrint() override
		{
			printedDefault = true;
		}

		void PrintDefault()
		{
			if (!printedDefault && log) {
				log->PrintLine("%s", defText);
				printedDefault = true;
			}
		}

		void Reset(const char* a_txt)
		{
			if (!log) {
				log = RE::ConsoleLog::GetSingleton();
			}
			defText = a_txt;
			printedDefault = false;
		}

	private:
		RE::ConsoleLog* log{ nullptr };
		const char* defText{ nullptr };
		bool printedDefault{ false };
	};

	static ConsoleImpl g_console;

	// Detour hook: REL::ID(65829) (parser step, returns 0xFFFF when done)
	using ExecuteCommandFunc = std::uint32_t(__fastcall*)(void* a_param1, void* a_param2);
	static ExecuteCommandFunc g_originalFunc = nullptr;

	static std::uint32_t __fastcall HookedExecuteCommand(void* a_param1, void* a_param2)
	{
		if (g_inHook) {
			return g_originalFunc ? g_originalFunc(a_param1, a_param2) : 0xFFFF;
		}
		g_inHook = true;

		const char* cmdString = nullptr;
		if (a_param2) {
			try {
				cmdString = *reinterpret_cast<const char**>(a_param2);
			} catch (...) {}
		}

		if (!cmdString || cmdString[0] == '\0') {
			g_inHook = false;
			return g_originalFunc ? g_originalFunc(a_param1, a_param2) : 0xFFFF;
		}

		std::string fullCmd(cmdString);
		auto args = Util::str_split(fullCmd, " ", '"');
		if (args.empty()) {
			g_inHook = false;
			return g_originalFunc ? g_originalFunc(a_param1, a_param2) : 0xFFFF;
		}

		std::string commandLower = Util::str_tolower(args[0]);
		std::unique_lock<std::mutex> lock(g_regLock);
		auto it = g_registrations.find(commandLower);

		if (it != g_registrations.end()) {
			args.erase(args.begin());
			auto argsSSV = Util::sv_vec_to_ssv_vec(args);

			MCF::simple_array<MCF::simple_string_view> argsArr;
			argsArr.data = argsSSV.data();
			argsArr.count = static_cast<uint64_t>(argsSSV.size());

			g_console.Reset(fullCmd.c_str());
			lock.unlock();

			try {
				it->second(argsArr, fullCmd.c_str(), &g_console);
			} catch (const std::exception& e) {
				spdlog::error("MCF: Exception in callback: {}", e.what());
			} catch (...) {
				spdlog::error("MCF: Unknown exception in callback");
			}

			g_console.PrintDefault();
			g_inHook = false;
			// Return 0xFFFF to stop further parsing; do NOT mutate buffer
			return 0xFFFF;
		}

		g_inHook = false;
		return g_originalFunc ? g_originalFunc(a_param1, a_param2) : 0xFFFF;
	}

	static void WriteAbsoluteJmp(std::uintptr_t from, std::uintptr_t to)
	{
		std::uint8_t jmp[14]{};
		jmp[0] = 0xFF;
		jmp[1] = 0x25;
		*reinterpret_cast<std::uint32_t*>(jmp + 2) = 0;
		*reinterpret_cast<std::uint64_t*>(jmp + 6) = to;

		DWORD oldProtect;
		VirtualProtect(reinterpret_cast<void*>(from), 14, PAGE_EXECUTE_READWRITE, &oldProtect);
		memcpy(reinterpret_cast<void*>(from), jmp, 14);
		VirtualProtect(reinterpret_cast<void*>(from), 14, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(from), 14);
	}

	static bool CreateDetourHook(std::uintptr_t targetAddr, void* hookFunc, ExecuteCommandFunc* outOriginal)
	{
		if (!targetAddr || !hookFunc || !outOriginal) return false;

		constexpr size_t bytesToCopy = 16;  // align to whole instructions (prologue >= 16 bytes)

		// Log original bytes for validation
		std::uint8_t origBytes[bytesToCopy]{};
		std::memcpy(origBytes, reinterpret_cast<void*>(targetAddr), bytesToCopy);
		spdlog::info("MCF: Target bytes  = {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
			origBytes[0], origBytes[1], origBytes[2], origBytes[3], origBytes[4], origBytes[5], origBytes[6], origBytes[7],
			origBytes[8], origBytes[9], origBytes[10], origBytes[11], origBytes[12], origBytes[13], origBytes[14], origBytes[15]);

		std::uintptr_t trampolineAddr = 0;
		for (int delta = -2000; delta <= 2000; delta += 64) {
			std::uintptr_t candidate = targetAddr + (delta * 1024 * 1024);
			candidate &= ~0xFFFF;
			void* result = VirtualAlloc(reinterpret_cast<void*>(candidate), 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (result) {
				trampolineAddr = reinterpret_cast<std::uintptr_t>(result);
				break;
			}
		}
		if (!trampolineAddr) {
			spdlog::error("MCF: Failed to allocate trampoline");
			return false;
		}

		memcpy(reinterpret_cast<void*>(trampolineAddr), reinterpret_cast<void*>(targetAddr), bytesToCopy);
		WriteAbsoluteJmp(trampolineAddr + bytesToCopy, targetAddr + bytesToCopy);

		*outOriginal = reinterpret_cast<ExecuteCommandFunc>(trampolineAddr);
		WriteAbsoluteJmp(targetAddr, reinterpret_cast<std::uintptr_t>(hookFunc));
		// Pad any remaining bytes with NOPs to avoid stray partial instructions
		if (bytesToCopy > 14) {
			const size_t pad = bytesToCopy - 14;
			std::uint8_t nops[16]{ 0x90 };
			DWORD oldProtect;
			VirtualProtect(reinterpret_cast<void*>(targetAddr + 14), pad, PAGE_EXECUTE_READWRITE, &oldProtect);
			std::memcpy(reinterpret_cast<void*>(targetAddr + 14), nops, pad);
			VirtualProtect(reinterpret_cast<void*>(targetAddr + 14), pad, oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(targetAddr + 14), pad);
		}

		spdlog::info("MCF: Hook installed successfully");
		spdlog::info("MCF:   Target:      {:X}", targetAddr);
		spdlog::info("MCF:   Hook:        {:X}", reinterpret_cast<std::uintptr_t>(hookFunc));
		spdlog::info("MCF:   Trampoline:  {:X}", trampolineAddr);
		return true;
	}

	static void InstallHooks()
	{
		if (g_hookInstalled) return;

		spdlog::info("MCF: Installing console hook (detour ID 65829)...");

		try {
			REL::Relocation<std::uintptr_t> target(REL::ID(65829));
			if (CreateDetourHook(target.address(), reinterpret_cast<void*>(HookedExecuteCommand), &g_originalFunc)) {
				g_hookInstalled = true;
				spdlog::info("MCF: Hook installed successfully!");
			} else {
				spdlog::error("MCF: Hook installation failed!");
			}
		} catch (const std::exception& e) {
			spdlog::error("MCF: Exception during hook install: {}", e.what());
		} catch (...) {
			spdlog::error("MCF: Unknown exception during hook install");
		}
	}

	void RegisterCommand(const char* a_name, MCF::CommandCallback a_callback)
	{
		if (!a_name || !a_callback) {
			spdlog::error("MCF: RegisterCommand called with null!");
			return;
		}

		std::string nameStr = Util::str_tolower(a_name);
		if (nameStr.empty()) {
			spdlog::error("MCF: RegisterCommand called with empty name");
			return;
		}

		std::unique_lock<std::mutex> lock(g_regLock);
		if (g_registrations.contains(nameStr)) {
			spdlog::warn("MCF: Command '{}' already registered", a_name);
			return;
		}

		g_registrations.insert({ nameStr, a_callback });
		spdlog::info("MCF: Registered command: '{}'", a_name);
		spdlog::info("MCF: Total commands: {}", g_registrations.size());

		if (!g_hookInstalled) {
			lock.unlock();
			InstallHooks();
		}
	}
}

extern "C" __declspec(dllexport) void RegisterCommand(const char* a_name, MCF::CommandCallback a_callback)
{
	spdlog::info("MCF EXPORT: RegisterCommand called for '{}'", a_name ? a_name : "(null)");
	Commands::RegisterCommand(a_name, a_callback);
}
