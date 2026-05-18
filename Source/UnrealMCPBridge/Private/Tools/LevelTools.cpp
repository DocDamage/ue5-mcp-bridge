// Copyright FatumGame. All Rights Reserved.

#include "LevelTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utility/MCPWorldContext.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTLS.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// LVL_ prefix per the unity-build symbol-collision pattern (MakeError/MakeSuccess clash with
	// UE's global ValueOrError templates).
	constexpr int32 kLVLErrorInvalidParams = -32602;
	constexpr int32 kLVLErrorInternal      = -32603;

	void LVL_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse LVL_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		LVL_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse LVL_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		LVL_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	/** Resolve target world: PIE world if active + read-only, else editor world. Returns null on failure. */
	UWorld* LVL_ResolveReadWorld()
	{
		if (FMCPWorldContext::IsPIEActive())
		{
			return GEditor->PlayWorld;
		}
		return FMCPWorldContext::GetEditorWorld();
	}

	/**
	 * Build a JSON summary of one ULevel — used by ``level.list_loaded``.
	 *
	 * Shape:
	 *   {
	 *     "map_path": "/Game/Maps/X",
	 *     "is_persistent": bool,
	 *     "is_visible": bool,
	 *     "actor_count": N,
	 *     "is_dirty": bool
	 *   }
	 */
	TSharedRef<FJsonObject> LVL_BuildLevelSummary(ULevel* Level, UWorld* OwningWorld)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Level)
		{
			Obj->SetField(TEXT("map_path"), MakeShared<FJsonValueNull>());
			return Obj;
		}
		const FString PackageName = Level->GetOutermost()->GetName();
		Obj->SetStringField(TEXT("map_path"), PackageName);
		Obj->SetBoolField(TEXT("is_persistent"), OwningWorld && OwningWorld->PersistentLevel == Level);
		Obj->SetBoolField(TEXT("is_visible"), Level->bIsVisible);
		Obj->SetNumberField(TEXT("actor_count"), static_cast<double>(Level->Actors.Num()));
		const UPackage* Pkg = Level->GetOutermost();
		Obj->SetBoolField(TEXT("is_dirty"), Pkg && Pkg->IsDirty());
		return Obj;
	}
} // namespace

namespace FLevelTools
{

// ─── Lane B sanity probe (per critic N1) ──────────────────────────────────────────────────────
//
// Purpose: prove the Lane B router (FMCPDispatchQueue::IsThreadSafe + DispatchInline +
// FMCPConnection short-circuit) is still alive after the Phase 2 hotfix demoted every AR/CB tool
// to Lane A. The handler does NO UObject access — pure string/JSON manipulation — so it satisfies
// the Lane B contract (no GWorld, no LoadObject, no NewObject, no GC interaction).
//
// Response shape:
//   { "echo": <args verbatim>, "thread_id": "139987..." }
//
// A non-game-thread thread_id (compared against the main thread's id observed in Phase 1
// initialisation logs) confirms inline dispatch. Phase 3 smoke spike-calls this 100× and asserts
// no crashes / no asserts.
FMCPResponse Tool_Phase3LaneBSanity(const FMCPRequest& Request)
{
	// Lane B handlers MUST NOT touch UObjects. Build a pure-string response and echo args.
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();

	// Echo args. If null (no payload), emit empty object so AI clients see the field consistently.
	TSharedRef<FJsonObject> EchoObj = Request.Args.IsValid()
		? Request.Args.ToSharedRef()
		: MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("echo"), EchoObj);

	// UE's platform thread id is a uint32 — stable for the lifetime of the OS thread. The smoke
	// spike compares this against the game-thread id captured at module init and asserts the
	// Lane B response came from a DIFFERENT thread.
	const uint32 TID = FPlatformTLS::GetCurrentThreadId();
	Out->SetStringField(TEXT("thread_id"), FString::FromInt(static_cast<int32>(TID)));

	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.list_loaded (read-only — works in PIE) ────────────────────────────────────────────
//
// Returns all currently loaded ULevels in either the editor world (no PIE) or the play world
// (PIE active). For each level emits {map_path, is_persistent, is_visible, actor_count, is_dirty}.
//
// Response: { "world_kind": "Editor"|"PIE", "world_map_path": "...", "levels": [...], "total": N }
FMCPResponse Tool_ListLoaded(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = LVL_ResolveReadWorld();
	if (!World)
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotFound,
			TEXT("no world available (GEditor missing and no PIE world)"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("world_kind"), FMCPWorldContext::IsPIEActive() ? TEXT("PIE") : TEXT("Editor"));
	Out->SetStringField(TEXT("world_map_path"), FMCPWorldContext::GetWorldPackagePath(World));

	TArray<TSharedPtr<FJsonValue>> Levels;
	const TArray<ULevel*>& AllLevels = World->GetLevels();
	Levels.Reserve(AllLevels.Num());
	for (ULevel* Level : AllLevels)
	{
		if (!Level)
		{
			continue;
		}
		Levels.Add(MakeShared<FJsonValueObject>(LVL_BuildLevelSummary(Level, World)));
	}
	Out->SetArrayField(TEXT("levels"), Levels);
	Out->SetNumberField(TEXT("total"), static_cast<double>(Levels.Num()));
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── level.current_map (read-only — works in PIE) ────────────────────────────────────────────
//
// Returns the persistent-level package path of the editor world (or PIE world during PIE).
//
// Response: { "map_path": "/Game/Maps/X", "world_kind": "Editor"|"PIE", "is_dirty": bool }
FMCPResponse Tool_CurrentMap(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UWorld* World = LVL_ResolveReadWorld();
	if (!World)
	{
		return LVL_MakeError(Request, kMCPErrorLevelNotFound,
			TEXT("no world available (GEditor missing and no PIE world)"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("map_path"), FMCPWorldContext::GetWorldPackagePath(World));
	Out->SetStringField(TEXT("world_kind"), FMCPWorldContext::IsPIEActive() ? TEXT("PIE") : TEXT("Editor"));

	const UPackage* Pkg = World->GetOutermost();
	Out->SetBoolField(TEXT("is_dirty"), Pkg && Pkg->IsDirty());
	return LVL_MakeSuccessObj(Request, Out);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Lane B sanity (kept around as dev utility — leading underscore convention keeps it out of
	// tools.list per existing Phase 2 hotfix-3 filter).
	RegisterTool(TEXT("_phase3_lane_b_sanity"), &Tool_Phase3LaneBSanity, /*Lane B*/ true);

	// Day 1: read-only enumeration.
	RegisterTool(TEXT("level.list_loaded"), &Tool_ListLoaded, /*Lane A*/ false);
	RegisterTool(TEXT("level.current_map"), &Tool_CurrentMap, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 3 Day 1: registered 2 level.* read tools + 1 sanity probe (_phase3_lane_b_sanity, Lane B)"));
}

} // namespace FLevelTools

#undef LOCTEXT_NAMESPACE
