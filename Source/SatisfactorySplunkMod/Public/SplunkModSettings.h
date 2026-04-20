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
     * False = Events mode: detailed per-machine events sent in batches.
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
    // Legacy Events Mode Settings (used when bUseMetricsMode=False)
    // ---------------------------------------------------------------

    /** Seconds between detailed collection cycles (default: 30.0) */
    UPROPERTY(Config, EditAnywhere, Category = "Legacy Events Mode")
    float CollectionInterval = 30.0f;

    /** Buffer this many events before sending to Splunk (default: 10) */
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
