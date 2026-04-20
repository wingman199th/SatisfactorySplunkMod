#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Http.h"
#include "Json.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TimerManager.h"

// Satisfactory includes
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableSubsystem.h"
#include "Buildables/FGBuildablePowerGenerator.h"
#include "FGPowerInfoComponent.h"
#include "FGPowerCircuit.h"
#include "FGBuildableDockingStation.h"
#include "FGWheeledVehicle.h"
#include "FGTrain.h"
#include "FGRailroadVehicle.h"
#include "FGLocomotive.h"
#include "FGFreightWagon.h"
#include "FGRailroadTimeTable.h"
#include "FGTrainStationIdentifier.h"
#include "FGCharacterPlayer.h"
#include "Equipment/FGJetPack.h"
#include "Components/FGInventoryComponent.h"
#include "FGItemDescriptor.h"
#include "FGItemDescriptorNuclearFuel.h"
#include "FGItemDescriptorBiomass.h"
#include "FGRecipe.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "SplunkModSettings.h"
#include "SplunkExporter.generated.h"

UCLASS(BlueprintType, Blueprintable)
class SATISFACTORYSPLUNKMOD_API ASplunkExporter : public AActor
{
    GENERATED_BODY()

public:
    ASplunkExporter();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
    // Blueprint callable functions
    UFUNCTION(BlueprintCallable, Category = "Splunk Exporter")
    void StartDataCollection();

    UFUNCTION(BlueprintCallable, Category = "Splunk Exporter")
    void StopDataCollection();

    UFUNCTION(BlueprintCallable, Category = "Splunk Exporter")
    void CollectAndSendData();

    UFUNCTION(BlueprintCallable, Category = "Splunk Exporter")
    void SendBufferedData();

    UFUNCTION(BlueprintCallable, Category = "Splunk Exporter")
    void CollectMetrics();

    // Load connection and collection settings from DefaultSatisfactorySplunkMod.ini
    UFUNCTION(BlueprintCallable, Category = "Splunk Exporter")
    void LoadSettingsFromConfig();

private:
    // Metrics collection (fast, aggregated)
    void CollectAggregateMetrics();
    void CheckAndFlushBuffer();

    // Data collection methods (legacy/detailed)
    void CollectProductionData();
    void CollectPowerData();
    void CollectAllVehicleData();
    void CollectPersonalVehicles();
    void CollectAutomatedVehicles();
    void CollectTrainData();
    void CollectPlayerMovementSystems();
    void CollectFactoryLayoutData();
    
    // HTTP communication
    void SendDataToSplunk(const FString& JsonData);
    void SendLayoutDataToSplunk(TSharedPtr<FJsonObject> LayoutData);
    void OnHttpResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
    
    // Utility methods
    FString GetVehicleTypeFromClass(const FString& ClassName);
    void AddEventToBuffer(TSharedPtr<FJsonObject> EventObject);
    TSharedPtr<FJsonObject> CreateBaseEvent(const FString& SourceType);

private:
    // Metrics mode timers
    FTimerHandle MetricsCollectionTimer;
    FTimerHandle BufferFlushTimer;

    // Legacy events mode: one independent timer per data type
    FTimerHandle PowerCollectionTimer;
    FTimerHandle ProductionCollectionTimer;
    FTimerHandle VehicleCollectionTimer;
    FTimerHandle PlayerCollectionTimer;
    FTimerHandle LegacyBufferFlushTimer;

    // Data buffer for batching
    TArray<TSharedPtr<FJsonObject>> DataBuffer;

    // Timestamps
    FDateTime LastBufferFlush;

    // Configuration properties
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Splunk Configuration", meta = (AllowPrivateAccess = "true"))
    FString SplunkURL = TEXT("https://your-splunk-instance:8088/services/collector");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Splunk Configuration", meta = (AllowPrivateAccess = "true"))
    FString HECToken = TEXT("your-hec-token-here");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Metrics Settings", meta = (AllowPrivateAccess = "true"))
    float MetricsInterval = 1.0f;  // Collect metrics every 1 second

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Metrics Settings", meta = (AllowPrivateAccess = "true"))
    float BufferFlushInterval = 1.0f;  // Flush buffer every 1 second (real-time streaming)

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Metrics Settings", meta = (AllowPrivateAccess = "true"))
    bool bUseMetricsMode = true;  // Use fast metrics mode instead of detailed events

    // Legacy events mode: per-type collection intervals
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Settings", meta = (AllowPrivateAccess = "true"))
    float PowerCollectionInterval = 2.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Settings", meta = (AllowPrivateAccess = "true"))
    float ProductionCollectionInterval = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Settings", meta = (AllowPrivateAccess = "true"))
    float VehicleCollectionInterval = 5.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Settings", meta = (AllowPrivateAccess = "true"))
    float PlayerCollectionInterval = 30.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Settings", meta = (AllowPrivateAccess = "true"))
    float LegacyBufferFlushInterval = 5.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Settings", meta = (AllowPrivateAccess = "true"))
    int32 BatchSize = 10;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Settings", meta = (AllowPrivateAccess = "true"))
    bool bCollectProductionData = true;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Settings", meta = (AllowPrivateAccess = "true"))
    bool bCollectVehicleData = true;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Settings", meta = (AllowPrivateAccess = "true"))
    bool bCollectPlayerData = true;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Settings", meta = (AllowPrivateAccess = "true"))
    bool bCollectPowerData = true;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Settings", meta = (AllowPrivateAccess = "true"))
    bool bCollectLayoutData = false;  // Only send layout data on demand
    
    // Status tracking
    UPROPERTY(BlueprintReadOnly, Category = "Status", meta = (AllowPrivateAccess = "true"))
    bool bIsCollecting = false;
    
    UPROPERTY(BlueprintReadOnly, Category = "Status", meta = (AllowPrivateAccess = "true"))
    int32 EventsSentTotal = 0;
    
    UPROPERTY(BlueprintReadOnly, Category = "Status", meta = (AllowPrivateAccess = "true"))
    int32 EventsInBuffer = 0;
    
    UPROPERTY(BlueprintReadOnly, Category = "Status", meta = (AllowPrivateAccess = "true"))
    FDateTime LastSuccessfulSend;
};