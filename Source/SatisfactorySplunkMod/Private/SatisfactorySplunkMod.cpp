#include "SatisfactorySplunkMod.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogSatisfactorySplunkMod);

IMPLEMENT_MODULE(FSatisfactorySplunkModModule, SatisfactorySplunkMod);

void FSatisfactorySplunkModModule::StartupModule()
{
    UE_LOG(LogSatisfactorySplunkMod, Warning, TEXT("Satisfactory Splunk Mod: Module Started"));
}

void FSatisfactorySplunkModModule::ShutdownModule()
{
    UE_LOG(LogSatisfactorySplunkMod, Warning, TEXT("Satisfactory Splunk Mod: Module Shutdown"));
}