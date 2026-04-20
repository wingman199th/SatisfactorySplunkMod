#include "SplunkExporter.h"
#include "SatisfactorySplunkMod.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "EngineIterator.h"

ASplunkExporter::ASplunkExporter()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = false;

    EventsInBuffer = 0;
    EventsSentTotal = 0;
    bIsCollecting = false;
    LastBufferFlush = FDateTime::Now();
}

void ASplunkExporter::LoadSettingsFromConfig()
{
    USplunkModSettings* Settings = USplunkModSettings::Get();
    if (!Settings)
    {
        UE_LOG(LogSatisfactorySplunkMod, Warning, TEXT("SplunkExporter: Could not load mod settings"));
        return;
    }

    SplunkURL                  = Settings->SplunkURL;
    HECToken                   = Settings->HECToken;
    bUseMetricsMode            = Settings->bUseMetricsMode;
    MetricsInterval            = Settings->MetricsInterval;
    BufferFlushInterval        = Settings->BufferFlushInterval;
    PowerCollectionInterval    = Settings->PowerCollectionInterval;
    ProductionCollectionInterval = Settings->ProductionCollectionInterval;
    VehicleCollectionInterval  = Settings->VehicleCollectionInterval;
    PlayerCollectionInterval   = Settings->PlayerCollectionInterval;
    LegacyBufferFlushInterval  = Settings->LegacyBufferFlushInterval;
    BatchSize                  = Settings->BatchSize;
    bCollectProductionData     = Settings->bCollectProductionData;
    bCollectVehicleData        = Settings->bCollectVehicleData;
    bCollectPlayerData         = Settings->bCollectPlayerData;
    bCollectPowerData          = Settings->bCollectPowerData;
    bCollectLayoutData         = Settings->bCollectLayoutData;

    UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: Config loaded - URL: %s | Mode: %s"),
        *SplunkURL, bUseMetricsMode ? TEXT("Metrics") : TEXT("Events"));
    if (!bUseMetricsMode)
    {
        UE_LOG(LogSatisfactorySplunkMod, Log,
            TEXT("SplunkExporter: Legacy intervals - Power: %.1fs  Production: %.1fs  Vehicles: %.1fs  Players: %.1fs"),
            PowerCollectionInterval, ProductionCollectionInterval, VehicleCollectionInterval, PlayerCollectionInterval);
    }

    if (!Settings->IsConfigured())
    {
        UE_LOG(LogSatisfactorySplunkMod, Warning,
            TEXT("SplunkExporter: HEC token and/or Splunk URL are still set to placeholder values. ")
            TEXT("Edit Config/DefaultSatisfactorySplunkMod.ini with your Splunk details and restart."));
    }
}

void ASplunkExporter::BeginPlay()
{
    Super::BeginPlay();

    UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: Starting up"));

    LoadSettingsFromConfig();
    StartDataCollection();
}

void ASplunkExporter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopDataCollection();
    
    // Send any remaining buffered data
    if (DataBuffer.Num() > 0)
    {
        SendBufferedData();
    }
    
    Super::EndPlay(EndPlayReason);
}

void ASplunkExporter::StartDataCollection()
{
    // Guard against unconfigured placeholders
    if (HECToken.IsEmpty() || HECToken == TEXT("your-hec-token-here"))
    {
        UE_LOG(LogSatisfactorySplunkMod, Error,
            TEXT("SplunkExporter: HEC Token is not set. ")
            TEXT("Open Config/DefaultSatisfactorySplunkMod.ini, set HECToken, and restart."));
        return;
    }
    if (SplunkURL.IsEmpty() || SplunkURL == TEXT("https://your-splunk-instance:8088/services/collector"))
    {
        UE_LOG(LogSatisfactorySplunkMod, Error,
            TEXT("SplunkExporter: Splunk URL is not set. ")
            TEXT("Open Config/DefaultSatisfactorySplunkMod.ini, set SplunkURL, and restart."));
        return;
    }

    UWorld* World = GetWorld();
    if (World && !bIsCollecting)
    {
        FTimerManager& TimerManager = World->GetTimerManager();

        if (bUseMetricsMode)
        {
            // Fast metrics collection every 1 second (or configured interval)
            TimerManager.SetTimer(
                MetricsCollectionTimer,
                this,
                &ASplunkExporter::CollectMetrics,
                MetricsInterval,
                true
            );

            // Time-based buffer flush every 30 seconds (or configured interval)
            TimerManager.SetTimer(
                BufferFlushTimer,
                this,
                &ASplunkExporter::CheckAndFlushBuffer,
                BufferFlushInterval,
                true
            );

            UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: Metrics mode started (collect: %.1fs, flush: %.1fs)"),
                MetricsInterval, BufferFlushInterval);
        }
        else
        {
            // Legacy mode: each data type on its own independent timer
            if (bCollectPowerData)
            {
                TimerManager.SetTimer(PowerCollectionTimer, this, &ASplunkExporter::CollectPowerData, PowerCollectionInterval, true);
            }
            if (bCollectProductionData)
            {
                TimerManager.SetTimer(ProductionCollectionTimer, this, &ASplunkExporter::CollectProductionData, ProductionCollectionInterval, true);
            }
            if (bCollectVehicleData)
            {
                TimerManager.SetTimer(VehicleCollectionTimer, this, &ASplunkExporter::CollectAllVehicleData, VehicleCollectionInterval, true);
            }
            if (bCollectPlayerData)
            {
                TimerManager.SetTimer(PlayerCollectionTimer, this, &ASplunkExporter::CollectPlayerMovementSystems, PlayerCollectionInterval, true);
            }

            // Shared buffer flush timer for legacy mode
            TimerManager.SetTimer(LegacyBufferFlushTimer, this, &ASplunkExporter::CheckAndFlushBuffer, LegacyBufferFlushInterval, true);

            UE_LOG(LogSatisfactorySplunkMod, Log,
                TEXT("SplunkExporter: Events mode started - Power: %.1fs  Production: %.1fs  Vehicles: %.1fs  Players: %.1fs  Flush: %.1fs"),
                PowerCollectionInterval, ProductionCollectionInterval, VehicleCollectionInterval, PlayerCollectionInterval, LegacyBufferFlushInterval);
        }

        bIsCollecting = true;
        LastBufferFlush = FDateTime::Now();
    }
}

void ASplunkExporter::StopDataCollection()
{
    UWorld* World = GetWorld();
    if (World && bIsCollecting)
    {
        FTimerManager& TimerManager = World->GetTimerManager();
        TimerManager.ClearTimer(MetricsCollectionTimer);
        TimerManager.ClearTimer(BufferFlushTimer);
        TimerManager.ClearTimer(PowerCollectionTimer);
        TimerManager.ClearTimer(ProductionCollectionTimer);
        TimerManager.ClearTimer(VehicleCollectionTimer);
        TimerManager.ClearTimer(PlayerCollectionTimer);
        TimerManager.ClearTimer(LegacyBufferFlushTimer);
        bIsCollecting = false;
        UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: Data collection stopped"));
    }
}

void ASplunkExporter::CollectAndSendData()
{
    if (!GetWorld())
    {
        UE_LOG(LogSatisfactorySplunkMod, Error, TEXT("SplunkExporter: No valid world context"));
        return;
    }
    
    UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: Starting data collection cycle"));
    
    // Collect different types of data based on configuration
    if (bCollectProductionData)
    {
        CollectProductionData();
    }
    
    if (bCollectVehicleData)
    {
        CollectAllVehicleData();
    }
    
    if (bCollectPlayerData)
    {
        CollectPlayerMovementSystems();
    }
    
    if (bCollectPowerData)
    {
        CollectPowerData();
    }
    
    // Update buffer count
    EventsInBuffer = DataBuffer.Num();
    
    // Send buffered data if we have enough
    if (DataBuffer.Num() >= BatchSize)
    {
        SendBufferedData();
    }
    
    UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: Data collection cycle completed. Buffer size: %d"), DataBuffer.Num());
}

void ASplunkExporter::SendBufferedData()
{
    if (DataBuffer.Num() == 0)
    {
        return;
    }

    // Splunk HEC batch format: one JSON object per line (NOT wrapped in an array)
    FString BatchString;
    for (auto& Event : DataBuffer)
    {
        FString EventString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&EventString);
        FJsonSerializer::Serialize(Event.ToSharedRef(), Writer);
        BatchString += EventString + TEXT("\n");
    }

    UE_LOG(LogSatisfactorySplunkMod, Log, TEXT("SplunkExporter: Sending batch of %d events to Splunk"), DataBuffer.Num());

    SendDataToSplunk(BatchString);
    DataBuffer.Empty();
    EventsInBuffer = 0;
}

TSharedPtr<FJsonObject> ASplunkExporter::CreateBaseEvent(const FString& SourceType)
{
    TSharedPtr<FJsonObject> EventObject = MakeShareable(new FJsonObject);
    
    EventObject->SetStringField(TEXT("time"), FString::Printf(TEXT("%.0f"), FDateTime::Now().ToUnixTimestamp()));
    EventObject->SetStringField(TEXT("host"), TEXT("satisfactory-game"));
    EventObject->SetStringField(TEXT("sourcetype"), SourceType);
    
    return EventObject;
}

void ASplunkExporter::AddEventToBuffer(TSharedPtr<FJsonObject> EventObject)
{
    if (EventObject.IsValid())
    {
        DataBuffer.Add(EventObject);
    }
}

void ASplunkExporter::CollectProductionData()
{
    UWorld* World = GetWorld();
    if (!World) return;

    // Collect manufacturer data
    for (TActorIterator<AFGBuildableManufacturer> ActorItr(World); ActorItr; ++ActorItr)
    {
        AFGBuildableManufacturer* Manufacturer = *ActorItr;
        if (!Manufacturer || !Manufacturer->IsValidLowLevel()) continue;

        TSharedPtr<FJsonObject> EventObject = CreateBaseEvent(TEXT("satisfactory:production"));
        TSharedPtr<FJsonObject> EventData = MakeShareable(new FJsonObject);
        
        // Machine data
        EventData->SetStringField(TEXT("machine_type"), TEXT("Manufacturer"));
        EventData->SetStringField(TEXT("machine_id"), Manufacturer->GetName());
        EventData->SetNumberField(TEXT("power_consumption"), Manufacturer->GetPowerConsumption());
        EventData->SetNumberField(TEXT("efficiency"), Manufacturer->GetProductionEfficiency());
        
        // Recipe information
        TSubclassOf<class UFGRecipe> CurrentRecipe = Manufacturer->GetCurrentRecipe();
        if (CurrentRecipe)
        {
            UFGRecipe* Recipe = CurrentRecipe->GetDefaultObject<UFGRecipe>();
            if (Recipe)
            {
                EventData->SetStringField(TEXT("recipe_name"), Recipe->GetDisplayName().ToString());

                TArray<FItemAmount> Products = Recipe->GetProducts();
                TArray<FItemAmount> Ingredients = Recipe->GetIngredients();

                if (Products.Num() > 0 && Products[0].ItemClass)
                {
                    UFGItemDescriptor* ProductDesc = Products[0].ItemClass->GetDefaultObject<UFGItemDescriptor>();
                    if (ProductDesc)
                    {
                        EventData->SetStringField(TEXT("output_item"), ProductDesc->GetDisplayName().ToString());
                        EventData->SetNumberField(TEXT("output_rate"), Products[0].Amount);
                    }
                }

                if (Ingredients.Num() > 0 && Ingredients[0].ItemClass)
                {
                    UFGItemDescriptor* IngredientDesc = Ingredients[0].ItemClass->GetDefaultObject<UFGItemDescriptor>();
                    if (IngredientDesc)
                    {
                        EventData->SetStringField(TEXT("input_item"), IngredientDesc->GetDisplayName().ToString());
                        EventData->SetNumberField(TEXT("input_rate"), Ingredients[0].Amount);
                    }
                }
                
                // Handle multi-input recipes
                if (Ingredients.Num() > 1)
                {
                    TArray<TSharedPtr<FJsonValue>> SecondaryInputs;
                    for (int32 i = 1; i < Ingredients.Num(); i++)
                    {
                        if (Ingredients[i].ItemClass)
                        {
                            UFGItemDescriptor* IngredientDesc = Ingredients[i].ItemClass->GetDefaultObject<UFGItemDescriptor>();
                            if (IngredientDesc)
                            {
                                TSharedPtr<FJsonObject> InputObj = MakeShareable(new FJsonObject);
                                InputObj->SetStringField(TEXT("item"), IngredientDesc->GetDisplayName().ToString());
                                InputObj->SetNumberField(TEXT("rate"), Ingredients[i].Amount);
                                SecondaryInputs.Add(MakeShareable(new FJsonValueObject(InputObj)));
                            }
                        }
                    }
                    if (SecondaryInputs.Num() > 0)
                    {
                        EventData->SetArrayField(TEXT("secondary_inputs"), SecondaryInputs);
                    }
                }
            }
        }
        
        // Location data
        FVector Location = Manufacturer->GetActorLocation();
        EventData->SetNumberField(TEXT("location_x"), Location.X);
        EventData->SetNumberField(TEXT("location_y"), Location.Y);
        EventData->SetNumberField(TEXT("location_z"), Location.Z);
        
        EventObject->SetObjectField(TEXT("event"), EventData);
        AddEventToBuffer(EventObject);
    }
    
    // Collect resource extractor data
    for (TActorIterator<AFGBuildableResourceExtractor> ActorItr(World); ActorItr; ++ActorItr)
    {
        AFGBuildableResourceExtractor* Extractor = *ActorItr;
        if (!Extractor || !Extractor->IsValidLowLevel()) continue;

        TSharedPtr<FJsonObject> EventObject = CreateBaseEvent(TEXT("satisfactory:extraction"));
        TSharedPtr<FJsonObject> EventData = MakeShareable(new FJsonObject);
        
        EventData->SetStringField(TEXT("machine_type"), TEXT("Extractor"));
        EventData->SetStringField(TEXT("machine_id"), Extractor->GetName());
        EventData->SetNumberField(TEXT("power_consumption"), Extractor->GetPowerConsumption());
        EventData->SetNumberField(TEXT("efficiency"), Extractor->GetProductionEfficiency());
        EventData->SetNumberField(TEXT("extraction_rate"), Extractor->GetExtractionRate());
        
        // Resource type
        TSubclassOf<UFGResourceDescriptor> ResourceClass = Extractor->GetResourceClass();
        if (ResourceClass)
        {
            UFGResourceDescriptor* ResourceDesc = ResourceClass->GetDefaultObject<UFGResourceDescriptor>();
            if (ResourceDesc)
            {
                EventData->SetStringField(TEXT("resource_type"), ResourceDesc->GetDisplayName().ToString());
            }
        }
        
        FVector Location = Extractor->GetActorLocation();
        EventData->SetNumberField(TEXT("location_x"), Location.X);
        EventData->SetNumberField(TEXT("location_y"), Location.Y);
        EventData->SetNumberField(TEXT("location_z"), Location.Z);
        
        EventObject->SetObjectField(TEXT("event"), EventData);
        AddEventToBuffer(EventObject);
    }
}

void ASplunkExporter::CollectPowerData()
{
    UWorld* World = GetWorld();
    if (!World) return;

    // Get power subsystem
    AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(World);
    if (!BuildableSubsystem) return;

    // Collect power generator data
    for (TActorIterator<AFGBuildablePowerGenerator> ActorItr(World); ActorItr; ++ActorItr)
    {
        AFGBuildablePowerGenerator* Generator = *ActorItr;
        if (!Generator || !Generator->IsValidLowLevel()) continue;

        TSharedPtr<FJsonObject> EventObject = CreateBaseEvent(TEXT("satisfactory:power:generator"));
        TSharedPtr<FJsonObject> EventData = MakeShareable(new FJsonObject);
        
        EventData->SetStringField(TEXT("generator_type"), Generator->GetClass()->GetName());
        EventData->SetStringField(TEXT("generator_id"), Generator->GetName());
        EventData->SetNumberField(TEXT("power_production"), Generator->GetPowerProduction());
        EventData->SetNumberField(TEXT("max_power_production"), Generator->GetMaxPowerProduction());
        EventData->SetNumberField(TEXT("efficiency"), Generator->GetProductionEfficiency());
        EventData->SetBoolField(TEXT("is_producing"), Generator->IsProducing());
        
        // Location
        FVector Location = Generator->GetActorLocation();
        EventData->SetNumberField(TEXT("location_x"), Location.X);
        EventData->SetNumberField(TEXT("location_y"), Location.Y);
        EventData->SetNumberField(TEXT("location_z"), Location.Z);
        
        // Fuel data for fuel-powered generators
        if (AFGBuildablePowerGeneratorFuel* FuelGenerator = Cast<AFGBuildablePowerGeneratorFuel>(Generator))
        {
            UFGInventoryComponent* FuelInventory = FuelGenerator->GetFuelInventory();
            if (FuelInventory)
            {
                float TotalFuelEnergy = 0.0f;
                int32 FuelStacks = 0;
                
                for (int32 i = 0; i < FuelInventory->GetSizeLinear(); i++)
                {
                    FInventoryStack Stack;
                    if (FuelInventory->GetStackFromIndex(i, Stack) && !Stack.Item.ItemClass.IsNull())
                    {
                        FuelStacks++;
                        UFGItemDescriptor* ItemDesc = Stack.Item.ItemClass->GetDefaultObject<UFGItemDescriptor>();
                        
                        // Get energy value based on fuel type
                        float EnergyValue = 100.0f; // Default
                        if (UFGItemDescriptorNuclearFuel* NuclearFuel = Cast<UFGItemDescriptorNuclearFuel>(ItemDesc))
                        {
                            EnergyValue = NuclearFuel->GetEnergyValue();
                        }
                        else if (UFGItemDescriptorBiomass* BiomassFuel = Cast<UFGItemDescriptorBiomass>(ItemDesc))
                        {
                            EnergyValue = BiomassFuel->GetEnergyValue();
                        }
                        
                        TotalFuelEnergy += EnergyValue * Stack.NumItems;
                    }
                }
                
                EventData->SetNumberField(TEXT("fuel_energy_available"), TotalFuelEnergy);
                EventData->SetNumberField(TEXT("fuel_stacks"), FuelStacks);
            }
        }
        
        EventObject->SetObjectField(TEXT("event"), EventData);
        AddEventToBuffer(EventObject);
    }

    // TODO: Implement power circuit collection
    // Power circuits need to be retrieved from the power subsystem
    // Current API for accessing power circuits is unclear and needs investigation
}

void ASplunkExporter::CollectAllVehicleData()
{
    CollectPersonalVehicles();
    CollectTrainData();
}

FString ASplunkExporter::GetVehicleTypeFromClass(const FString& ClassName)
{
    if (ClassName.Contains(TEXT("Tractor")))
    {
        return TEXT("Tractor");
    }
    else if (ClassName.Contains(TEXT("Explorer")))
    {
        return TEXT("Explorer");
    }
    else if (ClassName.Contains(TEXT("CyberWagon")))
    {
        return TEXT("CyberWagon");
    }
    else if (ClassName.Contains(TEXT("Cart")))
    {
        return TEXT("FactoryCart");
    }
    else if (ClassName.Contains(TEXT("Truck")))
    {
        return TEXT("Truck");
    }
    
    return TEXT("Unknown");
}

void ASplunkExporter::CollectPersonalVehicles()
{
    UWorld* World = GetWorld();
    if (!World) return;

    for (TActorIterator<AFGWheeledVehicle> ActorItr(World); ActorItr; ++ActorItr)
    {
        AFGWheeledVehicle* Vehicle = *ActorItr;
        if (!Vehicle || !Vehicle->IsValidLowLevel()) continue;

        bool bIsPlayerDriven = Vehicle->IsPlayerDriven();
        bool bIsAutomated = Vehicle->IsAutoPilotEnabled();
        
        TSharedPtr<FJsonObject> EventObject;
        if (bIsAutomated)
        {
            EventObject = CreateBaseEvent(TEXT("satisfactory:vehicle:automated"));
        }
        else
        {
            EventObject = CreateBaseEvent(TEXT("satisfactory:vehicle:personal"));
        }
        
        TSharedPtr<FJsonObject> EventData = MakeShareable(new FJsonObject);
        
        FString VehicleClass = Vehicle->GetClass()->GetName();
        FString VehicleType = GetVehicleTypeFromClass(VehicleClass);
        
        EventData->SetStringField(TEXT("vehicle_type"), VehicleType);
        EventData->SetStringField(TEXT("vehicle_id"), Vehicle->GetName());
        EventData->SetBoolField(TEXT("is_player_driven"), bIsPlayerDriven);
        EventData->SetBoolField(TEXT("is_automated"), bIsAutomated);
        
        // Position and movement
        FVector Location = Vehicle->GetActorLocation();
        FVector Velocity = Vehicle->GetVelocity();
        FRotator Rotation = Vehicle->GetActorRotation();
        
        EventData->SetNumberField(TEXT("location_x"), Location.X);
        EventData->SetNumberField(TEXT("location_y"), Location.Y);
        EventData->SetNumberField(TEXT("location_z"), Location.Z);
        EventData->SetNumberField(TEXT("speed"), Velocity.Size());
        EventData->SetNumberField(TEXT("heading"), Rotation.Yaw);
        EventData->SetNumberField(TEXT("pitch"), Rotation.Pitch);
        EventData->SetNumberField(TEXT("roll"), Rotation.Roll);
        
        // Player information if player-driven
        if (bIsPlayerDriven)
        {
            APawn* Driver = Vehicle->GetInstigator();
            if (Driver)
            {
                EventData->SetStringField(TEXT("driver_name"), Driver->GetName());
                
                APlayerController* PC = Cast<APlayerController>(Driver->GetController());
                if (PC)
                {
                    EventData->SetStringField(TEXT("player_id"), PC->GetName());
                }
            }
        }
        
        // Fuel system
        UFGInventoryComponent* FuelInventory = Vehicle->GetFuelInventory();
        if (FuelInventory)
        {
            TArray<TSharedPtr<FJsonValue>> FuelArray;
            float TotalFuelEnergy = 0.0f;
            float MaxFuelEnergy = 0.0f;
            
            for (int32 i = 0; i < FuelInventory->GetSizeLinear(); i++)
            {
                FInventoryStack Stack;
                if (FuelInventory->GetStackFromIndex(i, Stack) && !Stack.Item.ItemClass.IsNull())
                {
                    TSharedPtr<FJsonObject> FuelItem = MakeShareable(new FJsonObject);
                    UFGItemDescriptor* ItemDesc = Stack.Item.ItemClass->GetDefaultObject<UFGItemDescriptor>();
                    
                    FuelItem->SetStringField(TEXT("fuel_type"), ItemDesc->GetDisplayName().ToString());
                    FuelItem->SetNumberField(TEXT("quantity"), Stack.NumItems);
                    
                    // Calculate energy value
                    float EnergyValue = 100.0f; // Default energy value
                    UFGItemDescriptorNuclearFuel* NuclearFuel = Cast<UFGItemDescriptorNuclearFuel>(ItemDesc);
                    UFGItemDescriptorBiomass* BiomassFuel = Cast<UFGItemDescriptorBiomass>(ItemDesc);
                    
                    if (NuclearFuel)
                    {
                        EnergyValue = NuclearFuel->GetEnergyValue();
                    }
                    else if (BiomassFuel)
                    {
                        EnergyValue = BiomassFuel->GetEnergyValue();
                    }
                    
                    TotalFuelEnergy += EnergyValue * Stack.NumItems;
                    FuelItem->SetNumberField(TEXT("energy_value"), EnergyValue);
                    
                    FuelArray.Add(MakeShareable(new FJsonValueObject(FuelItem)));
                }
                
                MaxFuelEnergy += 100.0f; // Approximate max energy per slot
            }
            
            EventData->SetArrayField(TEXT("fuel_items"), FuelArray);
            EventData->SetNumberField(TEXT("fuel_energy_current"), TotalFuelEnergy);
            EventData->SetNumberField(TEXT("fuel_energy_max"), MaxFuelEnergy);
            EventData->SetNumberField(TEXT("fuel_percentage"), MaxFuelEnergy > 0 ? TotalFuelEnergy / MaxFuelEnergy : 0.0f);
        }
        
        // Inventory/Storage
        UFGInventoryComponent* Inventory = Vehicle->GetStorageInventory();
        if (Inventory)
        {
            TArray<TSharedPtr<FJsonValue>> CargoArray;
            int32 SlotsUsed = 0;
            int32 TotalSlots = Inventory->GetSizeLinear();
            float TotalWeight = 0.0f;
            
            for (int32 i = 0; i < TotalSlots; i++)
            {
                FInventoryStack Stack;
                if (Inventory->GetStackFromIndex(i, Stack) && !Stack.Item.ItemClass.IsNull())
                {
                    SlotsUsed++;
                    
                    TSharedPtr<FJsonObject> CargoItem = MakeShareable(new FJsonObject);
                    UFGItemDescriptor* ItemDesc = Stack.Item.ItemClass->GetDefaultObject<UFGItemDescriptor>();
                    
                    CargoItem->SetStringField(TEXT("item_name"), ItemDesc->GetDisplayName().ToString());
                    CargoItem->SetNumberField(TEXT("quantity"), Stack.NumItems);
                    
                    float ItemWeight = ItemDesc->GetWeight() * Stack.NumItems;
                    CargoItem->SetNumberField(TEXT("weight"), ItemWeight);
                    TotalWeight += ItemWeight;
                    
                    CargoArray.Add(MakeShareable(new FJsonValueObject(CargoItem)));
                }
            }
            
            EventData->SetArrayField(TEXT("cargo"), CargoArray);
            EventData->SetNumberField(TEXT("cargo_slots_used"), SlotsUsed);
            EventData->SetNumberField(TEXT("cargo_slots_total"), TotalSlots);
            EventData->SetNumberField(TEXT("cargo_utilization"), TotalSlots > 0 ? (float)SlotsUsed / TotalSlots : 0.0f);
            EventData->SetNumberField(TEXT("cargo_weight"), TotalWeight);
        }
        
        // Vehicle-specific data
        if (VehicleType == TEXT("CyberWagon"))
        {
            EventData->SetStringField(TEXT("power_type"), TEXT("Electric"));
        }
        else
        {
            EventData->SetStringField(TEXT("power_type"), TEXT("Fuel"));
        }
        
        EventData->SetNumberField(TEXT("max_speed"), Vehicle->GetMaxSpeed());
        
        // Autopilot data for automated vehicles
        if (bIsAutomated)
        {
            AFGBuildableDockingStation* TargetStation = Vehicle->GetTargetNodeLinkedDockingStation();
            if (TargetStation)
            {
                EventData->SetStringField(TEXT("target_station"), TargetStation->GetName());
                
                FVector TargetLocation = TargetStation->GetActorLocation();
                float DistanceToTarget = FVector::Dist(Location, TargetLocation);
                EventData->SetNumberField(TEXT("distance_to_target"), DistanceToTarget);
            }
        }
        
        EventObject->SetObjectField(TEXT("event"), EventData);
        AddEventToBuffer(EventObject);
    }
}

void ASplunkExporter::CollectTrainData()
{
    UWorld* World = GetWorld();
    if (!World) return;

    for (TActorIterator<AFGTrain> ActorItr(World); ActorItr; ++ActorItr)
    {
        AFGTrain* Train = *ActorItr;
        if (!Train || !Train->IsValidLowLevel()) continue;

        TSharedPtr<FJsonObject> EventObject = CreateBaseEvent(TEXT("satisfactory:vehicle:train"));
        TSharedPtr<FJsonObject> EventData = MakeShareable(new FJsonObject);
        
        EventData->SetStringField(TEXT("vehicle_type"), TEXT("Train"));
        EventData->SetStringField(TEXT("train_id"), Train->GetName());
        EventData->SetNumberField(TEXT("speed"), Train->GetVelocity().Size());
        EventData->SetBoolField(TEXT("is_player_driven"), Train->IsPlayerDriven());
        
        // Get all rolling stock
        TArray<AFGRailroadVehicle*> RollingStock = Train->GetConsist();
        EventData->SetNumberField(TEXT("car_count"), RollingStock.Num());
        
        TArray<TSharedPtr<FJsonValue>> CarsArray;
        float TotalPowerConsumption = 0.0f;
        
        for (int32 i = 0; i < RollingStock.Num(); i++)
        {
            AFGRailroadVehicle* Car = RollingStock[i];
            if (!Car) continue;
            
            TSharedPtr<FJsonObject> CarData = MakeShareable(new FJsonObject);
            
            FVector CarLocation = Car->GetActorLocation();
            CarData->SetNumberField(TEXT("car_index"), i);
            CarData->SetStringField(TEXT("car_id"), Car->GetName());
            CarData->SetNumberField(TEXT("location_x"), CarLocation.X);
            CarData->SetNumberField(TEXT("location_y"), CarLocation.Y);
            CarData->SetNumberField(TEXT("location_z"), CarLocation.Z);
            
            // Check if it's a locomotive
            AFGLocomotive* Locomotive = Cast<AFGLocomotive>(Car);
            if (Locomotive)
            {
                CarData->SetStringField(TEXT("car_type"), TEXT("Locomotive"));
                CarData->SetNumberField(TEXT("power_consumption"), Locomotive->GetPowerConsumption());
                TotalPowerConsumption += Locomotive->GetPowerConsumption();
                
                // Fuel status
                UFGInventoryComponent* FuelInventory = Locomotive->GetFuelInventory();
                if (FuelInventory)
                {
                    int32 FuelStacks = 0;
                    int32 MaxFuelStacks = FuelInventory->GetSizeLinear();
                    
                    for (int32 j = 0; j < MaxFuelStacks; j++)
                    {
                        FInventoryStack Stack;
                        if (FuelInventory->GetStackFromIndex(j, Stack) && !Stack.Item.ItemClass.IsNull())
                        {
                            FuelStacks++;
                        }
                    }
                    
                    float FuelPercentage = MaxFuelStacks > 0 ? (float)FuelStacks / MaxFuelStacks : 0.0f;
                    CarData->SetNumberField(TEXT("fuel_percentage"), FuelPercentage);
                }
            }
            else
            {
                // Freight car
                AFGFreightWagon* FreightCar = Cast<AFGFreightWagon>(Car);
                if (FreightCar)
                {
                    CarData->SetStringField(TEXT("car_type"), TEXT("Freight"));
                    
                    UFGInventoryComponent* CargoInventory = FreightCar->GetStorageInventory();
                    if (CargoInventory)
                    {
                        TArray<TSharedPtr<FJsonValue>> CargoArray;
                        int32 SlotsUsed = 0;
                        int32 TotalSlots = CargoInventory->GetSizeLinear();
                        
                        for (int32 j = 0; j < TotalSlots; j++)
                        {
                            FInventoryStack Stack;
                            if (CargoInventory->GetStackFromIndex(j, Stack) && !Stack.Item.ItemClass.IsNull())
                            {
                                SlotsUsed++;
                                
                                TSharedPtr<FJsonObject> CargoItem = MakeShareable(new FJsonObject);
                                UFGItemDescriptor* ItemDesc = Stack.Item.ItemClass->GetDefaultObject<UFGItemDescriptor>();
                                CargoItem->SetStringField(TEXT("item_name"), ItemDesc->GetDisplayName().ToString());
                                CargoItem->SetNumberField(TEXT("quantity"), Stack.NumItems);
                                
                                CargoArray.Add(MakeShareable(new FJsonValueObject(CargoItem)));
                            }
                        }
                        
                        CarData->SetArrayField(TEXT("cargo"), CargoArray);
                        CarData->SetNumberField(TEXT("cargo_utilization"), TotalSlots > 0 ? (float)SlotsUsed / TotalSlots : 0.0f);
                    }
                }
            }
            
            CarsArray.Add(MakeShareable(new FJsonValueObject(CarData)));
        }
        
        EventData->SetArrayField(TEXT("cars"), CarsArray);
        EventData->SetNumberField(TEXT("total_power_consumption"), TotalPowerConsumption);
        
        // Timetable information
        AFGRailroadTimeTable* TimeTable = Train->GetTimeTable();
        if (TimeTable)
        {
            TArray<AFGTrainStationIdentifier*> Stations = TimeTable->GetStations();
            EventData->SetNumberField(TEXT("timetable_stations"), Stations.Num());
            
            int32 CurrentStop = TimeTable->GetCurrentStop();
            EventData->SetNumberField(TEXT("current_stop_index"), CurrentStop);
            
            if (CurrentStop >= 0 && CurrentStop < Stations.Num())
            {
                AFGTrainStationIdentifier* CurrentStation = Stations[CurrentStop];
                if (CurrentStation)
                {
                    EventData->SetStringField(TEXT("current_station"), CurrentStation->GetStationName().ToString());
                }
            }
        }
        
        EventObject->SetObjectField(TEXT("event"), EventData);
        AddEventToBuffer(EventObject);
    }
}

void ASplunkExporter::CollectPlayerMovementSystems()
{
    UWorld* World = GetWorld();
    if (!World) return;

    for (TActorIterator<AFGCharacterPlayer> ActorItr(World); ActorItr; ++ActorItr)
    {
        AFGCharacterPlayer* Player = *ActorItr;
        if (!Player || !Player->IsValidLowLevel()) continue;

        TSharedPtr<FJsonObject> EventObject = CreateBaseEvent(TEXT("satisfactory:player:movement"));
        TSharedPtr<FJsonObject> EventData = MakeShareable(new FJsonObject);
        
        EventData->SetStringField(TEXT("player_name"), Player->GetName());
        
        // Position
        FVector Location = Player->GetActorLocation();
        FVector Velocity = Player->GetVelocity();
        
        EventData->SetNumberField(TEXT("location_x"), Location.X);
        EventData->SetNumberField(TEXT("location_y"), Location.Y);
        EventData->SetNumberField(TEXT("location_z"), Location.Z);
        EventData->SetNumberField(TEXT("speed"), Velocity.Size());
        
        // Movement state
        UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
        if (Movement)
        {
            bool bIsFlying = Movement->IsFlying();
            bool bIsFalling = Movement->IsFalling();
            bool bIsWalking = Movement->IsWalking();
            
            EventData->SetBoolField(TEXT("is_flying"), bIsFlying);
            EventData->SetBoolField(TEXT("is_falling"), bIsFalling);
            EventData->SetBoolField(TEXT("is_walking"), bIsWalking);
        }
        
        // Jetpack status
        UFGJetPack* JetPack = Player->GetJetPack();
        if (JetPack)
        {
            EventData->SetBoolField(TEXT("has_jetpack"), true);
            EventData->SetBoolField(TEXT("jetpack_active"), JetPack->IsActive());
            EventData->SetNumberField(TEXT("jetpack_fuel"), JetPack->GetCurrentFuel());
            EventData->SetNumberField(TEXT("jetpack_fuel_max"), JetPack->GetMaxFuel());
        }
        else
        {
            EventData->SetBoolField(TEXT("has_jetpack"), false);
        }
        
        // Check if in vehicle
        APawn* VehiclePawn = Player->GetVehicle();
        if (VehiclePawn)
        {
            EventData->SetBoolField(TEXT("in_vehicle"), true);
            EventData->SetStringField(TEXT("vehicle_name"), VehiclePawn->GetName());
        }
        else
        {
            EventData->SetBoolField(TEXT("in_vehicle"), false);
        }
        
        EventObject->SetObjectField(TEXT("event"), EventData);
        AddEventToBuffer(EventObject);
    }
}

void ASplunkExporter::CollectFactoryLayoutData()
{
    UWorld* World = GetWorld();
    if (!World) return;

    TSharedPtr<FJsonObject> LayoutEvent = CreateBaseEvent(TEXT("satisfactory:factory:layout"));
    TSharedPtr<FJsonObject> EventData = MakeShareable(new FJsonObject);

    EventData->SetStringField(TEXT("event_type"), TEXT("factory_layout"));

    TArray<TSharedPtr<FJsonValue>> BuildingsArray;

    for (TActorIterator<AFGBuildable> ActorItr(World); ActorItr; ++ActorItr)
    {
        AFGBuildable* Building = *ActorItr;
        if (!Building) continue;

        TSharedPtr<FJsonObject> BuildingData = MakeShareable(new FJsonObject);
        BuildingData->SetStringField(TEXT("building_type"), Building->GetClass()->GetName());
        BuildingData->SetStringField(TEXT("building_id"), Building->GetName());

        FVector Location = Building->GetActorLocation();
        BuildingData->SetNumberField(TEXT("x"), Location.X);
        BuildingData->SetNumberField(TEXT("y"), Location.Y);
        BuildingData->SetNumberField(TEXT("z"), Location.Z);

        FRotator Rotation = Building->GetActorRotation();
        BuildingData->SetNumberField(TEXT("rotation"), Rotation.Yaw);

        BuildingsArray.Add(MakeShareable(new FJsonValueObject(BuildingData)));
    }

    EventData->SetArrayField(TEXT("buildings"), BuildingsArray);
    LayoutEvent->SetObjectField(TEXT("event"), EventData);

    SendLayoutDataToSplunk(LayoutEvent);
}

// ===== METRICS MODE METHODS =====

void ASplunkExporter::CollectMetrics()
{
    if (!GetWorld())
    {
        return;
    }

    CollectAggregateMetrics();
}

void ASplunkExporter::CollectAggregateMetrics()
{
    UWorld* World = GetWorld();
    if (!World) return;

    // Create metrics event using Splunk metrics format
    TSharedPtr<FJsonObject> MetricsEvent = MakeShareable(new FJsonObject);
    TSharedPtr<FJsonObject> Fields = MakeShareable(new FJsonObject);

    double Timestamp = FDateTime::Now().ToUnixTimestamp();
    MetricsEvent->SetStringField(TEXT("time"), FString::Printf(TEXT("%.0f"), Timestamp));
    MetricsEvent->SetStringField(TEXT("event"), TEXT("metric"));
    MetricsEvent->SetStringField(TEXT("source"), TEXT("satisfactory-mod"));
    MetricsEvent->SetStringField(TEXT("sourcetype"), TEXT("satisfactory:metrics"));

    // Aggregate metrics
    float TotalPowerConsumption = 0.0f;
    float TotalPowerProduction = 0.0f;
    int32 ManufacturerCount = 0;
    int32 ExtractorCount = 0;
    int32 GeneratorCount = 0;
    int32 VehicleCount = 0;
    int32 TrainCount = 0;
    int32 PlayerCount = 0;
    float TotalProducingEfficiency = 0.0f;
    int32 ProducingMachinesCount = 0;

    // Count manufacturers and aggregate production stats
    for (TActorIterator<AFGBuildableManufacturer> ActorItr(World); ActorItr; ++ActorItr)
    {
        AFGBuildableManufacturer* Manufacturer = *ActorItr;
        if (Manufacturer && Manufacturer->IsValidLowLevel())
        {
            ManufacturerCount++;
            TotalPowerConsumption += Manufacturer->GetPowerConsumption();

            float Efficiency = Manufacturer->GetProductionEfficiency();
            if (Efficiency > 0.0f)
            {
                TotalProducingEfficiency += Efficiency;
                ProducingMachinesCount++;
            }
        }
    }

    // Count extractors
    for (TActorIterator<AFGBuildableResourceExtractor> ActorItr(World); ActorItr; ++ActorItr)
    {
        AFGBuildableResourceExtractor* Extractor = *ActorItr;
        if (Extractor && Extractor->IsValidLowLevel())
        {
            ExtractorCount++;
            TotalPowerConsumption += Extractor->GetPowerConsumption();

            float Efficiency = Extractor->GetProductionEfficiency();
            if (Efficiency > 0.0f)
            {
                TotalProducingEfficiency += Efficiency;
                ProducingMachinesCount++;
            }
        }
    }

    // Count generators and power production
    for (TActorIterator<AFGBuildablePowerGenerator> ActorItr(World); ActorItr; ++ActorItr)
    {
        AFGBuildablePowerGenerator* Generator = *ActorItr;
        if (Generator && Generator->IsValidLowLevel())
        {
            GeneratorCount++;
            TotalPowerProduction += Generator->GetPowerProduction();
        }
    }

    // Count vehicles
    for (TActorIterator<AFGWheeledVehicle> ActorItr(World); ActorItr; ++ActorItr)
    {
        if (ActorItr->IsValidLowLevel())
        {
            VehicleCount++;
        }
    }

    // Count trains
    for (TActorIterator<AFGTrain> ActorItr(World); ActorItr; ++ActorItr)
    {
        if (ActorItr->IsValidLowLevel())
        {
            TrainCount++;
        }
    }

    // Count players
    for (TActorIterator<AFGCharacterPlayer> ActorItr(World); ActorItr; ++ActorItr)
    {
        if (ActorItr->IsValidLowLevel())
        {
            PlayerCount++;
        }
    }

    // Calculate average efficiency
    float AvgEfficiency = ProducingMachinesCount > 0 ? TotalProducingEfficiency / ProducingMachinesCount : 0.0f;

    // Set metrics using Splunk metrics naming convention
    Fields->SetNumberField(TEXT("metric_name:factory.power.consumption"), TotalPowerConsumption);
    Fields->SetNumberField(TEXT("metric_name:factory.power.production"), TotalPowerProduction);
    Fields->SetNumberField(TEXT("metric_name:factory.power.net"), TotalPowerProduction - TotalPowerConsumption);
    Fields->SetNumberField(TEXT("metric_name:factory.machines.manufacturers"), ManufacturerCount);
    Fields->SetNumberField(TEXT("metric_name:factory.machines.extractors"), ExtractorCount);
    Fields->SetNumberField(TEXT("metric_name:factory.machines.generators"), GeneratorCount);
    Fields->SetNumberField(TEXT("metric_name:factory.efficiency.average"), AvgEfficiency);
    Fields->SetNumberField(TEXT("metric_name:factory.vehicles.wheeled"), VehicleCount);
    Fields->SetNumberField(TEXT("metric_name:factory.vehicles.trains"), TrainCount);
    Fields->SetNumberField(TEXT("metric_name:factory.players"), PlayerCount);

    MetricsEvent->SetObjectField(TEXT("fields"), Fields);
    AddEventToBuffer(MetricsEvent);

    // Update buffer count
    EventsInBuffer = DataBuffer.Num();
}

void ASplunkExporter::CheckAndFlushBuffer()
{
    // Always flush on timer regardless of buffer size
    if (DataBuffer.Num() > 0)
    {
        SendBufferedData();
    }
}

void ASplunkExporter::SendDataToSplunk(const FString& JsonData)
{
    if (SplunkURL.IsEmpty() || HECToken.IsEmpty())
    {
        UE_LOG(LogSatisfactorySplunkMod, Error, TEXT("SplunkExporter: Splunk URL or HEC Token not configured"));
        return;
    }

    FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
    Request->OnProcessRequestComplete().BindUObject(this, &ASplunkExporter::OnHttpResponse);
    
    Request->SetURL(SplunkURL);
    Request->SetVerb("POST");
    Request->SetHeader("User-Agent", "SatisfactoryMod/1.0");
    Request->SetHeader("Content-Type", "application/json");
    Request->SetHeader("Authorization", FString::Printf(TEXT("Splunk %s"), *HECToken));
    Request->SetContentAsString(JsonData);
    
    Request->ProcessRequest();
}

void ASplunkExporter::SendLayoutDataToSplunk(TSharedPtr<FJsonObject> LayoutData)
{
    if (!LayoutData.IsValid()) return;
    
    FString OutputString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
    FJsonSerializer::Serialize(LayoutData.ToSharedRef(), Writer);
    
    SendDataToSplunk(OutputString);
}

void ASplunkExporter::OnHttpResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    if (!bWasSuccessful || !Response.IsValid())
    {
        UE_LOG(LogSatisfactorySplunkMod, Error,
            TEXT("SplunkExporter: Network error - could not reach Splunk at %s"), *SplunkURL);
        return;
    }

    int32 ResponseCode = Response->GetResponseCode();

    if (ResponseCode >= 200 && ResponseCode < 300)
    {
        LastSuccessfulSend = FDateTime::Now();
        LastBufferFlush    = FDateTime::Now();
        EventsSentTotal++;
        UE_LOG(LogSatisfactorySplunkMod, Log,
            TEXT("SplunkExporter: Data sent successfully (HTTP %d). Total sends: %d"),
            ResponseCode, EventsSentTotal);
        return;
    }

    // Specific HEC error messages to help with troubleshooting
    FString Reason;
    switch (ResponseCode)
    {
        case 400: Reason = TEXT("Bad request - check JSON payload format");            break;
        case 401: Reason = TEXT("Unauthorized - HECToken is invalid or missing");     break;
        case 403: Reason = TEXT("Forbidden - HEC input may be disabled in Splunk");   break;
        case 404: Reason = TEXT("Not found - check SplunkURL path (/services/collector)"); break;
        case 503: Reason = TEXT("Splunk is busy or unavailable - will retry on next cycle"); break;
        default:  Reason = Response->GetContentAsString();                              break;
    }
    UE_LOG(LogSatisfactorySplunkMod, Error,
        TEXT("SplunkExporter: HTTP %d - %s"), ResponseCode, *Reason);
}