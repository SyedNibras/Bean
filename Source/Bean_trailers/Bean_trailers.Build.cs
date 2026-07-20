using UnrealBuildTool;

public class Bean_trailers : ModuleRules
{
    public Bean_trailers(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",

            "EnhancedInput",

            "HTTP",
            "Json",
            "JsonUtilities",

            "PakFile",
            "AssetRegistry",

            "UMG",
            "Slate",
            "SlateCore"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {

        });
    }
}