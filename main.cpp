#include "Pch.h"
#include "Commands.h"
#include <fstream>
#include <iostream>

namespace
{
	std::ofstream g_logFile;

	void InitializeLogging()
	{
		auto path = std::filesystem::path(std::getenv("USERPROFILE")) / "Documents" / "My Games" / "Starfield" / "SFSE" / "Logs";
		std::filesystem::create_directories(path);

		path /= std::format("{}.log", Plugin::NAME);
		g_logFile.open(path.string(), std::ios::app);
	}

	void MessageCallback(SFSE::MessagingInterface::Message* a_msg) noexcept
	{
		Commands::OnSFSEMessaging(a_msg);
	}
}

void LogMessage(const std::string& message)
{
	if (g_logFile.is_open()) {
		auto now = std::chrono::system_clock::now();
		auto time_t = std::chrono::system_clock::to_time_t(now);
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
		
		std::tm tm;
		localtime_s(&tm, &time_t);
		
		g_logFile << std::format("[{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}] [INFO] {}\n",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, ms.count(),
			message);
		// Avoid flush per line — reduces disk I/O during gameplay; OS buffers writes.
		static thread_local std::uint32_t s_writeCount = 0;
		if ((++s_writeCount & 15u) == 0u) {
			g_logFile.flush();
		}
	}
}

DLLEXPORT bool SFSEAPI SFSEPlugin_Load(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse, false);
	InitializeLogging();

	LogMessage(std::format("{} v{} loaded", Plugin::NAME, Plugin::Version));
	LogMessage("Built with libxe's updated CommonLibSF");
	LogMessage("About to call InstallHooks()...");

	Commands::InstallHooks();
	
	LogMessage("InstallHooks() returned");
	SFSE::GetMessagingInterface()->RegisterListener(MessageCallback);

	return true;
}
