#pragma once

#include "CoreMinimal.h"
#include "Module/GameWorldModule.h"
#include "SplunkGameWorldModule.generated.h"

/**
 * SML Game World Module for the Satisfactory Splunk Exporter.
 *
 * SML discovers this class automatically (bRootModule = true) and calls
 * DispatchLifecycleEvent each time a save is loaded. The exporter actor
 * is spawned during INITIALIZATION so users do not need to place anything
 * in their save file manually.
 */
UCLASS()
class SATISFACTORYSPLUNKMOD_API USplunkGameWorldModule : public UGameWorldModule
{
    GENERATED_BODY()

public:
    USplunkGameWorldModule();

    virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;
};
