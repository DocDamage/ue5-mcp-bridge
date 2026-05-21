// Copyright FatumGame. All Rights Reserved.

using UnrealBuildTool;

public class UnrealMCPBridgeCore : ModuleRules
{
	public UnrealMCPBridgeCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		CppStandard = CppStandardVersion.Cpp20;

		IWYUSupport = IWYUSupport.Full;

		// Phase 5 module split (2026-05-22): infrastructure module that owns server, dispatch,
		// helpers, utils, and shared marshalling types. Sister module ``UnrealMCPBridge`` (Tools)
		// publicly depends on this and houses the 63 tool-surface .cpp files.
		//
		// Anything used by 2+ surfaces lives here; anything used by exactly ONE surface stays in
		// that surface's owning module (currently the Tools module since we don't split per-surface).

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"Json",
				"JsonUtilities",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// Editor + transactional infrastructure (FScopedTransaction, GEditor, etc. — used by
				// MCPMutatorScope + MCPWorldContext + numerous Utils).
				"UnrealEd",
				// TCP listener (FMCPServer / FMCPConnection).
				"Sockets",
				"Networking",
				// Project paths sandbox (MCPPathSandbox) + general project resolution.
				"Projects",
				// FMCPPythonBootstrap / FMCPPythonEval embed Python expression evaluation in
				// editor process via the Python script plugin's runtime.
				"PythonScriptPlugin",
				// AssetRegistry / AssetTools — used by FMCPDay7Handlers, MCPAssetPathUtils,
				// MCPARFilterParser. Public so Tools surfaces also see them transitively.
				"AssetRegistry",
				"AssetTools",
				// Marshalling / reflection — MCPReflection + MCPPropertyPathParser walk FProperty
				// trees; MCPPinTypeUtils + MCPBlueprintUtils use BlueprintGraph K2 types.
				"KismetCompiler",
				"BlueprintGraph",
				// Material expression resolution (MCPMaterialUtils).
				"MaterialEditor",
				// Image encoding for MCPScreenshotUtils (PNG/JPG compression).
				"ImageCore",
				"ImageWrapper",
				// Content browser (UI singletons) — used by some shared Utils. Listed here so
				// MCPPathSandbox / asset path utils have transitive access.
				"ContentBrowser",
				"ContentBrowserData",
				"EditorScriptingUtilities",
			}
		);
	}
}
