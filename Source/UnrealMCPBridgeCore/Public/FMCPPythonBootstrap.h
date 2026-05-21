// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * One-shot Python `sys.path` bootstrap for the bridge plugin (M6 fix in blueprint v2).
 *
 * - PythonScriptPlugin loads in PreDefault; our module loads in Default. So by the time our
 *   StartupModule runs, IPythonScriptPlugin::Get() is valid BUT Python interpreter may or may
 *   not be initialised (depends on `bAutoLoadPythonScriptOnEditorStartup`).
 * - We use IPythonScriptPlugin::RegisterOnPythonInitialized which fires immediately if Python is
 *   already up, otherwise queues until init.
 * - On callback fire: inject Plugins/UnrealMCPBridge/Content/Python into sys.path and import the
 *   tools package so its `@tool` decorators populate the registry.
 *
 * Designed to be called exactly once from FUnrealMCPBridgeModule::StartupModule(). Subsequent
 * RegisterPythonInitCallback calls are a logged no-op (the registration is process-global).
 */
class UNREALMCPBRIDGECORE_API FMCPPythonBootstrap
{
public:
	/**
	 * Bind the Python-ready callback. Safe to call when PythonScriptPlugin is not loaded
	 * (degrades to a Warning log + skipped registration — bridge still works without Python).
	 */
	static void RegisterPythonInitCallback();

private:
	/** Resolved-path Python bootstrap command builder. Returns empty string on directory-missing. */
	static FString BuildBootstrapCommand();

	/** Actual sys.path mutation + tool import. Invoked by the OnPythonInitialized delegate. */
	static void OnPythonReady();

	/** Guards against double-registration if StartupModule is somehow re-invoked. */
	static bool bCallbackRegistered;
};
