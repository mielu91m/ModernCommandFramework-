#include "PCH.h"
#include "MCF_API.h"
#include "REL/Offset2ID.h"
#include "REL/Trampoline.h"
#include "REL/ASM.h"
#include "REL/Utility.h"
#include "SFSE/API.h"
#include <algorithm>
#include <cstring>
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
	// Limity – zabezpieczenie przy zmodyfikowanych INI / .bat / innych modach (zawieszki, crash przy additem)
	static constexpr size_t kMaxCommandLength = 8192;
	static constexpr size_t kMaxArgsCount = 128;
	static constexpr size_t kMaxPrintLineLength = 4096;
	static constexpr size_t kMaxHexFormLength = 32;

	static bool g_hookInstalled = false;
	static thread_local bool g_inHook = false;
	static std::mutex g_regLock;
	static std::unordered_map<std::string, MCF::CommandCallback> g_registrations;

	// Opcja B: pojedynczy getter (Selected Ref) + slot kontekstu – tylko RVA z IDA, bez REL::ID.
	// Getter: funkcja wywoływana przed "Selected Actor: %s" / "Selected Ref: %s", zwraca REFR* (RAX) lub void(ctx, NiPointer&).
	// Kontekst: globalny qword (np. qword_1450455F0 → 0x50455F0), przekazywany w RCX do gettera.
	static uint32_t g_getSelectedRefRVA = 0x2853E40u;   // getter z IDA (sub przy "Selected Actor")
	static bool g_getSelectedRefReturnsPtr = true;      // true = getter zwraca REFR* w RAX
	static uint32_t g_consoleContextSlotRVA = 0x50455F0u;  // slot kontekstu (mov rcx, cs:qword przed call gettera)
	// Opcja 2 (gdy opcja 1 nie działa): trzy osobne adresy (slot, GetConsoleHandle, LookupRefFromHandle).
	// 0x5C8E6D8 = RVA qword_145C8E6D8 z IDA (xrefs: 2 zapisy, ~30 odczytów; getter sub_142C54A80).
	static uint32_t g_consoleRefManagerSlotRVA = 0x5C8E6D8u;  // slot „managera konsoli”
	static uint32_t g_getConsoleHandleRVA = 0;       // Funkcja void(*)(void* manager, uint32_t* outHandle).
	static uint32_t g_lookupRefFromHandleRVA = 0;   // Funkcja void(*)(NiPointer<TESObjectREFR>& out, uint32_t* handle).

	static void GetRefrFromHandleImpl(uint32_t handle, RE::NiPointer<RE::TESObjectREFR>& out)
	{
		__try {
			if (g_lookupRefFromHandleRVA != 0) {
				uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("Starfield.exe"));
				auto fn = reinterpret_cast<void(*)(RE::NiPointer<RE::TESObjectREFR>&, uint32_t*)>(base + g_lookupRefFromHandleRVA);
				if (fn) fn(out, &handle);
			} else {
				REL::Relocation<void(RE::NiPointer<RE::TESObjectREFR>&, uint32_t*)> func(REL::ID(72399));
				func(out, &handle);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			out.reset();
		}
	}

	static RE::NiPointer<RE::TESObjectREFR> GetRefrFromHandle(uint32_t handle)
	{
		RE::NiPointer<RE::TESObjectREFR> result;
		GetRefrFromHandleImpl(handle, result);
		return result;
	}

	// __try w osobnym wrapperze (bez lokalnych obiektow z destruktorem).
	static void GetConsoleRefrImpl(RE::NiPointer<RE::TESObjectREFR>& out)
	{
		out.reset();
		int avStep = 0;  // 1=read slot, 2=GetConsoleHandle, 3=LookupRefFromHandle – do logu przy AV
		__try {
			// Opcja B: getter (RVA) + slot kontekstu (RVA) – bez REL::ID.
			if (g_getSelectedRefRVA != 0) {
				uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("Starfield.exe"));
				if (base) {
					static bool s_loggedOptionB = false;
					if (!s_loggedOptionB) {
						spdlog::info("MCF: GetConsoleRefr using Option B (getter RVA 0x{:X}, context slot RVA 0x{:X})",
							g_getSelectedRefRVA, g_consoleContextSlotRVA);
						s_loggedOptionB = true;
					}
					__try {
						void* ctx = nullptr;
						if (g_consoleContextSlotRVA != 0) {
							ctx = *reinterpret_cast<void**>(base + g_consoleContextSlotRVA);
						}
						if (g_getSelectedRefReturnsPtr) {
							auto fn = reinterpret_cast<RE::TESObjectREFR*(*)(void*)>(base + g_getSelectedRefRVA);
							if (fn) {
								RE::TESObjectREFR* raw = fn(ctx);
								if (raw) out.reset(raw);
							}
						} else {
							auto fn = reinterpret_cast<void(*)(void*, RE::NiPointer<RE::TESObjectREFR>&)>(base + g_getSelectedRefRVA);
							if (fn) fn(ctx, out);
						}
					}
					__except (EXCEPTION_EXECUTE_HANDLER) {
						static bool s_loggedAv = false;
						if (!s_loggedAv) {
							spdlog::warn("MCF: GetConsoleRefr Option B AV (getter 0x{:X} / context 0x{:X} – sprawdź RVA w IDA dla tej wersji .exe)",
								g_getSelectedRefRVA, g_consoleContextSlotRVA);
							s_loggedAv = true;
						}
						out.reset();
					}
				}
				return;
			}
			uintptr_t addrOfSlot = 0;
			if (g_consoleRefManagerSlotRVA != 0) {
				uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("Starfield.exe"));
				if (base) {
					addrOfSlot = base + g_consoleRefManagerSlotRVA;
					static bool s_loggedSlot = false;
					if (!s_loggedSlot) {
						spdlog::info("MCF: GetConsoleRefr using manager slot RVA 0x{:X} (qword_145C8E6D8 from IDA)", g_consoleRefManagerSlotRVA);
						s_loggedSlot = true;
					}
				}
			}
			if (!addrOfSlot) {
				// 879512 = oryginalny „slot” managera; 840929 to ID RTTI (typ), nie adres – cofamy 840929.
				REL::Relocation<std::uintptr_t> consoleReferencesManager(REL::ID(879512));
				addrOfSlot = consoleReferencesManager.address();
			}
			if (!addrOfSlot) return;

			void* managerPtr = nullptr;
			uint32_t outId = 0;

			__try {
				avStep = 1;
				managerPtr = reinterpret_cast<void*>(*reinterpret_cast<std::uintptr_t*>(addrOfSlot));
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				spdlog::warn("MCF: GetConsoleRefr AV at step 1 (reading manager from slot 0x{:X})", g_consoleRefManagerSlotRVA);
				out.reset();
				return;
			}
			if (!managerPtr) return;

			__try {
				avStep = 2;
				if (g_getConsoleHandleRVA != 0) {
					uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("Starfield.exe"));
					auto fn = reinterpret_cast<void(*)(void*, uint32_t*)>(base + g_getConsoleHandleRVA);
					if (fn) fn(managerPtr, &outId);
				} else {
					REL::Relocation<void(void*, uint32_t*)> GetConsoleHandle(REL::ID(166314));
					GetConsoleHandle(managerPtr, &outId);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				spdlog::warn("MCF: GetConsoleRefr AV at step 2 (GetConsoleHandle – REL::ID 166314; manager slot OK, funkcja AL może być inna w tej wersji)");
				out.reset();
				return;
			}
			if (outId == 0 || outId == 0xFFFFFFFF) return;

			__try {
				avStep = 3;
				GetRefrFromHandleImpl(outId, out);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				spdlog::warn("MCF: GetConsoleRefr AV at step 3 (LookupRefFromHandle – REL::ID 72399)");
				out.reset();
				return;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			spdlog::warn("MCF: GetConsoleRefr access violation (step {} or earlier)", avStep);
			out.reset();
		}
	}

	static RE::NiPointer<RE::TESObjectREFR> GetConsoleRefr()
	{
		RE::NiPointer<RE::TESObjectREFR> result;
		GetConsoleRefrImpl(result);
		return result;
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
			if (a_str.size > kMaxHexFormLength) return nullptr;
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
				size_t len = (a_txt.size <= kMaxPrintLineLength) ? a_txt.size : kMaxPrintLineLength;
				try {
					std::string line(a_txt.data, len);
					log->PrintLine("%s", line.c_str());
				} catch (...) {
					// Nie wywalać gry przy błędnej konsoli / INI
				}
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

	// Hook parsera (REL::ID(65829)) – stabilny wariant bez użycia starych ID z CCF.
	using ExecuteCommandFunc = std::uint32_t(__fastcall*)(void* a_param1, void* a_param2);
	static ExecuteCommandFunc g_originalFunc = nullptr;

	// Pierwszy token (słowo) z bufora – bez pełnego split, żeby od razu wiedzieć: MCF czy vanilla.
	static std::string GetFirstTokenLower(const char* s, size_t maxLen)
	{
		size_t i = 0;
		while (i < maxLen && s[i] != '\0' && (s[i] == ' ' || s[i] == '\t')) ++i;
		size_t start = i;
		while (i < maxLen && s[i] != '\0' && s[i] != ' ' && s[i] != '\t') ++i;
		if (start >= i) return std::string();
		return Util::str_tolower(std::string_view(s + start, i - start));
	}

	// Właściwa implementacja hooka – wywoływana z małego wrappera SEH bez obiektów z destruktorem.
	static std::uint32_t __fastcall HookedExecuteCommand_Impl(void* a_param1, void* a_param2)
	{
		if (g_inHook) {
			return g_originalFunc ? g_originalFunc(a_param1, a_param2) : 0xFFFF;
		}
		g_inHook = true;

		static std::atomic<uint32_t> s_hookCalls{ 0 };
		const uint32_t callNum = ++s_hookCalls;

		auto passToVanilla = [&]() {
			if (callNum <= 10) {
				spdlog::info("MCF: HookedExecuteCommand passToVanilla (call #{})", callNum);
			}
			g_inHook = false;
			return g_originalFunc ? g_originalFunc(a_param1, a_param2) : 0xFFFF;
		};

		try {
			const char* cmdString = nullptr;
			if (a_param2) {
				cmdString = *reinterpret_cast<const char**>(a_param2);
			}

			if (!cmdString || cmdString[0] == '\0') {
				return passToVanilla();
			}

			// Limit długości
			size_t len = 0;
			for (; len < kMaxCommandLength && cmdString[len] != '\0'; ++len) {}
			if (len >= kMaxCommandLength) {
				return passToVanilla();
			}

			// Od razu: tylko jeśli pierwszy token to zarejestrowana komenda (np. "saf") – wchodzimy w MCF. Inaczej vanilla.
			std::string firstToken = GetFirstTokenLower(cmdString, len);
			if (callNum <= 10) {
				spdlog::info("MCF: HookedExecuteCommand call #{} raw='{}', firstToken='{}'", callNum, cmdString, firstToken);
			}
			if (firstToken.empty()) {
				return passToVanilla();
			}
			// Dodatkowe twarde wykluczenie dla 'help' – zawsze pełna vanilla,
			// nawet jeśli jakiś inny mod przypadkowo zarejestrował taką komendę w MCF.
			if (firstToken == "help") {
				return passToVanilla();
			}
			bool hasCommand = false;
			{
				std::unique_lock<std::mutex> lock(g_regLock);
				hasCommand = g_registrations.find(firstToken) != g_registrations.end();
			}
			if (!hasCommand) {
				// Inna komenda (additem, coc, help, showlooksmenu, ...) – od razu vanilla, bez parsowania.
				return passToVanilla();
			}

			// Dopiero tu: pełne parsowanie tylko dla komend MCF (np. saf)
			std::string fullCmd(cmdString, len);
			auto args = Util::str_split(fullCmd, " ", '"');
			if (args.empty()) {
				return passToVanilla();
			}
			if (args.size() > kMaxArgsCount) {
				return passToVanilla();
			}

			std::string commandLower = Util::str_tolower(args[0]);

			// Pobierz callback spod mutexa; mutex musi być zwolniony PRZED passToVanilla() (nigdy return w bloku lock).
			MCF::CommandCallback callback;
			bool foundCommand = false;
			{
				std::unique_lock<std::mutex> lock(g_regLock);
				auto it = g_registrations.find(commandLower);
				if (it != g_registrations.end()) {
					callback = it->second;
					foundCommand = true;
				}
			}
			if (!foundCommand) {
				return passToVanilla();
			}

			args.erase(args.begin());
			auto argsSSV = Util::sv_vec_to_ssv_vec(args);

			MCF::simple_array<MCF::simple_string_view> argsArr;
			argsArr.data = argsSSV.data();
			argsArr.count = static_cast<uint64_t>(argsSSV.size());

			g_console.Reset(fullCmd.c_str());

			try {
				callback(argsArr, fullCmd.c_str(), &g_console);
			} catch (const std::exception& e) {
				spdlog::error("MCF: Exception in callback: {}", e.what());
			} catch (...) {
				spdlog::error("MCF: Unknown exception in callback");
			}

			g_console.PrintDefault();
			g_inHook = false;
			return 0xFFFF;
		} catch (const std::exception& e) {
			spdlog::error("MCF: Hook exception: {}", e.what());
			return passToVanilla();
		} catch (...) {
			spdlog::error("MCF: Hook unknown exception");
			return passToVanilla();
		}
	}

	// Cienki wrapper z SEH – zabezpiecza przed AV / innymi wyjątkami strukturalnymi
	// (funkcja nie ma lokalnych obiektów z destruktorem, więc nie łamie C2712).
	static std::uint32_t __fastcall HookedExecuteCommand(void* a_param1, void* a_param2)
	{
		__try {
			return HookedExecuteCommand_Impl(a_param1, a_param2);
		} 		__except (EXCEPTION_EXECUTE_HANDLER) {
			spdlog::error("MCF: SEH in HookedExecuteCommand (access violation or similar), passing to vanilla");
			g_inHook = false;
			return g_originalFunc ? g_originalFunc(a_param1, a_param2) : 0xFFFF;
		}
	}

	// Dla Twojej wersji gry: ładuje mapę offset→ID z Address Library i zwraca REL::ID dla podanego RVA (offset od bazy .exe).
	// Można wywołać np. z offsetem znalezionym w IDA – wtedy zobaczysz który ID ma Address Library dla tej wersji.
	static std::optional<std::uint64_t> GetRELIDForOffset(std::size_t a_rva)
	{
		try {
			auto* o2i = REL::Offset2ID::GetSingleton();
			if (o2i->size() == 0) {
				o2i->load_v5();
				if (o2i->size() == 0)
					o2i->load_v2();
			}
			if (o2i->size() == 0) return std::nullopt;
			return o2i->get_id(a_rva);
		} catch (...) {
			return std::nullopt;
		}
	}

	static void InstallHooks()
	{
		if (g_hookInstalled) return;

		spdlog::info("MCF: Installing console hook (REL::ID(65829))...");
		try {
			REL::Relocation<std::uintptr_t> target(REL::ID(65829));
			const std::uintptr_t targetAddr = target.address();
			spdlog::info("MCF: Target address = {:X}", targetAddr);

			// SFSE/CommonLibSF trampoline – kopiujemy 5 bajtów (pierwsza instrukcja), potem write_jmp5.
			auto& trampoline = REL::GetTrampoline();
			if (trampoline.empty()) {
				spdlog::warn("MCF: Trampoline empty (SFSE Init with trampoline=true?). Creating 128 bytes.");
				trampoline.create(128, nullptr);
			}
			constexpr std::size_t kPrologue = 5;
			void* trampMem = trampoline.allocate(kPrologue + sizeof(REL::ASM::JMP14));
			if (!trampMem) {
				spdlog::error("MCF: Trampoline allocate failed");
				return;
			}
			std::uintptr_t trampAddr = reinterpret_cast<std::uintptr_t>(trampMem);
			std::memcpy(trampMem, reinterpret_cast<const void*>(targetAddr), kPrologue);
			REL::ASM::JMP14 jmpBack(targetAddr + kPrologue);
			REL::WriteSafeData(trampAddr + kPrologue, jmpBack);
			trampoline.write_jmp5(targetAddr, reinterpret_cast<std::uintptr_t>(&HookedExecuteCommand));
			g_originalFunc = reinterpret_cast<ExecuteCommandFunc>(trampAddr);

			g_hookInstalled = true;
			spdlog::info("MCF: Hook installed successfully (SFSE/REL trampoline, 5-byte).");
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
