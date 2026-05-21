// Copyright FatumGame. All Rights Reserved.

#include "Modules/ModuleManager.h"

#include "MCPTypes.h"

/**
 * Define the shared LogMCP category here (declared extern in MCPTypes.h). Every TU in both
 * UnrealMCPBridgeCore + UnrealMCPBridge (Tools) uses `LogMCP` — single definition lives in Core.
 */
DEFINE_LOG_CATEGORY(LogMCP);

/**
 * Phase 5 module split (2026-05-22): UnrealMCPBridgeCore is a passive infrastructure module.
 * It owns FMCPServer/Dispatch/Job/Log primitives + shared helpers (MCPToolHelpers / AssetLoader /
 * MutatorScope / JsonBuilder) + Utils + marshalling. It has NO lifecycle responsibilities — the
 * sister module UnrealMCPBridge (Tools) does StartupModule / FMCPServer::Start + surface registration.
 *
 * Using FDefaultModuleImpl here because Core has no custom startup work; the global singletons it
 * exposes (FMCPDispatchQueue::Get, FMCPSurfaceRegistry::Get, FMCPLogStream::Get) are Meyers
 * singletons constructed lazily on first call, after Tools' StartupModule kicks them.
 */
IMPLEMENT_MODULE(FDefaultModuleImpl, UnrealMCPBridgeCore)
