using UnrealBuildTool;

public class SatisfactorySplunkMod : ModuleRules
{
    public SatisfactorySplunkMod(ReadOnlyTargetRules Target) : base(Target)
    {
        // Match the settings SML itself builds with.
        CppStandard = CppStandardVersion.Cpp20;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bLegacyPublicIncludePaths = false;

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
