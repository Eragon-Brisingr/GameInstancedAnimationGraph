#pragma once

#include "CoreMinimal.h"
#include "SceneTypes.h"

class USkeletalMesh;

namespace GIAG
{

/** Detect NumMaterialDataFloats from SkeletalMesh materials (runtime-safe, no editor-only data). */
GAMEINSTANCEDANIMATIONGRAPH_API int32 DetectNumMaterialDataFloatsFromMaterials(const USkeletalMesh& SkeletalMesh);

/**
 * Build a Name -> DataIndex map by scanning all materials on the SkeletalMesh.
 * Each entry maps a GIAG material parameter's ParameterName to its DataIndex (start float offset).
 * Runtime-safe (uses FMaterialCachedExpressionData).
 */
GAMEINSTANCEDANIMATIONGRAPH_API TMap<FName, int32> BuildMaterialDataIndexMapFromMaterials(const USkeletalMesh& SkeletalMesh);

} // namespace GIAG
