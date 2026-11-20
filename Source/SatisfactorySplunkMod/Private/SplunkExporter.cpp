#include "SplunkExporter.h"
#include "SatisfactorySplunkMod.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "GameFramework/PlayerState.h"

#include "HttpModule.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"

// FactoryGame
#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Buildables/FGBuildableGenerator.h"
#include "Buildables/FGBuildableGeneratorFuel.h"
#include "Buildables/FGBuildablePowerStorage.h"
#include "Buildables/FGBuildableStorage.h"
#include "FGPowerCircuit.h"
#include "FGPowerInfoComponent.h"
#include "FGInventoryComponent.h"
#include "FGRecipe.h"
#include "Resources/FGItemDescriptor.h"
#include "Resources/FGResourceDescriptor.h"
#include "Resources/FGExtractableResourceInterface.h"
#include "FGCharacterPlayer.h"
#include "WheeledVehicles/FGWheeledVehicle.h"
#include "FGRailroadSubsystem.h"
#include "FGTrain.h"
#include "FGRailroadVehicle.h"
#include "FGFreightWagon.h"
#include "FGRailroadTimeTable.h"
#include "FGTrainStationIdentifier.h"

namespace
{
    const TCHAR* ModVersion = TEXT("2.1.0");
    const TCHAR* ConfigFileName = TEXT("SatisfactorySplunkMod.ini");

    // cm/s -> km/h
    constexpr float SpeedToKmh = 0.036f;

    // Max events per HTTP request, keeps payloads well under HEC limits.
    constexpr int32 MaxEventsPerRequest = 500;

    // Written to FactoryGame/Configs/ on first run if no config exists yet.
    const TCHAR* DefaultConfigTemplate =
        TEXT("; Satisfactory Splunk Exporter configuration\n")
        TEXT("; Edit the two values below, then restart the game.\n")
        TEXT("; Full option list: see README.md in the mod's repository.\n")
        TEXT("\n")
        TEXT("[Splunk]\n")
        TEXT("SplunkURL=http://your-splunk-server:8088/services/collector/event\n")
        TEXT("HECToken=your-hec-token-here\n")
        TEXT("Index=\n")
        TEXT("\n")
        TEXT("[Collection]\n")
        TEXT("SummaryInterval=5\n")
        TEXT("PowerInterval=5\n")
        TEXT("ProductionInterval=30\n")
        TEXT("InventoryInterval=30\n")
        TEXT("VehicleInterval=10\n")
        TEXT("PlayerInterval=10\n")
        TEXT("FlushInterval=5\n")
        TEXT("bCollectSummary=True\n")
        TEXT("bCollectPower=True\n")
        TEXT("bCollectProduction=True\n")
        TEXT("bCollectInventory=True\n")
        TEXT("bCollectVehicles=True\n")
        TEXT("bCollectPlayers=True\n")
        TEXT("MaxBufferedEvents=50000\n");

    FString ItemName(TSubclassOf<UFGItemDescriptor> ItemClass)
    {
        return ItemClass ? UFGItemDescriptor::GetItemName(ItemClass).ToString() : TEXT("Unknown");
    }

    // Fluids are stored internally in liters; the game UI shows m^3.
    float UnitDivisor(TSubclassOf<UFGItemDescriptor> ItemClass)
    {
        if (!ItemClass) return 1.0f;
        const EResourceForm Form = UFGItemDescriptor::GetForm(ItemClass);
        return (Form == EResourceForm::RF_LIQUID || Form == EResourceForm::RF_GAS) ? 1000.0f : 1.0f;
    }

    void SetLocationFields(const TSharedPtr<FJsonObject>& Obj, const FVector& Location)
    {
        Obj->SetNumberField(TEXT("x"), Location.X);
        Obj->SetNumberField(TEXT("y"), Location.Y);
        Obj->SetNumberField(TEXT("z"), Location.Z);
    }

    TArray<TSharedPtr<FJsonValue>> InventoryToJson(UFGInventoryComponent* Inventory, int32& OutSlotsUsed)
    {
        OutSlotsUsed = 0;
        TArray<TSharedPtr<FJsonValue>> Result;
        if (!Inventory) return Result;

        for (int32 Index = 0; Index < Inventory->GetSizeLinear(); ++Index)
        {
            FInventoryStack Stack;
            if (Inventory->GetStackFromIndex(Index, Stack) && Stack.HasItems())
            {
                OutSlotsUsed++;
                TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("item"), ItemName(Stack.Item.GetItemClass()));
                Item->SetNumberField(TEXT("count"), Stack.NumItems / UnitDivisor(Stack.Item.GetItemClass()));
                Result.Add(MakeShared<FJsonValueObject>(Item));
            }
        }
        return Result;
    }

    struct FCircuitInfo
    {
        int32 Buildings = 0;
        int32 Batteries = 0;
        float BatteryStoredMWh = 0.0f;
        float BatteryCapacityMWh = 0.0f;
    };

    // Power circuits aren't publicly enumerable from the circuit subsystem, so
    // gather them from the power connections of every factory building. This
    // also skips stale circuits left over (until GC) after networks merge.
    void GatherCircuits(UWorld* World, TMap<UFGPowerCircuit*, FCircuitInfo>& Out)
    {
        for (TActorIterator<AFGBuildableFactory> It(World); It; ++It)
        {
            UFGPowerInfoComponent* PowerInfo = It->GetPowerInfo();
            if (!PowerInfo) continue;

            UFGPowerCircuit* Circuit = PowerInfo->GetPowerCircuit();
            if (!IsValid(Circuit)) continue;

            FCircuitInfo& Info = Out.FindOrAdd(Circuit);
            Info.Buildings++;
            if (AFGBuildablePowerStorage* Battery = Cast<AFGBuildablePowerStorage>(*It))
            {
                Info.Batteries++;
                Info.BatteryStoredMWh += Battery->GetPowerStore();
                Info.BatteryCapacityMWh += Battery->GetPowerStoreCapacity();
            }
        }
    }
}

ASplunkExporter::ASplunkExporter()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = false;
    HostName = FPlatformProcess::ComputerName();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ASplunkExporter::BeginPlay()
{
    Super::BeginPlay();

    // Only the machine that owns the game state exports. Note HasAuthority()
    // is NOT enough: a client's locally spawned actor also has authority.
    UWorld* World = GetWorld();
    if (!World || World->GetNetMode() == NM_Client)
    {
        UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: running as a multiplayer client - exporting is done by the host/server, staying idle"));
        return;
    }

    UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter %s: starting up on '%s'"), ModVersion, *HostName);
    LoadSettingsFromConfig();
    StartDataCollection();
}

void ASplunkExporter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopDataCollection();
    FlushBuffer();
    Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------
// Configuration
//
// The first file found is used:
//   1. FactoryGame/Configs/SatisfactorySplunkMod.ini    (user's config, survives
//      mod updates - SMM replaces the whole mod folder on update)
//   2. <mod folder>/Config/SatisfactorySplunkMod.ini    (shipped template)
// then SPLUNK_HEC_URL / SPLUNK_HEC_TOKEN / SPLUNK_HEC_INDEX environment
// variables override whatever the file said.
//
// If (1) doesn't exist it is created from (2), or from a built-in template.
// A tiny parser is used instead of FConfigFile on purpose: it has no
// dependency on engine config-system internals, which change between UE versions.
// ---------------------------------------------------------------------------

void ASplunkExporter::ApplySetting(const FString& Key, const FString& RawValue)
{
    FString Value = RawValue;
    if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
    {
        Value = Value.Mid(1, Value.Len() - 2);
    }

    auto Is = [&Key](const TCHAR* Name) { return Key.Equals(Name, ESearchCase::IgnoreCase); };

    if      (Is(TEXT("SplunkURL")))          { SplunkURL = Value; }
    else if (Is(TEXT("HECToken")))           { HECToken = Value; }
    else if (Is(TEXT("Index")))              { SplunkIndex = Value; }
    else if (Is(TEXT("SummaryInterval")))    { SummaryInterval = FCString::Atof(*Value); }
    else if (Is(TEXT("PowerInterval")))      { PowerInterval = FCString::Atof(*Value); }
    else if (Is(TEXT("ProductionInterval"))) { ProductionInterval = FCString::Atof(*Value); }
    else if (Is(TEXT("InventoryInterval")))  { InventoryInterval = FCString::Atof(*Value); }
    else if (Is(TEXT("VehicleInterval")))    { VehicleInterval = FCString::Atof(*Value); }
    else if (Is(TEXT("PlayerInterval")))     { PlayerInterval = FCString::Atof(*Value); }
    else if (Is(TEXT("FlushInterval")))      { FlushInterval = FCString::Atof(*Value); }
    else if (Is(TEXT("bCollectSummary")))    { bCollectSummary = Value.ToBool(); }
    else if (Is(TEXT("bCollectPower")))      { bCollectPower = Value.ToBool(); }
    else if (Is(TEXT("bCollectProduction"))) { bCollectProduction = Value.ToBool(); }
    else if (Is(TEXT("bCollectInventory")))  { bCollectInventory = Value.ToBool(); }
    else if (Is(TEXT("bCollectVehicles")))   { bCollectVehicles = Value.ToBool(); }
    else if (Is(TEXT("bCollectPlayers")))    { bCollectPlayers = Value.ToBool(); }
    else if (Is(TEXT("MaxBufferedEvents")))  { MaxBufferedEvents = FCString::Atoi(*Value); }
    else
    {
        UE_LOG(LogSatisfactorySplunkMod, Warning, TEXT("SplunkExporter: unknown config key '%s' ignored"), *Key);
    }
}

void ASplunkExporter::LoadSettingsFromConfig()
{
    FString ModConfigPath;
    if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SatisfactorySplunkMod")))
    {
        ModConfigPath = Plugin->GetBaseDir() / TEXT("Config") / ConfigFileName;
    }
    const FString UserConfigPath = FPaths::ProjectDir() / TEXT("Configs") / ConfigFileName;

    // First run: create the user config so there's always one obvious file to edit.
    if (!IFileManager::Get().FileExists(*UserConfigPath))
    {
        FString Template;
        if (ModConfigPath.IsEmpty() || !FFileHelper::LoadFileToString(Template, *ModConfigPath))
        {
            Template = DefaultConfigTemplate;
        }
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(UserConfigPath), true);
        if (FFileHelper::SaveStringToFile(Template, *UserConfigPath))
        {
            UE_LOG(LogSatisfactorySplunkMod, Warning,
                TEXT("SplunkExporter: created config file %s - set SplunkURL and HECToken in it, then restart the game"),
                *FPaths::ConvertRelativePathToFull(UserConfigPath));
        }
    }

    auto ParseFile = [this](const FString& Path) -> bool
    {
        TArray<FString> Lines;
        if (Path.IsEmpty() || !FFileHelper::LoadFileToStringArray(Lines, *Path))
        {
            return false;
        }
        for (FString Line : Lines)
        {
            Line.TrimStartAndEndInline();
            if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")) || Line.StartsWith(TEXT("[")))
            {
                continue;
            }
            FString Key, Value;
            if (Line.Split(TEXT("="), &Key, &Value))
            {
                ApplySetting(Key.TrimStartAndEnd(), Value.TrimStartAndEnd());
            }
        }
        return true;
    };

    if (ParseFile(UserConfigPath))
    {
        ConfigPathUsed = FPaths::ConvertRelativePathToFull(UserConfigPath);
    }
    else if (ParseFile(ModConfigPath))
    {
        ConfigPathUsed = FPaths::ConvertRelativePathToFull(ModConfigPath);
    }
    else
    {
        UE_LOG(LogSatisfactorySplunkMod, Error,
            TEXT("SplunkExporter: no config found. Tried '%s' and '%s'"),
            *FPaths::ConvertRelativePathToFull(UserConfigPath), *FPaths::ConvertRelativePathToFull(ModConfigPath));
    }

    // Environment variables override files - handy for dedicated servers.
    const FString EnvUrl   = FPlatformMisc::GetEnvironmentVariable(TEXT("SPLUNK_HEC_URL"));
    const FString EnvToken = FPlatformMisc::GetEnvironmentVariable(TEXT("SPLUNK_HEC_TOKEN"));
    const FString EnvIndex = FPlatformMisc::GetEnvironmentVariable(TEXT("SPLUNK_HEC_INDEX"));
    if (!EnvUrl.IsEmpty())   { SplunkURL = EnvUrl; }
    if (!EnvToken.IsEmpty()) { HECToken = EnvToken; }
    if (!EnvIndex.IsEmpty()) { SplunkIndex = EnvIndex; }

    // Clamp so a bad config can't spin-loop the game thread.
    SummaryInterval    = FMath::Max(1.0f, SummaryInterval);
    PowerInterval      = FMath::Max(1.0f, PowerInterval);
    ProductionInterval = FMath::Max(1.0f, ProductionInterval);
    InventoryInterval  = FMath::Max(1.0f, InventoryInterval);
    VehicleInterval    = FMath::Max(1.0f, VehicleInterval);
    PlayerInterval     = FMath::Max(1.0f, PlayerInterval);
    FlushInterval      = FMath::Max(1.0f, FlushInterval);
    MaxBufferedEvents  = FMath::Max(1000, MaxBufferedEvents);

    UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: config '%s' -> URL '%s', token '%s...', index '%s'"),
        *ConfigPathUsed, *SplunkURL, *HECToken.Left(4), SplunkIndex.IsEmpty() ? TEXT("(token default)") : *SplunkIndex);
}

bool ASplunkExporter::IsConfigured() const
{
    return !SplunkURL.IsEmpty()
        && !HECToken.IsEmpty()
        && !SplunkURL.Contains(TEXT("your-splunk"))
        && !HECToken.Contains(TEXT("your-hec-token"));
}

void ASplunkExporter::StartDataCollection()
{
    if (!IsConfigured())
    {
        UE_LOG(LogSatisfactorySplunkMod, Error,
            TEXT("SplunkExporter: NOT STARTED - SplunkURL / HECToken are still placeholders. Edit %s and restart the game."),
            ConfigPathUsed.IsEmpty() ? TEXT("FactoryGame/Configs/SatisfactorySplunkMod.ini") : *ConfigPathUsed);
        return;
    }
    if (!SplunkURL.StartsWith(TEXT("http://")) && !SplunkURL.StartsWith(TEXT("https://")))
    {
        UE_LOG(LogSatisfactorySplunkMod, Error, TEXT("SplunkExporter: NOT STARTED - SplunkURL must start with http:// or https:// (got '%s')"), *SplunkURL);
        return;
    }

    UWorld* World = GetWorld();
    if (!World || bIsCollecting)
    {
        return;
    }

    // First collection shortly after load so data shows up quickly.
    constexpr float FirstDelay = 3.0f;
    FTimerManager& TM = World->GetTimerManager();
    if (bCollectSummary)    { TM.SetTimer(SummaryTimer,    this, &ASplunkExporter::CollectSummary,    SummaryInterval,    true, FirstDelay); }
    if (bCollectPower)      { TM.SetTimer(PowerTimer,      this, &ASplunkExporter::CollectPower,      PowerInterval,      true, FirstDelay); }
    if (bCollectProduction) { TM.SetTimer(ProductionTimer, this, &ASplunkExporter::CollectProduction, ProductionInterval, true, FirstDelay); }
    if (bCollectInventory)  { TM.SetTimer(InventoryTimer,  this, &ASplunkExporter::CollectInventory,  InventoryInterval,  true, FirstDelay); }
    if (bCollectVehicles)   { TM.SetTimer(VehicleTimer,    this, &ASplunkExporter::CollectVehicles,   VehicleInterval,    true, FirstDelay); }
    if (bCollectPlayers)    { TM.SetTimer(PlayerTimer,     this, &ASplunkExporter::CollectPlayers,    PlayerInterval,     true, FirstDelay); }
    TM.SetTimer(FlushTimer, this, &ASplunkExporter::FlushBuffer, FlushInterval, true);

    bIsCollecting = true;
    UE_LOG(LogSatisfactorySplunkMod, Log,
        TEXT("SplunkExporter: collection started (summary %.0fs, power %.0fs, production %.0fs, inventory %.0fs, vehicles %.0fs, players %.0fs, flush %.0fs)"),
        SummaryInterval, PowerInterval, ProductionInterval, InventoryInterval, VehicleInterval, PlayerInterval, FlushInterval);

    // Send one event right away so connectivity problems show up in the log immediately.
    SendStartupEvent();
    FlushBuffer();
}

void ASplunkExporter::StopDataCollection()
{
    UWorld* World = GetWorld();
    if (!World || !bIsCollecting)
    {
        return;
    }

    FTimerManager& TM = World->GetTimerManager();
    TM.ClearTimer(SummaryTimer);
    TM.ClearTimer(PowerTimer);
    TM.ClearTimer(ProductionTimer);
    TM.ClearTimer(InventoryTimer);
    TM.ClearTimer(VehicleTimer);
    TM.ClearTimer(PlayerTimer);
    TM.ClearTimer(FlushTimer);

    bIsCollecting = false;
    UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: collection stopped (%d events sent, %d dropped)"), EventsSentTotal, EventsDropped);
}

// ---------------------------------------------------------------------------
// Event helpers
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> ASplunkExporter::NewSplunkEvent(const FString& SourceType) const
{
    TSharedPtr<FJsonObject> Event = MakeShared<FJsonObject>();
    Event->SetNumberField(TEXT("time"), static_cast<double>(FDateTime::UtcNow().ToUnixTimestamp()));
    Event->SetStringField(TEXT("host"), HostName);
    Event->SetStringField(TEXT("source"), TEXT("satisfactory-splunk-mod"));
    Event->SetStringField(TEXT("sourcetype"), SourceType);
    if (!SplunkIndex.IsEmpty())
    {
        Event->SetStringField(TEXT("index"), SplunkIndex);
    }
    return Event;
}

void ASplunkExporter::AddEventToBuffer(const TSharedPtr<FJsonObject>& EventObject)
{
    if (!EventObject.IsValid())
    {
        return;
    }
    if (DataBuffer.Num() >= MaxBufferedEvents)
    {
        EventsDropped++;
        return;
    }
    DataBuffer.Add(EventObject);
}

void ASplunkExporter::SendStartupEvent()
{
    TSharedPtr<FJsonObject> Event = NewSplunkEvent(TEXT("satisfactory:status"));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("message"), TEXT("exporter started"));
    Data->SetStringField(TEXT("mod_version"), ModVersion);
    Data->SetStringField(TEXT("net_mode"), GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer ? TEXT("dedicated_server") : TEXT("host"));
    Event->SetObjectField(TEXT("event"), Data);
    AddEventToBuffer(Event);
}

// ---------------------------------------------------------------------------
// Collectors
// ---------------------------------------------------------------------------

void ASplunkExporter::CollectSummary()
{
    UWorld* World = GetWorld();
    if (!World) return;

    int32 Manufacturers = 0;
    int32 Extractors = 0;
    int32 Generators = 0;
    int32 Producing = 0;
    float ProductivitySum = 0.0f;

    for (TActorIterator<AFGBuildableManufacturer> It(World); It; ++It)
    {
        Manufacturers++;
        ProductivitySum += It->GetProductivity();
        if (It->IsProducing()) { Producing++; }
    }
    for (TActorIterator<AFGBuildableResourceExtractor> It(World); It; ++It)
    {
        Extractors++;
        ProductivitySum += It->GetProductivity();
        if (It->IsProducing()) { Producing++; }
    }
    for (TActorIterator<AFGBuildableGenerator> It(World); It; ++It)
    {
        Generators++;
    }

    TMap<UFGPowerCircuit*, FCircuitInfo> Circuits;
    GatherCircuits(World, Circuits);

    float Produced = 0.0f, Consumed = 0.0f, Capacity = 0.0f, BatteryStored = 0.0f;
    int32 FusesTripped = 0;
    for (const TPair<UFGPowerCircuit*, FCircuitInfo>& Pair : Circuits)
    {
        FPowerCircuitStats Stats;
        Pair.Key->GetStats(Stats);
        Produced += Stats.PowerProduced;
        Consumed += Stats.PowerConsumed;
        Capacity += Stats.PowerProductionCapacity;
        BatteryStored += Pair.Value.BatteryStoredMWh;
        if (Pair.Key->IsFuseTriggered()) { FusesTripped++; }
    }

    int32 Players = 0;
    for (TActorIterator<AFGCharacterPlayer> It(World); It; ++It) { Players++; }

    int32 Trucks = 0;
    for (TActorIterator<AFGWheeledVehicle> It(World); It; ++It) { Trucks++; }

    int32 Trains = 0;
    if (AFGRailroadSubsystem* Railroad = AFGRailroadSubsystem::Get(World))
    {
        TArray<AFGTrain*> AllTrains;
        Railroad->GetAllTrains(AllTrains);
        Trains = AllTrains.Num();
    }

    const int32 Machines = Manufacturers + Extractors;

    TSharedPtr<FJsonObject> Event = NewSplunkEvent(TEXT("satisfactory:summary"));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("manufacturers"), Manufacturers);
    Data->SetNumberField(TEXT("extractors"), Extractors);
    Data->SetNumberField(TEXT("generators"), Generators);
    Data->SetNumberField(TEXT("machines_producing"), Producing);
    Data->SetNumberField(TEXT("machines_idle"), Machines - Producing);
    Data->SetNumberField(TEXT("avg_productivity"), Machines > 0 ? ProductivitySum / Machines : 0.0f);
    Data->SetNumberField(TEXT("power_circuits"), Circuits.Num());
    Data->SetNumberField(TEXT("power_produced_mw"), Produced);
    Data->SetNumberField(TEXT("power_consumed_mw"), Consumed);
    Data->SetNumberField(TEXT("power_capacity_mw"), Capacity);
    Data->SetNumberField(TEXT("battery_stored_mwh"), BatteryStored);
    Data->SetNumberField(TEXT("fuses_tripped"), FusesTripped);
    Data->SetNumberField(TEXT("players"), Players);
    Data->SetNumberField(TEXT("trucks"), Trucks);
    Data->SetNumberField(TEXT("trains"), Trains);
    Event->SetObjectField(TEXT("event"), Data);
    AddEventToBuffer(Event);
}

void ASplunkExporter::CollectPower()
{
    UWorld* World = GetWorld();
    if (!World) return;

    TMap<UFGPowerCircuit*, FCircuitInfo> Circuits;
    GatherCircuits(World, Circuits);

    for (const TPair<UFGPowerCircuit*, FCircuitInfo>& Pair : Circuits)
    {
        UFGPowerCircuit* Circuit = Pair.Key;
        FPowerCircuitStats Stats;
        Circuit->GetStats(Stats);

        TSharedPtr<FJsonObject> Event = NewSplunkEvent(TEXT("satisfactory:power"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetNumberField(TEXT("circuit_id"), Circuit->GetCircuitID());
        Data->SetNumberField(TEXT("buildings"), Pair.Value.Buildings);
        Data->SetNumberField(TEXT("produced_mw"), Stats.PowerProduced);
        Data->SetNumberField(TEXT("consumed_mw"), Stats.PowerConsumed);
        Data->SetNumberField(TEXT("capacity_mw"), Stats.PowerProductionCapacity);
        Data->SetNumberField(TEXT("headroom_mw"), Stats.PowerProductionCapacity - Stats.PowerConsumed);
        Data->SetNumberField(TEXT("battery_input_mw"), Stats.BatteryPowerInput);
        Data->SetNumberField(TEXT("batteries"), Pair.Value.Batteries);
        Data->SetNumberField(TEXT("battery_stored_mwh"), Pair.Value.BatteryStoredMWh);
        Data->SetNumberField(TEXT("battery_capacity_mwh"), Pair.Value.BatteryCapacityMWh);
        Data->SetBoolField(TEXT("fuse_triggered"), Circuit->IsFuseTriggered());
        Event->SetObjectField(TEXT("event"), Data);
        AddEventToBuffer(Event);
    }
}

void ASplunkExporter::CollectProduction()
{
    UWorld* World = GetWorld();
    if (!World) return;

    // Manufacturers: recipe, throughput, and what (if anything) they wait on.
    for (TActorIterator<AFGBuildableManufacturer> It(World); It; ++It)
    {
        AFGBuildableManufacturer* Machine = *It;

        TSharedPtr<FJsonObject> Event = NewSplunkEvent(TEXT("satisfactory:production"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

        Data->SetStringField(TEXT("machine_class"), TEXT("manufacturer"));
        Data->SetStringField(TEXT("machine_type"), Machine->GetClass()->GetName());
        Data->SetStringField(TEXT("machine_id"), Machine->GetName());
        SetLocationFields(Data, Machine->GetActorLocation());

        const bool bProducing = Machine->IsProducing();
        const bool bPaused = Machine->IsProductionPaused();
        const bool bHasPower = Machine->HasPower();
        const float Productivity = Machine->GetProductivity();
        const float RawBoost = Machine->GetCurrentProductionBoost();
        const float Boost = RawBoost > 0.0f ? RawBoost : 1.0f;

        Data->SetBoolField(TEXT("is_producing"), bProducing);
        Data->SetBoolField(TEXT("is_paused"), bPaused);
        Data->SetBoolField(TEXT("has_power"), bHasPower);
        Data->SetNumberField(TEXT("productivity"), Productivity);
        Data->SetNumberField(TEXT("clock_speed"), Machine->GetCurrentPotential());
        Data->SetNumberField(TEXT("production_boost"), Boost);
        if (UFGPowerInfoComponent* Power = Machine->GetPowerInfo())
        {
            Data->SetNumberField(TEXT("power_consumption_mw"), Power->GetActualConsumption());
        }

        FString Status = TEXT("idle");
        TArray<TSharedPtr<FJsonValue>> WaitingFor;

        TSubclassOf<UFGRecipe> Recipe = Machine->GetCurrentRecipe();
        if (!Recipe)
        {
            Status = TEXT("no_recipe");
        }
        else
        {
            Data->SetStringField(TEXT("recipe"), UFGRecipe::GetRecipeName(Recipe).ToString());

            // Cycle time already includes the clock speed (potential).
            const float CycleTime = Machine->GetProductionCycleTime();
            const float CyclesPerMin = (CycleTime > 0.0f) ? 60.0f / CycleTime : 0.0f;
            Data->SetNumberField(TEXT("cycle_time_s"), CycleTime);

            // Outputs: rate at this clock (incl. somersloop boost); actual = that * productivity.
            UFGInventoryComponent* OutputInv = Machine->GetOutputInventory();
            TArray<TSharedPtr<FJsonValue>> Outputs;
            for (const FItemAmount& Product : UFGRecipe::GetProducts(Recipe))
            {
                const float Div = UnitDivisor(Product.ItemClass);
                const float PerMin = Product.Amount * CyclesPerMin * Boost / Div;
                TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
                Out->SetStringField(TEXT("item"), ItemName(Product.ItemClass));
                Out->SetNumberField(TEXT("per_min_at_clock"), PerMin);
                Out->SetNumberField(TEXT("per_min_actual"), PerMin * Productivity);
                if (OutputInv && Product.ItemClass)
                {
                    Out->SetNumberField(TEXT("buffered"), OutputInv->GetNumItems(Product.ItemClass) / Div);
                }
                Outputs.Add(MakeShared<FJsonValueObject>(Out));
            }
            Data->SetArrayField(TEXT("outputs"), Outputs);

            // Inputs: required rate plus how much is buffered in the input inventory.
            UFGInventoryComponent* InputInv = Machine->GetInputInventory();
            TArray<TSharedPtr<FJsonValue>> Inputs;
            bool bInputStarved = false;
            for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(this, Recipe))
            {
                const float Div = UnitDivisor(Ingredient.ItemClass);
                const int32 Buffered = (InputInv && Ingredient.ItemClass) ? InputInv->GetNumItems(Ingredient.ItemClass) : 0;

                TSharedPtr<FJsonObject> In = MakeShared<FJsonObject>();
                In->SetStringField(TEXT("item"), ItemName(Ingredient.ItemClass));
                In->SetNumberField(TEXT("per_min_at_clock"), Ingredient.Amount * CyclesPerMin / Div);
                In->SetNumberField(TEXT("buffered"), Buffered / Div);
                Inputs.Add(MakeShared<FJsonValueObject>(In));

                // Not enough for one more cycle (raw units on both sides).
                if (Buffered < Ingredient.Amount)
                {
                    bInputStarved = true;
                    WaitingFor.Add(MakeShared<FJsonValueString>(ItemName(Ingredient.ItemClass)));
                }
            }
            Data->SetArrayField(TEXT("inputs"), Inputs);

            if (bPaused)              { Status = TEXT("paused"); }
            else if (!bHasPower)      { Status = TEXT("no_power"); }
            else if (bProducing)      { Status = TEXT("producing"); }
            else if (bInputStarved)   { Status = TEXT("waiting_for_input"); }
            else                      { Status = TEXT("output_backed_up"); }
        }

        Data->SetStringField(TEXT("status"), Status);
        if (WaitingFor.Num() > 0)
        {
            Data->SetArrayField(TEXT("waiting_for"), WaitingFor);
        }

        Event->SetObjectField(TEXT("event"), Data);
        AddEventToBuffer(Event);
    }

    // Resource extractors (miners, oil extractors, water pumps, ...).
    for (TActorIterator<AFGBuildableResourceExtractor> It(World); It; ++It)
    {
        AFGBuildableResourceExtractor* Extractor = *It;

        TSharedPtr<FJsonObject> Event = NewSplunkEvent(TEXT("satisfactory:production"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

        Data->SetStringField(TEXT("machine_class"), TEXT("extractor"));
        Data->SetStringField(TEXT("machine_type"), Extractor->GetClass()->GetName());
        Data->SetStringField(TEXT("machine_id"), Extractor->GetName());
        SetLocationFields(Data, Extractor->GetActorLocation());

        const bool bProducing = Extractor->IsProducing();
        const bool bPaused = Extractor->IsProductionPaused();
        const bool bHasPower = Extractor->HasPower();
        const float Productivity = Extractor->GetProductivity();
        // Already in UI units (items/min, or m^3/min for fluids).
        const float PerMin = Extractor->GetExtractionPerMinute();

        Data->SetBoolField(TEXT("is_producing"), bProducing);
        Data->SetBoolField(TEXT("is_paused"), bPaused);
        Data->SetBoolField(TEXT("has_power"), bHasPower);
        Data->SetNumberField(TEXT("productivity"), Productivity);
        Data->SetNumberField(TEXT("clock_speed"), Extractor->GetCurrentPotential());
        Data->SetNumberField(TEXT("per_min_at_clock"), PerMin);
        Data->SetNumberField(TEXT("per_min_actual"), PerMin * Productivity);
        if (UFGPowerInfoComponent* Power = Extractor->GetPowerInfo())
        {
            Data->SetNumberField(TEXT("power_consumption_mw"), Power->GetActualConsumption());
        }

        TScriptInterface<IFGExtractableResourceInterface> Resource = Extractor->GetExtractableResource();
        if (Resource.GetInterface())
        {
            Data->SetStringField(TEXT("resource"), ItemName(Resource->GetResourceClass()));
        }

        FString Status;
        if (bPaused)         { Status = TEXT("paused"); }
        else if (!bHasPower) { Status = TEXT("no_power"); }
        else if (bProducing) { Status = TEXT("producing"); }
        else                 { Status = TEXT("output_backed_up"); }
        Data->SetStringField(TEXT("status"), Status);

        Event->SetObjectField(TEXT("event"), Data);
        AddEventToBuffer(Event);
    }

    // Generators: capacity, fuel state.
    for (TActorIterator<AFGBuildableGenerator> It(World); It; ++It)
    {
        AFGBuildableGenerator* Generator = *It;

        TSharedPtr<FJsonObject> Event = NewSplunkEvent(TEXT("satisfactory:production"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

        Data->SetStringField(TEXT("machine_class"), TEXT("generator"));
        Data->SetStringField(TEXT("machine_type"), Generator->GetClass()->GetName());
        Data->SetStringField(TEXT("machine_id"), Generator->GetName());
        SetLocationFields(Data, Generator->GetActorLocation());
        Data->SetBoolField(TEXT("is_producing"), Generator->IsProducing());
        Data->SetNumberField(TEXT("capacity_mw"), Generator->GetPowerProductionCapacity());
        Data->SetNumberField(TEXT("clock_speed"), Generator->GetCurrentPotential());

        FString Status = Generator->IsProducing() ? TEXT("producing") : TEXT("idle");
        if (AFGBuildableGeneratorFuel* FuelGenerator = Cast<AFGBuildableGeneratorFuel>(Generator))
        {
            const bool bHasFuel = FuelGenerator->HasFuel();
            Data->SetBoolField(TEXT("has_fuel"), bHasFuel);
            Data->SetNumberField(TEXT("fuel_amount"), FuelGenerator->GetFuelAmount());
            if (!bHasFuel)
            {
                Status = TEXT("waiting_for_fuel");
            }
        }
        Data->SetStringField(TEXT("status"), Status);

        Event->SetObjectField(TEXT("event"), Data);
        AddEventToBuffer(Event);
    }
}

void ASplunkExporter::CollectInventory()
{
    UWorld* World = GetWorld();
    if (!World) return;

    // Total of every item type sitting in storage containers.
    TMap<FString, double> Totals;
    TMap<FString, int32> ContainersWithItem;
    int32 Containers = 0;

    for (TActorIterator<AFGBuildableStorage> It(World); It; ++It)
    {
        UFGInventoryComponent* Inventory = It->GetStorageInventory();
        if (!Inventory) continue;
        Containers++;

        TSet<FString> SeenHere;
        for (int32 Index = 0; Index < Inventory->GetSizeLinear(); ++Index)
        {
            FInventoryStack Stack;
            if (Inventory->GetStackFromIndex(Index, Stack) && Stack.HasItems())
            {
                const FString Name = ItemName(Stack.Item.GetItemClass());
                Totals.FindOrAdd(Name) += Stack.NumItems / UnitDivisor(Stack.Item.GetItemClass());
                SeenHere.Add(Name);
            }
        }
        for (const FString& Name : SeenHere)
        {
            ContainersWithItem.FindOrAdd(Name)++;
        }
    }

    for (const TPair<FString, double>& Pair : Totals)
    {
        TSharedPtr<FJsonObject> Event = NewSplunkEvent(TEXT("satisfactory:inventory"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("item"), Pair.Key);
        Data->SetNumberField(TEXT("count"), Pair.Value);
        Data->SetNumberField(TEXT("containers_with_item"), ContainersWithItem.FindRef(Pair.Key));
        Data->SetNumberField(TEXT("containers_scanned"), Containers);
        Event->SetObjectField(TEXT("event"), Data);
        AddEventToBuffer(Event);
    }
}

void ASplunkExporter::CollectVehicles()
{
    UWorld* World = GetWorld();
    if (!World) return;

    // Wheeled vehicles (trucks, tractors, explorers, factory carts).
    for (TActorIterator<AFGWheeledVehicle> It(World); It; ++It)
    {
        AFGWheeledVehicle* Vehicle = *It;

        TSharedPtr<FJsonObject> Event = NewSplunkEvent(TEXT("satisfactory:vehicle"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

        Data->SetStringField(TEXT("vehicle_class"), TEXT("wheeled"));
        Data->SetStringField(TEXT("vehicle_type"), Vehicle->GetClass()->GetName());
        Data->SetStringField(TEXT("vehicle_id"), Vehicle->GetName());
        SetLocationFields(Data, Vehicle->GetActorLocation());
        Data->SetNumberField(TEXT("speed_kmh"), Vehicle->GetVelocity().Size() * SpeedToKmh);
        Data->SetBoolField(TEXT("has_fuel"), Vehicle->HasFuel());

        if (UFGInventoryComponent* Cargo = Vehicle->GetStorageInventory())
        {
            int32 SlotsUsed = 0;
            Data->SetArrayField(TEXT("cargo"), InventoryToJson(Cargo, SlotsUsed));
            Data->SetNumberField(TEXT("cargo_slots_used"), SlotsUsed);
            Data->SetNumberField(TEXT("cargo_slots_total"), Cargo->GetSizeLinear());
        }

        Event->SetObjectField(TEXT("event"), Data);
        AddEventToBuffer(Event);
    }

    // Trains.
    if (AFGRailroadSubsystem* Railroad = AFGRailroadSubsystem::Get(World))
    {
        TArray<AFGTrain*> Trains;
        Railroad->GetAllTrains(Trains);

        for (AFGTrain* Train : Trains)
        {
            if (!IsValid(Train)) continue;

            TSharedPtr<FJsonObject> Event = NewSplunkEvent(TEXT("satisfactory:vehicle"));
            TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

            Data->SetStringField(TEXT("vehicle_class"), TEXT("train"));
            Data->SetStringField(TEXT("vehicle_id"), Train->GetName());
            Data->SetStringField(TEXT("train_name"), Train->GetTrainName().ToString());
            Data->SetBoolField(TEXT("self_driving"), Train->IsSelfDrivingEnabled());

            if (AFGRailroadVehicle* FirstVehicle = Train->GetFirstVehicle())
            {
                SetLocationFields(Data, FirstVehicle->GetActorLocation());
                Data->SetNumberField(TEXT("speed_kmh"), FirstVehicle->GetVelocity().Size() * SpeedToKmh);
            }

            if (AFGRailroadTimeTable* TimeTable = Train->GetTimeTable())
            {
                TArray<FTimeTableStop> Stops;
                TimeTable->GetStops(Stops);
                Data->SetNumberField(TEXT("timetable_stops"), Stops.Num());

                const int32 CurrentStop = TimeTable->GetCurrentStop();
                Data->SetNumberField(TEXT("current_stop_index"), CurrentStop);
                if (Stops.IsValidIndex(CurrentStop) && Stops[CurrentStop].Station)
                {
                    Data->SetStringField(TEXT("next_station"), Stops[CurrentStop].Station->GetStationName().ToString());
                }
            }

            Event->SetObjectField(TEXT("event"), Data);
            AddEventToBuffer(Event);
        }
    }

    // Freight wagon cargo - what each train is actually hauling.
    for (TActorIterator<AFGFreightWagon> It(World); It; ++It)
    {
        AFGFreightWagon* Wagon = *It;
        UFGInventoryComponent* Freight = Wagon->GetFreightInventory();
        if (!Freight) continue;

        TSharedPtr<FJsonObject> Event = NewSplunkEvent(TEXT("satisfactory:vehicle"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

        Data->SetStringField(TEXT("vehicle_class"), TEXT("freight_wagon"));
        Data->SetStringField(TEXT("vehicle_id"), Wagon->GetName());
        if (AFGTrain* Train = Wagon->GetTrain())
        {
            Data->SetStringField(TEXT("train_name"), Train->GetTrainName().ToString());
        }
        SetLocationFields(Data, Wagon->GetActorLocation());

        int32 SlotsUsed = 0;
        Data->SetArrayField(TEXT("cargo"), InventoryToJson(Freight, SlotsUsed));
        Data->SetNumberField(TEXT("cargo_slots_used"), SlotsUsed);
        Data->SetNumberField(TEXT("cargo_slots_total"), Freight->GetSizeLinear());

        Event->SetObjectField(TEXT("event"), Data);
        AddEventToBuffer(Event);
    }
}

void ASplunkExporter::CollectPlayers()
{
    UWorld* World = GetWorld();
    if (!World) return;

    for (TActorIterator<AFGCharacterPlayer> It(World); It; ++It)
    {
        AFGCharacterPlayer* Player = *It;

        TSharedPtr<FJsonObject> Event = NewSplunkEvent(TEXT("satisfactory:player"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

        // Offline players' characters stay in the world without a PlayerState.
        APlayerState* State = Player->GetPlayerState();
        Data->SetStringField(TEXT("player"), State ? State->GetPlayerName() : Player->GetName());
        Data->SetBoolField(TEXT("online"), State != nullptr);
        SetLocationFields(Data, Player->GetActorLocation());
        Data->SetNumberField(TEXT("speed_kmh"), Player->GetVelocity().Size() * SpeedToKmh);
        Data->SetBoolField(TEXT("in_vehicle"), Player->IsDrivingVehicle());

        Event->SetObjectField(TEXT("event"), Data);
        AddEventToBuffer(Event);
    }
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

void ASplunkExporter::FlushBuffer()
{
    if (DataBuffer.Num() == 0 || !IsConfigured())
    {
        DataBuffer.Reset();
        return;
    }

    // Splunk HEC batch format: concatenated JSON event objects.
    FString Payload;
    int32 InPayload = 0;
    for (const TSharedPtr<FJsonObject>& Event : DataBuffer)
    {
        FString EventString;
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&EventString);
        FJsonSerializer::Serialize(Event.ToSharedRef(), Writer);
        Payload += EventString;
        Payload += TEXT("\n");

        if (++InPayload >= MaxEventsPerRequest)
        {
            SendToSplunk(Payload, InPayload);
            Payload.Reset();
            InPayload = 0;
        }
    }
    if (InPayload > 0)
    {
        SendToSplunk(Payload, InPayload);
    }

    DataBuffer.Reset();
}

void ASplunkExporter::SendToSplunk(const FString& Payload, int32 EventCount)
{
    FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
    Request->OnProcessRequestComplete().BindUObject(this, &ASplunkExporter::OnHttpResponse, EventCount);
    Request->SetURL(SplunkURL);
    Request->SetVerb(TEXT("POST"));
    Request->SetTimeout(15.0f);
    Request->SetHeader(TEXT("User-Agent"), FString::Printf(TEXT("SatisfactorySplunkMod/%s"), ModVersion));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Splunk %s"), *HECToken));
    Request->SetContentAsString(Payload);
    Request->ProcessRequest();
}

void ASplunkExporter::OnHttpResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, int32 EventCount)
{
    const int32 Code = (bWasSuccessful && Response.IsValid()) ? Response->GetResponseCode() : 0;

    if (Code >= 200 && Code < 300)
    {
        if (ConsecutiveFailures > 0)
        {
            UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: connection to Splunk restored after %d failed sends"), ConsecutiveFailures);
        }
        ConsecutiveFailures = 0;
        BatchesSent++;
        EventsSentTotal += EventCount;
        if (BatchesSent == 1 || BatchesSent % 100 == 0)
        {
            UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: HTTP %d OK - %d batches / %d events sent (%d dropped)"),
                Code, BatchesSent, EventsSentTotal, EventsDropped);
        }
        return;
    }

    EventsDropped += EventCount;
    ConsecutiveFailures++;

    // Log the first failure, then once a minute-ish, to avoid flooding the log.
    if (ConsecutiveFailures != 1 && ConsecutiveFailures % 12 != 0)
    {
        return;
    }

    if (Code == 0)
    {
        UE_LOG(LogSatisfactorySplunkMod, Error,
            TEXT("SplunkExporter: network error - could not reach %s (failure #%d). Check host/port/firewall. ")
            TEXT("If using https:// with Splunk's default self-signed certificate the game rejects it - use http:// (disable SSL on HEC) or a trusted certificate."),
            *SplunkURL, ConsecutiveFailures);
        return;
    }

    FString Reason;
    switch (Code)
    {
        case 400: Reason = TEXT("bad request - often an Index the token isn't allowed to write to"); break;
        case 401:
        case 403: Reason = TEXT("auth failed - check HECToken and that the token is enabled");      break;
        case 404: Reason = TEXT("not found - URL should end in /services/collector/event");         break;
        case 503: Reason = TEXT("Splunk busy/unavailable, or HEC disabled in Global Settings");     break;
        default:  break;
    }
    UE_LOG(LogSatisfactorySplunkMod, Error, TEXT("SplunkExporter: HTTP %d (failure #%d) - %s | response: %s"),
        Code, ConsecutiveFailures, *Reason, *Response->GetContentAsString().Left(300));
}
