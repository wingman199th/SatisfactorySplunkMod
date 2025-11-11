# Satisfactory Splunk Exporter Mod

## Overview
This Unreal Engine mod exports production, vehicle, player, and power data from Satisfactory to Splunk for analytics and monitoring. The mod collects telemetry data at configurable intervals and sends it to a Splunk HTTP Event Collector (HEC) endpoint.

## Features
- **Production Data Collection**: Monitors manufacturers and resource extractors
- **Power System Monitoring**: Tracks generators and power circuits
- **Vehicle Tracking**: Collects data from wheeled vehicles and trains
- **Player Movement**: Monitors player position and movement systems
- **Factory Layout**: Can export complete factory building layouts
- **Configurable Collection**: Toggle different data types and adjust collection intervals
- **Data Batching**: Buffers events and sends them in batches to reduce network overhead

## Configuration
The mod can be configured through Blueprint with the following parameters:

- `SplunkURL`: Your Splunk HEC endpoint (default: `https://your-splunk-instance:8088/services/collector`)
- `HECToken`: Your Splunk HEC authentication token
- `CollectionInterval`: How often to collect data in seconds (default: 30.0s)
- `BatchSize`: Number of events to buffer before sending (default: 10)
- `bCollectProductionData`: Enable/disable production data collection
- `bCollectVehicleData`: Enable/disable vehicle data collection
- `bCollectPlayerData`: Enable/disable player movement data collection
- `bCollectPowerData`: Enable/disable power system data collection
- `bCollectLayoutData`: Enable/disable factory layout data collection

## Known Issues

### Critical Issues (Must Fix Before Use)

#### 1. Missing Include Files (SplunkExporter.cpp:269-358)
**Location**: `SplunkExporter.cpp:269`
**Severity**: Critical - Will not compile
**Description**: The `CollectPowerData()` function uses classes that are not included:
- `AFGBuildableSubsystem`
- `AFGBuildablePowerGenerator`
- `AFGBuildablePowerGeneratorFuel`

**Impact**: Compilation failure

**Fix Required**: Add missing includes to `SplunkExporter.h`:
```cpp
#include "Buildables/FGBuildableSubsystem.h"
#include "Buildables/FGBuildablePowerGenerator.h"
#include "Buildables/FGBuildablePowerGeneratorFuel.h"
```

#### 2. Incomplete Power Circuit Collection (SplunkExporter.cpp:335-358)
**Location**: `SplunkExporter.cpp:335`
**Severity**: Critical - Feature incomplete
**Description**: Power circuit collection is stubbed out with a TODO comment. The `PowerCircuits` array is created but never populated.

```cpp
TArray<AFGPowerCircuit*> PowerCircuits;
// Note: You'll need to get power circuits from the power subsystem
```

**Impact**: No power circuit data will be collected even when `bCollectPowerData` is enabled.

**Fix Required**: Implement power circuit retrieval from the power subsystem or remove this section.

#### 3. Missing Additional Header Includes (SplunkExporter.h)
**Location**: `SplunkExporter.h`
**Severity**: Critical - Potential compilation issues
**Description**: Missing forward declarations or includes for:
- `AFGBuildableSubsystem`
- `AFGBuildablePowerGenerator`
- `AFGBuildablePowerGeneratorFuel`
- `AFGBuildableDockingStation`

**Impact**: May cause compilation errors depending on include order.

#### 4. Placeholder Configuration Values (SatisfactorySplunkMod.uplugin)
**Location**: `SatisfactorySplunkMod.uplugin:8`
**Severity**: High - Not production ready
**Description**: Plugin metadata contains placeholder values:
- `"CreatedBy": "YourName"`
- Default Splunk URL: `"https://your-splunk-instance:8088/services/collector"`
- Default HEC Token: `"your-hec-token-here"`

**Impact**: Users may not realize configuration is required.

**Fix Required**: Update metadata with actual values and add clear setup instructions.

### Moderate Issues (Should Fix)

#### 5. Potential Null Pointer Dereferences
**Location**: Multiple locations
**Severity**: Moderate - Runtime crashes possible
**Examples**:
- `SplunkExporter.cpp:181-183`: Recipe data accessed without validation after `GetDefaultObject<UFGRecipe>()`
- `SplunkExporter.cpp:192-193`: Product item descriptor accessed without null check
- `SplunkExporter.cpp:309, 313, 317`: Item descriptors accessed without validation

**Impact**: Potential crashes if game objects return null.

**Fix Required**: Add null checks after all `GetDefaultObject()` calls:
```cpp
UFGRecipe* Recipe = CurrentRecipe->GetDefaultObject<UFGRecipe>();
if (Recipe)
{
    // Safe to use Recipe
}
```

#### 6. Inconsistent HTTP Error Handling
**Location**: `SplunkExporter.cpp:843-863`
**Severity**: Moderate - Data loss possible
**Issues**:
- Only handles HTTP 200, not other success codes (201, 202, etc.)
- No retry logic for failed requests
- No handling of network timeouts
- Failed events are lost forever

**Impact**: Data may be silently lost on network issues or non-200 success responses.

**Fix Required**:
- Accept all 2xx status codes
- Implement exponential backoff retry logic
- Add timeout handling
- Consider persisting failed batches

#### 7. Hardcoded Energy Values
**Location**: `SplunkExporter.cpp:312, 474, 493`
**Severity**: Moderate - Inaccurate data
**Description**: Default energy value of `100.0f` is hardcoded when energy value cannot be determined.

**Impact**: Incorrect fuel/energy calculations for items that don't inherit from expected classes.

**Fix Required**: Make this a configurable constant or improve item type detection.

#### 8. Incomplete Vehicle Type Detection
**Location**: `SplunkExporter.cpp:366-390`
**Severity**: Moderate - Incomplete feature
**Description**: `GetVehicleTypeFromClass()` returns "Unknown" for unrecognized vehicle types.

**Impact**: Some vehicles may not be properly categorized.

**Fix Required**: Research all vehicle types in Satisfactory and add missing ones.

#### 9. Missing World Timer Null Check
**Location**: `SplunkExporter.cpp:42-54`
**Severity**: Moderate - Potential crash
**Description**: `GetWorldTimerManager()` is called without null check in `StartDataCollection()`.

**Impact**: Could crash if World becomes invalid between the initial check and timer setup.

**Fix Required**: Add null check for timer manager.

### Minor Issues (Nice to Have)

#### 10. Logging Verbosity Issues
**Location**: `SatisfactorySplunkMod.cpp:10, 21` and `SplunkExporter.cpp:53`
**Severity**: Low - Log clutter
**Description**: Normal startup messages use `Warning` log level instead of `Log` or `Display`.

**Impact**: Log files may be cluttered with false warnings.

**Fix Required**: Change log level to appropriate severity.

#### 11. No Documentation
**Severity**: Low - User experience
**Description**:
- No README (this file addresses this)
- No inline documentation for complex functions
- No setup/usage instructions
- No example Splunk queries or dashboards

**Impact**: Difficult for others to use or maintain.

#### 12. Memory Management on Errors
**Location**: Throughout
**Severity**: Low - Memory leak potential
**Description**: Heavy use of `TSharedPtr` (correct), but no cleanup on error conditions. Buffers are only cleared on successful sends.

**Impact**: Memory could grow if HTTP requests continuously fail.

**Fix Required**: Clear buffer on critical errors or implement max buffer size.

#### 13. No API Version Compatibility Checks
**Severity**: Low - Maintenance burden
**Description**: No versioning or compatibility checks for Satisfactory API.

**Impact**: Mod will likely break silently when Satisfactory updates.

**Fix Required**: Add version detection and compatibility checks, or at minimum log warnings.

### Performance Concerns

#### 14. Actor Iteration Performance
**Location**: All `Collect*Data()` functions
**Severity**: Moderate - Performance impact
**Description**: Multiple `TActorIterator` loops in each collection cycle. In large factories with thousands of buildings, this could be expensive.

**Impact**: Frame rate drops or stuttering during collection cycles in mega-factories.

**Optimization**:
- Cache actor references and update on actor spawn/destroy events
- Implement actor filtering to skip irrelevant actors
- Consider spatial partitioning for large maps

#### 15. Buffer Flushing Logic Issue
**Location**: `SplunkExporter.cpp:102-105`
**Severity**: Moderate - Data staleness
**Description**: Events are only sent when `DataBuffer.Num() >= BatchSize`. If production is low or collection is disabled, events sit in buffer indefinitely.

**Impact**: Stale data in Splunk or data loss if server crashes before batch size is reached.

**Fix Required**: Add time-based buffer flushing (e.g., flush every 5 minutes regardless of batch size).

#### 16. JSON Serialization Overhead
**Location**: `SendBufferedData()`, `SendLayoutDataToSplunk()`
**Severity**: Low - Performance/bandwidth
**Description**:
- Serializing large amounts of data every collection interval
- No data compression before sending to Splunk
- JSON is verbose compared to binary formats

**Impact**: Higher CPU usage and network bandwidth consumption.

**Optimization**:
- Implement gzip compression for HTTP payloads
- Consider binary serialization for very large factories
- Send delta updates instead of full state

## Positive Aspects

Despite the issues listed above, this mod has a solid foundation:

- **Clean Architecture**: Good separation of concerns with dedicated collection methods
- **Comprehensive Coverage**: Collects data from all major game systems
- **Proper UE Patterns**: Correct use of UCLASS, UPROPERTY, and Unreal Engine conventions
- **Blueprint Integration**: Well-designed blueprint-callable functions for easy integration
- **Timer-Based Collection**: Proper use of Unreal's timer system
- **Event Batching**: Smart buffering system to reduce network overhead
- **Configurable**: Extensive configuration options for different use cases

## Installation

### Prerequisites
- Satisfactory with modding support (SML - Satisfactory Mod Loader)
- A Splunk instance with HTTP Event Collector (HEC) enabled
- HEC token from your Splunk instance

### Setup
1. Install the mod through the Satisfactory Mod Manager or manually place in mods folder
2. Configure Splunk HEC endpoint and token in Blueprint or through mod configuration
3. Adjust collection intervals and data types as needed
4. Start Satisfactory and verify data is flowing to Splunk in the logs

### Splunk Configuration
1. Enable HTTP Event Collector in Splunk
2. Create a new HEC token
3. Set up an index for Satisfactory data (e.g., `satisfactory`)
4. Configure the mod with your HEC URL and token

## Development

### Building
This mod requires:
- Unreal Engine (version matching your Satisfactory installation)
- Satisfactory Mod Loader SDK
- C++ compiler toolchain

### Testing
Before deploying to production:
1. Fix all critical issues listed above
2. Test with a small factory first
3. Monitor Splunk ingestion for errors
4. Verify data accuracy
5. Test with large factories to check performance

## Future Enhancements

Potential improvements:
- Add configuration UI
- Implement metric aggregation before sending
- Add support for custom metrics
- Create example Splunk dashboards
- Add alerts for production issues
- Support for other backends (Prometheus, InfluxDB, etc.)
- Delta updates to reduce data volume
- Compression support
- Better error recovery

## Support

This is an unofficial community mod. For issues or contributions, please check the repository.

## License

Please specify license information.

## Version History

- **1.0.0** - Initial release (unreleased - has critical issues)
