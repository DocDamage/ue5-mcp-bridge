// Copyright FatumGame. All Rights Reserved.

#include "AnimTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPWorldContext.h"

#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Factories/AnimMontageFactory.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// ANM_ prefix per the unity-build symbol-collision pattern.
	constexpr int32 kANMErrorInvalidParams = -32602;
	constexpr int32 kANMErrorInternal      = -32603;

	void ANM_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse ANM_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		ANM_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse ANM_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		ANM_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool ANM_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = ANM_MakeError(Request, kANMErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = ANM_MakeError(Request, kANMErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/** Load a UAnimSequence by path. Same error contract as the SequencerTools loader. */
	UAnimSequence* ANM_LoadSequenceByPath(const FString& Path, int32& OutErrorCode, FString& OutError)
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
		UAnimSequence* Seq = Cast<UAnimSequence>(Loaded);
		if (!Seq)
		{
			OutErrorCode = kMCPErrorWrongClass;
			OutError = FString::Printf(TEXT("'%s' is class '%s'; expected UAnimSequence"),
				*Path, *Loaded->GetClass()->GetPathName());
			return nullptr;
		}
		return Seq;
	}

	/** Load a UAnimMontage by path. */
	UAnimMontage* ANM_LoadMontageByPath(const FString& Path, int32& OutErrorCode, FString& OutError)
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
		UAnimMontage* M = Cast<UAnimMontage>(Loaded);
		if (!M)
		{
			OutErrorCode = kMCPErrorWrongClass;
			OutError = FString::Printf(TEXT("'%s' is class '%s'; expected UAnimMontage"),
				*Path, *Loaded->GetClass()->GetPathName());
			return nullptr;
		}
		return M;
	}
} // namespace

namespace FAnimTools
{

// ─── anim.list_sequences ──────────────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string (e.g. "/Game/Characters"), page_size?: int (default 100, clamp [1,1000]),
//            page_token?: string }
// Result:  { sequences: [{ asset_path, name, sequence_length, frame_rate, skeleton_path }],
//            next_page_token?, total_known }
FMCPResponse Tool_ListSequences(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PathPrefix;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix); }

	int32 PageSize = 100;
	if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("page_size"), PageSize); }
	PageSize = FMath::Clamp(PageSize, 1, 1000);

	FString PageToken;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("page_token"), PageToken); }

	// Build FilterHash so cursor staleness is detectable.
	const uint32 FilterHash = GetTypeHash(PathPrefix);

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = false;
	Filter.bRecursivePaths   = true;
	if (!PathPrefix.IsEmpty())
	{
		Filter.PackagePaths.Add(*PathPrefix);
	}
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	// Stable sort by ObjectPath.
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
	});

	// Decode cursor.
	int32 StartIdx = 0;
	FMCPPageCursor InCursor;
	if (!PageToken.IsEmpty())
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(PageToken, InCursor, DecodeErr))
		{
			return ANM_MakeError(Request, kANMErrorInvalidParams,
				FString::Printf(TEXT("invalid page_token: %s"), *DecodeErr));
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(InCursor, FilterHash))
		{
			return ANM_MakeError(Request, kMCPErrorStaleCursor,
				TEXT("filter mutated between pages (path_prefix changed); restart pagination"));
		}
		// Skip past LastAssetPath.
		while (StartIdx < Assets.Num() &&
		       Assets[StartIdx].GetSoftObjectPath().ToString() <= InCursor.LastAssetPath)
		{
			++StartIdx;
		}
	}

	TArray<TSharedPtr<FJsonValue>> SeqArr;
	const int32 EndIdx = FMath::Min(StartIdx + PageSize, Assets.Num());
	for (int32 i = StartIdx; i < EndIdx; ++i)
	{
		const FAssetData& A = Assets[i];
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());
		Obj->SetStringField(TEXT("name"), A.AssetName.ToString());

		// Load to fetch length + frame rate + skeleton.  Light-touch — single-asset LoadObject is fast.
		UAnimSequence* Seq = Cast<UAnimSequence>(A.GetAsset());
		if (Seq)
		{
			Obj->SetNumberField(TEXT("sequence_length"), Seq->GetPlayLength());
			Obj->SetNumberField(TEXT("frame_rate_decimal"), Seq->GetSamplingFrameRate().AsDecimal());
			if (USkeleton* Sk = Seq->GetSkeleton())
			{
				Obj->SetStringField(TEXT("skeleton_path"), Sk->GetPathName());
			}
		}
		SeqArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("sequences"), SeqArr);
	Out->SetNumberField(TEXT("total_known"), Assets.Num());

	if (EndIdx < Assets.Num() && EndIdx > 0)
	{
		FMCPPageCursor OutCursor;
		OutCursor.FilterHash = FilterHash;
		OutCursor.LastAssetPath = Assets[EndIdx - 1].GetSoftObjectPath().ToString();
		OutCursor.TotalKnownSnapshot = Assets.Num();
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(OutCursor));
	}

	return ANM_MakeSuccessObj(Request, Out);
}

// ─── anim.create_montage ──────────────────────────────────────────────────────────────────────
//
// Args:    { dest_path: string, source_sequence_path: string, target_skeleton?: string, save?: bool }
// Result:  { created, asset_path, saved, skeleton_path, source_length }
//
// Replicates UAnimMontageFactory::FactoryCreateNew inline:
//   1. NewObject<UAnimMontage>
//   2. SetSkeleton (from source sequence)
//   3. AddSlot("DefaultSlot") + push a FAnimSegment for the source
//   4. EnsureStartingSection (FCompositeSection "Default" at t=0)
//
// Errors: standard + -32054 SkeletonMismatch.
FMCPResponse Tool_CreateMontage(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ANM_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString DestPathRaw, SourceSeqPath;
	FMCPResponse Err;
	if (!ANM_RequireStringField(Request, TEXT("dest_path"),            DestPathRaw,  Err)) { return Err; }
	if (!ANM_RequireStringField(Request, TEXT("source_sequence_path"), SourceSeqPath, Err)) { return Err; }

	const FString DestPathNorm = FMCPAssetPathUtils::Normalize(DestPathRaw);
	if (DestPathNorm.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(DestPathNorm))
	{
		return ANM_MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("dest_path '%s' malformed or unknown mount"), *DestPathRaw));
	}

	const FString PackagePath = FPaths::GetPath(DestPathNorm);
	const FString AssetName   = FPaths::GetBaseFilename(DestPathNorm);

	if (FPackageName::DoesPackageExist(DestPathNorm) ||
	    FindObject<UObject>(nullptr, *(DestPathNorm + TEXT(".") + AssetName)) != nullptr)
	{
		return ANM_MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("dest_path '%s' already exists"), *DestPathNorm));
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UAnimSequence* SourceSeq = ANM_LoadSequenceByPath(SourceSeqPath, LoadErrCode, LoadErrMsg);
	if (!SourceSeq) { return ANM_MakeError(Request, LoadErrCode, LoadErrMsg); }

	USkeleton* Skeleton = SourceSeq->GetSkeleton();
	if (!Skeleton)
	{
		return ANM_MakeError(Request, kMCPErrorSkeletonMismatch,
			FString::Printf(TEXT("source_sequence '%s' has no skeleton bound"), *SourceSeqPath));
	}

	// Optional target_skeleton constraint check.
	FString TargetSkeletonPath;
	if (Request.Args->TryGetStringField(TEXT("target_skeleton"), TargetSkeletonPath) && !TargetSkeletonPath.IsEmpty())
	{
		USkeleton* TargetSk = LoadObject<USkeleton>(nullptr, *TargetSkeletonPath);
		if (!TargetSk)
		{
			return ANM_MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("target_skeleton '%s' not loadable"), *TargetSkeletonPath));
		}
		if (TargetSk != Skeleton)
		{
			return ANM_MakeError(Request, kMCPErrorSkeletonMismatch,
				FString::Printf(TEXT("target_skeleton '%s' does not match source's skeleton '%s'"),
					*TargetSkeletonPath, *Skeleton->GetPathName()));
		}
	}

	const FString PackageName = PackagePath + TEXT("/") + AssetName;
	UPackage* MontagePkg = CreatePackage(*PackageName);
	if (!MontagePkg)
	{
		return ANM_MakeError(Request, kANMErrorInternal,
			FString::Printf(TEXT("CreatePackage returned null for '%s'"), *PackageName));
	}
	MontagePkg->FullyLoad();

	FScopedTransaction Transaction(LOCTEXT("MCP_CreateMontage", "Create Anim Montage"));

	UAnimMontage* Montage = NewObject<UAnimMontage>(
		MontagePkg, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Montage)
	{
		return ANM_MakeError(Request, kANMErrorInternal,
			FString::Printf(TEXT("NewObject<UAnimMontage> returned null for %s"), *DestPathNorm));
	}

	Montage->SetSkeleton(Skeleton);

	// Build default slot + first segment referencing the source.
	FSlotAnimationTrack& NewSlot = Montage->AddSlot(FAnimSlotGroup::DefaultSlotName);
	FAnimSegment NewSegment;
	NewSegment.SetAnimReference(SourceSeq, /*bInitialize*/ true);
	NewSegment.StartPos      = 0.0f;
	NewSegment.AnimStartTime = 0.0f;
	NewSegment.AnimEndTime   = SourceSeq->GetPlayLength();
	NewSegment.AnimPlayRate  = 1.0f;
	NewSegment.LoopingCount  = 1;
	NewSlot.AnimTrack.AnimSegments.Add(NewSegment);

	// Sync montage length with the source — montage's overall length is the slot's max end position.
	Montage->SetCompositeLength(SourceSeq->GetPlayLength());

	// Add the default starting section (the factory does the same — it's required for the editor
	// to consider the montage usable).
	UAnimMontageFactory::EnsureStartingSection(Montage);

	FAssetRegistryModule::AssetCreated(Montage);
	MontagePkg->MarkPackageDirty();

	bool bSaveRequested = false, bSavedOk = false;
	Request.Args->TryGetBoolField(TEXT("save"), bSaveRequested);
	if (bSaveRequested)
	{
		if (UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			bSavedOk = EAS->SaveLoadedAsset(Montage, /*bOnlyIfIsDirty*/ true);
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("created"), true);
	Out->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	Out->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
	Out->SetNumberField(TEXT("source_length"), SourceSeq->GetPlayLength());
	Out->SetBoolField(TEXT("saved"), bSavedOk);
	return ANM_MakeSuccessObj(Request, Out);
}

// ─── anim.add_section ─────────────────────────────────────────────────────────────────────────
//
// Args:    { montage_path: string, section_name: string, start_time: number }
// Result:  { added, section_index, section_count }
FMCPResponse Tool_AddSection(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ANM_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString MontagePath, SectionName;
	FMCPResponse Err;
	if (!ANM_RequireStringField(Request, TEXT("montage_path"), MontagePath, Err)) { return Err; }
	if (!ANM_RequireStringField(Request, TEXT("section_name"), SectionName, Err)) { return Err; }

	double StartTime = 0.0;
	if (!Request.Args->TryGetNumberField(TEXT("start_time"), StartTime))
	{
		return ANM_MakeError(Request, kANMErrorInvalidParams,
			TEXT("anim.add_section requires args.start_time (seconds, non-negative)"));
	}
	if (StartTime < 0.0)
	{
		return ANM_MakeError(Request, kANMErrorInvalidParams,
			FString::Printf(TEXT("start_time %.3f must be >= 0"), StartTime));
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UAnimMontage* Montage = ANM_LoadMontageByPath(MontagePath, LoadErrCode, LoadErrMsg);
	if (!Montage) { return ANM_MakeError(Request, LoadErrCode, LoadErrMsg); }

	FScopedTransaction Transaction(LOCTEXT("MCP_AddMontageSection", "Add Montage Section"));
	Montage->Modify();

	FCompositeSection NewSection;
	NewSection.SectionName = FName(*SectionName);
	// FAnimLinkableElement::Link sets both the absolute time AND the protected LinkValue field.
	NewSection.Link(Montage, static_cast<float>(StartTime));

	Montage->CompositeSections.Add(NewSection);

	if (UPackage* Pkg = Montage->GetOutermost()) { Pkg->MarkPackageDirty(); }

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("added"), true);
	Out->SetNumberField(TEXT("section_index"), Montage->CompositeSections.Num() - 1);
	Out->SetNumberField(TEXT("section_count"), Montage->CompositeSections.Num());
	return ANM_MakeSuccessObj(Request, Out);
}

// ─── anim.add_notify ──────────────────────────────────────────────────────────────────────────
//
// Args:    { montage_path: string, notify_name: string, time: number,
//            duration?: number (>0 = state notify), notify_track_name?: string }
// Result:  { added, notify_index, total_notifies, track_index }
FMCPResponse Tool_AddNotify(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ANM_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString MontagePath, NotifyName;
	FMCPResponse Err;
	if (!ANM_RequireStringField(Request, TEXT("montage_path"), MontagePath, Err)) { return Err; }
	if (!ANM_RequireStringField(Request, TEXT("notify_name"), NotifyName, Err)) { return Err; }

	double Time = 0.0;
	if (!Request.Args->TryGetNumberField(TEXT("time"), Time))
	{
		return ANM_MakeError(Request, kANMErrorInvalidParams,
			TEXT("anim.add_notify requires args.time (seconds, non-negative)"));
	}
	if (Time < 0.0)
	{
		return ANM_MakeError(Request, kANMErrorInvalidParams,
			FString::Printf(TEXT("time %.3f must be >= 0"), Time));
	}

	double Duration = 0.0;
	Request.Args->TryGetNumberField(TEXT("duration"), Duration);

	FString TrackName;
	Request.Args->TryGetStringField(TEXT("notify_track_name"), TrackName);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UAnimMontage* Montage = ANM_LoadMontageByPath(MontagePath, LoadErrCode, LoadErrMsg);
	if (!Montage) { return ANM_MakeError(Request, LoadErrCode, LoadErrMsg); }

	FScopedTransaction Transaction(LOCTEXT("MCP_AddMontageNotify", "Add Montage Notify"));
	Montage->Modify();

	// Resolve notify track — default to first track (auto-create "Default" if none exist).
	int32 TrackIndex = 0;
	if (!TrackName.IsEmpty())
	{
		bool bFound = false;
		for (int32 i = 0; i < Montage->AnimNotifyTracks.Num(); ++i)
		{
			if (Montage->AnimNotifyTracks[i].TrackName == FName(*TrackName))
			{
				TrackIndex = i;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			return ANM_MakeError(Request, kMCPErrorNotifyTrackNotFound,
				FString::Printf(TEXT("notify_track '%s' not found on montage '%s' (available count: %d)"),
					*TrackName, *MontagePath, Montage->AnimNotifyTracks.Num()));
		}
	}
	else if (Montage->AnimNotifyTracks.Num() == 0)
	{
		FAnimNotifyTrack DefaultTrack;
		DefaultTrack.TrackName = TEXT("Default");
		DefaultTrack.TrackColor = FLinearColor::White;
		Montage->AnimNotifyTracks.Add(DefaultTrack);
	}

	FAnimNotifyEvent NotifyEvent;
	NotifyEvent.NotifyName        = FName(*NotifyName);
	NotifyEvent.TriggerTimeOffset = 0.0f;
	NotifyEvent.EndTriggerTimeOffset = 0.0f;
	NotifyEvent.TrackIndex        = TrackIndex;
	// Link() is the public setter for the protected LinkValue + LinkMethod fields.
	NotifyEvent.Link(Montage, static_cast<float>(Time));
	if (Duration > 0.0)
	{
		NotifyEvent.SetDuration(static_cast<float>(Duration));
	}

	Montage->Notifies.Add(NotifyEvent);
	Montage->RefreshCacheData();

	if (UPackage* Pkg = Montage->GetOutermost()) { Pkg->MarkPackageDirty(); }

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("added"), true);
	Out->SetNumberField(TEXT("notify_index"), Montage->Notifies.Num() - 1);
	Out->SetNumberField(TEXT("total_notifies"), Montage->Notifies.Num());
	Out->SetNumberField(TEXT("track_index"), TrackIndex);
	return ANM_MakeSuccessObj(Request, Out);
}

// ─── anim.set_blend_mode ──────────────────────────────────────────────────────────────────────
//
// Args:    { montage_path: string, blend_in_time?: number, blend_out_time?: number }
// Result:  { prior_blend_in, prior_blend_out, new_blend_in, new_blend_out }
FMCPResponse Tool_SetBlendMode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return ANM_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString MontagePath;
	FMCPResponse Err;
	if (!ANM_RequireStringField(Request, TEXT("montage_path"), MontagePath, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UAnimMontage* Montage = ANM_LoadMontageByPath(MontagePath, LoadErrCode, LoadErrMsg);
	if (!Montage) { return ANM_MakeError(Request, LoadErrCode, LoadErrMsg); }

	const float PriorIn  = Montage->BlendIn.GetBlendTime();
	const float PriorOut = Montage->BlendOut.GetBlendTime();

	double BlendInTime  = PriorIn;
	double BlendOutTime = PriorOut;
	const bool bHasIn  = Request.Args->TryGetNumberField(TEXT("blend_in_time"),  BlendInTime);
	const bool bHasOut = Request.Args->TryGetNumberField(TEXT("blend_out_time"), BlendOutTime);

	if (!bHasIn && !bHasOut)
	{
		return ANM_MakeError(Request, kANMErrorInvalidParams,
			TEXT("anim.set_blend_mode requires at least one of blend_in_time / blend_out_time"));
	}
	if (BlendInTime < 0.0 || BlendOutTime < 0.0)
	{
		return ANM_MakeError(Request, kANMErrorInvalidParams,
			TEXT("blend times must be >= 0"));
	}

	FScopedTransaction Transaction(LOCTEXT("MCP_SetMontageBlend", "Set Montage Blend"));
	Montage->Modify();

	if (bHasIn)  { Montage->BlendIn.SetBlendTime(static_cast<float>(BlendInTime));  }
	if (bHasOut) { Montage->BlendOut.SetBlendTime(static_cast<float>(BlendOutTime)); }

	if (UPackage* Pkg = Montage->GetOutermost()) { Pkg->MarkPackageDirty(); }

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("prior_blend_in"),  PriorIn);
	Out->SetNumberField(TEXT("prior_blend_out"), PriorOut);
	Out->SetNumberField(TEXT("new_blend_in"),    Montage->BlendIn.GetBlendTime());
	Out->SetNumberField(TEXT("new_blend_out"),   Montage->BlendOut.GetBlendTime());
	return ANM_MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("anim.list_sequences"),  &Tool_ListSequences, /*Lane A*/ false);
	RegisterTool(TEXT("anim.create_montage"),  &Tool_CreateMontage, /*Lane A*/ false);
	RegisterTool(TEXT("anim.add_section"),     &Tool_AddSection,    /*Lane A*/ false);
	RegisterTool(TEXT("anim.add_notify"),      &Tool_AddNotify,     /*Lane A*/ false);
	RegisterTool(TEXT("anim.set_blend_mode"),  &Tool_SetBlendMode,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Animation surface registered: 5 anim.* tools "
			 "(list_sequences + create_montage + add_section + add_notify + set_blend_mode), all Lane A"));
}

} // namespace FAnimTools

#undef LOCTEXT_NAMESPACE
