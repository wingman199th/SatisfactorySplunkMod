#include "SplunkGameWorldModule.h"
#include "SplunkExporter.h"
#include "SatisfactorySplunkMod.h"
#include "Engine/World.h"
#include "EngineUtils.h"

USplunkGameWorldModule::USplunkGameWorldModule()
{
    // Root module: SML discovers native classes with this flag automatically
    // (FPluginModuleLoader::FindRootModulesOfType) and dispatches lifecycle
    // events for every game world (save) that loads. The main menu uses a
    // different module type, so this never runs there.
    bRootModule = true;
}

void USplunkGameWorldModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
    Super::DispatchLifecycleEvent(Phase);

    // POST_INITIALIZATION fires from OnWorldBeginPlay: all game subsystems
    // (railroad, circuits, ...) exist by then.
    if (Phase != ELifecyclePhase::POST_INITIALIZATION)
    {
        return;
    }

    UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkGameWorldModule: POST_INITIALIZATION received"));

    UWorld* World = GetWorld();
    if (!World)
    {
        UE_LOG(LogSatisfactorySplunkMod, Error, TEXT("SplunkGameWorldModule: no world during POST_INITIALIZATION"));
        return;
    }

    // World modules also run on multiplayer clients; only the host/server exports.
    if (World->GetNetMode() == NM_Client)
    {
        UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkGameWorldModule: multiplayer client - not spawning exporter (the host/server exports)"));
        return;
    }

    TActorIterator<ASplunkExporter> Existing(World);
    if (Existing)
    {
        UE_LOG(LogSatisfactorySplunkMod, Warning, TEXT("SplunkGameWorldModule: exporter already exists, not spawning another"));
        return;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    ASplunkExporter* Exporter = World->SpawnActor<ASplunkExporter>(
        ASplunkExporter::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);

    if (Exporter)
    {
        UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkGameWorldModule: SplunkExporter spawned"));
    }
    else
    {
        UE_LOG(LogSatisfactorySplunkMod, Error, TEXT("SplunkGameWorldModule: failed to spawn SplunkExporter"));
    }
}
