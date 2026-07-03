#include "SplunkExporter.h"
#include "SatisfactorySplunkMod.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"
#include "GameFramework/PlayerState.h"

#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"

// FactoryGame
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Buildables/FGBuildableGenerator.h"
#include "Buildables/FGBuildableGeneratorFuel.h"
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
    // cm/s -> km/h
    constexpr float SpeedToKmh = 0.036f;

    FString ItemName(TSubclassOf<UFGItemDescriptor> ItemClass)
    {
        return ItemClass ? UFGItemDescriptor::GetItemName(ItemClass).ToString() : TEXT("Unknown");
    }

    void SetLocationFields(const TSharedPtr<FJsonObject>& Obj, const FVector& Location)
    {
        Obj->SetNumberField(TEXT("x"), Location.X);
        Obj->SetNumberField(TEXT("y"), Location.Y);
        Obj->SetNumberField(TEXT("z"), Location.Z);
    }
}

ASplunkExporter::ASplunkExporter()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = false;
    HostName = FPlatformProcess::ComputerName();
}

// ---------------------------------------------------------------------------
// Lifecycle / configuration
// ---------------------------------------------------------------------------

void ASplunkExporter::BeginPlay()
{
    Super::BeginPlay();

    // Only the authority (host / dedicated server) has the full factory state.
    if (!HasAuthority())
    {
        return;
    }

    UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: starting up"));
    LoadSettingsFromConfig();
    StartDataCollection();
}

void ASplunkExporter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopDataCollection();
    FlushBuffer();
    Super::EndPlay(EndPlayReason);
}

void ASplunkExporter::LoadSettingsFromConfig()
{
    FString ConfigPath;
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SatisfactorySplunkMod"));
    if (Plugin.IsValid())
    {
        ConfigPath = Plugin->GetBaseDir() / TEXT("Config") / TEXT("SatisfactorySplunkMod.ini");
    }

    if (!ConfigPath.IsEmpty() && FPaths::FileExists(ConfigPath))
    {
        FConfigFile Ini;
        Ini.Read(ConfigPath);

        Ini.GetString(TEXT("Splunk"), TEXT("SplunkURL"), SplunkURL);
        Ini.GetString(TEXT("Splunk"), TEXT("HECToken"), HECToken);
        Ini.GetString(TEXT("Splunk"), TEXT("Index"), SplunkIndex);

        Ini.GetFloat(TEXT("Collection"), TEXT("SummaryInterval"), SummaryInterval);
        Ini.GetFloat(TEXT("Collection"), TEXT("PowerInterval"), PowerInterval);
        Ini.GetFloat(TEXT("Collection"), TEXT("ProductionInterval"), ProductionInterval);
        Ini.GetFloat(TEXT("Collection"), TEXT("InventoryInterval"), InventoryInterval);
        Ini.GetFloat(TEXT("Collection"), TEXT("VehicleInterval"), VehicleInterval);
        Ini.GetFloat(TEXT("Collection"), TEXT("PlayerInterval"), PlayerInterval);
        Ini.GetFloat(TEXT("Collection"), TEXT("FlushInterval"), FlushInterval);

        Ini.GetBool(TEXT("Collection"), TEXT("bCollectSummary"), bCollectSummary);
        Ini.GetBool(TEXT("Collection"), TEXT("bCollectPower"), bCollectPower);
        Ini.GetBool(TEXT("Collection"), TEXT("bCollectProduction"), bCollectProduction);
        Ini.GetBool(TEXT("Collection"), TEXT("bCollectInventory"), bCollectInventory);
        Ini.GetBool(TEXT("Collection"), TEXT("bCollectVehicles"), bCollectVehicles);
        Ini.GetBool(TEXT("Collection"), TEXT("bCollectPlayers"), bCollectPlayers);

        Ini.GetInt(TEXT("Collection"), TEXT("MaxBufferedEvents"), MaxBufferedEvents);

        UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: loaded config from %s"), *ConfigPath);
    }
    else
    {
        UE_LOG(LogSatisfactorySplunkMod, Warning,
            TEXT("SplunkExporter: config file not found at '%s'"), *ConfigPath);
    }

    // Environment variables override the ini - handy for dedicated servers.
    const FString EnvUrl = FPlatformMisc::GetEnvironmentVariable(TEXT("SPLUNK_HEC_URL"));
    const FString EnvToken = FPlatformMisc::GetEnvironmentVariable(TEXT("SPLUNK_HEC_TOKEN"));
    if (!EnvUrl.IsEmpty())   { SplunkURL = EnvUrl; }
    if (!EnvToken.IsEmpty()) { HECToken = EnvToken; }

    // Clamp intervals so a bad config can't spin-loop the game thread.
    SummaryInterval    = FMath::Max(1.0f, SummaryInterval);
    PowerInterval      = FMath::Max(1.0f, PowerInterval);
    ProductionInterval = FMath::Max(1.0f, ProductionInterval);
    InventoryInterval  = FMath::Max(1.0f, InventoryInterval);
    VehicleInterval    = FMath::Max(1.0f, VehicleInterval);
    PlayerInterval     = FMath::Max(1.0f, PlayerInterval);
    FlushInterval      = FMath::Max(1.0f, FlushInterval);
    MaxBufferedEvents  = FMath::Max(100, MaxBufferedEvents);
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
            TEXT("SplunkExporter: SplunkURL / HECToken not configured. ")
            TEXT("Edit Config/SatisfactorySplunkMod.ini in the mod folder (or set the ")
            TEXT("SPLUNK_HEC_URL / SPLUNK_HEC_TOKEN environment variables) and restart."));
        return;
    }

    UWorld* World = GetWorld();
    if (!World || bIsCollecting)
    {
        return;
    }

    FTimerManager& TM = World->GetTimerManager();
    if (bCollectSummary)    { TM.SetTimer(SummaryTimer,    this, &ASplunkExporter::CollectSummary,    SummaryInterval,    true); }
    if (bCollectPower)      { TM.SetTimer(PowerTimer,      this, &ASplunkExporter::CollectPower,      PowerInterval,      true); }
    if (bCollectProduction) { TM.SetTimer(ProductionTimer, this, &ASplunkExporter::CollectProduction, ProductionInterval, true); }
    if (bCollectInventory)  { TM.SetTimer(InventoryTimer,  this, &ASplunkExporter::CollectInventory,  InventoryInterval,  true); }
    if (bCollectVehicles)   { TM.SetTimer(VehicleTimer,    this, &ASplunkExporter::CollectVehicles,   VehicleInterval,    true); }
    if (bCollectPlayers)    { TM.SetTimer(PlayerTimer,     this, &ASplunkExporter::CollectPlayers,    PlayerInterval,     true); }
    TM.SetTimer(FlushTimer, this, &ASplunkExporter::FlushBuffer, FlushInterval, true);

    bIsCollecting = true;
    UE_LOG(LogSatisfactorySplunkMod, Log,
        TEXT("SplunkExporter: collection started (summary %.0fs, power %.0fs, production %.0fs, inventory %.0fs, vehicles %.0fs, players %.0fs, flush %.0fs)"),
        SummaryInterval, PowerInterval, ProductionInterval, InventoryInterval, VehicleInterval, PlayerInterval, FlushInterval);
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
    UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: collection stopped"));
}

// ---------------------------------------------------------------------------
// Event helpers
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> ASplunkExporter::CreateEvent(const FString& SourceType) const
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
        // Splunk unreachable or flush interval too slow - drop oldest first.
        DataBuffer.RemoveAt(0);
        EventsDropped++;
    }
    DataBuffer.Add(EventObject);
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
    float PowerConsumption = 0.0f;

    for (TActorIterator<AFGBuildableManufacturer> It(World); It; ++It)
    {
        Manufacturers++;
        ProductivitySum += It->GetProductivity();
        if (It->IsProducing()) { Producing++; }
        if (UFGPowerInfoComponent* Power = It->GetPowerInfo()) { PowerConsumption += Power->GetActualConsumption(); }
    }
    for (TActorIterator<AFGBuildableResourceExtractor> It(World); It; ++It)
    {
        Extractors++;
        ProductivitySum += It->GetProductivity();
        if (It->IsProducing()) { Producing++; }
        if (UFGPowerInfoComponent* Power = It->GetPowerInfo()) { PowerConsumption += Power->GetActualConsumption(); }
    }
    for (TActorIterator<AFGBuildableGenerator> It(World); It; ++It)
    {
        Generators++;
    }

    float CircuitProduced = 0.0f;
    float CircuitConsumed = 0.0f;
    float CircuitCapacity = 0.0f;
    int32 Circuits = 0;
    for (TObjectIterator<UFGPowerCircuit> It; It; ++It)
    {
        UFGPowerCircuit* Circuit = *It;
        if (!IsValid(Circuit) || Circuit->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Circuit->GetWorld() != World)
        {
            continue;
        }
        FPowerCircuitStats Stats;
        Circuit->GetStats(Stats);
        CircuitProduced += Stats.PowerProduced;
        CircuitConsumed += Stats.PowerConsumed;
        CircuitCapacity += Stats.PowerProductionCapacity;
        Circuits++;
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

    TSharedPtr<FJsonObject> Event = CreateEvent(TEXT("satisfactory:summary"));
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("manufacturers"), Manufacturers);
    Data->SetNumberField(TEXT("extractors"), Extractors);
    Data->SetNumberField(TEXT("generators"), Generators);
    Data->SetNumberField(TEXT("machines_producing"), Producing);
    Data->SetNumberField(TEXT("avg_productivity"), Machines > 0 ? ProductivitySum / Machines : 0.0f);
    Data->SetNumberField(TEXT("power_circuits"), Circuits);
    Data->SetNumberField(TEXT("power_produced_mw"), CircuitProduced);
    Data->SetNumberField(TEXT("power_consumed_mw"), CircuitConsumed);
    Data->SetNumberField(TEXT("power_capacity_mw"), CircuitCapacity);
    Data->SetNumberField(TEXT("factory_power_consumption_mw"), PowerConsumption);
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

    for (TObjectIterator<UFGPowerCircuit> It; It; ++It)
    {
        UFGPowerCircuit* Circuit = *It;
        if (!IsValid(Circuit) || Circuit->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Circuit->GetWorld() != World)
        {
            continue;
        }

        FPowerCircuitStats Stats;
        Circuit->GetStats(Stats);

        TSharedPtr<FJsonObject> Event = CreateEvent(TEXT("satisfactory:power"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetNumberField(TEXT("circuit_id"), Circuit->GetCircuitID());
        Data->SetNumberField(TEXT("produced_mw"), Stats.PowerProduced);
        Data->SetNumberField(TEXT("consumed_mw"), Stats.PowerConsumed);
        Data->SetNumberField(TEXT("capacity_mw"), Stats.PowerProductionCapacity);
        Data->SetNumberField(TEXT("battery_input_mw"), Stats.BatteryPowerInput);
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

        TSharedPtr<FJsonObject> Event = CreateEvent(TEXT("satisfactory:production"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

        Data->SetStringField(TEXT("machine_class"), TEXT("manufacturer"));
        Data->SetStringField(TEXT("machine_type"), Machine->GetClass()->GetName());
        Data->SetStringField(TEXT("machine_id"), Machine->GetName());
        SetLocationFields(Data, Machine->GetActorLocation());

        const bool bProducing = Machine->IsProducing();
        const bool bPaused = Machine->IsProductionPaused();
        const bool bHasPower = Machine->HasPower();
        const float Productivity = Machine->GetProductivity();
        const float Potential = Machine->GetCurrentPotential();

        Data->SetBoolField(TEXT("is_producing"), bProducing);
        Data->SetBoolField(TEXT("is_paused"), bPaused);
        Data->SetBoolField(TEXT("has_power"), bHasPower);
        Data->SetNumberField(TEXT("productivity"), Productivity);
        Data->SetNumberField(TEXT("clock_speed"), Potential);
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

            // Effective cycle time at the current clock speed.
            const float Duration = UFGRecipe::GetManufacturingDuration(Recipe);
            const float Speed = Machine->GetManufacturingSpeed() * Potential;
            const float CycleTime = (Speed > 0.0f) ? Duration / Speed : 0.0f;
            const float CyclesPerMin = (CycleTime > 0.0f) ? 60.0f / CycleTime : 0.0f;
            Data->SetNumberField(TEXT("cycle_time_s"), CycleTime);

            // Outputs: theoretical rate at this clock, actual = theoretical * productivity.
            TArray<TSharedPtr<FJsonValue>> Outputs;
            for (const FItemAmount& Product : UFGRecipe::GetProducts(Recipe))
            {
                TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
                Out->SetStringField(TEXT("item"), ItemName(Product.ItemClass));
                Out->SetNumberField(TEXT("per_min_at_clock"), Product.Amount * CyclesPerMin);
                Out->SetNumberField(TEXT("per_min_actual"), Product.Amount * CyclesPerMin * Productivity);
                Outputs.Add(MakeShared<FJsonValueObject>(Out));
            }
            Data->SetArrayField(TEXT("outputs"), Outputs);

            // Inputs: required rate plus how much is buffered in the input inventory.
            UFGInventoryComponent* InputInv = Machine->GetInputInventory();
            TArray<TSharedPtr<FJsonValue>> Inputs;
            bool bInputStarved = false;
            for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(this, Recipe))
            {
                const int32 Buffered = (InputInv && Ingredient.ItemClass)
                    ? InputInv->GetNumItems(Ingredient.ItemClass) : 0;

                TSharedPtr<FJsonObject> In = MakeShared<FJsonObject>();
                In->SetStringField(TEXT("item"), ItemName(Ingredient.ItemClass));
                In->SetNumberField(TEXT("per_min_at_clock"), Ingredient.Amount * CyclesPerMin);
                In->SetNumberField(TEXT("buffered"), Buffered);
                Inputs.Add(MakeShared<FJsonValueObject>(In));

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

        TSharedPtr<FJsonObject> Event = CreateEvent(TEXT("satisfactory:production"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

        Data->SetStringField(TEXT("machine_class"), TEXT("extractor"));
        Data->SetStringField(TEXT("machine_type"), Extractor->GetClass()->GetName());
        Data->SetStringField(TEXT("machine_id"), Extractor->GetName());
        SetLocationFields(Data, Extractor->GetActorLocation());

        const bool bProducing = Extractor->IsProducing();
        const bool bHasPower = Extractor->HasPower();
        Data->SetBoolField(TEXT("is_producing"), bProducing);
        Data->SetBoolField(TEXT("is_paused"), Extractor->IsProductionPaused());
        Data->SetBoolField(TEXT("has_power"), bHasPower);
        Data->SetNumberField(TEXT("productivity"), Extractor->GetProductivity());
        Data->SetNumberField(TEXT("clock_speed"), Extractor->GetCurrentPotential());
        Data->SetNumberField(TEXT("per_min_at_clock"), Extractor->GetExtractionPerMinute());
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
        if (Extractor->IsProductionPaused()) { Status = TEXT("paused"); }
        else if (!bHasPower)                 { Status = TEXT("no_power"); }
        else if (bProducing)                 { Status = TEXT("producing"); }
        else                                 { Status = TEXT("output_backed_up"); }
        Data->SetStringField(TEXT("status"), Status);

        Event->SetObjectField(TEXT("event"), Data);
        AddEventToBuffer(Event);
    }

    // Generators: capacity, fuel state.
    for (TActorIterator<AFGBuildableGenerator> It(World); It; ++It)
    {
        AFGBuildableGenerator* Generator = *It;

        TSharedPtr<FJsonObject> Event = CreateEvent(TEXT("satisfactory:production"));
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
    TMap<FString, int64> Totals;
    int32 Containers = 0;

    for (TActorIterator<AFGBuildableStorage> It(World); It; ++It)
    {
        UFGInventoryComponent* Inventory = It->GetStorageInventory();
        if (!Inventory) continue;
        Containers++;

        for (int32 Index = 0; Index < Inventory->GetSizeLinear(); ++Index)
        {
            FInventoryStack Stack;
            if (Inventory->GetStackFromIndex(Index, Stack) && Stack.HasItems())
            {
                Totals.FindOrAdd(ItemName(Stack.Item.GetItemClass())) += Stack.NumItems;
            }
        }
    }

    for (const TPair<FString, int64>& Pair : Totals)
    {
        TSharedPtr<FJsonObject> Event = CreateEvent(TEXT("satisfactory:inventory"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("item"), Pair.Key);
        Data->SetNumberField(TEXT("count"), static_cast<double>(Pair.Value));
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

        TSharedPtr<FJsonObject> Event = CreateEvent(TEXT("satisfactory:vehicle"));
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
            int64 ItemCount = 0;
            TArray<TSharedPtr<FJsonValue>> CargoArray;
            for (int32 Index = 0; Index < Cargo->GetSizeLinear(); ++Index)
            {
                FInventoryStack Stack;
                if (Cargo->GetStackFromIndex(Index, Stack) && Stack.HasItems())
                {
                    SlotsUsed++;
                    ItemCount += Stack.NumItems;
                    TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
                    Item->SetStringField(TEXT("item"), ItemName(Stack.Item.GetItemClass()));
                    Item->SetNumberField(TEXT("count"), Stack.NumItems);
                    CargoArray.Add(MakeShared<FJsonValueObject>(Item));
                }
            }
            Data->SetArrayField(TEXT("cargo"), CargoArray);
            Data->SetNumberField(TEXT("cargo_slots_used"), SlotsUsed);
            Data->SetNumberField(TEXT("cargo_slots_total"), Cargo->GetSizeLinear());
            Data->SetNumberField(TEXT("cargo_items"), static_cast<double>(ItemCount));
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

            TSharedPtr<FJsonObject> Event = CreateEvent(TEXT("satisfactory:vehicle"));
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

        TSharedPtr<FJsonObject> Event = CreateEvent(TEXT("satisfactory:vehicle"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

        Data->SetStringField(TEXT("vehicle_class"), TEXT("freight_wagon"));
        Data->SetStringField(TEXT("vehicle_id"), Wagon->GetName());
        if (AFGTrain* Train = Wagon->GetTrain())
        {
            Data->SetStringField(TEXT("train_name"), Train->GetTrainName().ToString());
        }
        SetLocationFields(Data, Wagon->GetActorLocation());

        int32 SlotsUsed = 0;
        TArray<TSharedPtr<FJsonValue>> CargoArray;
        for (int32 Index = 0; Index < Freight->GetSizeLinear(); ++Index)
        {
            FInventoryStack Stack;
            if (Freight->GetStackFromIndex(Index, Stack) && Stack.HasItems())
            {
                SlotsUsed++;
                TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("item"), ItemName(Stack.Item.GetItemClass()));
                Item->SetNumberField(TEXT("count"), Stack.NumItems);
                CargoArray.Add(MakeShared<FJsonValueObject>(Item));
            }
        }
        Data->SetArrayField(TEXT("cargo"), CargoArray);
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

        TSharedPtr<FJsonObject> Event = CreateEvent(TEXT("satisfactory:player"));
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

        FString PlayerName = Player->GetName();
        if (APlayerState* State = Player->GetPlayerState())
        {
            PlayerName = State->GetPlayerName();
        }
        Data->SetStringField(TEXT("player"), PlayerName);
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
    if (DataBuffer.Num() == 0)
    {
        return;
    }

    // Splunk HEC batch format: concatenated JSON objects, one per event.
    FString Payload;
    for (const TSharedPtr<FJsonObject>& Event : DataBuffer)
    {
        FString EventString;
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&EventString);
        FJsonSerializer::Serialize(Event.ToSharedRef(), Writer);
        Payload += EventString;
        Payload += TEXT("\n");
    }

    const int32 Count = DataBuffer.Num();
    DataBuffer.Empty();
    SendToSplunk(Payload, Count);
}

void ASplunkExporter::SendToSplunk(const FString& Payload, int32 EventCount)
{
    if (!IsConfigured())
    {
        return;
    }

    FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
    Request->OnProcessRequestComplete().BindUObject(this, &ASplunkExporter::OnHttpResponse);
    Request->SetURL(SplunkURL);
    Request->SetVerb(TEXT("POST"));
    Request->SetTimeout(15.0f);
    Request->SetHeader(TEXT("User-Agent"), TEXT("SatisfactorySplunkMod/2.0"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Splunk %s"), *HECToken));
    Request->SetContentAsString(Payload);
    Request->ProcessRequest();

    EventsSentTotal += EventCount;
}

void ASplunkExporter::OnHttpResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    if (!bWasSuccessful || !Response.IsValid())
    {
        UE_LOG(LogSatisfactorySplunkMod, Error,
            TEXT("SplunkExporter: network error - could not reach %s. ")
            TEXT("If Splunk uses a self-signed HTTPS certificate the game will reject it; ")
            TEXT("either serve HEC over plain http:// or install a trusted certificate."),
            *SplunkURL);
        return;
    }

    const int32 Code = Response->GetResponseCode();
    if (Code >= 200 && Code < 300)
    {
        BatchesSent++;
        if (BatchesSent == 1 || BatchesSent % 100 == 0)
        {
            UE_LOG(LogSatisfactorySplunkMod, Log,
                TEXT("SplunkExporter: HTTP %d - %d batches / %d events sent (%d dropped)"),
                Code, BatchesSent, EventsSentTotal, EventsDropped);
        }
        return;
    }

    FString Reason;
    switch (Code)
    {
        case 400: Reason = TEXT("bad request - malformed payload");                              break;
        case 401:
        case 403: Reason = TEXT("auth failed - check HECToken and that the token is enabled");   break;
        case 404: Reason = TEXT("not found - URL should end in /services/collector/event");      break;
        case 503: Reason = TEXT("Splunk busy/unavailable");                                      break;
        default:  Reason = Response->GetContentAsString().Left(200);                             break;
    }
    UE_LOG(LogSatisfactorySplunkMod, Error, TEXT("SplunkExporter: HTTP %d - %s"), Code, *Reason);
}
