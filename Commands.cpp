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
	// Set true locally when debugging resolver/install/handler tracing (extra LogMessage + disk I/O).
	static constexpr bool kVerboseMCFLogging = false;
	static constexpr std::uint64_t kScriptGetConsoleCommandsId = 896666;
	static constexpr std::uint64_t kScriptGetScriptCommandsId = 896669;
	static std::atomic<bool> g_scriptSafInstalled = false;
	static std::string g_scriptSafName;
	static std::string g_scriptSafHelp;
	static std::array<RE::SCRIPT_PARAMETER, 8> g_scriptSafParams{};
	static std::atomic<bool> g_scriptInstallWorkerStarted = false;
	static std::mutex g_scriptSafInstallMux;
	static std::atomic<std::uint32_t> g_dispatcherHookHitCount = 0;
	class Interface;
	extern std::mutex regLock;
	extern std::unordered_map<std::string, MCF::CommandCallback> registrations;
	extern Interface intfc;

	std::uint64_t ResolveIdWithFallback(std::uint64_t commonlibId, std::uint64_t fallbackId, const char* name)
	{
		if (commonlibId != 0) {
			LogMessage(std::format("MCF: Using commonlib ID {} = {}", name, commonlibId));
			return commonlibId;
		}

		LogMessage(std::format("MCF: Using fallback ID {} = {}", name, fallbackId));
		return fallbackId;
	}

	// Hot path: table lookups during retries run often — never log here.
	[[nodiscard]] std::uint64_t ResolvedScriptGetScriptCommandsId() noexcept
	{
		static const std::uint64_t id = []() noexcept -> std::uint64_t {
			const auto c = RE::ID::Script::GetScriptCommands.id();
			return c != 0 ? c : kScriptGetScriptCommandsId;
		}();
		return id;
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
		REL::Relocation<void(RE::NiPointer<RE::TESObjectREFR>&, uint32_t*)> func(REL::ID(72399));
		func(result, &handle);
		return result;
	}

	RE::NiPointer<RE::TESObjectREFR> GetConsoleRefr()
	{
		REL::Relocation<uint64_t**> consoleReferencesManager(REL::ID(879512));
		REL::Relocation<uint32_t* (uint64_t*, uint32_t*)> GetConsoleHandle(REL::ID(166314));
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
			printedDefault = true;
		}

		void PrintDefault() {
			if (!printedDefault && log != nullptr) {
				log->PrintLine(defText);
				printedDefault = true;
			}
		}

		void Reset(const char* a_txt) {
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
	Interface intfc;

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
		if constexpr (kVerboseMCFLogging) {
			if (hit <= 20) {
				LogMessage(std::format("MCF: Dispatcher hook hit #{} cmd='{}' gotCmd={}", hit, gotCmd ? cmdLine : "<unreadable>", gotCmd ? 1 : 0));
			}
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
			intfc.Reset(cmdLine.c_str());
			if constexpr (kVerboseMCFLogging) {
				LogMessage(std::format("MCF: Dispatcher callsite handled '{}'", cmdLine));
			}
			iter->second(argsArr, cmdLine.c_str(), &intfc);
			intfc.PrintDefault();
			return 0xFFFF;
		}

		LogMessage("MCF: Dispatcher callsite saw saf, but no registration found");
		return OriginalDispatcherCallsite(a_ctx, a_dispatcherCtx);
	}

	std::span<RE::SCRIPT_FUNCTION, RE::Script::kNumScriptCommands> GetScriptCommandsTable()
	{
		static REL::Relocation<RE::SCRIPT_FUNCTION(*)[RE::Script::kNumScriptCommands]> table{ REL::ID(ResolvedScriptGetScriptCommandsId()) };
		return std::span{ *table };
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
				if constexpr (kVerboseMCFLogging) {
					LogMessage(std::format("MCF: ScriptCommands raw looked binary, using console history '{}'", historyCmd));
				}
				cmdLine = historyCmd;
			}
		}
		if (cmdLine.empty()) {
			cmdLine = "saf";
		}

		if constexpr (kVerboseMCFLogging) {
			LogMessage(std::format("MCF: ScriptCommands execute entry hit, raw='{}'", cmdLine));
		}
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
			intfc.Reset(cmdLine.c_str());
			if constexpr (kVerboseMCFLogging) {
				LogMessage(std::format("MCF: ScriptCommands dispatching '{}' with {} args", cmdLine, argsArr.size()));
			}
			iter->second(argsArr, cmdLine.c_str(), &intfc);
			intfc.PrintDefault();
			if (a_result) {
				*a_result = 1.0f;
			}
			if constexpr (kVerboseMCFLogging) {
				LogMessage(std::format("MCF: ScriptCommands execute handled '{}'", cmdLine));
			}
			return true;
		}

		LogMessage("MCF: ScriptCommands execute called for saf, but no registration found");
		return false;
	}

	bool TryInstallSafScriptCommand(bool logFailures)
	{
		std::lock_guard lock(g_scriptSafInstallMux);

		if (g_scriptSafInstalled.load(std::memory_order_acquire)) {
			return true;
		}

		auto commands = GetScriptCommandsTable();
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
			// Avoid hijacking slots vanilla/scripts rely on (fewer mod conflicts + fewer broken console cmds).
			if (nameLower == "reloadresources") {
				score -= 400;
			}
			if (nameLower == "savegame" || nameLower == "loadgame" || nameLower == "quitgame") {
				score -= 350;
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

		g_scriptSafInstalled.store(true, std::memory_order_release);
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

	void InstallSafScriptCommand()
	{
		if (TryInstallSafScriptCommand(true)) {
			return;
		}

		if (g_scriptInstallWorkerStarted.exchange(true)) {
			return;
		}

		std::thread([]() {
			LogMessage("MCF: Starting delayed ScriptCommands install retry worker (SFSE init messages also retry)");
			// Long window for slow disks / Proton–Linux; early exit when install succeeds.
			constexpr int kMaxAttempts = 320;
			constexpr int kSleepMs = 450;
			for (int attempt = 1; attempt <= kMaxAttempts && !g_scriptSafInstalled.load(std::memory_order_acquire); ++attempt) {
				std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
				if (TryInstallSafScriptCommand(false)) {
					LogMessage(std::format("MCF: Delayed ScriptCommands install succeeded on attempt {}", attempt));
					g_scriptInstallWorkerStarted = false;
					return;
				}
			}
			if (!g_scriptSafInstalled.load(std::memory_order_acquire)) {
				LogMessage("MCF: Delayed ScriptCommands install worker exhausted retries (SFSE phases may still install)");
			}
			g_scriptInstallWorkerStarted = false;
		}).detach();
	}

	void OnSFSEMessaging(SFSE::MessagingInterface::Message* a_msg) noexcept
	{
		if (!kEnableScriptTableInjection || !a_msg || g_scriptSafInstalled.load(std::memory_order_acquire)) {
			return;
		}

		const auto phase = static_cast<SFSE::MessagingInterface::MessageType>(a_msg->type);
		switch (phase) {
		case SFSE::MessagingInterface::kPostLoad:
		case SFSE::MessagingInterface::kPostPostLoad:
		case SFSE::MessagingInterface::kPostDataLoad:
		case SFSE::MessagingInterface::kPostPostDataLoad:
			break;
		default:
			return;
		}

		bool registeredSaf = false;
		try {
			std::unique_lock lk{ regLock };
			registeredSaf = registrations.contains("saf");
			lk.unlock();

			if (!registeredSaf) {
				return;
			}

			if (TryInstallSafScriptCommand(false)) {
				return;
			}

			if (phase == SFSE::MessagingInterface::kPostPostDataLoad && !g_scriptSafInstalled.load(std::memory_order_acquire)) {
				LogMessage(
					"MCF: saf is registered but ScriptCommands injection failed after kPostPostDataLoad "
					"(very slow VM init — try newer MCF build or report with log)");
			}
		} catch (...) {
			// no-throw barrier for messaging callback
		}
	}

	void ExecuteCommand(void* arg1, char* a_cmd)
	{
		if (!a_cmd) {
			if constexpr (kVerboseMCFLogging) {
				LogMessage("[MCF] ExecuteCommand: null cmd, forwarding");
			}
			return OriginalExecuteCommand(arg1, a_cmd);
		}

		std::string cmdView(a_cmd);
		if (cmdView.empty()) {
			if constexpr (kVerboseMCFLogging) {
				LogMessage("[MCF] ExecuteCommand: empty cmd, forwarding");
			}
			return OriginalExecuteCommand(arg1, a_cmd);
		}

		auto args = Util::str_split(cmdView, " ", '\"');
		if (args.empty()) {
			if constexpr (kVerboseMCFLogging) {
				LogMessage("[MCF] ExecuteCommand: no tokens, forwarding");
			}
			return OriginalExecuteCommand(arg1, a_cmd);
		}
		if constexpr (kVerboseMCFLogging) {
			LogMessage(std::format("[MCF] ExecuteCommand: parsed command='{}', args={}", args[0], args.size() - 1));
		}
		
		std::unique_lock l{ regLock };
		if (auto iter = registrations.find(Util::str_tolower(args[0])); iter != registrations.end()) {
			if constexpr (kVerboseMCFLogging) {
				LogMessage(std::format("[MCF] ExecuteCommand: handler found for '{}'", args[0]));
			}

			args.erase(args.begin());
			auto argsSSV = Util::sv_vec_to_ssv_vec(args);
			MCF::simple_array<MCF::simple_string_view> argsArr(argsSSV);

			intfc.Reset(cmdView.c_str());
			if constexpr (kVerboseMCFLogging) {
				LogMessage(std::format("[MCF] ExecuteCommand: calling handler for '{}'", cmdView));
			}
			iter->second(argsArr, cmdView.c_str(), &intfc);
			if constexpr (kVerboseMCFLogging) {
				LogMessage(std::format("[MCF] ExecuteCommand: handler returned for '{}'", cmdView));
			}
			intfc.PrintDefault();
			if constexpr (kVerboseMCFLogging) {
				LogMessage("[MCF] ExecuteCommand: EXIT (handled)");
			}
			return;
		}

		if constexpr (kVerboseMCFLogging) {
			LogMessage("[MCF] ExecuteCommand: no handler, forwarding");
		}
		return OriginalExecuteCommand(arg1, a_cmd);
	}

	void InstallHooks()
	{
		LogMessage("MCF: InstallHooks() STARTED");
		LogIdDiagnostics();
		REL::Trampoline* trampoline = nullptr;
		if (kEnableLegacyExecuteHook || kEnableScriptResolverProbeHook || kEnableDispatcherCallsiteHook) {
			SFSE::AllocTrampoline(64);
			trampoline = &SFSE::GetTrampoline();
		}

		if (kEnableLegacyExecuteHook) {
			REL::Relocation<uintptr_t> hookLoc{ REL::ID(166307), 0xD2 };
			OriginalExecuteCommand = reinterpret_cast<ExecuteCommandFunc>(trampoline->write_call<5>(hookLoc.address(), &ExecuteCommand));
			LogMessage("MCF: Installed command hook (1.15 logic)");
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
	}
}

DLLEXPORT void RegisterCommand(const char* a_name, MCF::CommandCallback a_func)
{
	Commands::RegisterCommand(a_name, a_func);
}
