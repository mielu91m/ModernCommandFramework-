#include "Pch.h"
#include "Commands.h"
#include <sstream>
#include <thread>
#include <algorithm>

// SFSEPlugin_Version definition
DLLEXPORT SFSE::PluginVersionData SFSEPlugin_Version{
	SFSE::PluginVersionData::kVersion,
	Plugin::Version,
	"ModernCommandFramework",
	"Mielu91",
	1, // addressIndependence (UsesSigScanning)
	1, // structureCompatibility (HasNoStructUse)
	{SFSE::RUNTIME_LATEST.pack()},
	0, // xseMinimum
	0, // reservedNonBreaking
	0  // reservedBreaking
};

// LogMessage is defined in main.cpp
extern void LogMessage(const std::string& message);

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

	std::vector<std::string_view> str_split(const std::string_view& s, const std::string_view& delimiter, const std::optional<char>& escapeChar)
	{
		std::vector<std::string_view> substrings;
		size_t start = 0;
		size_t end = 0;
		bool escaped = false;

		while (end < s.length()) {
			if (escapeChar.has_value() && s[end] == escapeChar) {
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

		// Strip quotes from substrings (both single and double quotes)
		for (auto& substring : substrings) {
			if (substring.size() > 1) {
				char front = substring.front();
				char back = substring.back();
				if ((front == '\'' && back == '\'') || (front == '"' && back == '"')) {
					substring.remove_prefix(1);
					substring.remove_suffix(1);
				}
			}
		}

		return substrings;
	}

	std::string str_tolower(const std::string_view s)
	{
		std::string result(s);
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
		return result;
	}
}

namespace Commands
{
	static constexpr bool kEnableLegacyExecuteHook = false;
	static constexpr bool kEnableScriptResolverProbeHook = false;
	static constexpr bool kEnableDispatcherCallsiteHook = false;
	static constexpr bool kEnableScriptTableInjection = true;
	static constexpr std::uint64_t kScriptGetConsoleCommandsId = 896666;
	static constexpr std::uint64_t kScriptGetScriptCommandsId = 896669;
	static bool g_scriptSafInstalled = false;
	static bool g_consoleSafInstalled = false;
	static bool g_scriptMcfClearcacheInstalled = false;
	static std::string g_scriptSafName;
	static std::string g_scriptSafHelp;
	static std::array<RE::SCRIPT_PARAMETER, 8> g_scriptSafParams{};
	static std::atomic<bool> g_scriptInstallWorkerStarted = false;
	static std::atomic<std::uint32_t> g_dispatcherHookHitCount = 0;
	class Interface;
	extern std::mutex regLock;
	extern std::unordered_map<std::string, MCF::CommandCallback> registrations;
	extern Interface* intfc;

	std::uint64_t ResolveIdWithFallback(std::uint64_t commonlibId, std::uint64_t fallbackId, const char* name)
	{
		if (commonlibId != 0) {
			LogMessage(std::format("MCF: Using commonlib ID {} = {}", name, commonlibId));
			return commonlibId;
		}

		LogMessage(std::format("MCF: Using fallback ID {} = {}", name, fallbackId));
		return fallbackId;
	}

	using ScriptResolverFunc = bool (*)(void*);
	ScriptResolverFunc OriginalScriptResolverCallA = nullptr;
	ScriptResolverFunc OriginalScriptResolverCallB = nullptr;
	using DispatcherCallsiteFunc = std::int32_t (*)(void*, void*);
	DispatcherCallsiteFunc OriginalDispatcherCallsite = nullptr;

	bool TryCopyToken(void* tokenCtx, std::string& outToken)
	{
		outToken.clear();
		if (!tokenCtx) {
			return false;
		}

		constexpr size_t kMaxTokenLen = 0x200;
		auto* ptr = static_cast<const char*>(tokenCtx);
		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
			return false;
		}

		const bool readable = (mbi.Protect & PAGE_READONLY) || (mbi.Protect & PAGE_READWRITE) || (mbi.Protect & PAGE_EXECUTE_READ) || (mbi.Protect & PAGE_EXECUTE_READWRITE);
		if ((mbi.State != MEM_COMMIT) || !readable) {
			return false;
		}

		size_t len = 0;
		while (len < kMaxTokenLen && ptr[len] != '\0') {
			++len;
		}
		outToken.assign(ptr, len);
		return true;
	}

	bool ScriptResolverProbeHookA(void* tokenCtx)
	{
		const auto result = OriginalScriptResolverCallA(tokenCtx);
		std::string token;
		if (TryCopyToken(tokenCtx, token) && Util::str_tolower(token) == "saf") {
			LogMessage(std::format("MCF: resolver call A hit for token='{}', result={}", token, result ? 1 : 0));
		}
		return result;
	}

	bool ScriptResolverProbeHookB(void* tokenCtx)
	{
		const auto result = OriginalScriptResolverCallB(tokenCtx);
		std::string token;
		if (TryCopyToken(tokenCtx, token) && Util::str_tolower(token) == "saf") {
			LogMessage(std::format("MCF: resolver call B hit for token='{}', result={}", token, result ? 1 : 0));
		}
		return result;
	}

	void LogIdDiagnostics()
	{
		LogMessage("MCF: ID diagnostics (commonlibsf)");

		const auto logIdState = [](const char* name, std::uint64_t id) {
			if (id == 0) {
				LogMessage(std::format("MCF: ID {} = 0 (INVALID/UNMAPPED in this commonlib build)", name));
			} else {
				LogMessage(std::format("MCF: ID {} = {} (non-zero, candidate for REL::ID)", name, id));
			}
		};

		// IDs used by legacy 1.15 logic in this project
		logIdState("Legacy.ExecuteHookBase", 166307);
		logIdState("GetConsoleHandle", 166314);
		logIdState("GetRefrFromHandle", 72399);
		logIdState("ConsoleReferencesManager", 879512);

		// Known script IDs from RE::IDs.h in current commonlibsf branch
		logIdState("RE::ID::Script::GetConsoleCommands", RE::ID::Script::GetConsoleCommands.id());
		logIdState("RE::ID::Script::GetScriptCommands", RE::ID::Script::GetScriptCommands.id());
		logIdState("MCF::Fallback::Script::GetConsoleCommands", kScriptGetConsoleCommandsId);
		logIdState("MCF::Fallback::Script::GetScriptCommands", kScriptGetScriptCommandsId);

		[[maybe_unused]] const auto resolvedConsoleCommands = ResolveIdWithFallback(
			RE::ID::Script::GetConsoleCommands.id(),
			kScriptGetConsoleCommandsId,
			"Script::GetConsoleCommands");
		[[maybe_unused]] const auto resolvedScriptCommands = ResolveIdWithFallback(
			RE::ID::Script::GetScriptCommands.id(),
			kScriptGetScriptCommandsId,
			"Script::GetScriptCommands");
	}

	RE::NiPointer<RE::TESObjectREFR> GetRefrFromHandle(uint32_t handle)
	{
		RE::NiPointer<RE::TESObjectREFR> result;
		REL::Relocation<void(RE::NiPointer<RE::TESObjectREFR>&, uint32_t*)> func(REL::Offset(0xEA7360));
		func(result, &handle);
		return result;
	}

	RE::NiPointer<RE::TESObjectREFR> GetConsoleRefr()
	{
		REL::Relocation<uint64_t**> consoleReferencesManager(REL::Offset(0x5D98D70));
		REL::Relocation<uint32_t* (uint64_t*, uint32_t*)> GetConsoleHandle(REL::Offset(0x324F780));
		uint32_t outId = 0;
		GetConsoleHandle(*consoleReferencesManager, &outId);
		return GetRefrFromHandle(outId);
	}

	class Interface : public MCF::ConsoleInterface
	{
	public:
		virtual RE::NiPointer<RE::TESObjectREFR> GetSelectedReference() {
			return GetConsoleRefr();
		}

		virtual RE::TESForm* HexStrToForm(const MCF::simple_string_view& a_str) {
			uint32_t formId;
			try {
				formId = std::stoul(std::string{ a_str.get() }, nullptr, 16);
			} catch (std::exception) {
				return nullptr;
			}

			return RE::TESForm::LookupByID(formId);
		}

		virtual void PrintLn(const MCF::simple_string_view& a_txt) {
			if (log != nullptr) {
				if (!printedDefault) {
					log->PrintLine(defText);
					printedDefault = true;
				}
				log->PrintLine(std::string{ a_txt.get() }.c_str());
			}
		}

		virtual void PreventDefaultPrint() {
			if (!this) return;
			LogMessage(std::format("MCF: PreventDefaultPrint called, this={}", static_cast<void*>(this)));
			printedDefault = true;
		}

		void PrintDefault() {
			if (!printedDefault && log != nullptr) {
				log->PrintLine(defText);
				printedDefault = true;
			}
		}

		void Reset(const char* a_txt) {
			LogMessage(std::format("MCF: Reset called, this={}, vtable={}",
				static_cast<void*>(this),
				this ? static_cast<void*>(*(reinterpret_cast<void**>(this))) : static_cast<void*>(nullptr)));
			if (log == nullptr) {
				log = RE::ConsoleLog::GetSingleton();
			}
			defText = a_txt;
			printedDefault = false;
		}

		RE::ConsoleLog* log = nullptr;
		const char* defText = nullptr;
		bool printedDefault = false;
	};

	typedef void (*ExecuteCommandFunc)(void*, char*);

	ExecuteCommandFunc OriginalExecuteCommand;
	std::mutex regLock;
	std::unordered_map<std::string, MCF::CommandCallback> registrations;
	static Interface* intfc = nullptr;

	void InitializeInterface() {
		if (!intfc) {
			intfc = new Interface();
			LogMessage(std::format("MCF: Interface initialized, intfc={}, vtable={}",
				static_cast<void*>(intfc),
				static_cast<void*>(*(reinterpret_cast<void**>(intfc)))));
		}
	}

	bool TryCopyCommandFromDispatcherCtx(void* dispatcherCtx, std::string& outCmd)
	{
		outCmd.clear();
		if (!dispatcherCtx) {
			return false;
		}

		auto* text = reinterpret_cast<const char*>(dispatcherCtx) + 0x4;
		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(text, &mbi, sizeof(mbi)) == 0) {
			return false;
		}

		const bool readable = (mbi.Protect & PAGE_READONLY) || (mbi.Protect & PAGE_READWRITE) ||
			(mbi.Protect & PAGE_EXECUTE_READ) || (mbi.Protect & PAGE_EXECUTE_READWRITE);
		if ((mbi.State != MEM_COMMIT) || !readable) {
			return false;
		}

		constexpr size_t kMaxLen = 0x200;
		size_t len = 0;
		while (len < kMaxLen && text[len] != '\0') {
			++len;
		}
		if (len == 0 || len >= kMaxLen) {
			return false;
		}

		outCmd.assign(text, len);
		return true;
	}

	std::int32_t DispatcherCallsiteHook(void* a_ctx, void* a_dispatcherCtx)
	{
		std::string cmdLine;
		const bool gotCmd = TryCopyCommandFromDispatcherCtx(a_dispatcherCtx, cmdLine);
		const auto hit = ++g_dispatcherHookHitCount;
		if (hit <= 20) {
			LogMessage(std::format("MCF: Dispatcher hook hit #{} cmd='{}' gotCmd={}", hit, gotCmd ? cmdLine : "<unreadable>", gotCmd ? 1 : 0));
		}

		if (!gotCmd || cmdLine.empty()) {
			return OriginalDispatcherCallsite(a_ctx, a_dispatcherCtx);
		}

		auto args = Util::str_split(cmdLine, " ", '\"');
		if (args.empty()) {
			return OriginalDispatcherCallsite(a_ctx, a_dispatcherCtx);
		}

		if (Util::str_tolower(args[0]) != "saf") {
			return OriginalDispatcherCallsite(a_ctx, a_dispatcherCtx);
		}

		std::unique_lock l{ regLock };
		if (auto iter = registrations.find("saf"); iter != registrations.end()) {
			args.erase(args.begin());
			auto argsSSV = Util::sv_vec_to_ssv_vec(args);
			MCF::simple_array<MCF::simple_string_view> argsArr(argsSSV);
			if (intfc) intfc->Reset(cmdLine.c_str());
			LogMessage(std::format("MCF: Dispatcher callsite handled '{}'", cmdLine));
			iter->second(argsArr, cmdLine.c_str(), intfc);
			if (intfc) intfc->PrintDefault();
			return 0xFFFF;
		}

		LogMessage("MCF: Dispatcher callsite saw saf, but no registration found");
		return OriginalDispatcherCallsite(a_ctx, a_dispatcherCtx);
	}

	static bool IsValidCommandName(const char* ptr)
	{
		if (!ptr) return false;
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
		if (mbi.State != MEM_COMMIT) return false;
		bool readable = (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
		if (!readable) return false;
		size_t len = 0;
		while (len < 64 && ptr[len]) {
			auto c = static_cast<unsigned char>(ptr[len]);
			if (c < 0x20 || c > 0x7E) return false;
			++len;
		}
		return len >= 1 && len < 64;
	}

	static bool IsLikelyScriptEntry(uintptr_t addr)
	{
		if (!IsValidCommandName(*reinterpret_cast<const char**>(addr)))
			return false;
		uint32_t output = *reinterpret_cast<const uint32_t*>(addr + 0x10);
		if (output > 10)
			return false;
		return true;
	}

	static std::span<RE::SCRIPT_FUNCTION> FindScriptTableByScanInData()
	{
		auto module = REL::Module::GetSingleton();
		auto dataSeg = module->segment(REL::Segment::data);
		if (dataSeg.size() == 0) return {};

		static constexpr size_t kEntrySize = 0x58;
		static constexpr size_t kMinConsecutive = 5;
		const auto start = dataSeg.address();
		const auto end = start + dataSeg.size();

		for (uintptr_t addr = start; addr < end - (kEntrySize * kMinConsecutive) + 8; addr += 8) {
			bool match = true;
			for (size_t i = 0; i < kMinConsecutive; ++i) {
				if (!IsLikelyScriptEntry(addr + i * kEntrySize)) { match = false; break; }
			}
			if (match) {
				LogMessage(std::format("MCF: ScriptCommands table FOUND by pattern scan in .data at +0x{:X}", addr - module->base()));
				return { reinterpret_cast<RE::SCRIPT_FUNCTION*>(addr), RE::Script::kNumScriptCommands };
			}
		}
		LogMessage("MCF: ScriptCommands table NOT FOUND in .data");
		return {};
	}

	static std::span<RE::SCRIPT_FUNCTION> FindScriptTableByScan()
	{
		auto module = REL::Module::GetSingleton();
		auto rdataSeg = module->segment(REL::Segment::rdata);
		if (rdataSeg.size() == 0) return {};

		static constexpr size_t kEntrySize = 0x58;
		static constexpr size_t kMinConsecutive = 5;
		const auto start = rdataSeg.address();
		const auto end = start + rdataSeg.size();

		for (uintptr_t addr = start; addr < end - (kEntrySize * kMinConsecutive) + 8; addr += 8) {
			bool match = true;
			for (size_t i = 0; i < kMinConsecutive; ++i) {
				if (!IsLikelyScriptEntry(addr + i * kEntrySize)) { match = false; break; }
			}
			if (match) {
				LogMessage(std::format("MCF: ScriptCommands table FOUND by pattern scan at +0x{:X}", addr - module->base()));
				return { reinterpret_cast<RE::SCRIPT_FUNCTION*>(addr), RE::Script::kNumScriptCommands };
			}
		}
		LogMessage("MCF: ScriptCommands table NOT FOUND by pattern scan");
		return {};
	}

	std::span<RE::SCRIPT_FUNCTION> GetScriptCommandsTable()
	{
		auto commands = RE::Script::GetScriptCommands();
		auto* raw = commands.data();
		if (raw) {
			for (size_t i = 0; i < 20 && i < RE::Script::kNumScriptCommands; ++i) {
				auto name = raw[i].functionName;
				if (name && strlen(name) > 0 && strlen(name) < 64) {
					return { raw, RE::Script::kNumScriptCommands };
				}
			}
			return {};
		}
		LogMessage("MCF: RE::Script::GetScriptCommands() returned null - trying .data scan...");
		return FindScriptTableByScanInData();
	}

	static std::span<RE::SCRIPT_FUNCTION> GetConsoleCommandsTable()
	{
		auto commands = RE::Script::GetConsoleCommands();
		auto* raw = commands.data();
		if (raw) {
			for (size_t i = 0; i < 10 && i < RE::Script::kNumConsoleCommands; ++i) {
				auto name = raw[i].functionName;
				if (name && strlen(name) > 0 && strlen(name) < 64) {
					return { raw, RE::Script::kNumConsoleCommands };
				}
			}
		}
		return {};
	}

	bool MCFScriptMcfClearcacheExecute(const RE::SCRIPT_PARAMETER*, const char* a_scriptText, RE::TESObjectREFR*, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*, float* a_result, std::uint32_t*)
	{
		std::string cmdLine = a_scriptText ? a_scriptText : "";
		LogMessage(std::format("MCF: ScriptCommands mcf_clearcache execute entry hit, raw='{}'", cmdLine));

		auto args = Util::str_split(cmdLine, " ", '\"');
		if (args.empty()) {
			args.push_back("mcf_clearcache");
		}

		// Normalize input so callback always receives args without "mcf_clearcache".
		const auto lower0 = Util::str_tolower(args[0]);
		if (lower0 == "mcf_clearcache") {
			args.erase(args.begin());
		} else {
			auto it = std::find_if(args.begin(), args.end(), [](const std::string_view v) {
				return Util::str_tolower(v) == "mcf_clearcache";
			});
			if (it != args.end()) {
				args.erase(args.begin(), std::next(it));
			}
		}

		std::unique_lock l{ regLock };
		if (auto iter = registrations.find("mcf_clearcache"); iter != registrations.end()) {
			auto argsSSV = Util::sv_vec_to_ssv_vec(args);
			MCF::simple_array<MCF::simple_string_view> argsArr(argsSSV);
			if (intfc) intfc->Reset(cmdLine.c_str());
			LogMessage(std::format("MCF: ScriptCommands dispatching mcf_clearcache with {} args", argsArr.size()));
			iter->second(argsArr, cmdLine.c_str(), intfc);
			if (intfc) intfc->PrintDefault();
			if (a_result) {
				*a_result = 1.0f;
			}
			LogMessage(std::format("MCF: ScriptCommands mcf_clearcache execute handled '{}'", cmdLine));
			return true;
		}

		LogMessage("MCF: ScriptCommands execute called for mcf_clearcache, but no registration found");
		return false;
	}

	bool MCFScriptSafExecute(const RE::SCRIPT_PARAMETER*, const char* a_scriptText, RE::TESObjectREFR*, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*, float* a_result, std::uint32_t*)
	{
		auto isMostlyPrintable = [](const std::string& s) {
			if (s.empty()) {
				return false;
			}
			std::size_t printable = 0;
			for (unsigned char c : s) {
				if ((c >= 32 && c <= 126) || c == '\t') {
					++printable;
				}
			}
			return printable * 100 >= s.size() * 85;
		};

		auto readLastConsoleHistorySaf = []() -> std::string {
			const auto path = std::filesystem::path(std::getenv("USERPROFILE")) / "Documents" / "My Games" / "Starfield" / "StarfieldConsoleHistory.log";
			std::ifstream in(path);
			if (!in.is_open()) {
				return {};
			}

			std::string line;
			std::string lastSaf;
			while (std::getline(in, line)) {
				if (line.empty()) {
					continue;
				}
				auto lower = Util::str_tolower(line);
				if (lower.rfind("saf ", 0) == 0 || lower == "saf") {
					lastSaf = line;
				}
			}
			return lastSaf;
		};

		std::string cmdLine = a_scriptText ? a_scriptText : "";
		if (!isMostlyPrintable(cmdLine)) {
			auto historyCmd = readLastConsoleHistorySaf();
			if (!historyCmd.empty()) {
				LogMessage(std::format("MCF: ScriptCommands raw looked binary, using console history '{}'", historyCmd));
				cmdLine = historyCmd;
			}
		}
		if (cmdLine.empty()) {
			cmdLine = "saf";
		}

		LogMessage(std::format("MCF: ScriptCommands execute entry hit, raw='{}'", cmdLine));
		auto args = Util::str_split(cmdLine, " ", '\"');
		if (args.empty()) {
			args.push_back("saf");
		}

		// Normalize input so SAF callback always receives args without "saf".
		const auto lower0 = Util::str_tolower(args[0]);
		if (lower0 == "saf") {
			args.erase(args.begin());
		} else {
			auto it = std::find_if(args.begin(), args.end(), [](const std::string_view v) {
				return Util::str_tolower(v) == "saf";
			});
			if (it != args.end()) {
				args.erase(args.begin(), std::next(it));
			}
		}
		if (args.empty()) {
			// Keep explicit empty-args semantics for plain "saf".
			cmdLine = "saf";
		} else {
			std::string rebuilt = "saf";
			for (auto a : args) {
				rebuilt += " ";
				rebuilt += std::string(a);
			}
			cmdLine = rebuilt;
		}

		std::unique_lock l{ regLock };
		if (auto iter = registrations.find("saf"); iter != registrations.end()) {
			auto argsSSV = Util::sv_vec_to_ssv_vec(args);
			MCF::simple_array<MCF::simple_string_view> argsArr(argsSSV);
			if (intfc) intfc->Reset(cmdLine.c_str());
			LogMessage(std::format("MCF: ScriptCommands dispatching '{}' with {} args", cmdLine, argsArr.size()));
			iter->second(argsArr, cmdLine.c_str(), intfc);
			if (intfc) intfc->PrintDefault();
			if (a_result) {
				*a_result = 1.0f;
			}
			LogMessage(std::format("MCF: ScriptCommands execute handled '{}'", cmdLine));
			return true;
		}

		LogMessage("MCF: ScriptCommands execute called for saf, but no registration found");
		return false;
	}

	bool TryInstallMcfClearcacheScriptCommand(bool logFailures)
	{
		if (g_scriptMcfClearcacheInstalled) {
			return true;
		}

		auto commands = GetScriptCommandsTable();
		if (commands.empty()) {
			if (logFailures) {
				LogMessage("MCF: ScriptCommands table unavailable - blocking all writes to avoid memory corruption");
			}
			return false;
		}
		RE::SCRIPT_FUNCTION* templateEntry = nullptr;
		RE::SCRIPT_FUNCTION* targetEntry = nullptr;
		RE::SCRIPT_FUNCTION* nullExecEntry = nullptr;
		RE::SCRIPT_FUNCTION* unusedNamedEntry = nullptr;
		std::size_t emptyNameCount = 0;
		std::size_t nullExecCount = 0;
		int bestTemplateScore = std::numeric_limits<int>::min();
		std::string chosenTemplateParams;
		const auto scoreTemplate = [](const RE::SCRIPT_FUNCTION& command) {
			if (!command.functionName || !command.executeFunction) {
				return std::numeric_limits<int>::min();
			}

			int score = 0;
			const auto nameLower = Util::str_tolower(command.functionName);
			if (nameLower == "help" || nameLower == "cqf") {
				score += 120;
			}
			if (nameLower == "setstage" || nameLower == "startquest" || nameLower == "sqs") {
				score -= 200;
			}

			if (command.referenceFunction == 0) {
				score += 15;
			}
			if (command.numParams >= 2 && command.numParams <= 4) {
				score += 20;
			} else if (command.numParams == 1) {
				score += 5;
			}

			for (std::uint16_t i = 0; i < command.numParams && command.params; ++i) {
				const char* p = command.params[i].paramName ? command.params[i].paramName : "";
				auto pLower = Util::str_tolower(p);
				if (pLower.find("quest") != std::string::npos || pLower.find("ref") != std::string::npos ||
					pLower.find("actor") != std::string::npos || pLower.find("cell") != std::string::npos ||
					pLower.find("object") != std::string::npos || pLower.find("alias") != std::string::npos) {
					score -= 80;
				}
				if (pLower.find("string") != std::string::npos || pLower.find("text") != std::string::npos ||
					pLower.find("name") != std::string::npos || pLower.find("command") != std::string::npos ||
					pLower.find("file") != std::string::npos) {
					score += 40;
				}
			}

			return score;
		};

		for (auto& command : commands) {
			if (command.functionName && command.executeFunction) {
				const int score = scoreTemplate(command);
				if (score > bestTemplateScore) {
					bestTemplateScore = score;
					templateEntry = std::addressof(command);
					chosenTemplateParams.clear();
					for (std::uint16_t i = 0; i < command.numParams && command.params; ++i) {
						if (!chosenTemplateParams.empty()) {
							chosenTemplateParams += ", ";
						}
						chosenTemplateParams += (command.params[i].paramName ? command.params[i].paramName : "<null>");
					}
				}
			}
			if (command.functionName && _stricmp(command.functionName, "mcf_clearcache") == 0) {
				targetEntry = std::addressof(command);
				break;
			}
			if ((!command.functionName || command.functionName[0] == '\0')) {
				++emptyNameCount;
			}
			if (!command.executeFunction) {
				++nullExecCount;
				if (!nullExecEntry) {
					nullExecEntry = std::addressof(command);
				}
			}
			if (command.functionName) {
				auto nameLower = Util::str_tolower(command.functionName);
				if (!unusedNamedEntry && (nameLower.find("unused") != std::string::npos || nameLower.find("reserved") != std::string::npos)) {
					unusedNamedEntry = std::addressof(command);
				}
			}
		}

		bool hijackingLiveSlot = false;
		if (targetEntry && targetEntry->executeFunction != &MCFScriptMcfClearcacheExecute) {
			targetEntry = nullptr;
		}
		if (!targetEntry && templateEntry) {
			targetEntry = templateEntry;
			hijackingLiveSlot = true;
			LogMessage(std::format("MCF: Forcing live ScriptCommands slot hijack for mcf_clearcache '{}'", templateEntry->functionName ? templateEntry->functionName : "<null>"));
		}
		if (!targetEntry && nullExecEntry) {
			targetEntry = nullExecEntry;
			LogMessage("MCF: Reusing ScriptCommands slot with null executeFunction for mcf_clearcache");
		}
		if (!targetEntry && unusedNamedEntry) {
			targetEntry = unusedNamedEntry;
			LogMessage(std::format("MCF: Reusing ScriptCommands slot named '{}' for mcf_clearcache", targetEntry->functionName ? targetEntry->functionName : "<null>"));
		}

		if (!templateEntry || !targetEntry) {
			if (emptyNameCount == RE::Script::kNumScriptCommands && nullExecCount == RE::Script::kNumScriptCommands) {
				if (logFailures) {
					LogMessage("MCF: ScriptCommands table not initialized yet (all entries empty)");
				}
			} else if (logFailures) {
				LogMessage(std::format(
					"MCF: Failed to install mcf_clearcache into ScriptCommands table (no template/slot). emptyNameCount={}, nullExecCount={}",
					emptyNameCount,
					nullExecCount));
			}
			return false;
		}

		std::string scriptName = "mcf_clearcache";
		std::string scriptHelp = "Clear SFSE cache and optionally save cache";

		DWORD oldProtect = 0;
		if (!VirtualProtect(targetEntry, sizeof(RE::SCRIPT_FUNCTION), PAGE_EXECUTE_READWRITE, &oldProtect)) {
			LogMessage("MCF: VirtualProtect failed for ScriptCommands entry");
			return false;
		}

		*targetEntry = *templateEntry;
		targetEntry->functionName = scriptName.c_str();
		targetEntry->shortName = scriptName.c_str();
		targetEntry->helpString = scriptHelp.c_str();
		const std::uint32_t paramType = (templateEntry->numParams > 0 && templateEntry->params) ? templateEntry->params[0].paramType : 0;
		static std::array<RE::SCRIPT_PARAMETER, 8> scriptParams{};
		for (std::size_t i = 0; i < scriptParams.size(); ++i) {
			scriptParams[i].paramName = "Arg (Optional)";
			scriptParams[i].paramType = paramType;
			scriptParams[i].optional = true;
		}
		targetEntry->numParams = static_cast<std::uint16_t>(scriptParams.size());
		targetEntry->params = scriptParams.data();
		targetEntry->executeFunction = &MCFScriptMcfClearcacheExecute;

		DWORD tmpProtect = 0;
		(void)VirtualProtect(targetEntry, sizeof(RE::SCRIPT_FUNCTION), oldProtect, &tmpProtect);

		g_scriptMcfClearcacheInstalled = true;
		LogMessage(std::format(
			"MCF: Installed mcf_clearcache entry into ScriptCommands table (template='{}', output={}, refFn={}, numParams={}, score={}, hijackLiveSlot={}, params=[{}])",
			templateEntry->functionName ? templateEntry->functionName : "<null>",
			templateEntry->output,
			templateEntry->referenceFunction,
			templateEntry->numParams,
			bestTemplateScore,
			hijackingLiveSlot ? 1 : 0,
			chosenTemplateParams));
		return true;
	}

	void InstallMcfClearcacheScriptCommand()
	{
		if (TryInstallMcfClearcacheScriptCommand(true)) {
			return;
		}

		if (!g_scriptInstallWorkerStarted.exchange(true)) {
			LogMessage("MCF: Starting delayed ScriptCommands install retry worker for mcf_clearcache");
			for (int attempt = 1; attempt <= 120 && !g_scriptMcfClearcacheInstalled; ++attempt) {
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				if (TryInstallMcfClearcacheScriptCommand(false)) {
					LogMessage(std::format("MCF: Delayed ScriptCommands install for mcf_clearcache succeeded on attempt {}", attempt));
					g_scriptInstallWorkerStarted = false;
					return;
				}
			}
			LogMessage("MCF: Delayed ScriptCommands install for mcf_clearcache failed after 120 attempts");
			g_scriptInstallWorkerStarted = false;
		}
	}

	bool TryInstallSafScriptCommand(bool logFailures)
	{
		if (g_scriptSafInstalled) {
			return true;
		}

		auto commands = GetScriptCommandsTable();
		if (commands.empty()) {
			if (logFailures) {
				LogMessage("MCF: ScriptCommands table unavailable - blocking all writes to avoid memory corruption");
			}
			return false;
		}
		RE::SCRIPT_FUNCTION* templateEntry = nullptr;
		RE::SCRIPT_FUNCTION* targetEntry = nullptr;
		RE::SCRIPT_FUNCTION* nullExecEntry = nullptr;
		RE::SCRIPT_FUNCTION* unusedNamedEntry = nullptr;
		std::size_t emptyNameCount = 0;
		std::size_t nullExecCount = 0;
		int bestTemplateScore = std::numeric_limits<int>::min();
		std::string chosenTemplateParams;
		const auto scoreTemplate = [](const RE::SCRIPT_FUNCTION& command) {
			if (!command.functionName || !command.executeFunction) {
				return std::numeric_limits<int>::min();
			}

			int score = 0;
			const auto nameLower = Util::str_tolower(command.functionName);
			if (nameLower == "help" || nameLower == "cqf") {
				score += 120;
			}
			if (nameLower == "setstage" || nameLower == "startquest" || nameLower == "sqs") {
				score -= 200;
			}

			if (command.referenceFunction == 0) {
				score += 15;
			}
			if (command.numParams >= 2 && command.numParams <= 4) {
				score += 20;
			} else if (command.numParams == 1) {
				score += 5;
			}

			for (std::uint16_t i = 0; i < command.numParams && command.params; ++i) {
				const char* p = command.params[i].paramName ? command.params[i].paramName : "";
				auto pLower = Util::str_tolower(p);
				if (pLower.find("quest") != std::string::npos || pLower.find("ref") != std::string::npos ||
					pLower.find("actor") != std::string::npos || pLower.find("cell") != std::string::npos ||
					pLower.find("object") != std::string::npos || pLower.find("alias") != std::string::npos) {
					score -= 80;
				}
				if (pLower.find("string") != std::string::npos || pLower.find("text") != std::string::npos ||
					pLower.find("name") != std::string::npos || pLower.find("command") != std::string::npos ||
					pLower.find("file") != std::string::npos) {
					score += 40;
				}
			}

			return score;
		};

		for (auto& command : commands) {
			if (command.functionName && command.executeFunction) {
				const int score = scoreTemplate(command);
				if (score > bestTemplateScore) {
					bestTemplateScore = score;
					templateEntry = std::addressof(command);
					chosenTemplateParams.clear();
					for (std::uint16_t i = 0; i < command.numParams && command.params; ++i) {
						if (!chosenTemplateParams.empty()) {
							chosenTemplateParams += ", ";
						}
						chosenTemplateParams += (command.params[i].paramName ? command.params[i].paramName : "<null>");
					}
				}
			}
			if (command.functionName && _stricmp(command.functionName, "saf") == 0) {
				targetEntry = std::addressof(command);
				break;
			}
			if ((!command.functionName || command.functionName[0] == '\0')) {
				++emptyNameCount;
			}
			if (!command.executeFunction) {
				++nullExecCount;
				if (!nullExecEntry) {
					nullExecEntry = std::addressof(command);
				}
			}
			if (command.functionName) {
				auto nameLower = Util::str_tolower(command.functionName);
				if (!unusedNamedEntry && (nameLower.find("unused") != std::string::npos || nameLower.find("reserved") != std::string::npos)) {
					unusedNamedEntry = std::addressof(command);
				}
			}
		}

		bool hijackingLiveSlot = false;
		if (targetEntry && targetEntry->executeFunction != &MCFScriptSafExecute) {
			// Ignore pre-existing saf entry if it is not ours; force a live slot hijack path.
			targetEntry = nullptr;
		}
		if (!targetEntry && templateEntry) {
			// Force live-slot hijack for 1.16: empty slots appear non-executable in runtime dispatch.
			targetEntry = templateEntry;
			hijackingLiveSlot = true;
			LogMessage(std::format("MCF: Forcing live ScriptCommands slot hijack '{}'", templateEntry->functionName ? templateEntry->functionName : "<null>"));
		}
		if (!targetEntry && nullExecEntry) {
			targetEntry = nullExecEntry;
			LogMessage("MCF: Reusing ScriptCommands slot with null executeFunction");
		}
		if (!targetEntry && unusedNamedEntry) {
			targetEntry = unusedNamedEntry;
			LogMessage(std::format("MCF: Reusing ScriptCommands slot named '{}'", targetEntry->functionName ? targetEntry->functionName : "<null>"));
		}

		if (!templateEntry || !targetEntry) {
			if (emptyNameCount == RE::Script::kNumScriptCommands && nullExecCount == RE::Script::kNumScriptCommands) {
				if (logFailures) {
					LogMessage("MCF: ScriptCommands table not initialized yet (all entries empty)");
				}
			} else if (logFailures) {
				LogMessage(std::format(
					"MCF: Failed to install saf into ScriptCommands table (no template/slot). emptyNameCount={}, nullExecCount={}",
					emptyNameCount,
					nullExecCount));
			}
			return false;
		}

		g_scriptSafName = "saf";
		g_scriptSafHelp = "MCF custom command dispatcher";

		DWORD oldProtect = 0;
		if (!VirtualProtect(targetEntry, sizeof(RE::SCRIPT_FUNCTION), PAGE_EXECUTE_READWRITE, &oldProtect)) {
			LogMessage("MCF: VirtualProtect failed for ScriptCommands entry");
			return false;
		}

		*targetEntry = *templateEntry;
		targetEntry->functionName = g_scriptSafName.c_str();
		targetEntry->shortName = g_scriptSafName.c_str();
		targetEntry->helpString = g_scriptSafHelp.c_str();
		// Allow SAF commands with multiple optional arguments, e.g. "saf play pen 5998".
		// Reuse template paramType (typically String) to satisfy parser expectations.
		const std::uint32_t safParamType = (templateEntry->numParams > 0 && templateEntry->params) ? templateEntry->params[0].paramType : 0;
		for (std::size_t i = 0; i < g_scriptSafParams.size(); ++i) {
			g_scriptSafParams[i].paramName = "Arg (Optional)";
			g_scriptSafParams[i].paramType = safParamType;
			g_scriptSafParams[i].optional = true;
		}
		targetEntry->numParams = static_cast<std::uint16_t>(g_scriptSafParams.size());
		targetEntry->params = g_scriptSafParams.data();
		targetEntry->executeFunction = &MCFScriptSafExecute;

		DWORD tmpProtect = 0;
		(void)VirtualProtect(targetEntry, sizeof(RE::SCRIPT_FUNCTION), oldProtect, &tmpProtect);

		g_scriptSafInstalled = true;
		LogMessage(std::format(
			"MCF: Installed saf entry into ScriptCommands table (template='{}', output={}, refFn={}, numParams={}, score={}, hijackLiveSlot={}, params=[{}])",
			templateEntry->functionName ? templateEntry->functionName : "<null>",
			templateEntry->output,
			templateEntry->referenceFunction,
			templateEntry->numParams,
			bestTemplateScore,
			hijackingLiveSlot ? 1 : 0,
			chosenTemplateParams));
		return true;
	}

	bool TryInstallSafConsoleCommand(bool logFailures)
	{
		if (g_consoleSafInstalled) {
			return true;
		}

		auto commands = GetConsoleCommandsTable();
		if (commands.empty()) {
			if (logFailures) {
				LogMessage("MCF: ConsoleCommands table unavailable");
			}
			return false;
		}

		RE::SCRIPT_FUNCTION* templateEntry = nullptr;
		RE::SCRIPT_FUNCTION* targetEntry = nullptr;
		int bestScore = std::numeric_limits<int>::min();

		for (auto& command : commands) {
			if (!command.functionName || !command.executeFunction) {
				if (!targetEntry) targetEntry = std::addressof(command);
				continue;
			}
			int score = 0;
			if (command.referenceFunction == 0) score += 10;
			if (command.editorFilter == 0) score += 5;
			if (command.numParams <= 2) score += 10;
			auto name = command.functionName ? command.functionName : "";
			if (strlen(name) > 0 && strlen(name) < 32) score += 10;
			if (score > bestScore) {
				bestScore = score;
				templateEntry = std::addressof(command);
			}
		}

		if (!targetEntry || !templateEntry) {
			if (logFailures) {
				LogMessage("MCF: No empty slot or template in ConsoleCommands table");
			}
			return false;
		}

		DWORD oldProtect = 0;
		if (!VirtualProtect(targetEntry, sizeof(RE::SCRIPT_FUNCTION), PAGE_EXECUTE_READWRITE, &oldProtect)) {
			LogMessage("MCF: VirtualProtect failed for ConsoleCommands entry");
			return false;
		}

		*targetEntry = *templateEntry;
		targetEntry->functionName = g_scriptSafName.c_str();
		targetEntry->shortName = g_scriptSafName.c_str();
		targetEntry->helpString = g_scriptSafHelp.c_str();
		// Allow SAF commands with multiple optional arguments, e.g. "saf play pen 5998".
		// Reuse template paramType (typically String) to satisfy parser expectations.
		const std::uint32_t safParamType = (templateEntry->numParams > 0 && templateEntry->params) ? templateEntry->params[0].paramType : 0;
		for (std::size_t i = 0; i < g_scriptSafParams.size(); ++i) {
			g_scriptSafParams[i].paramName = "Arg (Optional)";
			g_scriptSafParams[i].paramType = safParamType;
			g_scriptSafParams[i].optional = true;
		}
		targetEntry->numParams = static_cast<std::uint16_t>(g_scriptSafParams.size());
		targetEntry->params = g_scriptSafParams.data();
		targetEntry->executeFunction = &MCFScriptSafExecute;

		DWORD tmpProtect = 0;
		(void)VirtualProtect(targetEntry, sizeof(RE::SCRIPT_FUNCTION), oldProtect, &tmpProtect);

		g_consoleSafInstalled = true;
		LogMessage(std::format("MCF: Installed saf entry into ConsoleCommands table (template='{}', numParams={})", templateEntry->functionName ? templateEntry->functionName : "<null>", g_scriptSafParams.size()));
		return true;
	}

	void InstallSafScriptCommand()
	{
		if (TryInstallSafScriptCommand(true)) {
			return;
		}

		if (g_scriptInstallWorkerStarted.exchange(true)) {
			return;
		}

		std::thread([]() {
			LogMessage("MCF: Starting delayed ScriptCommands install retry worker");
			for (int attempt = 1; attempt <= 120 && !g_scriptSafInstalled; ++attempt) {
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				if (TryInstallSafScriptCommand(false)) {
					LogMessage(std::format("MCF: Delayed ScriptCommands install succeeded on attempt {}", attempt));
					g_scriptInstallWorkerStarted = false;
					return;
				}
			}
			LogMessage("MCF: Delayed ScriptCommands install failed after retries");
			g_scriptInstallWorkerStarted = false;
		}).detach();
	}

	void ExecuteCommand(void* arg1, char* a_cmd)
	{
		if (!a_cmd) {
			LogMessage("[MCF] ExecuteCommand: null cmd, forwarding");
			return OriginalExecuteCommand(arg1, a_cmd);
		}

		std::string cmdView(a_cmd);
		if (cmdView.empty()) {
			LogMessage("[MCF] ExecuteCommand: empty cmd, forwarding");
			return OriginalExecuteCommand(arg1, a_cmd);
		}

		auto args = Util::str_split(cmdView, " ", '\"');
		if (args.empty()) {
			LogMessage("[MCF] ExecuteCommand: no tokens, forwarding");
			return OriginalExecuteCommand(arg1, a_cmd);
		}
		LogMessage(std::format("[MCF] ExecuteCommand: parsed command='{}', args={}", args[0], args.size() - 1));
		
		std::unique_lock l{ regLock };
		if (auto iter = registrations.find(Util::str_tolower(args[0])); iter != registrations.end()) {
			LogMessage(std::format("[MCF] ExecuteCommand: handler found for '{}'", args[0]));

			args.erase(args.begin());
			auto argsSSV = Util::sv_vec_to_ssv_vec(args);
			MCF::simple_array<MCF::simple_string_view> argsArr(argsSSV);

			if (intfc) intfc->Reset(cmdView.c_str());
			LogMessage(std::format("[MCF] ExecuteCommand: calling handler for '{}'", cmdView));
			iter->second(argsArr, cmdView.c_str(), intfc);
			LogMessage(std::format("[MCF] ExecuteCommand: handler returned for '{}'", cmdView));
			if (intfc) intfc->PrintDefault();
			LogMessage("[MCF] ExecuteCommand: EXIT (handled)");
			return;
		}

		LogMessage("[MCF] ExecuteCommand: no handler, forwarding");
		return OriginalExecuteCommand(arg1, a_cmd);
	}

	void MCFClearCacheHandler(const MCF::simple_array<MCF::simple_string_view>& a_args, const char* a_fullString, MCF::ConsoleInterface* a_intfc)
	{
		LogMessage(std::format("[MCF] ClearCache: ENTRY - args.size()={}, fullString='{}'", a_args.size(), a_fullString));

		auto userProfile = std::filesystem::path(std::getenv("USERPROFILE"));
		int filesDeleted = 0;

		// Clear SFSE Logs
		auto sfsePath = userProfile / "Documents" / "My Games" / "Starfield" / "SFSE";
		auto logsPath = sfsePath / "Logs";
		if (std::filesystem::exists(logsPath)) {
			try {
				for (const auto& entry : std::filesystem::directory_iterator(logsPath)) {
					if (entry.is_regular_file()) {
						std::filesystem::remove(entry.path());
						filesDeleted++;
					}
				}
				if (a_intfc) a_intfc->PrintLn("Cleared SFSE Logs");
				LogMessage("[MCF] ClearCache: Cleared SFSE Logs");
			} catch (const std::exception& e) {
				if (a_intfc) a_intfc->PrintLn(std::format("Error clearing Logs: {}", e.what()).c_str());
				LogMessage(std::format("[MCF] ClearCache: Error clearing Logs - {}", e.what()));
			}
		}

		// Clear SFSE Crashlogs
		auto crashlogsPath = sfsePath / "Crashlogs";
		if (std::filesystem::exists(crashlogsPath)) {
			try {
				for (const auto& entry : std::filesystem::directory_iterator(crashlogsPath)) {
					if (entry.is_regular_file()) {
						std::filesystem::remove(entry.path());
						filesDeleted++;
					}
				}
				if (a_intfc) a_intfc->PrintLn("Cleared SFSE Crashlogs");
				LogMessage("[MCF] ClearCache: Cleared SFSE Crashlogs");
			} catch (const std::exception& e) {
				if (a_intfc) a_intfc->PrintLn(std::format("Error clearing Crashlogs: {}", e.what()).c_str());
				LogMessage(std::format("[MCF] ClearCache: Error clearing Crashlogs - {}", e.what()));
			}
		}

		if (a_intfc) a_intfc->PrintLn(std::format("Total files deleted: {}", filesDeleted).c_str());
		LogMessage(std::format("[MCF] ClearCache: EXIT - Total files deleted: {}", filesDeleted));
	}

	void InstallHooks()
	{
		LogMessage("MCF: InstallHooks() STARTED");
		InitializeInterface();
		LogIdDiagnostics();
		REL::Trampoline* trampoline = nullptr;
		if (kEnableLegacyExecuteHook || kEnableScriptResolverProbeHook || kEnableDispatcherCallsiteHook) {
			SFSE::AllocTrampoline(64);
			trampoline = &SFSE::GetTrampoline();
		}

		if (kEnableLegacyExecuteHook) {
			REL::Relocation<uintptr_t> hookLoc{ REL::Offset(0x324F2E0).address() + 0xD0 };
			OriginalExecuteCommand = reinterpret_cast<ExecuteCommandFunc>(trampoline->write_call<5>(hookLoc.address(), &ExecuteCommand));
			LogMessage("MCF: Installed command hook (legacy ExecuteHook)");
		} else {
			LogMessage("MCF: Execute hook disabled (stability mode)");
		}

		if (kEnableScriptResolverProbeHook) {
			REL::Relocation<uintptr_t> resolverCallA{ REL::Offset(0xC1594E) };
			REL::Relocation<uintptr_t> resolverCallB{ REL::Offset(0xC15A6E) };
			OriginalScriptResolverCallA = reinterpret_cast<ScriptResolverFunc>(trampoline->write_call<5>(resolverCallA.address(), &ScriptResolverProbeHookA));
			OriginalScriptResolverCallB = reinterpret_cast<ScriptResolverFunc>(trampoline->write_call<5>(resolverCallB.address(), &ScriptResolverProbeHookB));
			LogMessage("MCF: Installed resolver probe hooks at RVA 0xC1594E and 0xC15A6E");
		} else {
			LogMessage("MCF: Resolver probe hooks disabled");
		}

		if (kEnableDispatcherCallsiteHook) {
			REL::Relocation<uintptr_t> dispatcherCallsite{ REL::Offset(0xC14B73) };
			OriginalDispatcherCallsite = reinterpret_cast<DispatcherCallsiteFunc>(
				trampoline->write_call<5>(dispatcherCallsite.address(), &DispatcherCallsiteHook));
			LogMessage("MCF: Installed dispatcher callsite hook at RVA 0xC14B73");
		} else {
			LogMessage("MCF: Dispatcher callsite hook disabled");
		}

		LogMessage("MCF: InstallHooks() COMPLETED");
	}

	void RegisterCommand(const char* a_name, MCF::CommandCallback a_func)
	{
		std::unique_lock l{ regLock };
		std::string nameStr = Util::str_tolower(a_name);
		if (nameStr.empty()) {
			return;
		}

		if (registrations.contains(nameStr)) {
			LogMessage(std::format("MCF: A plugin tried to register command '{}', but that command has already been registered", a_name));
			return;
		}
		registrations.insert(std::make_pair(nameStr, a_func));
		LogMessage(std::format("MCF: Command {} registered", a_name));
		if (nameStr == "saf" && kEnableScriptTableInjection) {
			InstallSafScriptCommand();
		}
		// Note: mcf_clearcache does not install to ScriptCommands because the table is unavailable during InstallHooks()
		// It is available through the internal MCF system when dispatcher callsite hook is enabled
	}
}

DLLEXPORT void RegisterCommand(const char* a_name, MCF::CommandCallback a_func)
{
	Commands::RegisterCommand(a_name, a_func);
}
