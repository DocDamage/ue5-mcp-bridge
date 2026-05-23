// Copyright FatumGame. All Rights Reserved.

#include "InputTools.h"

#include "MCPSurfaceRegistry.h"

#include "FMCPDispatchQueue.h"
#include "MCPAssetLoader.h"
#include "MCPJsonBuilder.h"
#include "MCPToolHelpers.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPActorPathUtils.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPWorldContext.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "MCPMutatorScope.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Kismet/GameplayStatics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// INP_ prefix per the unity-build symbol-collision pattern (matches ACT_/COMP_/APT_/etc).

	/**
	 * Parse the ``value_type`` argument into ``EInputActionValueType``. Accepts case-sensitive
	 * "Boolean" / "Axis1D" / "Axis2D" / "Axis3D" matching UE's enum string names. Empty / missing
	 * → caller-supplied default (typically Boolean).
	 *
	 * Returns true on success; false populates ``OutError`` with an InvalidParams response.
	 */
	bool INP_ParseValueType(
		const FMCPRequest& Request,
		const FString& Raw,
		EInputActionValueType& OutType,
		FMCPResponse& OutError)
	{
		if (Raw.Equals(TEXT("Boolean"))) { OutType = EInputActionValueType::Boolean; return true; }
		if (Raw.Equals(TEXT("Axis1D")))  { OutType = EInputActionValueType::Axis1D;  return true; }
		if (Raw.Equals(TEXT("Axis2D")))  { OutType = EInputActionValueType::Axis2D;  return true; }
		if (Raw.Equals(TEXT("Axis3D")))  { OutType = EInputActionValueType::Axis3D;  return true; }
		OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("value_type '%s' not recognised; expected one of "
				"'Boolean' | 'Axis1D' | 'Axis2D' | 'Axis3D' (case-sensitive)"), *Raw));
		return false;
	}

	/** Stringify EInputActionValueType for response echo. */
	const TCHAR* INP_ValueTypeToString(EInputActionValueType T)
	{
		switch (T)
		{
		case EInputActionValueType::Boolean: return TEXT("Boolean");
		case EInputActionValueType::Axis1D:  return TEXT("Axis1D");
		case EInputActionValueType::Axis2D:  return TEXT("Axis2D");
		case EInputActionValueType::Axis3D:  return TEXT("Axis3D");
		}
		return TEXT("Unknown");
	}

	/**
	 * Normalise + validate an asset destination path. Returns true on success; false populates
	 * ``OutError`` with an InvalidPath/PathInUse error. ``OutPathNorm`` is the normalised path
	 * (e.g. ``/Game/Input/Actions/IA_Jump``); ``OutPackagePath`` is the parent folder; ``OutAssetName``
	 * is the asset's base filename. Caller uses these to construct the package + UObject name.
	 *
	 * Mirrors the validation done by AssetRegistryTools::Tool_AssetCreate.
	 */
	bool INP_NormaliseDestPath(
		const FMCPRequest& Request,
		const FString& Raw,
		FString& OutPathNorm,
		FString& OutPackagePath,
		FString& OutAssetName,
		FMCPResponse& OutError)
	{
		OutPathNorm = FMCPAssetPathUtils::Normalize(Raw);
		if (OutPathNorm.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(OutPathNorm))
		{
			OutError = FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidPath,
				FString::Printf(TEXT("path '%s' is not a valid mount-prefixed asset path "
					"(expected /Game/... or /<Plugin>/...)"), *Raw));
			return false;
		}
		OutPackagePath = FPaths::GetPath(OutPathNorm);
		OutAssetName   = FPaths::GetBaseFilename(OutPathNorm);
		return true;
	}

	/**
	 * Templated NewObject-on-fresh-package factory used by both ``input.create_input_action`` and
	 * ``input.create_mapping_context``. Encapsulates the standard pattern:
	 *
	 *   1. CreatePackage + FullyLoad
	 *   2. NewObject<T>(Pkg, *AssetName, RF_Public | RF_Standalone | RF_Transactional)
	 *   3. Caller initialises T-specific fields BETWEEN return and SavePackage call
	 *   4. FAssetRegistryModule::AssetCreated
	 *   5. MarkPackageDirty (via Scope.DirtyPackage)
	 *
	 * Returns the created object or nullptr on failure (caller responsible for OutError).
	 */
	template<typename T>
	T* INP_CreateAssetInPackage(
		const FString& PackagePath,
		const FString& AssetName,
		FMCPMutatorScope& Scope)
	{
		const FString PackageName = PackagePath / AssetName;
		UPackage* Pkg = CreatePackage(*PackageName);
		if (!Pkg) { return nullptr; }
		Pkg->FullyLoad();

		T* Asset = NewObject<T>(Pkg, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Asset) { return nullptr; }

		FAssetRegistryModule::AssetCreated(Asset);
		Scope.DirtyPackage(Pkg);
		return Asset;
	}

	/**
	 * Paginated asset enumeration helper. Mirrors AnimTools::Tool_ListSequences /
	 * MeshTools::Tool_List shape so the wire contract is identical across surfaces — caller
	 * sees ``{items: [{ asset_path, name }], total_known, next_page_token? }`` with the array
	 * field name supplied by the tool.
	 */
	FMCPResponse INP_ListAssetsOfClass(
		const FMCPRequest& Request,
		UClass* AssetClass,
		const TCHAR* ArrayFieldName)
	{
		check(IsInGameThread());
		check(AssetClass != nullptr);

		FString PathPrefix;
		if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("path_prefix"), PathPrefix); }

		int32 PageSize = 100;
		if (Request.Args.IsValid()) { Request.Args->TryGetNumberField(TEXT("page_size"), PageSize); }
		PageSize = FMath::Clamp(PageSize, 1, 1000);

		FString PageToken;
		if (Request.Args.IsValid()) { Request.Args->TryGetStringField(TEXT("page_token"), PageToken); }

		// FilterHash so cursor staleness is detectable across pages.
		const uint32 FilterHash = GetTypeHash(PathPrefix);

		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.ClassPaths.Add(AssetClass->GetClassPathName());
		Filter.bRecursiveClasses = false;
		Filter.bRecursivePaths   = true;
		if (!PathPrefix.IsEmpty())
		{
			Filter.PackagePaths.Add(*PathPrefix);
		}
		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		// Stable sort by ObjectPath (keyset pagination sort key).
		Assets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
		});

		// Decode + validate cursor.
		int32 StartIdx = 0;
		FMCPPageCursor InCursor;
		if (!PageToken.IsEmpty())
		{
			FString DecodeErr;
			if (!FMCPPageCursorUtils::Decode(PageToken, InCursor, DecodeErr))
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
					FString::Printf(TEXT("invalid page_token: %s"), *DecodeErr));
			}
			if (!FMCPPageCursorUtils::ValidateAgainstFilter(InCursor, FilterHash))
			{
				return FMCPToolHelpers::MakeError(Request, kMCPErrorStaleCursor,
					TEXT("filter mutated between pages (path_prefix changed); restart pagination"));
			}
			while (StartIdx < Assets.Num() &&
			       Assets[StartIdx].GetSoftObjectPath().ToString() <= InCursor.LastAssetPath)
			{
				++StartIdx;
			}
		}

		TArray<TSharedPtr<FJsonValue>> ItemArr;
		const int32 EndIdx = FMath::Min(StartIdx + PageSize, Assets.Num());
		ItemArr.Reserve(EndIdx - StartIdx);
		for (int32 i = StartIdx; i < EndIdx; ++i)
		{
			const FAssetData& A = Assets[i];
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("asset_path"), A.GetSoftObjectPath().ToString());
			Obj->SetStringField(TEXT("name"), A.AssetName.ToString());
			ItemArr.Add(MakeShared<FJsonValueObject>(Obj));
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetArrayField(ArrayFieldName, ItemArr);
		Out->SetNumberField(TEXT("total_known"), Assets.Num());

		if (EndIdx < Assets.Num() && EndIdx > 0)
		{
			FMCPPageCursor OutCursor;
			OutCursor.FilterHash = FilterHash;
			OutCursor.LastAssetPath = Assets[EndIdx - 1].GetSoftObjectPath().ToString();
			OutCursor.TotalKnownSnapshot = Assets.Num();
			Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(OutCursor));
		}

		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}
} // namespace

namespace FInputTools
{

// ─── input.list_mapping_contexts ──────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string, page_size?: int (default 100, clamp [1,1000]), page_token?: string }
// Result:  { mapping_contexts: [{ asset_path, name }], total_known, next_page_token? }
FMCPResponse Tool_ListMappingContexts(const FMCPRequest& Request)
{
	return INP_ListAssetsOfClass(Request, UInputMappingContext::StaticClass(), TEXT("mapping_contexts"));
}

// ─── input.list_input_actions ─────────────────────────────────────────────────────────────────
//
// Args:    { path_prefix?: string, page_size?: int (default 100, clamp [1,1000]), page_token?: string }
// Result:  { input_actions: [{ asset_path, name }], total_known, next_page_token? }
FMCPResponse Tool_ListInputActions(const FMCPRequest& Request)
{
	return INP_ListAssetsOfClass(Request, UInputAction::StaticClass(), TEXT("input_actions"));
}

// ─── input.get_context_bindings ───────────────────────────────────────────────────────────────
//
// Args:    { mapping_context_path: string }
// Result:  { mapping_context: string,
//            mappings: [{ action: string (path | "" if null),
//                         key: string (e.g. "Gamepad_FaceButton_Bottom" or "" if Invalid),
//                         modifiers: [class_name_string],
//                         triggers:  [class_name_string] }],
//            mapping_count: int }
//
// Walks UIMC->GetMappings() (the default key mappings; profile overrides are not enumerated here —
// callers seeking per-profile overrides should use marshall.read_property on
// UInputMappingContext.MappingProfileOverrides directly).
FMCPResponse Tool_GetContextBindings(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString MappingContextPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("mapping_context_path"), MappingContextPath, Err))
	{
		return Err;
	}

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UInputMappingContext* IMC = FMCPAssetLoader::Load<UInputMappingContext>(MappingContextPath, LoadErrCode, LoadErrMsg);
	if (!IMC) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();

	TArray<TSharedPtr<FJsonValue>> MappingArr;
	MappingArr.Reserve(Mappings.Num());
	for (const FEnhancedActionKeyMapping& Mapping : Mappings)
	{
		TSharedRef<FJsonObject> MapObj = MakeShared<FJsonObject>();

		// Action path — Mapping.Action is a TObjectPtr<const UInputAction>.
		MapObj->SetStringField(TEXT("action"),
			Mapping.Action ? Mapping.Action->GetPathName() : FString());

		// Key name — FKey's FName ToString gives the canonical short name (e.g.
		// "Gamepad_FaceButton_Bottom", "LeftMouseButton", "A"). EKeys::Invalid → empty.
		MapObj->SetStringField(TEXT("key"),
			Mapping.Key.IsValid() ? Mapping.Key.GetFName().ToString() : FString());

		// Modifier class list — short class names so the wire payload stays compact. Skip null
		// entries (legacy data can have unset slots in the instanced array).
		TArray<TSharedPtr<FJsonValue>> ModArr;
		ModArr.Reserve(Mapping.Modifiers.Num());
		for (const UInputModifier* Mod : Mapping.Modifiers)
		{
			if (Mod && Mod->GetClass())
			{
				ModArr.Add(MakeShared<FJsonValueString>(Mod->GetClass()->GetName()));
			}
		}
		MapObj->SetArrayField(TEXT("modifiers"), ModArr);

		// Trigger class list.
		TArray<TSharedPtr<FJsonValue>> TrigArr;
		TrigArr.Reserve(Mapping.Triggers.Num());
		for (const UInputTrigger* Trig : Mapping.Triggers)
		{
			if (Trig && Trig->GetClass())
			{
				TrigArr.Add(MakeShared<FJsonValueString>(Trig->GetClass()->GetName()));
			}
		}
		MapObj->SetArrayField(TEXT("triggers"), TrigArr);

		MappingArr.Add(MakeShared<FJsonValueObject>(MapObj));
	}

	return FMCPJsonBuilder()
		.Str(TEXT("mapping_context"), IMC->GetPathName())
		.Arr(TEXT("mappings"), MoveTemp(MappingArr))
		.Int(TEXT("mapping_count"), Mappings.Num())
		.BuildSuccess(Request);
}

// ─── input.list_player_contexts ───────────────────────────────────────────────────────────────
//
// Args:    { player_controller_path?: string (default = first PC in current world) }
// Result:  { player_controller: string,
//            contexts: [{ context: string (path), priority: int }],
//            context_count: int,
//            hint?: string (only when the probe-based enumeration path was taken — see header) }
//
// Resolution order for the player controller:
//   1. If args.player_controller_path supplied → FMCPActorPathUtils::ResolveActorOrNull (PIE-safe).
//   2. Else: walk worlds — prefer GEditor->PlayWorld (PIE) when active, else editor world. Pick
//      ``UGameplayStatics::GetPlayerController(World, 0)``.
//
// **UE 5.7 enumeration approach.** UEnhancedPlayerInput::AppliedInputContextData is protected.
// IEnhancedInputSubsystemInterface::HasMappingContext(IMC, OutPriority) IS public, so we probe
// every UInputMappingContext asset in the AssetRegistry against the player's subsystem. O(N)
// where N = total IMC asset count (typically <50). The response includes a ``hint`` field noting
// the probe-based approach so callers know the data is best-effort (won't surface contexts whose
// IMC asset was deleted but is still referenced by a strong pointer somewhere in the live
// subsystem state — these are rare).
FMCPResponse Tool_ListPlayerContexts(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString PCPath;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("player_controller_path"), PCPath);
	}

	// 1) Resolve player controller.
	APlayerController* PC = nullptr;
	if (!PCPath.IsEmpty())
	{
		AActor* Actor = FMCPActorPathUtils::ResolveActorOrNull(PCPath, /*bRejectPIE=*/ false);
		if (!Actor)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorObjectNotFound,
				FString::Printf(TEXT("player_controller_path '%s' not resolved"), *PCPath));
		}
		PC = Cast<APlayerController>(Actor);
		if (!PC)
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorWrongClass,
				FString::Printf(TEXT("'%s' is class '%s'; expected APlayerController"),
					*PCPath, *Actor->GetClass()->GetPathName()));
		}
	}
	else
	{
		// Default: first PC in current world. Prefer PIE world when active so the tool surfaces
		// runtime input state without needing the caller to pass paths.
		UWorld* World = nullptr;
		if (FMCPWorldContext::IsPIEActive() && GEditor && GEditor->PlayWorld)
		{
			World = GEditor->PlayWorld;
		}
		else
		{
			World = FMCPWorldContext::GetEditorWorld();
		}
		if (World)
		{
			// Iterate player controllers in the world directly (FConstPlayerControllerIterator
			// gives us controllers across all PlayerControllerList entries).
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				if (APlayerController* Candidate = It->Get())
				{
					PC = Candidate;
					break;
				}
			}
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();

	if (!PC)
	{
		// No player controller available — typical in editor world without PIE. Return an empty
		// contexts list with a descriptive hint rather than erroring out, so callers can probe
		// before/after PIE-start without conditional code paths.
		Out->SetStringField(TEXT("player_controller"), FString());
		TArray<TSharedPtr<FJsonValue>> Empty;
		Out->SetArrayField(TEXT("contexts"), Empty);
		Out->SetNumberField(TEXT("context_count"), 0);
		Out->SetStringField(TEXT("hint"),
			TEXT("no APlayerController in current world; start PIE or pass player_controller_path"));
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	Out->SetStringField(TEXT("player_controller"), PC->GetPathName());

	// 2) Get the enhanced input local-player subsystem off the PC.
	ULocalPlayer* LP = PC->GetLocalPlayer();
	if (!LP)
	{
		TArray<TSharedPtr<FJsonValue>> Empty;
		Out->SetArrayField(TEXT("contexts"), Empty);
		Out->SetNumberField(TEXT("context_count"), 0);
		Out->SetStringField(TEXT("hint"),
			TEXT("APlayerController has no ULocalPlayer (likely remote/standalone controller)"));
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	UEnhancedInputLocalPlayerSubsystem* EISS = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	if (!EISS)
	{
		TArray<TSharedPtr<FJsonValue>> Empty;
		Out->SetArrayField(TEXT("contexts"), Empty);
		Out->SetNumberField(TEXT("context_count"), 0);
		Out->SetStringField(TEXT("hint"),
			TEXT("ULocalPlayer has no UEnhancedInputLocalPlayerSubsystem (Enhanced Input plugin "
				 "may be disabled or subsystem not yet initialised)"));
		return FMCPToolHelpers::MakeSuccessObj(Request, Out);
	}

	// 3) Probe-enumerate active contexts by asking HasMappingContext for every known IMC asset.
	// AppliedInputContextData lives on UEnhancedPlayerInput protected, so this is the only public
	// path in 5.7. AssetRegistry walk is O(N) over IMC assets — typically <50 per project.
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UInputMappingContext::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = false;
	Filter.bRecursivePaths   = true;
	TArray<FAssetData> IMCAssets;
	AR.GetAssets(Filter, IMCAssets);

	// Sort for stable response ordering (priority desc, then path asc).
	struct FActiveCtx
	{
		FString Path;
		int32   Priority;
	};
	TArray<FActiveCtx> Active;
	Active.Reserve(IMCAssets.Num());

	for (const FAssetData& A : IMCAssets)
	{
		// Probe without forcing a load — only check IMCs that are already in memory. Loading every
		// IMC asset just to probe HasMappingContext would blow editor memory on large projects;
		// the live subsystem only references IMCs that are already loaded anyway.
		UObject* Obj = A.FastGetAsset(/*bLoad=*/ false);
		UInputMappingContext* IMC = Obj ? Cast<UInputMappingContext>(Obj) : nullptr;
		if (!IMC) { continue; }

		int32 FoundPriority = -1;
		if (EISS->HasMappingContext(IMC, FoundPriority))
		{
			Active.Add({ IMC->GetPathName(), FoundPriority });
		}
	}

	// Stable sort: priority DESC (higher priority first), path ASC tiebreak.
	Active.Sort([](const FActiveCtx& X, const FActiveCtx& Y)
	{
		if (X.Priority != Y.Priority) { return X.Priority > Y.Priority; }
		return X.Path < Y.Path;
	});

	TArray<TSharedPtr<FJsonValue>> CtxArr;
	CtxArr.Reserve(Active.Num());
	for (const FActiveCtx& Ctx : Active)
	{
		TSharedRef<FJsonObject> CObj = MakeShared<FJsonObject>();
		CObj->SetStringField(TEXT("context"), Ctx.Path);
		CObj->SetNumberField(TEXT("priority"), Ctx.Priority);
		CtxArr.Add(MakeShared<FJsonValueObject>(CObj));
	}

	Out->SetArrayField(TEXT("contexts"), CtxArr);
	Out->SetNumberField(TEXT("context_count"), Active.Num());
	Out->SetStringField(TEXT("hint"),
		TEXT("contexts enumerated via HasMappingContext probe over loaded UInputMappingContext "
			 "assets (UE 5.7 has no public bulk enumerator). Unloaded IMC assets are skipped."));
	return FMCPToolHelpers::MakeSuccessObj(Request, Out);
}

// ─── Wave N: Input Authoring (5 tools) ────────────────────────────────────────────────────────
//
// Closes the "Input Actions / Input Mapping Context create+edit" gap. Wave E S5 shipped 4 read-only
// introspection tools (list_mapping_contexts / list_input_actions / get_context_bindings /
// list_player_contexts); Wave N adds 5 authoring tools to make the surface end-to-end.
//
// **N2.5 (input.list_mappings) intentionally SKIPPED** — verification against existing
// ``input.get_context_bindings`` shows identical wire shape (per-mapping action/key/modifiers/triggers
// arrays). Caller should use ``input.get_context_bindings`` for enumeration. Wave N nets 5 tools, not 6.
//
// All 5 tools are Lane A — FMCPMutatorScope + NewObject + Modify + FAssetRegistryModule::AssetCreated
// + UEnhancedInputLocalPlayerSubsystem all require the game thread. Mutators are PIE-guarded EXCEPT
// add_context_to_player which is RUNTIME-ONLY (errors if PIE NOT active — inverse PIE gate).

// ─── input.create_input_action ─────────────────────────────────────────────────────────────────
//
// Args:    { path: string (required, /Game/.../IA_Foo),
//            value_type?: string (default "Boolean"; Boolean/Axis1D/Axis2D/Axis3D),
//            consume_input?: bool (default true),
//            reserve_all_mappings?: bool (default false) }
// Result:  { asset_path, value_type, consume_input, reserve_all_mappings, created }
//
// If asset already exists at ``path``, returns existing object's settings with ``created=false``
// (DOES NOT overwrite — caller should use ``asset.set_property`` for that).
//
// Lane A. PIE-guarded.
FMCPResponse Tool_CreateInputAction(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_CreateInputAction", "MCP: create Input Action"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString PathRaw;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("path"), PathRaw, Err)) { return Err; }

	FString PathNorm, PackagePath, AssetName;
	if (!INP_NormaliseDestPath(Request, PathRaw, PathNorm, PackagePath, AssetName, Err))
	{
		return Err;
	}

	// Optional value_type (default Boolean).
	EInputActionValueType ValueType = EInputActionValueType::Boolean;
	FString ValueTypeRaw;
	if (Request.Args->TryGetStringField(TEXT("value_type"), ValueTypeRaw) && !ValueTypeRaw.IsEmpty())
	{
		if (!INP_ParseValueType(Request, ValueTypeRaw, ValueType, Err)) { return Err; }
	}

	bool bConsumeInput = true;
	bool bReserveAllMappings = false;
	Request.Args->TryGetBoolField(TEXT("consume_input"),       bConsumeInput);
	Request.Args->TryGetBoolField(TEXT("reserve_all_mappings"), bReserveAllMappings);

	// Existing-asset short-circuit: if the package already exists, load + return current state with
	// created=false. Avoids accidental overwrite of designer-tuned IA assets.
	if (FPackageName::DoesPackageExist(PathNorm))
	{
		int32 LoadErrCode = 0;
		FString LoadErrMsg;
		UInputAction* Existing = FMCPAssetLoader::Load<UInputAction>(PathNorm, LoadErrCode, LoadErrMsg);
		if (!Existing)
		{
			return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg);
		}
		return FMCPJsonBuilder()
			.Str (TEXT("asset_path"),           Existing->GetPathName())
			.Str (TEXT("value_type"),           INP_ValueTypeToString(Existing->ValueType))
			.Bool(TEXT("consume_input"),        Existing->bConsumeInput)
			.Bool(TEXT("reserve_all_mappings"), Existing->bReserveAllMappings)
			.Bool(TEXT("created"),              false)
			.BuildSuccess(Request);
	}

	// Create fresh IA in a new package.
	UInputAction* IA = INP_CreateAssetInPackage<UInputAction>(PackagePath, AssetName, Scope);
	if (!IA)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInternal,
			FString::Printf(TEXT("CreatePackage + NewObject<UInputAction> failed for '%s'"), *PathNorm));
	}

	// Direct field assignment on a freshly-created object — no nested FMCPWritePropertyScope needed
	// (outer FMCPMutatorScope already owns the transaction, and there are no editor observers on a
	// brand-new asset yet). Mirrors the AnimTools::Tool_CreateMontage pattern (SetSkeleton, AddSlot
	// called directly without per-field Pre/Post). MarkPackageDirty is queued by Scope.DirtyPackage
	// inside INP_CreateAssetInPackage.
	IA->ValueType           = ValueType;
	IA->bConsumeInput       = bConsumeInput;
	IA->bReserveAllMappings = bReserveAllMappings;

	return FMCPJsonBuilder()
		.Str (TEXT("asset_path"),           IA->GetPathName())
		.Str (TEXT("value_type"),           INP_ValueTypeToString(IA->ValueType))
		.Bool(TEXT("consume_input"),        IA->bConsumeInput)
		.Bool(TEXT("reserve_all_mappings"), IA->bReserveAllMappings)
		.Bool(TEXT("created"),              true)
		.BuildSuccess(Request);
}

// ─── input.create_mapping_context ──────────────────────────────────────────────────────────────
//
// Args:    { path: string (required, /Game/.../IMC_Foo) }
// Result:  { asset_path, created, mapping_count }
//
// Empty IMC — no mappings, no triggers, no modifiers. Caller seeds via ``input.add_mapping``.
// Existing-asset short-circuit same as create_input_action.
//
// Lane A. PIE-guarded.
FMCPResponse Tool_CreateMappingContext(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_CreateMappingContext", "MCP: create Input Mapping Context"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString PathRaw;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("path"), PathRaw, Err)) { return Err; }

	FString PathNorm, PackagePath, AssetName;
	if (!INP_NormaliseDestPath(Request, PathRaw, PathNorm, PackagePath, AssetName, Err))
	{
		return Err;
	}

	if (FPackageName::DoesPackageExist(PathNorm))
	{
		int32 LoadErrCode = 0;
		FString LoadErrMsg;
		UInputMappingContext* Existing = FMCPAssetLoader::Load<UInputMappingContext>(PathNorm, LoadErrCode, LoadErrMsg);
		if (!Existing)
		{
			return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg);
		}
		return FMCPJsonBuilder()
			.Str (TEXT("asset_path"),    Existing->GetPathName())
			.Bool(TEXT("created"),       false)
			.Int (TEXT("mapping_count"), Existing->GetMappings().Num())
			.BuildSuccess(Request);
	}

	UInputMappingContext* IMC = INP_CreateAssetInPackage<UInputMappingContext>(PackagePath, AssetName, Scope);
	if (!IMC)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInternal,
			FString::Printf(TEXT("CreatePackage + NewObject<UInputMappingContext> failed for '%s'"), *PathNorm));
	}

	return FMCPJsonBuilder()
		.Str (TEXT("asset_path"),    IMC->GetPathName())
		.Bool(TEXT("created"),       true)
		.Int (TEXT("mapping_count"), 0)
		.BuildSuccess(Request);
}

// ─── input.add_mapping ─────────────────────────────────────────────────────────────────────────
//
// Args:    { imc_path: string (required),
//            ia_path:  string (required),
//            key:      string (required, FKey short name e.g. "SpaceBar" / "Gamepad_FaceButton_Bottom") }
// Result:  { imc_path, ia_path, key, mapping_index, total_mappings }
//
// Calls ``UIMC->MapKey(IA, FKey)``. Triggers + Modifiers default-empty (a Wave N+1 candidate adds
// trigger/modifier authoring). Returned mapping_index is ``Num()-1`` post-append.
//
// Lane A. PIE-guarded.
FMCPResponse Tool_AddMapping(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_AddMapping", "MCP: add IMC mapping"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString IMCPath, IAPath, KeyStr;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("imc_path"), IMCPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("ia_path"),  IAPath,  Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("key"),      KeyStr,  Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;

	UInputMappingContext* IMC = FMCPAssetLoader::Load<UInputMappingContext>(IMCPath, LoadErrCode, LoadErrMsg);
	if (!IMC) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UInputAction* IA = FMCPAssetLoader::Load<UInputAction>(IAPath, LoadErrCode, LoadErrMsg);
	if (!IA) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	// FKey parse — TCHAR ctor calls FName(InName). IsValid() checks the key details registry.
	const FKey ParsedKey(*KeyStr);
	if (!ParsedKey.IsValid())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
			FString::Printf(TEXT("key '%s' is not a recognised FKey name "
				"(examples: 'SpaceBar', 'LeftMouseButton', 'Gamepad_FaceButton_Bottom', 'A')"), *KeyStr));
	}

	// MapKey mutates DefaultKeyMappings — Modify() the IMC first so the transaction can roll back,
	// then call the IMC's authoring API. Mark the package dirty via Scope.
	IMC->Modify();
	IMC->MapKey(IA, ParsedKey);
	Scope.DirtyPackage(IMC->GetOutermost());

	const int32 TotalMappings = IMC->GetMappings().Num();
	const int32 MappingIndex  = TotalMappings - 1;

	return FMCPJsonBuilder()
		.Str(TEXT("imc_path"),       IMC->GetPathName())
		.Str(TEXT("ia_path"),        IA->GetPathName())
		.Str(TEXT("key"),            KeyStr)
		.Int(TEXT("mapping_index"),  MappingIndex)
		.Int(TEXT("total_mappings"), TotalMappings)
		.BuildSuccess(Request);
}

// ─── input.remove_mapping ──────────────────────────────────────────────────────────────────────
//
// Args:    { imc_path: string (required),
//            ia_path:  string (required),
//            key?:     string (optional; if omitted, removes ALL keys mapped to this IA) }
// Result:  { imc_path, ia_path, removed_count, total_mappings }
//
// - key provided: ``IMC->UnmapKey(IA, ParsedKey)``
// - key omitted:  ``IMC->UnmapAllKeysFromAction(IA)``
// removed_count = pre-count − post-count.
//
// Lane A. PIE-guarded.
FMCPResponse Tool_RemoveMapping(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FMCPMutatorScope Scope(Request, LOCTEXT("MCP_RemoveMapping", "MCP: remove IMC mapping"));
	if (Scope.PIEBlocked()) { return Scope.Error(); }

	FString IMCPath, IAPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("imc_path"), IMCPath, Err)) { return Err; }
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("ia_path"),  IAPath,  Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;

	UInputMappingContext* IMC = FMCPAssetLoader::Load<UInputMappingContext>(IMCPath, LoadErrCode, LoadErrMsg);
	if (!IMC) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	UInputAction* IA = FMCPAssetLoader::Load<UInputAction>(IAPath, LoadErrCode, LoadErrMsg);
	if (!IA) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	const int32 PreCount = IMC->GetMappings().Num();

	FString KeyStr;
	const bool bKeyProvided = Request.Args->TryGetStringField(TEXT("key"), KeyStr) && !KeyStr.IsEmpty();

	IMC->Modify();
	if (bKeyProvided)
	{
		const FKey ParsedKey(*KeyStr);
		if (!ParsedKey.IsValid())
		{
			return FMCPToolHelpers::MakeError(Request, kMCPErrorInvalidParams,
				FString::Printf(TEXT("key '%s' is not a recognised FKey name"), *KeyStr));
		}
		IMC->UnmapKey(IA, ParsedKey);
	}
	else
	{
		IMC->UnmapAllKeysFromAction(IA);
	}
	Scope.DirtyPackage(IMC->GetOutermost());

	const int32 PostCount    = IMC->GetMappings().Num();
	const int32 RemovedCount = FMath::Max(0, PreCount - PostCount);

	return FMCPJsonBuilder()
		.Str(TEXT("imc_path"),       IMC->GetPathName())
		.Str(TEXT("ia_path"),        IA->GetPathName())
		.Int(TEXT("removed_count"),  RemovedCount)
		.Int(TEXT("total_mappings"), PostCount)
		.BuildSuccess(Request);
}

// ─── input.add_context_to_player ───────────────────────────────────────────────────────────────
//
// Args:    { imc_path: string (required),
//            priority?: int (default 0),
//            player_index?: int (default 0) }
// Result:  { imc_path, priority, player_index, applied }
//
// RUNTIME-ONLY — requires an active PIE session (or standalone game) with a ULocalPlayer at the
// specified index. Inverse PIE gate compared to all other Wave N mutators: returns
// kMCPErrorOperationFailed (-32058) with explanatory message when PIE is NOT active.
//
// Workflow: PIE → resolve PlayerController via UGameplayStatics::GetPlayerController →
// ULocalPlayer → UEnhancedInputLocalPlayerSubsystem → AddMappingContext(IMC, Priority).
//
// Lane A. NO PIE guard — this is the one Wave N mutator that REQUIRES PIE/standalone.
FMCPResponse Tool_AddContextToPlayer(const FMCPRequest& Request)
{
	check(IsInGameThread());

	// Inverse PIE gate — this tool is meaningful ONLY in a live play session.
	if (!FMCPWorldContext::IsPIEActive())
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOperationFailed,
			TEXT("PIE not active; input.add_context_to_player requires a live play session "
				 "(start PIE via pie.start, or run in standalone)"));
	}

	FString IMCPath;
	FMCPResponse Err;
	if (!FMCPToolHelpers::RequireStringField(Request, TEXT("imc_path"), IMCPath, Err)) { return Err; }

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UInputMappingContext* IMC = FMCPAssetLoader::Load<UInputMappingContext>(IMCPath, LoadErrCode, LoadErrMsg);
	if (!IMC) { return FMCPToolHelpers::MakeError(Request, LoadErrCode, LoadErrMsg); }

	int32 Priority = 0;
	int32 PlayerIndex = 0;
	Request.Args->TryGetNumberField(TEXT("priority"),     Priority);
	Request.Args->TryGetNumberField(TEXT("player_index"), PlayerIndex);

	UWorld* World = (GEditor && GEditor->PlayWorld) ? GEditor->PlayWorld : nullptr;
	if (!World)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOperationFailed,
			TEXT("GEditor->PlayWorld is null despite IsPIEActive==true (race?); retry"));
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(World, PlayerIndex);
	if (!PC)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOperationFailed,
			FString::Printf(TEXT("no APlayerController at player_index %d in current PIE world"), PlayerIndex));
	}

	ULocalPlayer* LP = PC->GetLocalPlayer();
	if (!LP)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOperationFailed,
			TEXT("APlayerController has no ULocalPlayer (likely remote/standalone controller)"));
	}

	UEnhancedInputLocalPlayerSubsystem* EISS = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	if (!EISS)
	{
		return FMCPToolHelpers::MakeError(Request, kMCPErrorOperationFailed,
			TEXT("ULocalPlayer has no UEnhancedInputLocalPlayerSubsystem "
				 "(Enhanced Input plugin disabled or not initialised)"));
	}

	// FModifyContextOptions defaulted — caller can drive these via future tool extensions if needed.
	EISS->AddMappingContext(IMC, Priority);

	return FMCPJsonBuilder()
		.Str (TEXT("imc_path"),     IMC->GetPathName())
		.Int (TEXT("priority"),     Priority)
		.Int (TEXT("player_index"), PlayerIndex)
		.Bool(TEXT("applied"),      true)
		.BuildSuccess(Request);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Wave E S5 — read-only introspection (4 tools)
	RegisterTool(TEXT("input.list_mapping_contexts"), &Tool_ListMappingContexts, /*Lane A*/ false);
	RegisterTool(TEXT("input.list_input_actions"),    &Tool_ListInputActions,    /*Lane A*/ false);
	RegisterTool(TEXT("input.get_context_bindings"),  &Tool_GetContextBindings,  /*Lane A*/ false);
	RegisterTool(TEXT("input.list_player_contexts"),  &Tool_ListPlayerContexts,  /*Lane A*/ false);

	// Wave N — authoring (5 tools; N2.5 input.list_mappings dropped — duplicates get_context_bindings)
	RegisterTool(TEXT("input.create_input_action"),    &Tool_CreateInputAction,    /*Lane A*/ false);
	RegisterTool(TEXT("input.create_mapping_context"), &Tool_CreateMappingContext, /*Lane A*/ false);
	RegisterTool(TEXT("input.add_mapping"),            &Tool_AddMapping,           /*Lane A*/ false);
	RegisterTool(TEXT("input.remove_mapping"),         &Tool_RemoveMapping,        /*Lane A*/ false);
	RegisterTool(TEXT("input.add_context_to_player"),  &Tool_AddContextToPlayer,   /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Input surface registered: 9 input.* tools "
			 "(Wave E S5: list_mapping_contexts + list_input_actions + get_context_bindings + "
			 "list_player_contexts; Wave N: create_input_action + create_mapping_context + add_mapping "
			 "+ remove_mapping + add_context_to_player), all Lane A"));
}

} // namespace FInputTools

#undef LOCTEXT_NAMESPACE

MCP_REGISTER_SURFACE(InputTools, &FInputTools::Register)
