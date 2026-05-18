// Copyright FatumGame. All Rights Reserved.

#include "MaterialTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPAssetPathUtils.h"
#include "Utils/MCPMaterialUtils.h"
#include "Utils/MCPPageCursor.h"
#include "Utils/MCPReflection.h"
#include "Utils/MCPWorldContext.h"

#include "AssetToolsModule.h"
#include "Engine/Texture.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "ShaderCompiler.h"
#include "UObject/Package.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// MAT_ prefix per the unity-build symbol-collision pattern (MakeError/MakeSuccess clash with
	// UE's global ValueOrError templates).
	constexpr int32 kMATErrorInvalidParams = -32602;
	constexpr int32 kMATErrorInternal      = -32603;

	/**
	 * Shader compile queue soft cap (D8). Refuses ``material.set_static_switch`` writes when the
	 * pending queue is >= this many jobs. Default 1000 matches FShaderCompilingManager's default
	 * queue depth; tunable via CVar ``mcp.material.shader_queue_soft_limit`` (NYI — see plan D8
	 * future work).
	 */
	constexpr int32 kMATShaderQueueSoftLimit = 1000;

	void MAT_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse MAT_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		MAT_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse MAT_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		MAT_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	/** Frozen PIE-mutator refusal (per D11 — smoke asserts both "Phase 5" and "pie." substrings). */
	FMCPResponse MAT_MakePIEError(const FMCPRequest& Request)
	{
		return MAT_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	bool MAT_IsPIEActive()
	{
		return FMCPWorldContext::IsPIEActive();
	}

	// ─── arg parsing helpers ─────────────────────────────────────────────────────────────────────

	/** Read ``args.material_path`` field; emit -32602 InvalidParams on missing/empty. */
	bool MAT_RequireMaterialPath(const FMCPRequest& Request, FString& OutPath, FMCPResponse& OutError,
		const TCHAR* FieldName = TEXT("material_path"))
	{
		if (!Request.Args.IsValid())
		{
			OutError = MAT_MakeError(Request, kMATErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutPath) || OutPath.IsEmpty())
		{
			OutError = MAT_MakeError(Request, kMATErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/** Read ``args.parameter_name`` field; emit -32602 InvalidParams on missing/empty. */
	bool MAT_RequireParameterName(const FMCPRequest& Request, FName& OutName, FMCPResponse& OutError)
	{
		FString NameStr;
		if (!Request.Args.IsValid())
		{
			OutError = MAT_MakeError(Request, kMATErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(TEXT("parameter_name"), NameStr) || NameStr.IsEmpty())
		{
			OutError = MAT_MakeError(Request, kMATErrorInvalidParams,
				TEXT("missing required string field 'parameter_name'"));
			return false;
		}
		OutName = FName(*NameStr);
		return true;
	}

	int32 MAT_ClampPageSize(const TSharedPtr<FJsonObject>& Args, const TCHAR* FieldName, int32 Default)
	{
		int32 Out = Default;
		if (Args.IsValid())
		{
			Args->TryGetNumberField(FieldName, Out);
		}
		return FMath::Clamp(Out, 1, 1000);
	}

	bool MAT_DecodeCursor(
		const FMCPRequest& Request,
		const FString& TokenWire,
		uint64 ExpectedFilterHash,
		FMCPPageCursor& OutCursor,
		FMCPResponse& OutError)
	{
		FString DecodeErr;
		if (!FMCPPageCursorUtils::Decode(TokenWire, OutCursor, DecodeErr))
		{
			OutError = MAT_MakeError(Request, kMCPErrorStaleCursor,
				FString::Printf(TEXT("page_token decode failed: %s"), *DecodeErr));
			return false;
		}
		if (!FMCPPageCursorUtils::ValidateAgainstFilter(OutCursor, ExpectedFilterHash))
		{
			OutError = MAT_MakeError(Request, kMCPErrorStaleCursor,
				TEXT("page_token filter_hash mismatch — caller mutated filter (likely material_path) "
					 "between pages; restart pagination with page_token=null"));
			return false;
		}
		return true;
	}

	/**
	 * Filter-hash for paginated material.* tools. Material path is the dominant key; any change
	 * between pages → -32015 StaleCursor on next call.
	 */
	uint64 MAT_HashFilter(const FString& MaterialPath)
	{
		const uint32 H1 = GetTypeHash(MaterialPath);
		const uint32 H2 = 0u;
		return (static_cast<uint64>(H1) << 32) ^ H2;
	}

	// ─── parameter type strings (wire-canonical) ─────────────────────────────────────────────────

	const TCHAR* MAT_ParameterTypeScalar       = TEXT("scalar");
	const TCHAR* MAT_ParameterTypeVector       = TEXT("vector");
	const TCHAR* MAT_ParameterTypeTexture      = TEXT("texture");
	const TCHAR* MAT_ParameterTypeStaticSwitch = TEXT("static_switch");

	// ─── per-parameter JSON builders ─────────────────────────────────────────────────────────────

	/** {r,g,b,a} JSON serialiser for FLinearColor — matches FMCPReflection's canonical shape. */
	TSharedRef<FJsonObject> MAT_LinearColorToJson(const FLinearColor& C)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("_kind"), TEXT("LinearColor"));
		Obj->SetNumberField(TEXT("r"), C.R);
		Obj->SetNumberField(TEXT("g"), C.G);
		Obj->SetNumberField(TEXT("b"), C.B);
		Obj->SetNumberField(TEXT("a"), C.A);
		return Obj;
	}

	/** Parse FLinearColor from JSON — accepts {r,g,b,a} shorthand OR {_kind:"LinearColor",r,g,b,a}. */
	bool MAT_ReadJsonLinearColor(const TSharedPtr<FJsonObject>& Obj, FLinearColor& OutC)
	{
		if (!Obj.IsValid()) { return false; }
		double R = 0.0, G = 0.0, B = 0.0, A = 1.0;
		const bool bHasR = Obj->TryGetNumberField(TEXT("r"), R);
		const bool bHasG = Obj->TryGetNumberField(TEXT("g"), G);
		const bool bHasB = Obj->TryGetNumberField(TEXT("b"), B);
		Obj->TryGetNumberField(TEXT("a"), A); // alpha optional, defaults to 1
		if (!bHasR && !bHasG && !bHasB) { return false; }
		OutC = FLinearColor(static_cast<float>(R), static_cast<float>(G),
			static_cast<float>(B), static_cast<float>(A));
		return true;
	}

	TSharedRef<FJsonObject> MAT_BuildScalarParamJson(
		UMaterialInterface* Material,
		const FName& Name)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name.ToString());

		// Walk to base UMaterial for the DEFAULT (parent-chain root).
		UMaterial* Base = FMCPMaterialUtils::WalkToBaseMaterial(Material);
		const float DefaultValue = Base
			? UMaterialEditingLibrary::GetMaterialDefaultScalarParameterValue(Base, Name)
			: 0.0f;
		Obj->SetNumberField(TEXT("default"), DefaultValue);

		// Current value — for MIC, the override; for base UMaterial, the same default. We must
		// reflect whatever the asset would resolve to via ``UMaterialInterface::GetScalarParameterValue``.
		float CurrentValue = DefaultValue;
		Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), CurrentValue);
		Obj->SetNumberField(TEXT("value"), CurrentValue);

		Obj->SetStringField(TEXT("group"), TEXT("")); // see header note on group field (D5/R5)
		return Obj;
	}

	TSharedRef<FJsonObject> MAT_BuildVectorParamJson(
		UMaterialInterface* Material,
		const FName& Name)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name.ToString());

		UMaterial* Base = FMCPMaterialUtils::WalkToBaseMaterial(Material);
		const FLinearColor DefaultValue = Base
			? UMaterialEditingLibrary::GetMaterialDefaultVectorParameterValue(Base, Name)
			: FLinearColor::Black;
		Obj->SetObjectField(TEXT("default"), MAT_LinearColorToJson(DefaultValue));

		FLinearColor CurrentValue = DefaultValue;
		Material->GetVectorParameterValue(FHashedMaterialParameterInfo(Name), CurrentValue);
		Obj->SetObjectField(TEXT("value"), MAT_LinearColorToJson(CurrentValue));

		Obj->SetStringField(TEXT("group"), TEXT(""));
		return Obj;
	}

	TSharedRef<FJsonObject> MAT_BuildTextureParamJson(
		UMaterialInterface* Material,
		const FName& Name)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name.ToString());

		UMaterial* Base = FMCPMaterialUtils::WalkToBaseMaterial(Material);
		UTexture* DefaultTex = Base
			? UMaterialEditingLibrary::GetMaterialDefaultTextureParameterValue(Base, Name)
			: nullptr;
		Obj->SetStringField(TEXT("default_path"),
			DefaultTex ? DefaultTex->GetPathName() : FString());

		UTexture* CurrentTex = nullptr;
		Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Name), CurrentTex);
		if (!CurrentTex)
		{
			CurrentTex = DefaultTex;
		}
		Obj->SetStringField(TEXT("value_path"),
			CurrentTex ? CurrentTex->GetPathName() : FString());

		Obj->SetStringField(TEXT("group"), TEXT(""));
		return Obj;
	}

	TSharedRef<FJsonObject> MAT_BuildStaticSwitchParamJson(
		UMaterialInterface* Material,
		const FName& Name)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name.ToString());

		UMaterial* Base = FMCPMaterialUtils::WalkToBaseMaterial(Material);
		const bool bDefaultValue = Base
			? UMaterialEditingLibrary::GetMaterialDefaultStaticSwitchParameterValue(Base, Name)
			: false;
		Obj->SetBoolField(TEXT("default"), bDefaultValue);

		// Current value: GetStaticSwitchParameterValue takes (info, out, guid). The guid out param
		// is the parameter's expression guid (we don't surface it).
		bool bCurrentValue = bDefaultValue;
		FGuid Unused;
		Material->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(Name), bCurrentValue, Unused);
		Obj->SetBoolField(TEXT("value"), bCurrentValue);

		return Obj;
	}

	/**
	 * Build the FULL parameter inventory of a material. Used by material.list_parameters AND as
	 * the source-of-truth for material.get_parameter's auto-detect 4-way search. Pure read — no
	 * mutations.
	 *
	 * Each category is sorted lexicographically by parameter name so pagination cursors are stable
	 * across calls.
	 */
	void MAT_CollectAllParameters(
		UMaterialInterface* Material,
		TArray<FName>& OutScalarNames,
		TArray<FName>& OutVectorNames,
		TArray<FName>& OutTextureNames,
		TArray<FName>& OutStaticSwitchNames)
	{
		check(Material);
		UMaterialEditingLibrary::GetScalarParameterNames(Material, OutScalarNames);
		UMaterialEditingLibrary::GetVectorParameterNames(Material, OutVectorNames);
		UMaterialEditingLibrary::GetTextureParameterNames(Material, OutTextureNames);
		UMaterialEditingLibrary::GetStaticSwitchParameterNames(Material, OutStaticSwitchNames);

		// Stable sort (lex by FName string) for pagination cursor stability.
		auto SortFn = [](const FName& A, const FName& B)
		{
			return A.LexicalLess(B);
		};
		OutScalarNames.Sort(SortFn);
		OutVectorNames.Sort(SortFn);
		OutTextureNames.Sort(SortFn);
		OutStaticSwitchNames.Sort(SortFn);
	}
} // namespace

namespace FMaterialTools
{

// ─── material.list_parameters (Lane A, no PIE guard) ──────────────────────────────────────────
//
// Args:    { material_path, page_token?: string, page_size?: int=100 }
// Result:  {
//            parameters: {
//              scalar:        [{name, default, value, group}, ...],
//              vector:        [{name, default, value, group}, ...],
//              texture:       [{name, default_path, value_path, group}, ...],
//              static_switch: [{name, default, value}, ...]
//            },
//            source_class: "/Script/Engine.MaterialInstanceConstant",
//            next_page_token?: string,
//            total_known: int
//          }
//
// Errors:
//   -32010 InvalidPath  / -32004 ObjectNotFound  / -32034 MaterialClassMismatch (not UMaterialInterface)
//   -32015 StaleCursor (filter changed mid-pagination)
//
// Pagination: sentinel cursor over the FLATTENED list (scalar then vector then texture then
// switch), sorted lexicographically WITHIN each category. The cursor key is the parameter name
// regardless of category — first page returns up to page_size params from the start of the union;
// next page resumes after the last seen name within its category. Cursor's filter hash is the
// material_path so swapping mid-pagination → -32015.
FMCPResponse Tool_ListParameters(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!MAT_RequireMaterialPath(Request, Path, PathErr)) { return PathErr; }

	int32 ErrCode = 0;
	FString ErrMsg;
	UMaterialInterface* Material = FMCPMaterialUtils::LoadMaterialInterfaceByPath(Path, ErrCode, ErrMsg);
	if (!Material) { return MAT_MakeError(Request, ErrCode, ErrMsg); }

	TArray<FName> ScalarNames, VectorNames, TextureNames, StaticSwitchNames;
	MAT_CollectAllParameters(Material, ScalarNames, VectorNames, TextureNames, StaticSwitchNames);

	const int32 TotalKnown = ScalarNames.Num() + VectorNames.Num() + TextureNames.Num()
		+ StaticSwitchNames.Num();

	// Cursor + pagination decode.
	FString PageTokenWire;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("page_token"), PageTokenWire);
	}
	const uint64 FilterHash = MAT_HashFilter(Path);

	FMCPPageCursor Cursor;
	if (!MAT_DecodeCursor(Request, PageTokenWire, FilterHash, Cursor, PathErr))
	{
		return PathErr;
	}

	const int32 PageSize = MAT_ClampPageSize(Request.Args, TEXT("page_size"), 100);
	const FString& LastName = Cursor.LastAssetPath; // last param name seen on prior page

	// Build per-category JSON arrays, splicing in respect of the cursor + page_size budget.
	// We treat the categories as a sequence: scalar → vector → texture → static_switch. For each
	// category in order: if the cursor is empty (first page) take from the start; else skip until
	// after LastName (lex compare within that category). Once page_size is exhausted, stop and
	// emit a cursor pointing at the last emitted name.
	TArray<TSharedPtr<FJsonValue>> ScalarArr, VectorArr, TextureArr, StaticSwitchArr;
	int32 Budget = PageSize;
	// EmittedCursorKey encodes BOTH category index (0..3) AND the last emitted name within it.
	// On the next page we resume by skipping past the matching category's emitted name AND
	// skipping any earlier categories entirely (already drained on prior pages).
	// Encoding: "<category_idx>|<name>". Empty = first page.
	FString EmittedCursorKey = LastName;
	bool bAnyEmittedThisPage = false;
	int32 RemainingAfterPage = 0;

	// Decode the cursor's category index + tail name. Empty cursor → start from category 0 with
	// no tail-skip.
	int32 ResumeCategoryIdx = 0;
	FString ResumeTailName;
	if (!LastName.IsEmpty())
	{
		int32 PipeIdx = INDEX_NONE;
		if (LastName.FindChar(TEXT('|'), PipeIdx) && PipeIdx > 0)
		{
			ResumeCategoryIdx = FCString::Atoi(*LastName.Left(PipeIdx));
			ResumeTailName    = LastName.Mid(PipeIdx + 1);
		}
		else
		{
			// Backwards-compat / forgiving decode: assume scalar (idx 0) if no pipe.
			ResumeCategoryIdx = 0;
			ResumeTailName    = LastName;
		}
	}

	auto EmitFromCategory = [&](
		int32 ThisCategoryIdx,
		const TArray<FName>& Names,
		TArray<TSharedPtr<FJsonValue>>& OutArr,
		TFunctionRef<TSharedRef<FJsonObject>(UMaterialInterface*, const FName&)> BuildFn)
	{
		// Category was drained on a prior page — skip entirely.
		if (ThisCategoryIdx < ResumeCategoryIdx) { return; }

		// In the resume category: skip every name <= ResumeTailName. Past the resume category:
		// take from the start (no tail filter).
		const bool bApplyTailFilter = (ThisCategoryIdx == ResumeCategoryIdx) && !ResumeTailName.IsEmpty();

		for (const FName& N : Names)
		{
			const FString NameStr = N.ToString();
			if (bApplyTailFilter && NameStr.Compare(ResumeTailName, ESearchCase::IgnoreCase) <= 0)
			{
				continue;
			}
			if (Budget <= 0)
			{
				++RemainingAfterPage;
				continue;
			}
			OutArr.Add(MakeShared<FJsonValueObject>(BuildFn(Material, N)));
			// Build the cursor key for this emission.
			EmittedCursorKey = FString::Printf(TEXT("%d|%s"), ThisCategoryIdx, *NameStr);
			bAnyEmittedThisPage = true;
			--Budget;
		}
	};

	EmitFromCategory(0, ScalarNames,       ScalarArr,       MAT_BuildScalarParamJson);
	EmitFromCategory(1, VectorNames,       VectorArr,       MAT_BuildVectorParamJson);
	EmitFromCategory(2, TextureNames,      TextureArr,      MAT_BuildTextureParamJson);
	EmitFromCategory(3, StaticSwitchNames, StaticSwitchArr, MAT_BuildStaticSwitchParamJson);

	TSharedRef<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
	ParamsObj->SetArrayField(TEXT("scalar"),        ScalarArr);
	ParamsObj->SetArrayField(TEXT("vector"),        VectorArr);
	ParamsObj->SetArrayField(TEXT("texture"),       TextureArr);
	ParamsObj->SetArrayField(TEXT("static_switch"), StaticSwitchArr);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("parameters"), ParamsObj);
	Out->SetStringField(TEXT("source_class"), Material->GetClass()->GetPathName());
	Out->SetNumberField(TEXT("total_known"), static_cast<double>(TotalKnown));

	// Emit next_page_token iff there are params past the last emitted entry. RemainingAfterPage
	// counts entries skipped because Budget hit zero — non-zero means there's at least one more
	// page worth of data. EmittedCursorKey is the "<cat_idx>|<name>" encoding the next-page
	// resume gate consumes.
	if (bAnyEmittedThisPage && RemainingAfterPage > 0)
	{
		FMCPPageCursor NextCursor;
		NextCursor.FilterHash         = FilterHash;
		NextCursor.LastAssetPath      = EmittedCursorKey;
		NextCursor.TotalKnownSnapshot = TotalKnown;
		Out->SetStringField(TEXT("next_page_token"), FMCPPageCursorUtils::Encode(NextCursor));
	}

	return MAT_MakeSuccessObj(Request, Out);
}

// ─── material.get_parameter (Lane A, no PIE guard) ────────────────────────────────────────────
//
// Args:    { material_path, parameter_name, parameter_type?: "scalar"|"vector"|"texture"|"static_switch" }
// Result:  { found: bool, type: string, value: <typed>, default: <typed>, group: string }
//
// If parameter_type specified: try ONLY that category. If omitted: auto-detect via 4-way search
// (scalar → vector → texture → static_switch — first match wins). -32036 ParameterNotFound if
// none of the searched categories contain the name.
//
// Errors:
//   -32010 / -32004 / -32034 (resolve)
//   -32036 ParameterNotFound
//   -32602 InvalidParams (bad parameter_type string)
FMCPResponse Tool_GetParameter(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!MAT_RequireMaterialPath(Request, Path, PathErr)) { return PathErr; }

	FName ParamName;
	if (!MAT_RequireParameterName(Request, ParamName, PathErr)) { return PathErr; }

	int32 ErrCode = 0;
	FString ErrMsg;
	UMaterialInterface* Material = FMCPMaterialUtils::LoadMaterialInterfaceByPath(Path, ErrCode, ErrMsg);
	if (!Material) { return MAT_MakeError(Request, ErrCode, ErrMsg); }

	FString ExplicitType;
	if (Request.Args.IsValid())
	{
		Request.Args->TryGetStringField(TEXT("parameter_type"), ExplicitType);
	}

	// Local helpers — return populated success response if the param exists in that category.
	auto TryScalar = [&]() -> bool
	{
		float Value = 0.0f;
		if (!Material->GetScalarParameterValue(FHashedMaterialParameterInfo(ParamName), Value))
		{
			return false;
		}
		UMaterial* Base = FMCPMaterialUtils::WalkToBaseMaterial(Material);
		const float Default = Base
			? UMaterialEditingLibrary::GetMaterialDefaultScalarParameterValue(Base, ParamName)
			: 0.0f;
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("found"),   true);
		Out->SetStringField(TEXT("type"),  MAT_ParameterTypeScalar);
		Out->SetNumberField(TEXT("value"), Value);
		Out->SetNumberField(TEXT("default"), Default);
		Out->SetStringField(TEXT("group"), TEXT(""));
		PathErr = MAT_MakeSuccessObj(Request, Out);
		return true;
	};

	auto TryVector = [&]() -> bool
	{
		FLinearColor Value = FLinearColor::Black;
		if (!Material->GetVectorParameterValue(FHashedMaterialParameterInfo(ParamName), Value))
		{
			return false;
		}
		UMaterial* Base = FMCPMaterialUtils::WalkToBaseMaterial(Material);
		const FLinearColor Default = Base
			? UMaterialEditingLibrary::GetMaterialDefaultVectorParameterValue(Base, ParamName)
			: FLinearColor::Black;
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("found"),  true);
		Out->SetStringField(TEXT("type"), MAT_ParameterTypeVector);
		Out->SetObjectField(TEXT("value"),   MAT_LinearColorToJson(Value));
		Out->SetObjectField(TEXT("default"), MAT_LinearColorToJson(Default));
		Out->SetStringField(TEXT("group"), TEXT(""));
		PathErr = MAT_MakeSuccessObj(Request, Out);
		return true;
	};

	auto TryTexture = [&]() -> bool
	{
		UTexture* Value = nullptr;
		if (!Material->GetTextureParameterValue(FHashedMaterialParameterInfo(ParamName), Value))
		{
			return false;
		}
		UMaterial* Base = FMCPMaterialUtils::WalkToBaseMaterial(Material);
		UTexture* Default = Base
			? UMaterialEditingLibrary::GetMaterialDefaultTextureParameterValue(Base, ParamName)
			: nullptr;
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("found"),   true);
		Out->SetStringField(TEXT("type"),  MAT_ParameterTypeTexture);
		Out->SetStringField(TEXT("value"),   Value   ? Value->GetPathName()   : FString());
		Out->SetStringField(TEXT("default"), Default ? Default->GetPathName() : FString());
		Out->SetStringField(TEXT("group"), TEXT(""));
		PathErr = MAT_MakeSuccessObj(Request, Out);
		return true;
	};

	auto TryStaticSwitch = [&]() -> bool
	{
		bool bValue = false;
		FGuid Unused;
		if (!Material->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(ParamName), bValue, Unused))
		{
			return false;
		}
		UMaterial* Base = FMCPMaterialUtils::WalkToBaseMaterial(Material);
		const bool bDefault = Base
			? UMaterialEditingLibrary::GetMaterialDefaultStaticSwitchParameterValue(Base, ParamName)
			: false;
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("found"),  true);
		Out->SetStringField(TEXT("type"), MAT_ParameterTypeStaticSwitch);
		Out->SetBoolField(TEXT("value"),   bValue);
		Out->SetBoolField(TEXT("default"), bDefault);
		Out->SetStringField(TEXT("group"), TEXT(""));
		PathErr = MAT_MakeSuccessObj(Request, Out);
		return true;
	};

	// Type dispatch.
	if (!ExplicitType.IsEmpty())
	{
		if (ExplicitType == MAT_ParameterTypeScalar)
		{
			if (TryScalar()) { return PathErr; }
		}
		else if (ExplicitType == MAT_ParameterTypeVector)
		{
			if (TryVector()) { return PathErr; }
		}
		else if (ExplicitType == MAT_ParameterTypeTexture)
		{
			if (TryTexture()) { return PathErr; }
		}
		else if (ExplicitType == MAT_ParameterTypeStaticSwitch)
		{
			if (TryStaticSwitch()) { return PathErr; }
		}
		else
		{
			return MAT_MakeError(Request, kMATErrorInvalidParams,
				FString::Printf(
					TEXT("parameter_type '%s' invalid; expected one of 'scalar' / 'vector' / 'texture' / 'static_switch'"),
					*ExplicitType));
		}
	}
	else
	{
		// Auto-detect: try every category in sequence.
		if (TryScalar())       { return PathErr; }
		if (TryVector())       { return PathErr; }
		if (TryTexture())      { return PathErr; }
		if (TryStaticSwitch()) { return PathErr; }
	}

	return MAT_MakeError(Request, kMCPErrorParameterNotFound,
		FString::Printf(
			TEXT("parameter '%s' not found on '%s'; check material.list_parameters"),
			*ParamName.ToString(), *Path));
}

// ─── shared MIC write scaffolding ─────────────────────────────────────────────────────────────

/**
 * Pre-write scaffold shared by set_scalar / set_vector / set_texture / set_static_switch:
 *   1. PIE guard          → -32027
 *   2. material_path required → -32602
 *   3. parameter_name required → -32602
 *   4. Resolve MIC        → -32010 / -32004 / -32034
 *   5. Verify parameter exists on this material chain → -32036
 *
 * Returns true on success and populates OutMIC + OutParamName. On false populates OutError and
 * the caller returns it.
 */
bool MAT_PreWriteResolve(
	const FMCPRequest& Request,
	UMaterialInstanceConstant*& OutMIC,
	FName& OutParamName,
	FString& OutPath,
	FMCPResponse& OutError)
{
	if (MAT_IsPIEActive())
	{
		OutError = MAT_MakePIEError(Request);
		return false;
	}
	if (!MAT_RequireMaterialPath(Request, OutPath, OutError)) { return false; }
	if (!MAT_RequireParameterName(Request, OutParamName, OutError)) { return false; }

	int32 ErrCode = 0;
	FString ErrMsg;
	OutMIC = FMCPMaterialUtils::LoadMICByPath(OutPath, ErrCode, ErrMsg);
	if (!OutMIC)
	{
		OutError = MAT_MakeError(Request, ErrCode, ErrMsg);
		return false;
	}
	return true;
}

/**
 * Locate the FProperty backing one of the MIC's parameter-override arrays so we can run the
 * edit-const 3-flag gate and own PreEditChange/Modify/Post via FMCPWritePropertyScope.
 *
 * UE 5.7 layout (Materials/MaterialInstance.h):
 *   - UMaterialInstance::ScalarParameterValues     → ScalarParameterValues (TArray<FScalarParameterValue>)
 *   - UMaterialInstance::VectorParameterValues     → VectorParameterValues
 *   - UMaterialInstance::TextureParameterValues    → TextureParameterValues
 *   - UMaterialInstance::StaticParameters          → StaticParameters (FStaticParameterSet)
 *
 * Returns nullptr if the property lookup somehow fails (engine version skew); caller treats as
 * -32603 internal error. NOT expected in practice — these field names have been stable for years.
 */
FProperty* MAT_FindOverrideProperty(UMaterialInstanceConstant* MIC, const TCHAR* FieldName)
{
	check(MIC);
	return MIC->GetClass()->FindPropertyByName(FName(FieldName));
}

/** Edit-const 3-flag gate (Phase 3 D8). Returns true if the property is writable. */
bool MAT_PassEditConstGate(FProperty* Prop, bool bBypassReadonly)
{
	if (bBypassReadonly) { return true; }
	if (!Prop) { return true; } // can't gate what we can't find; let downstream error report
	constexpr EPropertyFlags kBlock =
		CPF_EditConst | CPF_BlueprintReadOnly | CPF_DisableEditOnInstance;
	return !Prop->HasAnyPropertyFlags(kBlock);
}

bool MAT_GetBypassReadonly(const TSharedPtr<FJsonObject>& Args)
{
	bool b = false;
	if (Args.IsValid())
	{
		Args->TryGetBoolField(TEXT("bypass_readonly"), b);
	}
	return b;
}

// ─── material.set_scalar_param (Lane A, PIE-guarded, MIC-only) ────────────────────────────────
//
// Args:    { material_path, parameter_name, value: float, bypass_readonly?: bool=false }
// Result:  { applied: bool, prior_value: float|null }
//
// prior_value is null when the parameter was inherited (no override existed on the MIC); the
// number when there WAS a prior override. We detect "had override" by reading the value BEFORE
// the write through UMaterialEditingLibrary's MIC-specific getter (which returns the override
// value if present, else the parent's resolved value — we can't disambiguate via that alone, but
// we treat "the inherited value matches what's about to be written" as a no-op caveat in result
// by emitting the prior as a number every time the parameter resolves on the MIC — and null only
// if it doesn't resolve at all, which would already be -32036 ParameterNotFound).
//
// In short: prior_value is the numerical value the MIC would have returned for that parameter
// BEFORE the write call, whether it came from an override or inherited from parent. AI tooling
// gets "the value about to change". Null is unused today; reserved for a future refinement that
// queries FMaterialParameterInfo::bOverride.
//
// Errors:
//   -32027 PIEActive
//   -32010 / -32004 / -32034 (resolve)
//   -32036 ParameterNotFound
//   -32007 PropertyAccessDenied (edit-const gate)
//   -32602 InvalidParams (missing/non-number 'value')
FMCPResponse Tool_SetScalarParam(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UMaterialInstanceConstant* MIC = nullptr;
	FName ParamName;
	FString Path;
	FMCPResponse Err;
	if (!MAT_PreWriteResolve(Request, MIC, ParamName, Path, Err)) { return Err; }

	double Value = 0.0;
	if (!Request.Args->TryGetNumberField(TEXT("value"), Value))
	{
		return MAT_MakeError(Request, kMATErrorInvalidParams,
			TEXT("missing required number field 'value'"));
	}
	const float ValueF = static_cast<float>(Value);

	// Pre-read to capture prior. Returns false if the parameter doesn't exist on the parent chain.
	float PriorValue = 0.0f;
	if (!MIC->GetScalarParameterValue(FHashedMaterialParameterInfo(ParamName), PriorValue))
	{
		return MAT_MakeError(Request, kMCPErrorParameterNotFound,
			FString::Printf(
				TEXT("scalar parameter '%s' not found on '%s'; check material.list_parameters"),
				*ParamName.ToString(), *Path));
	}

	// Edit-const gate on the override-array property.
	FProperty* OverrideProp = MAT_FindOverrideProperty(MIC, TEXT("ScalarParameterValues"));
	if (!MAT_PassEditConstGate(OverrideProp, MAT_GetBypassReadonly(Request.Args)))
	{
		return MAT_MakeError(Request, kMCPErrorPropertyAccessDenied,
			FString::Printf(
				TEXT("ScalarParameterValues blocked by CPF_EditConst/BlueprintReadOnly/DisableEditOnInstance ")
				TEXT("for '%s'; pass args.bypass_readonly=true to override"),
				*Path));
	}

	// RAII scope owns PreEditChange / Modify / FScopedTransaction / PostEditChangeProperty.
	{
		FMCPWritePropertyScope Scope(MIC, OverrideProp,
			LOCTEXT("MCPSetScalarParam", "MCP: set scalar param"));
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, ParamName, ValueF);
	}

	MIC->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("applied"), true);
	Out->SetNumberField(TEXT("prior_value"), PriorValue);
	return MAT_MakeSuccessObj(Request, Out);
}

// ─── material.set_vector_param (Lane A, PIE-guarded, MIC-only) ────────────────────────────────
//
// Args:    { material_path, parameter_name, value: {r,g,b,a}, bypass_readonly? }
// Result:  { applied: bool, prior_value: {r,g,b,a} }
//
// Same shape as scalar; the value field is FLinearColor JSON.
FMCPResponse Tool_SetVectorParam(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UMaterialInstanceConstant* MIC = nullptr;
	FName ParamName;
	FString Path;
	FMCPResponse Err;
	if (!MAT_PreWriteResolve(Request, MIC, ParamName, Path, Err)) { return Err; }

	const TSharedPtr<FJsonObject>* ValueObj = nullptr;
	if (!Request.Args->TryGetObjectField(TEXT("value"), ValueObj) || !ValueObj || !ValueObj->IsValid())
	{
		return MAT_MakeError(Request, kMATErrorInvalidParams,
			TEXT("missing required object field 'value' (expected {r,g,b,a})"));
	}
	FLinearColor Value;
	if (!MAT_ReadJsonLinearColor(*ValueObj, Value))
	{
		return MAT_MakeError(Request, kMATErrorInvalidParams,
			TEXT("'value' object lacks any of r/g/b numeric fields"));
	}

	FLinearColor PriorValue = FLinearColor::Black;
	if (!MIC->GetVectorParameterValue(FHashedMaterialParameterInfo(ParamName), PriorValue))
	{
		return MAT_MakeError(Request, kMCPErrorParameterNotFound,
			FString::Printf(
				TEXT("vector parameter '%s' not found on '%s'; check material.list_parameters"),
				*ParamName.ToString(), *Path));
	}

	FProperty* OverrideProp = MAT_FindOverrideProperty(MIC, TEXT("VectorParameterValues"));
	if (!MAT_PassEditConstGate(OverrideProp, MAT_GetBypassReadonly(Request.Args)))
	{
		return MAT_MakeError(Request, kMCPErrorPropertyAccessDenied,
			FString::Printf(
				TEXT("VectorParameterValues blocked by CPF_EditConst/BlueprintReadOnly/DisableEditOnInstance ")
				TEXT("for '%s'; pass args.bypass_readonly=true to override"),
				*Path));
	}

	{
		FMCPWritePropertyScope Scope(MIC, OverrideProp,
			LOCTEXT("MCPSetVectorParam", "MCP: set vector param"));
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MIC, ParamName, Value);
	}

	MIC->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("applied"), true);
	Out->SetObjectField(TEXT("prior_value"), MAT_LinearColorToJson(PriorValue));
	return MAT_MakeSuccessObj(Request, Out);
}

// ─── material.set_texture_param (Lane A, PIE-guarded, MIC-only) ───────────────────────────────
//
// Args:    { material_path, parameter_name, texture_path: string, bypass_readonly? }
// Result:  { applied: bool, prior_value: string }
//
// Errors: same as scalar PLUS -32011 WrongClass when texture_path resolves to non-UTexture.
FMCPResponse Tool_SetTextureParam(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UMaterialInstanceConstant* MIC = nullptr;
	FName ParamName;
	FString Path;
	FMCPResponse Err;
	if (!MAT_PreWriteResolve(Request, MIC, ParamName, Path, Err)) { return Err; }

	FString TexPathRaw;
	if (!Request.Args->TryGetStringField(TEXT("texture_path"), TexPathRaw) || TexPathRaw.IsEmpty())
	{
		return MAT_MakeError(Request, kMATErrorInvalidParams,
			TEXT("missing required string field 'texture_path'"));
	}

	// Normalise + resolve. We accept both /Game/Foo/T_X and /Game/Foo/T_X.T_X object paths.
	const FString TexPath = FMCPAssetPathUtils::Normalize(TexPathRaw);
	if (TexPath.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(TexPath))
	{
		return MAT_MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("texture_path '%s' is malformed or references an unknown mount point"),
				*TexPathRaw));
	}

	UObject* Loaded = LoadObject<UObject>(nullptr, *TexPath);
	if (!Loaded)
	{
		const FString ObjectPath = FMCPAssetPathUtils::ToObjectPath(TexPath);
		if (!ObjectPath.IsEmpty() && ObjectPath != TexPath)
		{
			Loaded = LoadObject<UObject>(nullptr, *ObjectPath);
		}
	}
	if (!Loaded)
	{
		return MAT_MakeError(Request, kMCPErrorObjectNotFound,
			FString::Printf(TEXT("texture_path '%s' could not be loaded (no asset found)"),
				*TexPathRaw));
	}
	UTexture* Texture = Cast<UTexture>(Loaded);
	if (!Texture)
	{
		return MAT_MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(
				TEXT("texture_path '%s' is class '%s', not UTexture (any subclass — Texture2D / TextureCube / etc.)"),
				*TexPathRaw, *Loaded->GetClass()->GetPathName()));
	}

	UTexture* PriorTexture = nullptr;
	if (!MIC->GetTextureParameterValue(FHashedMaterialParameterInfo(ParamName), PriorTexture))
	{
		return MAT_MakeError(Request, kMCPErrorParameterNotFound,
			FString::Printf(
				TEXT("texture parameter '%s' not found on '%s'; check material.list_parameters"),
				*ParamName.ToString(), *Path));
	}

	FProperty* OverrideProp = MAT_FindOverrideProperty(MIC, TEXT("TextureParameterValues"));
	if (!MAT_PassEditConstGate(OverrideProp, MAT_GetBypassReadonly(Request.Args)))
	{
		return MAT_MakeError(Request, kMCPErrorPropertyAccessDenied,
			FString::Printf(
				TEXT("TextureParameterValues blocked by CPF_EditConst/BlueprintReadOnly/DisableEditOnInstance ")
				TEXT("for '%s'; pass args.bypass_readonly=true to override"),
				*Path));
	}

	{
		FMCPWritePropertyScope Scope(MIC, OverrideProp,
			LOCTEXT("MCPSetTextureParam", "MCP: set texture param"));
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MIC, ParamName, Texture);
	}

	MIC->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("applied"), true);
	Out->SetStringField(TEXT("prior_value"),
		PriorTexture ? PriorTexture->GetPathName() : FString());
	return MAT_MakeSuccessObj(Request, Out);
}

// ─── material.set_static_switch (Lane A, PIE-guarded, MIC-only, backpressure) ─────────────────
//
// Args:    { material_path, parameter_name, value: bool, bypass_readonly? }
// Result:  {
//            applied: bool,
//            prior_value: bool,
//            recompile_triggered: true,
//            recompile_already_pending: bool   // queue had work BEFORE this write
//          }
//
// Errors: same as scalar PLUS -32035 ShaderRecompilePending when the shader queue is saturated.
//
// Triggers an asynchronous shader recompile. Backpressure: if GShaderCompilingManager's pending
// job count is >= kMATShaderQueueSoftLimit, the write is refused with -32035 (caller should poll
// material.is_shader_compiling and retry).
FMCPResponse Tool_SetStaticSwitch(const FMCPRequest& Request)
{
	check(IsInGameThread());

	UMaterialInstanceConstant* MIC = nullptr;
	FName ParamName;
	FString Path;
	FMCPResponse Err;
	if (!MAT_PreWriteResolve(Request, MIC, ParamName, Path, Err)) { return Err; }

	bool bValue = false;
	if (!Request.Args->TryGetBoolField(TEXT("value"), bValue))
	{
		return MAT_MakeError(Request, kMATErrorInvalidParams,
			TEXT("missing required boolean field 'value'"));
	}

	// Backpressure (D8) — refuse if the shader queue is already saturated.
	int32 PendingJobs = 0;
	if (GShaderCompilingManager)
	{
		PendingJobs = GShaderCompilingManager->GetNumRemainingJobs();
		if (PendingJobs >= kMATShaderQueueSoftLimit)
		{
			return MAT_MakeError(Request, kMCPErrorShaderRecompilePending,
				FString::Printf(
					TEXT("shader compile queue at %d jobs (>= soft limit %d); retry after `material.is_shader_compiling` reports compiling=false"),
					PendingJobs, kMATShaderQueueSoftLimit));
		}
	}
	const bool bRecompileAlreadyPending = (PendingJobs > 0);

	bool bPriorValue = false;
	FGuid Unused;
	if (!MIC->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(ParamName), bPriorValue, Unused))
	{
		return MAT_MakeError(Request, kMCPErrorParameterNotFound,
			FString::Printf(
				TEXT("static switch parameter '%s' not found on '%s'; check material.list_parameters"),
				*ParamName.ToString(), *Path));
	}

	FProperty* OverrideProp = MAT_FindOverrideProperty(MIC, TEXT("StaticParameters"));
	if (!MAT_PassEditConstGate(OverrideProp, MAT_GetBypassReadonly(Request.Args)))
	{
		return MAT_MakeError(Request, kMCPErrorPropertyAccessDenied,
			FString::Printf(
				TEXT("StaticParameters blocked by CPF_EditConst/BlueprintReadOnly/DisableEditOnInstance ")
				TEXT("for '%s'; pass args.bypass_readonly=true to override"),
				*Path));
	}

	{
		FMCPWritePropertyScope Scope(MIC, OverrideProp,
			LOCTEXT("MCPSetStaticSwitch", "MCP: set static switch param"));
		// 4th arg = EMaterialParameterAssociation::GlobalParameter (default), 5th = bUpdateMaterialInstance.
		// We pass bUpdateMaterialInstance=true so MIC->UpdateStaticPermutation runs immediately and
		// triggers the shader recompile pipeline (otherwise the static switch change wouldn't take
		// effect until something else touched the MIC).
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(
			MIC, ParamName, bValue,
			EMaterialParameterAssociation::GlobalParameter, /*bUpdateMaterialInstance*/ true);
	}

	MIC->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("applied"), true);
	Out->SetBoolField(TEXT("prior_value"), bPriorValue);
	Out->SetBoolField(TEXT("recompile_triggered"), true);
	Out->SetBoolField(TEXT("recompile_already_pending"), bRecompileAlreadyPending);
	return MAT_MakeSuccessObj(Request, Out);
}

// ─── material.is_shader_compiling (Lane A, no PIE guard, trivial) ─────────────────────────────
//
// Args:    {}
// Result:  { compiling: bool, remaining_jobs: int }
//
// Pulls live state from GShaderCompilingManager. No mutation; safe under any editor state.
FMCPResponse Tool_IsShaderCompiling(const FMCPRequest& Request)
{
	check(IsInGameThread());

	bool bCompiling = false;
	int32 RemainingJobs = 0;
	if (GShaderCompilingManager)
	{
		bCompiling    = GShaderCompilingManager->IsCompiling();
		RemainingJobs = GShaderCompilingManager->GetNumRemainingJobs();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("compiling"), bCompiling);
	Out->SetNumberField(TEXT("remaining_jobs"), static_cast<double>(RemainingJobs));
	return MAT_MakeSuccessObj(Request, Out);
}

// ─── material.create_instance (Lane A, PIE-guarded) ───────────────────────────────────────────
//
// Args:    { parent_material_path: string, dest_path: string }
// Result:  { created: bool, mic_path: string }
//
// Errors:
//   -32027 PIEActive
//   -32010 InvalidPath (parent or dest)
//   -32004 ObjectNotFound (parent)
//   -32034 MaterialClassMismatch (parent not UMaterialInterface)
//   -32014 PathInUse (dest_path exists)
//   -32603 InternalError (factory returned null)
//
// Creates a UMaterialInstanceConstant pointing at parent_material_path, saved at dest_path. The
// dest_path is split into ``{PackagePath, AssetName}`` for IAssetTools::CreateAsset.
FMCPResponse Tool_CreateInstance(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (MAT_IsPIEActive()) { return MAT_MakePIEError(Request); }

	if (!Request.Args.IsValid())
	{
		return MAT_MakeError(Request, kMATErrorInvalidParams, TEXT("missing args object"));
	}

	FString ParentPathRaw;
	if (!Request.Args->TryGetStringField(TEXT("parent_material_path"), ParentPathRaw) || ParentPathRaw.IsEmpty())
	{
		return MAT_MakeError(Request, kMATErrorInvalidParams,
			TEXT("missing required string field 'parent_material_path'"));
	}
	FString DestPathRaw;
	if (!Request.Args->TryGetStringField(TEXT("dest_path"), DestPathRaw) || DestPathRaw.IsEmpty())
	{
		return MAT_MakeError(Request, kMATErrorInvalidParams,
			TEXT("missing required string field 'dest_path'"));
	}

	int32 ErrCode = 0;
	FString ErrMsg;
	UMaterialInterface* ParentMaterial = FMCPMaterialUtils::LoadMaterialInterfaceByPath(
		ParentPathRaw, ErrCode, ErrMsg);
	if (!ParentMaterial)
	{
		return MAT_MakeError(Request, ErrCode, ErrMsg);
	}

	const FString DestPath = FMCPAssetPathUtils::Normalize(DestPathRaw);
	if (DestPath.IsEmpty() || !FMCPAssetPathUtils::IsValidGameOrPlugin(DestPath))
	{
		return MAT_MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("dest_path '%s' is malformed or references an unknown mount point"),
				*DestPathRaw));
	}

	// Conflict check (D10).
	if (FPackageName::DoesPackageExist(DestPath))
	{
		return MAT_MakeError(Request, kMCPErrorPathInUse,
			FString::Printf(TEXT("dest_path '%s' already exists; cb.delete then retry or pick a new path"),
				*DestPath));
	}

	const FString PackagePath = FPaths::GetPath(DestPath);
	const FString AssetName   = FPaths::GetBaseFilename(DestPath);
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		return MAT_MakeError(Request, kMCPErrorInvalidPath,
			FString::Printf(TEXT("dest_path '%s' split into (folder='%s', name='%s') — neither may be empty"),
				*DestPath, *PackagePath, *AssetName));
	}

	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = ParentMaterial;

	UObject* NewAsset = nullptr;
	{
		FScopedTransaction Transaction(LOCTEXT("MCPCreateMIC", "MCP: create material instance"));
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(
			TEXT("AssetTools"));
		NewAsset = AssetToolsModule.Get().CreateAsset(
			AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);
	}

	if (!NewAsset)
	{
		return MAT_MakeError(Request, kMATErrorInternal,
			FString::Printf(
				TEXT("IAssetTools::CreateAsset returned null for '%s/%s' (factory init failure?)"),
				*PackagePath, *AssetName));
	}

	UMaterialInstanceConstant* NewMIC = Cast<UMaterialInstanceConstant>(NewAsset);
	if (!NewMIC)
	{
		// Defensive — factory should always produce a MIC, but if some custom override changed
		// the class we want to fail loudly.
		return MAT_MakeError(Request, kMATErrorInternal,
			FString::Printf(
				TEXT("CreateAsset produced class '%s', expected UMaterialInstanceConstant"),
				*NewAsset->GetClass()->GetPathName()));
	}

	// Belt-and-suspenders: the factory does set Parent during FactoryCreateNew via InitialParent,
	// but SetParentEditorOnly additionally recaches shaders + notifies the editor. This makes the
	// MIC immediately usable for parameter overrides on the same tick.
	NewMIC->SetParentEditorOnly(ParentMaterial);
	{
		FPropertyChangedEvent ChangeEvent(nullptr);
		NewMIC->PostEditChangeProperty(ChangeEvent);
	}
	NewMIC->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("created"), true);
	Out->SetStringField(TEXT("mic_path"), NewMIC->GetPathName());
	return MAT_MakeSuccessObj(Request, Out);
}

// ─── material.get_compile_errors (Lane A, no PIE guard) ───────────────────────────────────────
//
// Args:    { material_path }
// Result:  { has_errors: bool, errors: [string], warnings: [string] }
//
// Reads ``FMaterialResource::GetCompileErrors()`` from the underlying UMaterial (walks MIC parent
// chain if needed). Warnings array is reserved for future expansion — UE 5.7's FMaterialResource
// surfaces only errors via the public API.
//
// Errors:
//   -32010 / -32004 / -32034 (resolve)
FMCPResponse Tool_GetCompileErrors(const FMCPRequest& Request)
{
	check(IsInGameThread());

	FString Path;
	FMCPResponse PathErr;
	if (!MAT_RequireMaterialPath(Request, Path, PathErr)) { return PathErr; }

	int32 ErrCode = 0;
	FString ErrMsg;
	UMaterialInterface* Material = FMCPMaterialUtils::LoadMaterialInterfaceByPath(Path, ErrCode, ErrMsg);
	if (!Material) { return MAT_MakeError(Request, ErrCode, ErrMsg); }

	UMaterial* Base = FMCPMaterialUtils::WalkToBaseMaterial(Material);
	TArray<TSharedPtr<FJsonValue>> ErrorArr;
	if (Base)
	{
		const FMaterialResource* Resource = Base->GetMaterialResource(GMaxRHIShaderPlatform);
		if (Resource)
		{
			const TArray<FString>& CompileErrors = Resource->GetCompileErrors();
			ErrorArr.Reserve(CompileErrors.Num());
			for (const FString& E : CompileErrors)
			{
				ErrorArr.Add(MakeShared<FJsonValueString>(E));
			}
		}
	}

	// Warnings: not surfaced separately by FMaterialResource in 5.7 (compile output funnels through
	// MessageLog and CompileErrors). Reserved for future material.* extension if a downstream user
	// needs the distinction.
	TArray<TSharedPtr<FJsonValue>> WarningArr;

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("has_errors"), ErrorArr.Num() > 0);
	Out->SetArrayField(TEXT("errors"),    ErrorArr);
	Out->SetArrayField(TEXT("warnings"),  WarningArr);
	return MAT_MakeSuccessObj(Request, Out);
}

// ─── Registration ────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	// Day 11: parameter enumeration
	RegisterTool(TEXT("material.list_parameters"), &Tool_ListParameters, /*Lane A*/ false);
	RegisterTool(TEXT("material.get_parameter"),   &Tool_GetParameter,   /*Lane A*/ false);

	// Day 12: scalar + vector MIC writes
	RegisterTool(TEXT("material.set_scalar_param"), &Tool_SetScalarParam, /*Lane A*/ false);
	RegisterTool(TEXT("material.set_vector_param"), &Tool_SetVectorParam, /*Lane A*/ false);

	// Day 13: texture + static switch MIC writes + shader diagnostic
	RegisterTool(TEXT("material.set_texture_param"),  &Tool_SetTextureParam,   /*Lane A*/ false);
	RegisterTool(TEXT("material.set_static_switch"),  &Tool_SetStaticSwitch,   /*Lane A*/ false);
	RegisterTool(TEXT("material.is_shader_compiling"), &Tool_IsShaderCompiling, /*Lane A*/ false);

	// Day 14: MIC factory + compile error read
	RegisterTool(TEXT("material.create_instance"),    &Tool_CreateInstance,    /*Lane A*/ false);
	RegisterTool(TEXT("material.get_compile_errors"), &Tool_GetCompileErrors,  /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("Phase 4 Days 11-14: registered 9 material.* handlers (2 reads + 4 MIC writes + 1 create + 2 diagnostic, all Lane A)"));
}

} // namespace FMaterialTools

#undef LOCTEXT_NAMESPACE
