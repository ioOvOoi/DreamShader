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
				"AssetTools",
				"ContentBrowser",
				"Core",
				"CoreUObject",
				"DirectoryWatcher",
				"DreamShader",
				"DreamShaderCompiler",
				"Engine",
				"InputCore",
				"Json",
				"MaterialEditor",
				"Projects",
				"RHI",
				"RenderCore",
				"Renderer",
				"Slate",
				"SlateCore",
				"SQLiteCore",
				"ToolMenus",
				"ToolWidgets",
				"UnrealEd",
				"WebSocketNetworking",
				"WorkspaceMenuStructure"
			});
	}
}
