#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "SplunkExporter.generated.h"

/**
 * Collects factory telemetry on independent timers and ships it to a
 * Splunk HTTP Event Collector (HEC) endpoint as batched JSON events.
 *
 * Spawned automatically by USplunkGameWorldModule when a save loads, and
 * only does anything on the machine that owns the game state (single
 * player, listen-server host, or dedicated server).
 *
 * Configuration: see LoadSettingsFromConfig() and README.md.
 *
 * Sourcetypes emitted:
 *   satisfactory:status      - mod startup event (proves connectivity)
 *   satisfactory:summary     - aggregate counts / power / productivity
 *   satisfactory:power       - one event per power circuit
 *   satisfactory:production  - one event per manufacturer, extractor and
 *                              generator: throughput + "waiting for" status
 *   satisfactory:inventory   - one event per item type stored in containers
 *   satisfactory:vehicle     - trucks, trains and freight wagon cargo
 *   satisfactory:player      - player positions
 */
UCLASS()
class SATISFACTORYSPLUNKMOD_API ASplunkExporter : public AActor
{
    GENERATED_BODY()

public:
    ASplunkExporter();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
    void StartDataCollection();
    void StopDataCollection();
    void FlushBuffer();
    void LoadSettingsFromConfig();

private:
    // Collectors - one per timer
    void CollectSummary();
    void CollectPower();
    void CollectProduction();
    void CollectInventory();
    void CollectVehicles();
    void CollectPlayers();
    void SendStartupEvent();

    // Event helpers
    TSharedPtr<FJsonObject> NewSplunkEvent(const FString& SourceType) const;
    void AddEventToBuffer(const TSharedPtr<FJsonObject>& EventObject);

    // HTTP
    void SendToSplunk(const FString& Payload, int32 EventCount);
    void OnHttpResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, int32 EventCount);

    // Config
    void ApplySetting(const FString& Key, const FString& Value);
    bool IsConfigured() const;

private:
    FTimerHandle SummaryTimer;
    FTimerHandle PowerTimer;
    FTimerHandle ProductionTimer;
    FTimerHandle InventoryTimer;
    FTimerHandle VehicleTimer;
    FTimerHandle PlayerTimer;
    FTimerHandle FlushTimer;

    TArray<TSharedPtr<FJsonObject>> DataBuffer;
    FString HostName;
    FString ConfigPathUsed;

    // ---------------------------------------------------------------
    // Configuration
    // ---------------------------------------------------------------
    FString SplunkURL;
    FString HECToken;
    FString SplunkIndex;   // optional, empty = token default

    float SummaryInterval    = 5.0f;
    float PowerInterval      = 5.0f;
    float ProductionInterval = 30.0f;
    float InventoryInterval  = 30.0f;
    float VehicleInterval    = 10.0f;
    float PlayerInterval     = 10.0f;
    float FlushInterval      = 5.0f;

    bool bCollectSummary    = true;
    bool bCollectPower      = true;
    bool bCollectProduction = true;
    bool bCollectInventory  = true;
    bool bCollectVehicles   = true;
    bool bCollectPlayers    = true;

    int32 MaxBufferedEvents = 2000;

    // Status
    bool  bIsCollecting    = false;
    int32 BatchesSent      = 0;
    int32 EventsSentTotal  = 0;
    int32 EventsDropped    = 0;
    int32 ConsecutiveFailures = 0;
};
