# Satisfactory Splunk Exporter Mod

## Overview
This Unreal Engine mod exports production, vehicle, player, and power data from Satisfactory to Splunk for real-time analytics and monitoring. The mod uses a fast metrics collection system that sends aggregated factory data to Splunk every second, perfect for live dashboards.

## Features
- **Real-Time Metrics Mode**: Collects and sends aggregated metrics every 1 second for live dashboards
- **Production Data Collection**: Monitors manufacturers and resource extractors
- **Power System Monitoring**: Tracks power consumption, production, and generator stats
- **Vehicle Tracking**: Collects data from wheeled vehicles and trains
- **Player Monitoring**: Tracks player count and positions
- **Factory Efficiency**: Calculates average efficiency across all producing machines
- **Low Overhead**: Aggregated metrics mode uses minimal bandwidth (~300 bytes/sec)
- **Configurable**: Switch between fast metrics mode or detailed events mode

## Quick Start

### What You Need
1. **Satisfactory** with Satisfactory Mod Loader (SML) installed
2. **Splunk** instance with HTTP Event Collector (HEC) enabled
3. **HEC Token** from your Splunk configuration

### Installation Steps

#### 1. Install Satisfactory Mod Loader (SML)
If you haven't already:
- Download and install [Satisfactory Mod Manager](https://smm.ficsit.app/)
- Launch SMM and let it install SML automatically

#### 2. Install This Mod
Choose one of these methods:

**Method A: Direct Installation (Recommended)**
1. Download or clone this repository
2. Copy the entire `SatisfactorySplunkMod` folder to:
   ```
   <Satisfactory Install Dir>/FactoryGame/Mods/
   ```
   Example path: `C:\Program Files\Epic Games\SatisfactoryEarlyAccess\FactoryGame\Mods\SatisfactorySplunkMod\`

3. Your folder structure should look like:
   ```
   FactoryGame/
   └── Mods/
       └── SatisfactorySplunkMod/
           ├── SatisfactorySplunkMod.uplugin
           └── Source/
               └── SatisfactorySplunkMod/
                   ├── Public/
                   │   ├── SplunkExporter.h
                   │   └── SatisfactorySplunkMod.h
                   ├── Private/
                   │   ├── SplunkExporter.cpp
                   │   └── SatisfactorySplunkMod.cpp
                   └── SatisfactorySplunkMod.Build.cs
   ```

**Method B: Via Satisfactory Mod Manager**
1. Package the mod as a `.smod` file (requires packaging with Alpakit)
2. Install through Satisfactory Mod Manager

#### 3. Configure Splunk HEC

In your Splunk instance:

1. Go to **Settings** > **Data Inputs** > **HTTP Event Collector**
2. Click **New Token**
3. Configure:
   - **Name**: `Satisfactory-Metrics`
   - **Source type**: Leave as Automatic (or set to `satisfactory:metrics`)
   - **Index**: Create or select index (e.g., `satisfactory`)
4. Click **Review** > **Submit**
5. **Copy the token value** - you'll need this!

Your HEC URL will be:
```
https://<your-splunk-server>:8088/services/collector
```

#### 4. Configure the Mod

You have two options to configure the mod:

**Option A: In-Game Configuration (Blueprint)**
1. Launch Satisfactory
2. Load your save game
3. Open the console (`` ` `` key)
4. Spawn the exporter actor (you'll need to create a Blueprint for this)
5. Set the following properties:
   - `SplunkURL`: Your HEC endpoint URL
   - `HECToken`: Your HEC token from step 3

**Option B: Code Configuration (Before Building)**
Edit `Source/SatisfactorySplunkMod/Public/SplunkExporter.h`:
```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Splunk Configuration")
FString SplunkURL = TEXT("https://your-splunk-server:8088/services/collector");

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Splunk Configuration")
FString HECToken = TEXT("your-actual-token-here");
```

#### 5. Verify It's Working

1. **Launch Satisfactory** and load a save with some buildings
2. **Check the logs** at:
   ```
   <Satisfactory Install Dir>/FactoryGame/Saved/Logs/FactoryGame.log
   ```
   Look for:
   ```
   LogSatisfactorySplunkMod: Satisfactory Splunk Mod: Module Started
   LogSatisfactorySplunkMod: SplunkExporter: Starting up
   LogSatisfactorySplunkMod: SplunkExporter: Metrics mode started (collect: 1.0s, flush: 1.0s)
   LogSatisfactorySplunkMod: SplunkExporter: Data successfully sent to Splunk (HTTP 200)
   ```

3. **Check Splunk** for incoming data:
   ```spl
   index=satisfactory sourcetype="satisfactory:metrics"
   | head 10
   ```

## Configuration Options

### Metrics Mode Settings (Default - Real-Time)
- `MetricsInterval`: How often to collect metrics (default: **1.0s**)
- `BufferFlushInterval`: How often to send to Splunk (default: **1.0s**)
- `bUseMetricsMode`: Use metrics mode (default: **true**)

### Legacy Events Mode Settings
- `CollectionInterval`: How often to collect detailed events (default: 30.0s)
- `BatchSize`: Number of events to buffer before sending (default: 10)
- `bCollectProductionData`: Enable/disable production data collection
- `bCollectVehicleData`: Enable/disable vehicle data collection
- `bCollectPlayerData`: Enable/disable player movement data collection
- `bCollectPowerData`: Enable/disable power system data collection

### Splunk Connection
- `SplunkURL`: Your Splunk HEC endpoint (**REQUIRED**)
- `HECToken`: Your Splunk HEC token (**REQUIRED**)

## What Data You'll See in Splunk

### Metrics Mode (Default)

Every second, you'll receive metrics like this:

```json
{
  "time": "1699564800",
  "event": "metric",
  "source": "satisfactory-mod",
  "sourcetype": "satisfactory:metrics",
  "fields": {
    "metric_name:factory.power.consumption": 1500.0,
    "metric_name:factory.power.production": 2000.0,
    "metric_name:factory.power.net": 500.0,
    "metric_name:factory.machines.manufacturers": 45,
    "metric_name:factory.machines.extractors": 12,
    "metric_name:factory.machines.generators": 8,
    "metric_name:factory.efficiency.average": 0.95,
    "metric_name:factory.vehicles.wheeled": 3,
    "metric_name:factory.vehicles.trains": 2,
    "metric_name:factory.players": 1
  }
}
```

### Example Splunk Queries

```spl
# Real-time power monitoring
index=satisfactory sourcetype="satisfactory:metrics"
| timechart span=1s
    avg(metric_name:factory.power.production) as "Power Production"
    avg(metric_name:factory.power.consumption) as "Power Consumption"

# Factory efficiency over time
index=satisfactory sourcetype="satisfactory:metrics"
| timechart avg(metric_name:factory.efficiency.average) as "Average Efficiency"

# Machine counts
index=satisfactory sourcetype="satisfactory:metrics"
| timechart
    latest(metric_name:factory.machines.manufacturers) as Manufacturers
    latest(metric_name:factory.machines.extractors) as Extractors
    latest(metric_name:factory.machines.generators) as Generators
```

## Known Issues

### Critical Issues - ✅ FIXED

All critical compilation and runtime issues have been fixed in the current version:
- ✅ Missing includes added
- ✅ Null pointer checks implemented
- ✅ Power circuit stub documented (TODO for future)
- ✅ Time-based buffer flushing implemented
- ✅ Logging levels corrected

### Moderate Issues (Future Improvements)

#### 1. Incomplete Power Circuit Collection
**Location**: `SplunkExporter.cpp:335-338`
**Status**: Documented as TODO
**Description**: Power circuit collection from the power subsystem is not yet implemented.
**Impact**: Individual power circuit metrics are not available (aggregate power production/consumption still works).

#### 2. HTTP Error Handling
**Improvement Opportunity**: Only handles HTTP 200 status code. Could be enhanced to:
- Accept all 2xx success codes
- Implement retry logic with exponential backoff
- Handle network timeouts more gracefully

#### 3. Incomplete Vehicle Type Detection
**Location**: `SplunkExporter.cpp:366-390` (Legacy events mode)
**Description**: Some vehicle types may return "Unknown" in detailed events mode.
**Impact**: Minor - only affects legacy events mode, not metrics mode.

### Performance Considerations

The mod has been optimized for real-time streaming, but for very large factories (1000+ buildings):

- **Metrics Mode**: Very efficient - single aggregation pass per second
- **Actor Iteration**: Runs every second, may cause minor performance impact in mega-factories
- **Network**: 1 HTTP request/second (~300 bytes) - negligible overhead

**Future Optimizations**:
- Actor caching with spawn/destroy event listeners
- Spatial partitioning for large maps
- Optional gzip compression for HTTP payloads

## Technical Details

### Architecture

- **Clean Separation**: Metrics mode and legacy events mode operate independently
- **Proper UE Patterns**: Correct use of UCLASS, UPROPERTY, and Unreal Engine conventions
- **Blueprint Integration**: All functions are Blueprint-callable for easy integration
- **Timer-Based Collection**: Uses Unreal's timer system for reliable scheduling
- **Memory Safe**: Uses TSharedPtr for automatic memory management
- **Configurable**: Extensive UPROPERTY configuration options

### Metrics Collected

In metrics mode, these aggregate values are sent every second:
- `factory.power.consumption` - Total MW consumed by all machines
- `factory.power.production` - Total MW produced by all generators
- `factory.power.net` - Net power (production - consumption)
- `factory.machines.manufacturers` - Total count of manufacturers
- `factory.machines.extractors` - Total count of extractors
- `factory.machines.generators` - Total count of generators
- `factory.efficiency.average` - Average efficiency of all producing machines
- `factory.vehicles.wheeled` - Total count of wheeled vehicles
- `factory.vehicles.trains` - Total count of trains
- `factory.players` - Total count of players

## Troubleshooting

### Mod Not Loading
1. Check logs at `<Satisfactory>/FactoryGame/Saved/Logs/FactoryGame.log`
2. Verify SML is installed and working
3. Ensure folder structure matches installation instructions

### No Data in Splunk
1. Check mod logs for "Data successfully sent to Splunk (HTTP 200)"
2. Verify HEC token is correct
3. Test HEC endpoint with curl:
   ```bash
   curl -k https://your-splunk:8088/services/collector \
     -H "Authorization: Splunk your-token" \
     -d '{"event":"test"}'
   ```
4. Check Splunk HEC input is enabled
5. Verify firewall allows outbound HTTPS on port 8088

### Performance Issues
1. Try increasing `MetricsInterval` to 2-5 seconds
2. Disable legacy events mode if accidentally enabled
3. Check CPU/memory usage in Task Manager

## Development

### Building from Source
Requirements:
- Unreal Engine (version matching Satisfactory - check SML docs)
- Satisfactory Mod Loader SDK
- Visual Studio 2019 or later
- Alpakit (for packaging)

Build steps:
1. Clone this repository
2. Generate Visual Studio project files
3. Build in Visual Studio (Development Editor configuration)
4. Use Alpakit to package for distribution

### Contributing
Pull requests welcome! Areas for improvement:
- Power circuit collection implementation
- Additional metrics
- Configuration UI
- Performance optimizations
- Error handling improvements

## Future Enhancements

Planned features:
- ⏳ Power circuit individual metrics
- ⏳ Per-resource production tracking
- ⏳ Alert thresholds (configurable in-game)
- ⏳ Dashboard examples for Splunk
- ⏳ Configuration file support (JSON config)
- ⏳ Support for other backends (Prometheus, InfluxDB)
- ⏳ Compression support (gzip)
- ⏳ Retry logic with exponential backoff

## Version History

- **1.0.0** (Current) - Real-time metrics streaming
  - Fast metrics collection mode (1-second intervals)
  - Time-based buffer flushing
  - Aggregated factory statistics
  - Fixed all critical compilation issues
  - Production ready

## License

MIT License - Feel free to use and modify as needed.

## Support

This is an unofficial community mod. For issues or contributions:
- Report bugs via GitHub Issues
- Submit pull requests for improvements
- Check Satisfactory Modding Discord for community support
