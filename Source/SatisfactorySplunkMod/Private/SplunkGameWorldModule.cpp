#include "SplunkGameWorldModule.h"
#include "SplunkExporter.h"
#include "SatisfactorySplunkMod.h"
#include "Engine/World.h"

USplunkGameWorldModule::USplunkGameWorldModule()
{
    // Tell SML this is the root module for this mod so it is auto-discovered
    bRootModule = true;
}

void USplunkGameWorldModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
    Super::DispatchLifecycleEvent(Phase);

    if (Phase == ELifecyclePhase::INITIALIZATION)
    {
        UWorld* World = GetWorld();
        if (!World)
        {
            UE_LOG(LogSatisfactorySplunkMod, Error, TEXT("SplunkGameWorldModule: No world context during INITIALIZATION"));
            return;
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

        ASplunkExporter* Exporter = World->SpawnActor<ASplunkExporter>(
            ASplunkExporter::StaticClass(),
            FVector::ZeroVector,
            FRotator::ZeroRotator,
            SpawnParams
        );

        if (Exporter)
        {
            UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkGameWorldModule: SplunkExporter spawned successfully"));
        }
        else
        {
            UE_LOG(LogSatisfactorySplunkMod, Error, TEXT("SplunkGameWorldModule: Failed to spawn SplunkExporter"));
        }
    }
}
