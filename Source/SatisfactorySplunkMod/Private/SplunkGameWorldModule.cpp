#include "SplunkGameWorldModule.h"
#include "SplunkExporter.h"
#include "SatisfactorySplunkMod.h"
#include "Engine/World.h"

USplunkGameWorldModule::USplunkGameWorldModule()
{
    // Root module: SML discovers this class automatically and dispatches
    // lifecycle events for every game world (save) that loads.
    bRootModule = true;
}

void USplunkGameWorldModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
    Super::DispatchLifecycleEvent(Phase);

    // POST_INITIALIZATION: all game subsystems (railroad, circuits, ...) exist.
    if (Phase != ELifecyclePhase::POST_INITIALIZATION)
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        UE_LOG(LogSatisfactorySplunkMod, Error, TEXT("SplunkGameWorldModule: no world during POST_INITIALIZATION"));
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
