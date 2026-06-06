using UnrealBuildTool;

public class DreamShaderEditor : ModuleRules
{
	public DreamShaderEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"ApplicationCore",
				"AssetRegistry",
				"ContentBrowser",
				"Core",
				"CoreUObject",
				"DirectoryWatcher",
				"DreamShader",
				"DreamShaderCompiler",
				"Engine",
				"Json",
				"MaterialEditor",
				"Projects",
				"RHI",
				"RenderCore",
				"Renderer",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"UnrealEd",
				"WebSocketNetworking"
			});
	}
}
