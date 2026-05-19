// Copyright FatumGame. All Rights Reserved.

#include "BlueprintGraphTools.h"

#include "FMCPDispatchQueue.h"
#include "UnrealMCPBridge.h"
#include "Utils/MCPBlueprintUtils.h"
#include "Utils/MCPPinTypeUtils.h"
#include "Utils/MCPWorldContext.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MCPBridge"

namespace
{
	// BGT_ prefix per the unity-build symbol-collision pattern.
	constexpr int32 kBGTErrorInvalidParams = -32602;
	constexpr int32 kBGTErrorInternal      = -32603;

	void BGT_StampIds(const FMCPRequest& Request, FMCPResponse& Response)
	{
		Response.RequestId = Request.RequestId;
		Response.OriginalIdString = Request.OriginalIdString;
	}

	FMCPResponse BGT_MakeError(const FMCPRequest& Request, int32 Code, const FString& Message)
	{
		FMCPResponse R;
		BGT_StampIds(Request, R);
		R.bIsError = true;
		R.ErrorCode = Code;
		R.ErrorMessage = Message;
		return R;
	}

	FMCPResponse BGT_MakeSuccessObj(const FMCPRequest& Request, TSharedPtr<FJsonObject> Result)
	{
		FMCPResponse R;
		BGT_StampIds(Request, R);
		R.bIsError = false;
		R.Result = MakeShared<FJsonValueObject>(MoveTemp(Result));
		return R;
	}

	bool BGT_RequireStringField(const FMCPRequest& Request, const TCHAR* FieldName,
		FString& OutValue, FMCPResponse& OutError)
	{
		if (!Request.Args.IsValid())
		{
			OutError = BGT_MakeError(Request, kBGTErrorInvalidParams, TEXT("missing args object"));
			return false;
		}
		if (!Request.Args->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
		{
			OutError = BGT_MakeError(Request, kBGTErrorInvalidParams,
				FString::Printf(TEXT("missing required string field '%s'"), FieldName));
			return false;
		}
		return true;
	}

	/**
	 * Find a graph by name across UbergraphPages + FunctionGraphs + MacroGraphs.
	 * Returns nullptr if not found.
	 */
	UEdGraph* BGT_FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
	{
		if (!Blueprint) { return nullptr; }
		const FName Target(*GraphName);

		// 1. Event graphs (UbergraphPages — canonical default is "EventGraph").
		for (UEdGraph* G : Blueprint->UbergraphPages)
		{
			if (G && G->GetFName() == Target) { return G; }
		}
		// 2. User function graphs + construction script.
		for (UEdGraph* G : Blueprint->FunctionGraphs)
		{
			if (G && G->GetFName() == Target) { return G; }
		}
		// 3. Macro graphs.
		for (UEdGraph* G : Blueprint->MacroGraphs)
		{
			if (G && G->GetFName() == Target) { return G; }
		}
		return nullptr;
	}

	/** Find a node in a graph by Guid string. Returns nullptr if not found. */
	UEdGraphNode* BGT_FindNodeByGuid(UEdGraph* Graph, const FString& GuidString)
	{
		if (!Graph) { return nullptr; }
		FGuid Guid;
		if (!FGuid::Parse(GuidString, Guid)) { return nullptr; }
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->NodeGuid == Guid) { return N; }
		}
		return nullptr;
	}

	/** Build JSON {name, direction, pin_type} for a single UEdGraphPin (no LinkedTo for brevity). */
	TSharedRef<FJsonObject> BGT_BuildPinSummary(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Pin) { return Obj; }
		Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Obj->SetStringField(TEXT("direction"),
			Pin->Direction == EGPD_Input ? TEXT("input") :
			Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("unknown"));
		Obj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			Obj->SetStringField(TEXT("subcategory_object"),
				Pin->PinType.PinSubCategoryObject->GetPathName());
		}
		Obj->SetStringField(TEXT("container"),
			Pin->PinType.ContainerType == EPinContainerType::Array ? TEXT("array") :
			Pin->PinType.ContainerType == EPinContainerType::Set   ? TEXT("set") :
			Pin->PinType.ContainerType == EPinContainerType::Map   ? TEXT("map") : TEXT("none"));
		Obj->SetBoolField(TEXT("is_reference"), Pin->PinType.bIsReference);
		return Obj;
	}
} // namespace

namespace FBlueprintGraphTools
{

// ─── bp.add_node ───────────────────────────────────────────────────────────────────────────────
//
// Args:    { blueprint_path: string,
//            node_class:     string  (e.g. "/Script/BlueprintGraph.K2Node_VariableGet"),
//            graph_name?:    string  (default "EventGraph"),
//            position?:      [x, y]  (default [0, 0]),
//            variable_name?: string  (K2Node_Variable* — sets VariableReference SelfMember),
//            function_name?: string  (K2Node_CallFunction — sets FunctionReference Name),
//            function_class?: string (K2Node_CallFunction — owning class path; self if omitted),
//            event_name?:    string  (K2Node_CustomEvent — CustomFunctionName) }
// Result:  { node_guid, node_class, position: [x, y], pins: [{name, direction, category, ...}] }
//
// Errors: standard kMCPError* + -32050 GraphNotFound.
FMCPResponse Tool_AddNode(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return BGT_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString BlueprintPath;
	FMCPResponse Err;
	if (!BGT_RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }

	FString NodeClassPath;
	if (!BGT_RequireStringField(Request, TEXT("node_class"), NodeClassPath, Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 PosX = 0, PosY = 0;
	const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
	if (Request.Args->TryGetArrayField(TEXT("position"), PositionArr) && PositionArr && PositionArr->Num() == 2)
	{
		PosX = static_cast<int32>((*PositionArr)[0]->AsNumber());
		PosY = static_cast<int32>((*PositionArr)[1]->AsNumber());
	}

	// ─── Resolve blueprint + graph ──────────────────────────────────────────────────────────────
	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return BGT_MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return BGT_MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(
				TEXT("graph '%s' not found on blueprint '%s' (searched UbergraphPages/FunctionGraphs/MacroGraphs)"),
				*GraphName, *BlueprintPath));
	}

	// ─── Resolve node class ─────────────────────────────────────────────────────────────────────
	UClass* NodeClass = LoadObject<UClass>(nullptr, *NodeClassPath);
	if (!NodeClass)
	{
		return BGT_MakeError(Request, kMCPErrorClassNotFound,
			FString::Printf(TEXT("node_class '%s' could not be loaded — expected e.g. "
				"'/Script/BlueprintGraph.K2Node_VariableGet'"), *NodeClassPath));
	}
	if (!NodeClass->IsChildOf(UK2Node::StaticClass()))
	{
		return BGT_MakeError(Request, kMCPErrorWrongClass,
			FString::Printf(TEXT("node_class '%s' is not a UK2Node subclass (class hierarchy: %s)"),
				*NodeClassPath, *NodeClass->GetSuperClass()->GetPathName()));
	}
	if (NodeClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return BGT_MakeError(Request, kMCPErrorClassAbstract,
			FString::Printf(TEXT("node_class '%s' is abstract — cannot instantiate"), *NodeClassPath));
	}

	// ─── Construct + configure + place ──────────────────────────────────────────────────────────
	FScopedTransaction Transaction(LOCTEXT("MCP_AddBPNode", "Add Blueprint Node"));
	Blueprint->Modify();
	Graph->Modify();

	UK2Node* NewNode = NewObject<UK2Node>(Graph, NodeClass, NAME_None, RF_Transactional);

	// Type-specific config — apply BEFORE AllocateDefaultPins so pin layout reflects the binding.
	FString VariableName, FunctionName, FunctionClassPath, EventName;
	Request.Args->TryGetStringField(TEXT("variable_name"), VariableName);
	Request.Args->TryGetStringField(TEXT("function_name"), FunctionName);
	Request.Args->TryGetStringField(TEXT("function_class"), FunctionClassPath);
	Request.Args->TryGetStringField(TEXT("event_name"), EventName);

	if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(NewNode))
	{
		if (!VariableName.IsEmpty())
		{
			VarGet->VariableReference.SetSelfMember(FName(*VariableName));
		}
	}
	else if (UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(NewNode))
	{
		if (!VariableName.IsEmpty())
		{
			VarSet->VariableReference.SetSelfMember(FName(*VariableName));
		}
	}
	else if (UK2Node_CallFunction* CallFn = Cast<UK2Node_CallFunction>(NewNode))
	{
		if (!FunctionName.IsEmpty())
		{
			UClass* OwnerClass = Blueprint->ParentClass; // default to self
			if (!FunctionClassPath.IsEmpty())
			{
				if (UClass* Resolved = LoadObject<UClass>(nullptr, *FunctionClassPath))
				{
					OwnerClass = Resolved;
				}
			}
			if (OwnerClass)
			{
				CallFn->FunctionReference.SetExternalMember(FName(*FunctionName), OwnerClass);
			}
			else
			{
				CallFn->FunctionReference.SetSelfMember(FName(*FunctionName));
			}
		}
	}
	else if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(NewNode))
	{
		if (!EventName.IsEmpty())
		{
			CustomEvent->CustomFunctionName = FName(*EventName);
		}
	}
	// Other K2Node subclasses: caller wires details via subsequent marshall.write_property calls.

	NewNode->NodePosX = PosX;
	NewNode->NodePosY = PosY;
	NewNode->CreateNewGuid();

	Graph->AddNode(NewNode, /*bUserAction*/ false, /*bSelectNewNode*/ false);
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();

	// For function-call style nodes the pin set depends on the resolved UFunction signature;
	// ReconstructNode() rebuilds pins to match. Cheap no-op for nodes that already have correct pins.
	NewNode->ReconstructNode();

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// ─── Build response ─────────────────────────────────────────────────────────────────────────
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("node_guid"), NewNode->NodeGuid.ToString(EGuidFormats::Digits));
	Out->SetStringField(TEXT("node_class"), NodeClass->GetPathName());
	Out->SetStringField(TEXT("title"), NewNode->GetNodeTitle(ENodeTitleType::ListView).ToString());

	TArray<TSharedPtr<FJsonValue>> PositionResp;
	PositionResp.Add(MakeShared<FJsonValueNumber>(NewNode->NodePosX));
	PositionResp.Add(MakeShared<FJsonValueNumber>(NewNode->NodePosY));
	Out->SetArrayField(TEXT("position"), PositionResp);

	TArray<TSharedPtr<FJsonValue>> PinArr;
	PinArr.Reserve(NewNode->Pins.Num());
	for (const UEdGraphPin* Pin : NewNode->Pins)
	{
		if (Pin)
		{
			PinArr.Add(MakeShared<FJsonValueObject>(BGT_BuildPinSummary(Pin)));
		}
	}
	Out->SetArrayField(TEXT("pins"), PinArr);

	return BGT_MakeSuccessObj(Request, Out);
}

// ─── bp.connect_pins ───────────────────────────────────────────────────────────────────────────
//
// Args:    { blueprint_path, graph_name?, from_node, from_pin, to_node, to_pin }
// Result:  { connected: bool, broke_existing_count: int, response: string }
//
// Errors: -32050 GraphNotFound, -32051 NodeNotFound, -32052 PinNotFound, -32053 PinConnectionRefused.
FMCPResponse Tool_ConnectPins(const FMCPRequest& Request)
{
	check(IsInGameThread());

	if (FMCPWorldContext::IsPIEActive())
	{
		return BGT_MakeError(Request, kMCPErrorPIEActive, kMCPMessagePIEActive);
	}

	FString BlueprintPath, FromNodeGuid, FromPinName, ToNodeGuid, ToPinName;
	FMCPResponse Err;
	if (!BGT_RequireStringField(Request, TEXT("blueprint_path"), BlueprintPath, Err)) { return Err; }
	if (!BGT_RequireStringField(Request, TEXT("from_node"),      FromNodeGuid,  Err)) { return Err; }
	if (!BGT_RequireStringField(Request, TEXT("from_pin"),       FromPinName,   Err)) { return Err; }
	if (!BGT_RequireStringField(Request, TEXT("to_node"),        ToNodeGuid,    Err)) { return Err; }
	if (!BGT_RequireStringField(Request, TEXT("to_pin"),         ToPinName,     Err)) { return Err; }

	FString GraphName = TEXT("EventGraph");
	Request.Args->TryGetStringField(TEXT("graph_name"), GraphName);

	int32 LoadErrCode = 0;
	FString LoadErrMsg;
	UBlueprint* Blueprint = FMCPBlueprintUtils::LoadBlueprintByPath(BlueprintPath, LoadErrCode, LoadErrMsg);
	if (!Blueprint) { return BGT_MakeError(Request, LoadErrCode, LoadErrMsg); }

	UEdGraph* Graph = BGT_FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return BGT_MakeError(Request, kMCPErrorGraphNotFound,
			FString::Printf(TEXT("graph '%s' not found on blueprint '%s'"), *GraphName, *BlueprintPath));
	}

	UEdGraphNode* FromNode = BGT_FindNodeByGuid(Graph, FromNodeGuid);
	if (!FromNode)
	{
		return BGT_MakeError(Request, kMCPErrorNodeNotFound,
			FString::Printf(TEXT("from_node '%s' not found in graph '%s'"), *FromNodeGuid, *GraphName));
	}
	UEdGraphNode* ToNode = BGT_FindNodeByGuid(Graph, ToNodeGuid);
	if (!ToNode)
	{
		return BGT_MakeError(Request, kMCPErrorNodeNotFound,
			FString::Printf(TEXT("to_node '%s' not found in graph '%s'"), *ToNodeGuid, *GraphName));
	}

	UEdGraphPin* FromPin = FromNode->FindPin(FName(*FromPinName));
	if (!FromPin)
	{
		return BGT_MakeError(Request, kMCPErrorPinNotFound,
			FString::Printf(TEXT("from_pin '%s' not found on node '%s'"),
				*FromPinName, *FromNode->GetNodeTitle(ENodeTitleType::ListView).ToString()));
	}
	UEdGraphPin* ToPin = ToNode->FindPin(FName(*ToPinName));
	if (!ToPin)
	{
		return BGT_MakeError(Request, kMCPErrorPinNotFound,
			FString::Printf(TEXT("to_pin '%s' not found on node '%s'"),
				*ToPinName, *ToNode->GetNodeTitle(ENodeTitleType::ListView).ToString()));
	}

	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
	if (!Schema)
	{
		return BGT_MakeError(Request, kBGTErrorInternal,
			FString::Printf(TEXT("graph '%s' schema is not UEdGraphSchema_K2 (class=%s)"),
				*GraphName, *Graph->GetSchema()->GetClass()->GetPathName()));
	}

	// CanCreateConnection reports the schema's verdict + reason BEFORE we modify state, so we can
	// surface a clean PinConnectionRefused error.
	const FPinConnectionResponse CanConnect = Schema->CanCreateConnection(FromPin, ToPin);
	if (CanConnect.Response == CONNECT_RESPONSE_DISALLOW)
	{
		return BGT_MakeError(Request, kMCPErrorPinConnectionRefused,
			FString::Printf(TEXT("schema rejected connection '%s.%s' → '%s.%s': %s"),
				*FromNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *FromPinName,
				*ToNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *ToPinName,
				*CanConnect.Message.ToString()));
	}

	FScopedTransaction Transaction(LOCTEXT("MCP_ConnectPins", "Connect Blueprint Pins"));
	Blueprint->Modify();
	Graph->Modify();
	FromNode->Modify();
	ToNode->Modify();

	// Count link counts BEFORE so we can report break-existing semantics from
	// CONNECT_RESPONSE_BREAK_OTHERS_A/B/AB.
	const int32 PriorFromLinks = FromPin->LinkedTo.Num();
	const int32 PriorToLinks   = ToPin->LinkedTo.Num();

	const bool bConnected = Schema->TryCreateConnection(FromPin, ToPin);

	const int32 PostFromLinks = FromPin->LinkedTo.Num();
	const int32 PostToLinks   = ToPin->LinkedTo.Num();
	// Break delta: if a side was made unique it now holds exactly 1 link (the new one); prior link
	// count minus current new-link contribution = number broken. We compute (Prior + 1 - Post) per
	// side and sum; clamps to >= 0.
	const int32 BrokeFrom = FMath::Max(0, (PriorFromLinks + 1) - PostFromLinks);
	const int32 BrokeTo   = FMath::Max(0, (PriorToLinks   + 1) - PostToLinks);
	const int32 BrokeTotal = BrokeFrom + BrokeTo;

	if (bConnected)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("connected"), bConnected);
	Out->SetNumberField(TEXT("broke_existing_count"), static_cast<double>(BrokeTotal));
	Out->SetStringField(TEXT("response"), CanConnect.Message.ToString());
	return BGT_MakeSuccessObj(Request, Out);
}

// ─── Registration ──────────────────────────────────────────────────────────────────────────────
void Register(FMCPDispatchQueue& Queue, TArray<FString>& OutRegisteredMethodNames)
{
	auto RegisterTool = [&](const TCHAR* MethodName, FMCPDispatchQueue::FHandler Handler, bool bThreadSafe)
	{
		Queue.RegisterHandler(MethodName, MoveTemp(Handler), bThreadSafe);
		OutRegisteredMethodNames.Add(MethodName);
	};

	RegisterTool(TEXT("bp.add_node"),     &Tool_AddNode,     /*Lane A*/ false);
	RegisterTool(TEXT("bp.connect_pins"), &Tool_ConnectPins, /*Lane A*/ false);

	UE_LOG(LogMCP, Log,
		TEXT("BP graph surface registered: bp.add_node + bp.connect_pins (Wave B Tier 4, Lane A)"));
}

} // namespace FBlueprintGraphTools

#undef LOCTEXT_NAMESPACE
