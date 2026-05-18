// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMCP, Log, All);

/**
 * Public module interface for the Unreal MCP Bridge.
 *
 * Phase 1 Day 2: TCP listener live on 127.0.0.1:30020, game-thread dispatch queue drained on
 * OnEndFrame, single demo handler `editor.ping`. See `D:/tmp/mcp_unreal_blueprint_v2_patch.md` for
 * the full architectural plan.
 */
class IUnrealMCPBridgeModule : public IModuleInterface
{
public:
	/** Returns the loaded module singleton, loading it if necessary. Editor-only. */
	static IUnrealMCPBridgeModule& Get()
	{
		return FModuleManager::LoadModuleChecked<IUnrealMCPBridgeModule>("UnrealMCPBridge");
	}

	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("UnrealMCPBridge");
	}

	/** True when the TCP listener has been started and is accepting connections. */
	virtual bool IsListening() const = 0;

	/** Currently-bound port (0 when not listening). */
	virtual int32 GetListenPort() const = 0;

	/** Total dispatched requests since process start (diagnostic). */
	virtual int64 GetDispatchedRequestCount() const = 0;
};

/**
 * Concrete module implementation. Owns:
 * - The TCP listener (FMCPServer) bound at StartupModule time.
 * - The OnEndFrame handler that drains FMCPDispatchQueue every game-thread tick.
 * - Default dispatch handler registrations (editor.ping today; more as Days 3-5 land).
 * - The Python sys.path bootstrap binding.
 */
class FUnrealMCPBridgeModule : public IUnrealMCPBridgeModule
{
public:
	// IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// IUnrealMCPBridgeModule
	virtual bool IsListening() const override;
	virtual int32 GetListenPort() const override;
	virtual int64 GetDispatchedRequestCount() const override;

private:
	/** Handler bag: Day 2 registers only editor.ping. Day 3+ adds Python eval + CallFunction routing. */
	void RegisterDefaultDispatchHandlers();
	void UnregisterDefaultDispatchHandlers();

	/** OnEndFrame sink — calls FMCPDispatchQueue::Drain + GC of closed connections. */
	void OnEndFrame();

	/** Registered method names so ShutdownModule can clean up. */
	TArray<FString> RegisteredMethodNames;

	FDelegateHandle OnEndFrameHandle;
	bool bStarted = false;
};
