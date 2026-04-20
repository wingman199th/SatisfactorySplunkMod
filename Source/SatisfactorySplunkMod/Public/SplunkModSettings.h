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

    /** Splunk HEC endpoint URL, e.g. https://splunk.mycompany.com:8088/services/collector */
    UPROPERTY(Config, EditAnywhere, Category = "Splunk Connection")
    FString SplunkURL = TEXT("https://your-splunk-instance:8088/services/collector");

    /** Splunk HTTP Event Collector authentication token */
    UPROPERTY(Config, EditAnywhere, Category = "Splunk Connection")
    FString HECToken = TEXT("your-hec-token-here");

    // ---------------------------------------------------------------
    // Collection Mode
    // ---------------------------------------------------------------

    /**
     * True  = Metrics mode: fast aggregated totals sent every second (recommended).
     * False = Events mode: detailed per-machine events on independent timers.
     */
    UPROPERTY(Config, EditAnywhere, Category = "Collection")
    bool bUseMetricsMode = true;

    // ---------------------------------------------------------------
    // Metrics Mode Settings (used when bUseMetricsMode=True)
    // ---------------------------------------------------------------

    /** Seconds between metric collection cycles (default: 1.0) */
    UPROPERTY(Config, EditAnywhere, Category = "Metrics Mode")
    float MetricsInterval = 1.0f;

    /** Seconds between buffer flushes to Splunk (default: 1.0) */
    UPROPERTY(Config, EditAnywhere, Category = "Metrics Mode")
    float BufferFlushInterval = 1.0f;

    // ---------------------------------------------------------------
    // Legacy Events Mode - Per-type collection intervals
    // Each data type has its own timer so high-priority data (power)
    // can be collected more frequently than low-priority data (players).
    // ---------------------------------------------------------------

    /**
     * How often to collect power generator data, in seconds.
     * Keep this low - power fluctuations happen fast (brownouts, etc).
     * Default: 2.0
     */
    UPROPERTY(Config, EditAnywhere, Category = "Legacy Events Mode")
    float PowerCollectionInterval = 2.0f;

    /**
     * How often to collect manufacturer and extractor data, in seconds.
     * Efficiency changes slowly so this can be less frequent.
     * Default: 10.0
     */
    UPROPERTY(Config, EditAnywhere, Category = "Legacy Events Mode")
    float ProductionCollectionInterval = 10.0f;

    /**
     * How often to collect vehicle positions, fuel, and cargo, in seconds.
     * Default: 5.0
     */
    UPROPERTY(Config, EditAnywhere, Category = "Legacy Events Mode")
    float VehicleCollectionInterval = 5.0f;

    /**
     * How often to collect player position and equipment data, in seconds.
     * Players move around but this data is low priority for most dashboards.
     * Default: 30.0
     */
    UPROPERTY(Config, EditAnywhere, Category = "Legacy Events Mode")
    float PlayerCollectionInterval = 30.0f;

    /**
     * How often to flush the data buffer to Splunk in legacy events mode, in seconds.
     * Should be <= the shortest collection interval above.
     * Default: 5.0
     */
    UPROPERTY(Config, EditAnywhere, Category = "Legacy Events Mode")
    float LegacyBufferFlushInterval = 5.0f;

    /** Buffer this many events before flushing (whichever comes first: size or time) */
    UPROPERTY(Config, EditAnywhere, Category = "Legacy Events Mode")
    int32 BatchSize = 10;

    /** Collect detailed manufacturer and extractor data */
    UPROPERTY(Config, EditAnywhere, Category = "Legacy Events Mode")
    bool bCollectProductionData = true;

    /** Collect wheeled vehicle and train data */
    UPROPERTY(Config, EditAnywhere, Category = "Legacy Events Mode")
    bool bCollectVehicleData = true;

    /** Collect player movement and equipment data */
    UPROPERTY(Config, EditAnywhere, Category = "Legacy Events Mode")
    bool bCollectPlayerData = true;

    /** Collect power generator data */
    UPROPERTY(Config, EditAnywhere, Category = "Legacy Events Mode")
    bool bCollectPowerData = true;

    /**
     * Collect a full factory layout snapshot (all buildings).
     * WARNING: This can produce very large payloads. Only enable if needed.
     */
    UPROPERTY(Config, EditAnywhere, Category = "Legacy Events Mode")
    bool bCollectLayoutData = false;

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
