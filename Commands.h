#include "MCF_API.h"
#include "SFSE/Interfaces.h"

namespace Commands
{
	void InstallHooks();
	void RegisterCommand(const char* name, MCF::CommandCallback func);
	void OnSFSEMessaging(SFSE::MessagingInterface::Message* a_msg) noexcept;
}