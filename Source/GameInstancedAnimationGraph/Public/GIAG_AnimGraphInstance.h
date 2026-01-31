#pragma once

#include "CoreMinimal.h"
#include "GIAG_AnimGraphInstance.generated.h"

/**
 * Runtime data for a compiled AnimGraph.
 *
 * - Users should derive and add per-node UPROPERTY members (structs) for each node in the graph.
 *
 */
USTRUCT(BlueprintType)
struct GAMEINSTANCEDANIMATIONGRAPH_API FGIAG_AnimGraphInstance
{
	GENERATED_BODY()
};


