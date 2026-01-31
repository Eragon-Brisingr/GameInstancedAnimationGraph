#pragma once

#include "CoreMinimal.h"
#include "AnimGraphNode_Base.h"
#include "AnimNode_GIAG_PoseFromHandle.h"
#include "AnimGraphNode_GIAG_PoseFromHandle.generated.h"

UCLASS()
class UAnimGraphNode_GIAG_PoseFromHandle : public UAnimGraphNode_Base
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category="Settings")
	FAnimNode_GIAG_PoseFromHandle Node;

	// UEdGraphNode
	FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	FText GetTooltipText() const override;

	// UAnimGraphNode_Base
	FString GetNodeCategory() const override;
};

