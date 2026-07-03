using UnrealBuildTool;

public class SatisfactorySplunkMod : ModuleRules
{
    public SatisfactorySplunkMod(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "HTTP",
            "Json",
            "FactoryGame",
            "SML"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Projects" // IPluginManager, to locate the mod's Config folder
        });
    }
}
