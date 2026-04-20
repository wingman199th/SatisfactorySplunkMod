#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SplunkModSettings.generated.h"

/**
 * Singleton config object for the Satisfactory Splunk Exporter mod.
 * Values are read from Config/DefaultSatisfactorySplunkMod.ini at startup.
 * Edit that file to configure your Splunk connection, then restart the game.
 */
UCLASS(Config=SatisfactorySplunkMod, defaultconfig)
class SATISFACTORYSPLUNKMOD_API USplunkModSettings : public UObject
{
    GENERATED_BODY()

public:
    // ---------------------------------------------------------------
    // Splunk Connection
    // ---------------------------------------------------------------

    UPROPERTY(Config, EditAnywhere, Category = "Splunk Connection")
    FString SplunkURL = TEXT("https://your-splunk-instance:8088/services/collector");

    UPROPERTY(Config, EditAnywhere, Category = "Splunk Connection")
    FString HECToken = TEXT("your-hec-token-here");

    // ---------------------------------------------------------------
    // Collection Mode
    // ---------------------------------------------------------------

    /**
     * True  = Metrics mode: fast aggregated totals (recommended)
     * False = Events mode: detailed per-machine data
     * Both modes use the same per-type intervals below.
     */
    UPROPERTY(Config, EditAnywhere, Category = "Collection")
    bool bUseMetricsMode = true;

    // ---------------------------------------------------------------
    // Collection Intervals (apply to BOTH metrics and events mode)
    //
    // Each data type collects on its own independent timer.
    // Set a type to False below to disable it entirely.
    // ---------------------------------------------------------------

    /** Seconds between power data collections. Keep low to catch brownouts fast. */
    UPROPERTY(Config, EditAnywhere, Category = "Collection Intervals")
    float PowerInterval = 2.0f;

    /** Seconds between production machine data collections. Efficiency changes slowly. */
    UPROPERTY(Config, EditAnywhere, Category = "Collection Intervals")
    float ProductionInterval = 10.0f;

    /** Seconds between vehicle (trucks + trains) data collections. */
    UPROPERTY(Config, EditAnywhere, Category = "Collection Intervals")
    float VehicleInterval = 5.0f;

    /** Seconds between player data collections. Low-priority positional data. */
    UPROPERTY(Config, EditAnywhere, Category = "Collection Intervals")
    float PlayerInterval = 30.0f;

    /** Seconds between buffer flushes to Splunk. Should be <= PowerInterval. */
    UPROPERTY(Config, EditAnywhere, Category = "Collection Intervals")
    float BufferFlushInterval = 5.0f;

    // ---------------------------------------------------------------
    // What to collect
    // ---------------------------------------------------------------

    UPROPERTY(Config, EditAnywhere, Category = "Data Types")
    bool bCollectPowerData = true;

    UPROPERTY(Config, EditAnywhere, Category = "Data Types")
    bool bCollectProductionData = true;

    UPROPERTY(Config, EditAnywhere, Category = "Data Types")
    bool bCollectVehicleData = true;

    UPROPERTY(Config, EditAnywhere, Category = "Data Types")
    bool bCollectPlayerData = true;

    /** WARNING: Snapshots all buildings - produces very large payloads. Events mode only. */
    UPROPERTY(Config, EditAnywhere, Category = "Data Types")
    bool bCollectLayoutData = false;

    // ---------------------------------------------------------------
    // Events Mode Only
    // ---------------------------------------------------------------

    /** Buffer this many events before forcing an early flush (events mode only). */
    UPROPERTY(Config, EditAnywhere, Category = "Events Mode")
    int32 BatchSize = 10;

    // ---------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------

    static USplunkModSettings* Get()
    {
        return GetMutableDefault<USplunkModSettings>();
    }

    bool IsConfigured() const
    {
        return !HECToken.IsEmpty()
            && HECToken != TEXT("your-hec-token-here")
            && !SplunkURL.IsEmpty()
            && SplunkURL != TEXT("https://your-splunk-instance:8088/services/collector");
    }
};
