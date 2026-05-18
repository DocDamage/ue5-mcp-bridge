// Copyright FatumGame. All Rights Reserved.

#include "FMCPPythonBootstrap.h"

#include "UnrealMCPBridge.h"

#include "IPythonScriptPlugin.h"
#include "Misc/Paths.h"

bool FMCPPythonBootstrap::bCallbackRegistered = false;

void FMCPPythonBootstrap::RegisterPythonInitCallback()
{
	if (bCallbackRegistered)
	{
		UE_LOG(LogMCP, Verbose, TEXT("FMCPPythonBootstrap::RegisterPythonInitCallback: already registered, skipping"));
		return;
	}

	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin)
	{
		// Plugin disabled entirely (uncommon — we declared it as a hard dependency in the .uplugin).
		UE_LOG(LogMCP, Warning,
			TEXT("FMCPPythonBootstrap: PythonScriptPlugin not loaded; tool registry will not be populated. ")
			TEXT("MCP bridge will still accept connections but only C++-registered handlers will respond."));
		return;
	}

	// RegisterOnPythonInitialized fires synchronously if Python is already initialised, else queues.
	PythonPlugin->RegisterOnPythonInitialized(FSimpleDelegate::CreateStatic(&FMCPPythonBootstrap::OnPythonReady));
	bCallbackRegistered = true;
	UE_LOG(LogMCP, Log, TEXT("FMCPPythonBootstrap: registered OnPythonInitialized callback (Python already-init=%d)"),
		PythonPlugin->IsPythonInitialized() ? 1 : 0);
}

FString FMCPPythonBootstrap::BuildBootstrapCommand()
{
	const FString PluginPythonDir = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectPluginsDir() / TEXT("UnrealMCPBridge/Content/Python"));

	if (!FPaths::DirectoryExists(PluginPythonDir))
	{
		UE_LOG(LogMCP, Error, TEXT("FMCPPythonBootstrap: plugin Python directory missing: %s"), *PluginPythonDir);
		return FString();
	}

	// Forward-slash form is safe on Windows (Python accepts both) AND removes the backslash-escape
	// headache entirely — no need to double-escape. Belt-and-braces: we still wrap in r"..." raw
	// string syntax for safety against any path containing quote-shaped weirdness.
	const FString NormalisedPath = PluginPythonDir.Replace(TEXT("\\"), TEXT("/"));

	// One-liner Python:
	//   - import sys
	//   - if path not in sys.path, prepend (idempotent — hot reload safe)
	//   - import MCPTools.tools.smoke_tools to trigger @tool decorator side-effects
	// Triple single-quotes around the path so embedded single quotes in NormalisedPath (unlikely
	// on a typical project tree but defensive) won't break the literal.
	const FString Cmd = FString::Printf(
		TEXT("import sys; ")
		TEXT("_p = r'''%s'''; ")
		TEXT("_p in sys.path or sys.path.insert(0, _p); ")
		TEXT("import MCPTools.tools.smoke_tools as _; ")
		TEXT("import unreal; unreal.log('[MCP] Python bootstrap OK, sys.path[0]=' + sys.path[0])"),
		*NormalisedPath);

	return Cmd;
}

void FMCPPythonBootstrap::OnPythonReady()
{
	UE_LOG(LogMCP, Log, TEXT("FMCPPythonBootstrap::OnPythonReady fired"));

	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin || !PythonPlugin->IsPythonInitialized())
	{
		// Should never happen — the delegate by contract only fires after init.
		UE_LOG(LogMCP, Error, TEXT("FMCPPythonBootstrap::OnPythonReady: Python not initialised"));
		return;
	}

	const FString Cmd = BuildBootstrapCommand();
	if (Cmd.IsEmpty())
	{
		return; // Already logged in BuildBootstrapCommand.
	}

	const bool bOk = PythonPlugin->ExecPythonCommand(*Cmd);
	if (!bOk)
	{
		UE_LOG(LogMCP, Error, TEXT("FMCPPythonBootstrap: ExecPythonCommand failed — sys.path bootstrap aborted"));
	}
	else
	{
		UE_LOG(LogMCP, Log, TEXT("FMCPPythonBootstrap: sys.path injected + MCPTools.tools imported"));
	}
}
