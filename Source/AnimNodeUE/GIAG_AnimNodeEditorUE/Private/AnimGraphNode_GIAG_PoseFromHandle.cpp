#include "AnimGraphNode_GIAG_PoseFromHandle.h"

#define LOCTEXT_NAMESPACE "GIAG"

FText UAnimGraphNode_GIAG_PoseFromHandle::GetNodeTitle(ENodeTitleType::Type /*TitleType*/) const
{
	return LOCTEXT("GIAG_PoseFromHandle_Title", "GIAG Pose From Handle");
}

FText UAnimGraphNode_GIAG_PoseFromHandle::GetTooltipText() const
{
	return LOCTEXT("GIAG_PoseFromHandle_Tooltip", "Outputs the current frame GIAG CPU pose for the given handle.");
}

FString UAnimGraphNode_GIAG_PoseFromHandle::GetNodeCategory() const
{
	return TEXT("GameInstancedAnim");
}

#undef LOCTEXT_NAMESPACE

