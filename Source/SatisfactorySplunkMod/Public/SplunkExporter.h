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
    UFUNCTION(BlueprintCallable, Category = "Splunk Exporter")
    void StartDataCollection();

    UFUNCTION(BlueprintCallable, Category = "Splunk Exporter")
    void StopDataCollection();

    UFUNCTION(BlueprintCallable, Category = "Splunk Exporter")
    void CollectAndSendData();

    UFUNCTION(BlueprintCallable, Category = "Splunk Exporter")
    void SendBufferedData();

    UFUNCTION(BlueprintCallable, Category = "Splunk Exporter")
    void LoadSettingsFromConfig();

private:
    // ---------------------------------------------------------------
    // Metrics mode collectors (aggregated totals)
    // ---------------------------------------------------------------
    void CollectPowerMetrics();
    void CollectProductionMetrics();
    void CollectVehicleMetrics();
    void CollectPlayerMetrics();

    // ---------------------------------------------------------------
    // Events mode collectors (detailed per-machine data)
    // ---------------------------------------------------------------
    void CollectProductionData();
    void CollectPowerData();
    void CollectAllVehicleData();
    void CollectPersonalVehicles();
    void CollectAutomatedVehicles();
    void CollectTrainData();
    void CollectPlayerMovementSystems();
    void CollectFactoryLayoutData();

    // Buffer flush (shared by both modes)
    void CheckAndFlushBuffer();

    // HTTP
    void SendDataToSplunk(const FString& JsonData);
    void SendLayoutDataToSplunk(TSharedPtr<FJsonObject> LayoutData);
    void OnHttpResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

    // Utilities
    FString GetVehicleTypeFromClass(const FString& ClassName);
    void AddEventToBuffer(TSharedPtr<FJsonObject> EventObject);
    TSharedPtr<FJsonObject> CreateBaseEvent(const FString& SourceType);
    TSharedPtr<FJsonObject> CreateMetricsEvent();

private:
    // One independent timer per data type + one flush timer
    FTimerHandle PowerTimer;
    FTimerHandle ProductionTimer;
    FTimerHandle VehicleTimer;
    FTimerHandle PlayerTimer;
    FTimerHandle BufferFlushTimer;

    // Data buffer
    TArray<TSharedPtr<FJsonObject>> DataBuffer;
    FDateTime LastBufferFlush;

    // ---------------------------------------------------------------
    // Configuration (loaded from ini via LoadSettingsFromConfig)
    // ---------------------------------------------------------------
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Splunk Configuration", meta = (AllowPrivateAccess = "true"))
    FString SplunkURL = TEXT("https://your-splunk-instance:8088/services/collector");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Splunk Configuration", meta = (AllowPrivateAccess = "true"))
    FString HECToken = TEXT("your-hec-token-here");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection", meta = (AllowPrivateAccess = "true"))
    bool bUseMetricsMode = true;

    // Intervals (seconds) - apply to both metrics and events mode
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Intervals", meta = (AllowPrivateAccess = "true"))
    float PowerInterval = 2.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Intervals", meta = (AllowPrivateAccess = "true"))
    float ProductionInterval = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Intervals", meta = (AllowPrivateAccess = "true"))
    float VehicleInterval = 5.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Intervals", meta = (AllowPrivateAccess = "true"))
    float PlayerInterval = 30.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collection Intervals", meta = (AllowPrivateAccess = "true"))
    float BufferFlushInterval = 5.0f;

    // What to collect
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data Types", meta = (AllowPrivateAccess = "true"))
    bool bCollectPowerData = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data Types", meta = (AllowPrivateAccess = "true"))
    bool bCollectProductionData = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data Types", meta = (AllowPrivateAccess = "true"))
    bool bCollectVehicleData = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data Types", meta = (AllowPrivateAccess = "true"))
    bool bCollectPlayerData = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Data Types", meta = (AllowPrivateAccess = "true"))
    bool bCollectLayoutData = false;

    // Events mode only
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Events Mode", meta = (AllowPrivateAccess = "true"))
    int32 BatchSize = 10;

    // Status
    UPROPERTY(BlueprintReadOnly, Category = "Status", meta = (AllowPrivateAccess = "true"))
    bool bIsCollecting = false;

    UPROPERTY(BlueprintReadOnly, Category = "Status", meta = (AllowPrivateAccess = "true"))
    int32 EventsSentTotal = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Status", meta = (AllowPrivateAccess = "true"))
    int32 EventsInBuffer = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Status", meta = (AllowPrivateAccess = "true"))
    FDateTime LastSuccessfulSend;
};
