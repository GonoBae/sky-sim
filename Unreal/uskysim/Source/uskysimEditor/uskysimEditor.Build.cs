using UnrealBuildTool;

public class uskysimEditor : ModuleRules
{
	public uskysimEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetRegistry",
			"AssetTools",
			"Core",
			"CoreUObject",
			"Engine",
			"MaterialEditor",
			"UnrealEd"
		});
	}
}
