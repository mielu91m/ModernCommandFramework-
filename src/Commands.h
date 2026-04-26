#include "MCF_API.h"

namespace Commands
{
	void InstallHooks();
	void RegisterCommand(const char* name, MCF::CommandCallback func);
}