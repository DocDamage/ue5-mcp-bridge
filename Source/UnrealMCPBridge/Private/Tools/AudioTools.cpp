// Copyright FatumGame. All Rights Reserved.

#include "AudioTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPWorldContext.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// AUD_ prefix per unity-build convention.
	constexpr int32 kAUDErrorInvalidParams = -32602;
	constexpr int32 kAUDErrorInternal      = -32603;

	void AUD_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse AUD_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		AUD_StampIds(Request, R);
		R.bIsError = true; R.ErrorCode = Code; R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse AUD_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		AUD_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool AUD_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = AUD_MakeError(Request, kAUDErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = AUD_MakeError(Request, kAUDErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	UObject* AUD_LoadByPath(const FString& Path, int32& OutErrorCode, FString& OutError)
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
		}
		return Loaded;
	}
} // namespace

namespace FAudioTools
{

// ─── audio.create_sound_cue ───────────────────────────────────────────────────────────────────
FMCPResponse Tool_CreateSoundCue(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return AUD_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString DestPathRaw;
	FMCPResponse Err;
	if (!AUD_RequireStringField(Request, TEXT("dest_path"), DestPathRaw, Err)) { return Err; }

	const FString DestPathNorm = FMCPAssetPathUtils::Normalize(DestPathRaw);
	if (DestPathNorm.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(DestPathNorm))
	{
		return AUD_MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("dest_path '%s' malformed or unknown mount"), *DestPathRaw));
	}

	const FString PackagePath = FPaths::GetPath(DestPathNorm);
	const FString AssetName   = FPaths::GetBaseFilename(DestPathNorm);

	if (FPackageName::DoesPackageExist(DestPathNorm) ||
	    FindObject<UObject>(nullptr, *(DestPathNorm + TEXT(".") + AssetName)) != nullptr)
	{
		return AUD_MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("dest_path '%s' already exists"), *DestPathNorm));
	}

	// Optional initial sound wave.
	USoundWave* SourceWave = nullptr;
	FString SourceWavePath;
	if (Request.Args->TryGetStringField(TEXT("source_wave_path"), SourceWavePath) && !SourceWavePath.IsEmpty())
	{
		int32 LoadErrCode = 0;
		FString LoadErrMsg;
		UObject* Loaded = AUD_LoadByPath(SourceWavePath, LoadErrCode, LoadErrMsg);
		if (!Loaded) { return AUD_MakeError(Request, LoadErrCode, LoadErrMsg); }
		SourceWave = Cast<USoundWave>(Loaded);
		if (!SourceWave)
		{
			return AUD_MakeError(Request, kMCPErrorWrongClass,
				FString::Printf(TEXT("source_wave_path '%s' is class '%s'; expected USoundWave"),
					*SourceWavePath, *Loaded->GetClass()->GetPathName()));
		}
	}

	const FString PackageName = PackagePath + TEXT("/") + AssetName;
	UPackage* CuePkg = CreatePackage(*PackageName);
	if (!CuePkg)
	{
		return AUD_MakeError(Request, kAUDErrorInternal,
			FString::Printf(TEXT("CreatePackage returned null for '%s'"), *PackageName));
	}
	CuePkg->FullyLoad();

	FScopedTransaction Transaction(LOCTEXT("MCP_CreateSoundCue", "Create Sound Cue"));

	USoundCue* Cue = NewObject<USoundCue>(CuePkg, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Cue)
	{
		return AUD_MakeError(Request, kAUDErrorInternal,
			FString::Printf(TEXT("NewObject<USoundCue> returned null for %s"), *DestPathNorm));
	}

	if (SourceWave)
	{
		// Construct a single USoundNodeWavePlayer pointing at the source wave and wire it as FirstNode.
		USoundNodeWavePlayer* WavePlayer = Cue->ConstructSoundNode<USoundNodeWavePlayer>();
		WavePlayer->SetSoundWave(SourceWave);
		Cue->FirstNode = WavePlayer;
		Cue->LinkGraphNodesFromSoundNodes();
	}

	FAssetRegistryModule::AssetCreated(Cue);
	CuePkg->MarkPackageDirty();

	bool bSaveRequested = false, bSavedOk = false;
	Request.Args->TryGetBoolField(TEXT("save"), bSaveRequested);
	if (bSaveRequested)
	{
		if (UEditorAssetSubsystem* EAS = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			bSavedOk = EAS->SaveLoadedAsset(Cue, /*bOnlyIfIsDirty*/ true);
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("created"), true);
	Out->SetStringField(TEXT("asset_path"), Cue->GetPathName());
	Out->SetBoolField(TEXT("has_source_wave"), SourceWave != nullptr);
	Out->SetBoolField(TEXT("saved"), bSavedOk);
	return AUD_MakeSuccessObj(Request, Out);
}

// ─── audio.set_attenuation ────────────────────────────────────────────────────────────────────
//
// Args:    { sound_path: string, attenuation_path?: string (null/empty = clear) }
// Result:  { prior_attenuation, new_attenuation, sound_class }
FMCPResponse Tool_SetAttenuation(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return AUD_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString SoundPath;
	FMCPResponse Err;
	if (!AUD_RequireStringField(Request, TEXT("sound_path"), SoundPath, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UObject* SoundObj = AUD_LoadByPath(SoundPath, LoadErrCode, LoadErrMsg);
	if (!SoundObj) { return AUD_MakeError(Request, LoadErrCode, LoadErrMsg); }
	USoundBase* Sound = Cast<USoundBase>(SoundObj);
	if (!Sound)
	{
		return AUD_MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("sound_path '%s' is class '%s'; expected USoundBase (USoundCue / USoundWave / ...)"),
				*SoundPath, *SoundObj->GetClass()->GetPathName()));
	}

	// Optional attenuation_path — null/empty/missing → clear existing.
	FString AttenuationPath;
	USoundAttenuation* Attenuation = nullptr;
	if (Request.Args->TryGetStringField(TEXT("attenuation_path"), AttenuationPath) && !AttenuationPath.IsEmpty())
	{
		UObject* AttenObj = AUD_LoadByPath(AttenuationPath, LoadErrCode, LoadErrMsg);
		if (!AttenObj) { return AUD_MakeError(Request, LoadErrCode, LoadErrMsg); }
		Attenuation = Cast<USoundAttenuation>(AttenObj);
		if (!Attenuation)
		{
			return AUD_MakeError(Request, kMCPErrorWrongClass,
				FString::Printf(TEXT("attenuation_path '%s' is class '%s'; expected USoundAttenuation"),
					*AttenuationPath, *AttenObj->GetClass()->GetPathName()));
		}
	}

	FScopedTransaction Transaction(LOCTEXT("MCP_SetAttenuation", "Set Sound Attenuation"));
	Sound->Modify();

	const FString PriorPath = Sound->AttenuationSettings
		? Sound->AttenuationSettings->GetPathName()
		: FString();

	Sound->AttenuationSettings = Attenuation;

	if (UPackage* Pkg = Sound->GetOutermost()) { Pkg->MarkPackageDirty(); }

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("sound_class"), Sound->GetClass()->GetPathName());
	Out->SetStringField(TEXT("prior_attenuation"), PriorPath);
	Out->SetStringField(TEXT("new_attenuation"),
		Attenuation ? Attenuation->GetPathName() : FString());
	Out->SetBoolField(TEXT("cleared"), Attenuation == nullptr);
	return AUD_MakeSuccessObj(Request, Out);
}

// ─── audio.list_mix_classes ───────────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string }
// Result:  { sound_classes: [{ path, name }], sound_mixes: [{ path, name }] }
FMCPResponse Tool_ListMixClasses(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PathPrefix;
	if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix); }

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	auto QueryClass = [&](UClass* Cls) -> TArray<TSharedPtr<FJsonValue>>
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(Cls->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths   = true;
		if (!PathPrefix.IsEmpty()) { Filter.PackagePaths.Add(*PathPrefix); }
		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		Assets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
		});

		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Assets.Num());
		for (const FAssetData& A : Assets)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("path"), A.GetSoftObjectPath().ToString());
			Obj->SetStringField(TEXT("name"), A.AssetName.ToString());
			Obj->SetStringField(TEXT("class"), A.AssetClassPath.ToString());
			Result.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Result;
	};

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("sound_classes"), QueryClass(USoundClass::StaticClass()));
	Out->SetArrayField(TEXT("sound_mixes"),   QueryClass(USoundMix::StaticClass()));
	return AUD_MakeSuccessObj(Request, Out);
}

void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("audio.create_sound_cue"),  &Tool_CreateSoundCue,  /*Lane A*/ false);
	RegisterTool(TEXT("audio.set_attenuation"),   &Tool_SetAttenuation,  /*Lane A*/ false);
	RegisterTool(TEXT("audio.list_mix_classes"),  &Tool_ListMixClasses,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Audio surface registered: 3 audio.* tools (create_sound_cue + set_attenuation + list_mix_classes), all Lane A"));
}

} // namespace FAudioTools

#undef LOCTEXT_NAMESPACE
