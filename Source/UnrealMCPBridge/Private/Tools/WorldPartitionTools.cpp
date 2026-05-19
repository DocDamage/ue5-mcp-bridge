// Copyright FatumGame. All Rights Reserved.

#include "WorldPartitionTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "WorldPartition/WorldPartition.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// WP_ prefix per unity-build convention.
	constexpr int32 kWPErrorInvalidParams = -32602;
	constexpr int32 kWPErrorInternal      = -32603;

	void WP_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse WP_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		WP_StampIds(Request, R);
		R.bIsError = true; R.ErrorCode = Code; R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse WP_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		WP_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool WP_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = WP_MakeError(Request, kWPErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = WP_MakeError(Request, kWPErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/** Load a UWorld by path. Returns nullptr + populates Out* on failure. */
	UWorld* WP_LoadWorldByPath(const FString& Path, int32& OutErrorCode, FString& OutError)
	{
		if (Path.IsEmpty()) { OutErrorCode = kMCPErrorInvalidPath; OutError = TEXT("path is empty"); return nullptr; }
		const FString Normalised = FMCPAssetPathUtils::Normalize(Path);
		if (Normalised.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(Normalised))
		{
			OutErrorCode = kMCPErrorInvalidPath;
			OutError = FString::Printf(TEXT("path '%s' malformed or unknown mount"), *Path);
			return nullptr;
		}
		UObject* Loaded = LoadObject<UObject>(nullptr, *Normalised);
		if (!Loaded)
		{
			const FString ObjPath = FMCPAssetPathUtils::ToObjectPath(Normalised);
			if (!ObjPath.IsEmpty() && ObjPath != Normalised) { Loaded = LoadObject<UObject>(nullptr, *ObjPath); }
		}
		if (!Loaded)
		{
			OutErrorCode = kMCPErrorObjectNotFound;
			OutError = FString::Printf(TEXT("'%s' not loadable"), *Path);
			return nullptr;
		}
		UWorld* World = Cast<UWorld>(Loaded);
		if (!World)
		{
			OutErrorCode = kMCPErrorWrongClass;
			OutError = FString::Printf(TEXT("'%s' is class '%s'; expected UWorld"),
				*Path, *Loaded->GetClass()->GetPathName());
			return nullptr;
		}
		return World;
	}
} // namespace

namespace FWorldPartitionTools
{

// ─── wp.is_partitioned ────────────────────────────────────────────────────────────────────────
//
// Args:    { level_path: string }
// Result:  { partitioned: bool, partition_path?: string }
FMCPResponse Tool_IsPartitioned(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString LevelPath;
	FMCPResponse Err;
	if (!WP_RequireStringField(Request, TEXT("level_path"), LevelPath, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UWorld* World = WP_LoadWorldByPath(LevelPath, LoadErrCode, LoadErrMsg);
	if (!World) { return WP_MakeError(Request, LoadErrCode, LoadErrMsg); }

	UWorldPartition* WP = World->GetWorldPartition();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("partitioned"), WP != nullptr);
	if (WP) { Out->SetStringField(TEXT("partition_path"), WP->GetPathName()); }
	Out->SetStringField(TEXT("world_path"), World->GetPathName());
	return WP_MakeSuccessObj(Request, Out);
}

// ─── wp.get_actor_runtime_grid ────────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string }
// Result:  { actor_path, runtime_grid }
FMCPResponse Tool_GetActorRuntimeGrid(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString ActorPath;
	FMCPResponse Err;
	if (!WP_RequireStringField(Request, TEXT("actor_path"), ActorPath, Err)) { return Err; }

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ false,
		bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return WP_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Out->SetStringField(TEXT("runtime_grid"), Actor->GetRuntimeGrid().ToString());
	return WP_MakeSuccessObj(Request, Out);
}

// ─── wp.set_actor_runtime_grid ────────────────────────────────────────────────────────────────
//
// Args:    { actor_path: string, runtime_grid: string (empty/None to clear) }
// Result:  { actor_path, prior_grid, new_grid }
FMCPResponse Tool_SetActorRuntimeGrid(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return WP_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString ActorPath;
	FMCPResponse Err;
	if (!WP_RequireStringField(Request, TEXT("actor_path"), ActorPath, Err)) { return Err; }

	FString NewGrid;
	if (!Request.Args->TryGetStringField(TEXT("runtime_grid"), NewGrid))
	{
		return WP_MakeError(Request, kWPErrorInvalidParams,
			TEXT("wp.set_actor_runtime_grid requires args.runtime_grid (string; empty to clear)"));
	}

	bool bAmbiguous = false;
	FString AmbiguityHint, ResolveErr;
	AActor* Actor = FMCPActorPathUtils::ResolveActor(ActorPath, /*bRejectPIE*/ true,
		bAmbiguous, AmbiguityHint, ResolveErr);
	if (!Actor)
	{
		return WP_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("actor '%s' not found: %s"), *ActorPath, *ResolveErr));
	}

	const FName Prior = Actor->GetRuntimeGrid();
	const FName Desired = NewGrid.IsEmpty() ? NAME_None : FName(*NewGrid);

	FScopedTransaction Transaction(LOCTEXT("MCP_SetActorRuntimeGrid", "Set Actor RuntimeGrid"));
	Actor->Modify();
	Actor->SetRuntimeGrid(Desired);

	if (UPackage* ExternalPkg = Actor->GetExternalPackage())
	{
		ExternalPkg->MarkPackageDirty();
	}
	else if (UPackage* OuterPkg = Actor->GetOutermost())
	{
		OuterPkg->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Out->SetStringField(TEXT("prior_grid"), Prior.ToString());
	Out->SetStringField(TEXT("new_grid"),   Desired.ToString());
	return WP_MakeSuccessObj(Request, Out);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("wp.is_partitioned"),         &Tool_IsPartitioned,        /*Lane A*/ false);
	RegisterTool(TEXT("wp.get_actor_runtime_grid"), &Tool_GetActorRuntimeGrid,  /*Lane A*/ false);
	RegisterTool(TEXT("wp.set_actor_runtime_grid"), &Tool_SetActorRuntimeGrid,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("WorldPartition surface registered: 3 wp.* tools (is_partitioned + get/set_actor_runtime_grid), all Lane A"));
}

} // namespace FWorldPartitionTools

#undef LOCTEXT_NAMESPACE
