#pragma once

#include "CoreMinimal.h"
#include <array>
#include <string_view>
#include <type_traits>
#include <utility>
#include "GIAG_AnimCommon.h"
#include "RenderGraphResources.h"
#include "StructUtils/StructView.h"
#include "UObject/NameTypes.h"
#include "GIAG_AnimNodeBase.generated.h"

class UAnimSequence;
class UGameInstancedAnimationGraphSubsystem;
class FRDGBuilder;
class USkeleton;
class USkeletalMesh;

/** Pin types for GIA AnimGraph. */
enum class EGIAG_AnimPinType : uint8
{
	Pose,
};

/** An input pin reference used in compile-time graph wiring. */
struct FGIAG_AnimInputPinRef
{
	int32 NodeIndex = INDEX_NONE;
	int32 PinIndex = INDEX_NONE;

	static FGIAG_AnimInputPinRef Make(int32 NodeIdx, int32 PinIdx) { return { NodeIdx, PinIdx }; }
};

/** An output pin reference used in compile-time graph wiring. */
struct FGIAG_AnimOutputPinRef
{
	int32 NodeIndex = INDEX_NONE;
	int32 PinIndex = INDEX_NONE;

	static FGIAG_AnimOutputPinRef Make(int32 NodeIdx, int32 PinIdx) { return { NodeIdx, PinIdx }; }
};

enum class EGIAG_AnimResourceKind : uint8
{
	Buffer,
};

enum class EGIAG_AnimResourceAccess : uint8
{
	None = 0,
	SRV = 1 << 0,
	UAV = 1 << 1,
};
ENUM_CLASS_FLAGS(EGIAG_AnimResourceAccess);

/** Where a resource request will be consumed. */
enum class EGIAG_AnimResourceTarget : uint8
{
	GPU,
	CPU,
};

/**
 * Key used for optional resource sharing/caching across nodes.
 *
 * Contract:
 * - Object/SecondaryObject may be null; in that case AddonDesc alone must be unique within the owning cache domain.
 * - This key must include any data that affects resource layout/content to avoid incorrect sharing.
 */
struct FGIAG_AnimResourceKey
{
	TWeakObjectPtr<const UObject> Object;
	TWeakObjectPtr<const UObject> SecondaryObject;
	FName AddonDesc = NAME_None;
	uint32 AddonHash = 0;

	FORCEINLINE bool IsNone() const { return !Object.IsValid() && !SecondaryObject.IsValid() && AddonDesc.IsNone() && AddonHash == 0; }

	friend FORCEINLINE bool operator==(const FGIAG_AnimResourceKey& A, const FGIAG_AnimResourceKey& B) = default;

	friend FORCEINLINE uint32 GetTypeHash(const FGIAG_AnimResourceKey& Key)
	{
		return HashCombine(HashCombine(HashCombine(GetTypeHash(Key.Object), GetTypeHash(Key.SecondaryObject)), GetTypeHash(Key.AddonDesc)), Key.AddonHash);
	}
};

/**
 * Resource layout description for a node-requested resource.
 */
struct FGIAG_AnimResourceLayout
{
	EGIAG_AnimResourceKind Kind = EGIAG_AnimResourceKind::Buffer;

	/** For Buffer: element stride in bytes. */
	uint32 StrideBytes = 0;

	/** For Buffer: number of elements. */
	uint32 NumElements = 0;
};

/**
 * A node-declared resource request.
 * ShareKey controls sharing: nodes producing the same ShareKey will share the same cached external resource.
 */
struct FGIAG_AnimResourceRequest
{
	/** Logical slot used to bind this resource into dispatch Context. */
	uint8 Slot = 0;

	/** Key used for resource sharing. Must include any data that affects layout/content. */
	FGIAG_AnimResourceKey ShareKey;

	FGIAG_AnimResourceLayout Layout;

	EGIAG_AnimResourceAccess Access = EGIAG_AnimResourceAccess::SRV;
};

/** Runtime shader-visible pose buffer: float4 pixels, 3 rows per bone. */
struct FGIAG_RDGPoseBuffer
{
	FRDGBufferSRVRef SRV = nullptr;
	FRDGBufferUAVRef UAV = nullptr;
};

/** Runtime shader-visible bone weights buffer: float weights, 1 per bone. */
struct FGIAG_RDGBoneWeights
{
	FRDGBufferSRVRef SRV = nullptr;
};

/** One batch dispatch context: all nodes share the same type. */
struct FGIAG_AnimNodeDispatchContext
{
	FRDGBuilder& GraphBuilder;
	float CurrentTimeSeconds = 0.0f;

	int32 NumInstances = 0;
	int32 NumBones = 0;

	/** Shared skeleton parent indices (bone -> parent). */
	FRDGBufferSRVRef ParentIndicesSRV = nullptr; // StructuredBuffer<int>

	/** Per-slot world->component transform (TRS), sized by SlotCapacity. */
	FRDGBufferSRVRef WorldToComponentBySlotSRV = nullptr; // StructuredBuffer<FGIAG_Transform>

	/** ActiveIndex -> SlotIndex mapping (slot-indexed buffers). */
	FRDGBufferSRVRef ActiveInstanceIndicesSRV = nullptr; // StructuredBuffer<uint>

	/**
	 * Optional per-slot node execution mask (GPU-side culling prepass output).
	 * If null, nodes should assume they are always needed.
	 *
	 * Layout (slot-major): NeedNodeBits[(SlotIndex * NeedNodeWordsPerSlot) + (NodeIndex>>5)] bit (NodeIndex&31).
	 */
	FRDGBufferSRVRef NeedNodeBitsSRV = nullptr; // StructuredBuffer<uint>
	uint32 NeedNodeWordsPerSlot = 0;

	/** Per-node parameter SRVs (same order as NodeIndices). Node-type specific layout. */
	TConstArrayView<FRDGBufferSRVRef> NodeParamSRVsPerNode;

	/**
	 * Optional per-node extra SRVs, grouped by logical slot.
	 * Layout: OptionalBufferSRVsPerNodeBySlot[Slot][NodeInBatch].
	 */
	TConstArrayView<TConstArrayView<FRDGBufferSRVRef>> OptionalBufferSRVsPerNodeBySlot;

	/** ClipPlayer-only shared inputs (may be null for other node types). */
	FRDGBufferSRVRef ClipMetasSRV = nullptr;
	FRDGBufferSRVRef AnimTRSSRV = nullptr;
	uint32 NumClips = 0;

	/** Shared skeleton RefPose in TRS layout (NumBones * FGIAG_BoneTRS). */
	FRDGBufferSRVRef RefPoseLocalTRSSRV = nullptr;

	/** Node indices (in the compiled graph) that are being executed in this batch, in order. */
	TConstArrayView<int32> NodeIndices;

	/** Per node settings blob (same order as NodeIndices). */
	TConstArrayView<FConstStructView> NodeSettingsPerNode;

	/** Per node (same order as NodeIndices): input pose[pin], output pose[pin], etc. */
	TConstArrayView<TConstArrayView<FGIAG_RDGPoseBuffer>> InputPosesPerNode;
	TConstArrayView<TConstArrayView<FGIAG_RDGPoseBuffer>> OutputPosesPerNode;
	TConstArrayView<TConstArrayView<FGIAG_RDGBoneWeights>> InputBoneWeightsPerNode;
};

// ---------------- CPU dispatch (new) ----------------

/** CPU pose buffer view: slot-indexed LocalPose in skeleton bone order. */
struct FGIAG_CPUPoseBufferView
{
	/** Points to SlotCapacity * NumBones transforms. Index = SlotIndex*NumBones + BoneIndex. */
	FGIAG_BoneTRS* Data = nullptr;

	int32 NumBones = 0;
	int32 SlotCapacity = 0;

	FORCEINLINE bool IsValid() const { return Data != nullptr && NumBones > 0 && SlotCapacity > 0; }

	FORCEINLINE int32 Index(int32 SlotIndex, int32 BoneIndex) const
	{
		return SlotIndex * NumBones + BoneIndex;
	}

	FORCEINLINE FGIAG_BoneTRS& At(int32 SlotIndex, int32 BoneIndex) const
	{
		return Data[Index(SlotIndex, BoneIndex)];
	}
};

/**
 * One batch CPU dispatch context: matches the GPU dispatch shape but operates on CPU pose buffers.
 *
 * Conventions:
 * - SlotIndex refers to a packed slot in [0, SlotCapacity). For CPU mode we may pack ActiveIndex==SlotIndex.
 * - ActiveInstanceIndices maps ActiveIndex -> SlotIndex.
 */
struct FGIAG_AnimNodeCpuDispatchContext
{
	float CurrentTimeSeconds = 0.0f;

	int32 NumInstances = 0;   // active instance count
	int32 SlotCapacity = 0;   // slot capacity for this evaluation
	int32 NumBones = 0;

	/** SkeletalMesh used for this evaluation (source of Skeleton / RefSkeleton). */
	USkeletalMesh* SkeletalMesh = nullptr;

	/** Skeleton bone parent indices (bone -> parent), size == NumBones. */
	TConstArrayView<int32> ParentIndices;

	/** Skeleton local RefPose (bone order == Skeleton reference skeleton), size == NumBones. */
	TConstArrayView<FTransform> RefPoseLocal;

	/** Per-slot component transform (for world/component conversion), size == SlotCapacity. */
	TConstArrayView<FTransform> ComponentToWorldBySlot;

	/** ActiveIndex -> SlotIndex mapping (slot-indexed buffers). Size == NumInstances. */
	TConstArrayView<uint32> ActiveInstanceIndices;

	/** ClipIndex -> AnimSequence mapping. May be empty if graph does not use ClipPlayer. */
	TConstArrayView<const UAnimSequence*> AnimSequencesByClipIndex;

	/** Compiled graph (for InstanceDataOffset lookups). */
	const struct FGIAG_AnimGraphCompiledData* Compiled = nullptr;

	/** Per-node AoS instance storage: NodeData[NodeIdx] points to SlotCapacity*StrideBytes bytes. */
	TConstArrayView<const uint8*> NodeData;
	TConstArrayView<uint32> NodeStrideBytes;

	/** Node indices (in the compiled graph) that are being executed in this batch, in order. */
	TConstArrayView<int32> NodeIndices;

	/** Per node settings blob (same order as NodeIndices). */
	TConstArrayView<FConstStructView> NodeSettingsPerNode;

	/** Per node (same order as NodeIndices): input poses / output poses views. */
	TConstArrayView<TConstArrayView<FGIAG_CPUPoseBufferView>> InputPosesPerNode;
	TConstArrayView<TConstArrayView<FGIAG_CPUPoseBufferView>> OutputPosesPerNode;

	/**
	 * Optional per-node extra buffers (CPU pointers), grouped by logical slot.
	 * Layout: OptionalBufferPtrsPerNodeBySlot[Slot][NodeInBatch] (nullptr if absent).
	 */
	TConstArrayView<TConstArrayView<const void*>> OptionalBufferPtrsPerNodeBySlot;
	/** Optional buffer byte sizes aligned with OptionalBufferPtrsPerNodeBySlot. */
	TConstArrayView<TConstArrayView<uint32>> OptionalBufferNumBytesPerNodeBySlot;

	/**
	 * Optional per-node extra CPU resources, grouped by logical slot.
	 * Layout: OptionalResourcesPerNodeBySlot[Slot][NodeInBatch] (null if absent).
	 *
	 * Contract: Resource lifetime is guaranteed for the duration of dispatch. Type safety is the caller's responsibility.
	 */
	TConstArrayView<TConstArrayView<TSharedPtr<void>>> OptionalResourcesPerNodeBySlot;

	template<typename T>
	FORCEINLINE TSharedPtr<T> GetOptionalResourceShared(int32 Slot, int32 NodeIndexInBatch) const
	{
		if (!OptionalResourcesPerNodeBySlot.IsValidIndex(Slot))
		{
			return nullptr;
		}
		if (!OptionalResourcesPerNodeBySlot[Slot].IsValidIndex(NodeIndexInBatch))
		{
			return nullptr;
		}
		return StaticCastSharedPtr<T>(OptionalResourcesPerNodeBySlot[Slot][NodeIndexInBatch]);
	}

	template<typename T>
	FORCEINLINE const T* GetOptionalResourcePtr(int32 Slot, int32 NodeIndexInBatch) const
	{
		return GetOptionalResourceShared<T>(Slot, NodeIndexInBatch).Get();
	}

	FORCEINLINE const uint8* GetNodePtrBySlotRaw(int32 NodeIdx, int32 SlotIndex) const
	{
		check(NodeData.IsValidIndex(NodeIdx));
		check(NodeStrideBytes.IsValidIndex(NodeIdx));
		check(SlotIndex >= 0 && SlotIndex < SlotCapacity);
		return NodeData[NodeIdx] + (int64)NodeStrideBytes[NodeIdx] * (int64)SlotIndex;
	}

	template<typename T>
	FORCEINLINE const T* GetNodePtrBySlot(int32 NodeIdx, int32 SlotIndex) const
	{
		return reinterpret_cast<const T*>(GetNodePtrBySlotRaw(NodeIdx, SlotIndex));
	}
};

USTRUCT(BlueprintType, BlueprintInternalUseOnly)
struct FGIAG_AnimNodeRef
{
	GENERATED_BODY()

	UGameInstancedAnimationGraphSubsystem* System = nullptr;
	int32 RecordIndex = INDEX_NONE;
	int32 GroupIndex = INDEX_NONE;
	int32 BucketIndex = INDEX_NONE;
	int32 ShardIndex = INDEX_NONE;
	int32 SlotIndex = INDEX_NONE;
	int32 NodeIndex = INDEX_NONE;

	GAMEINSTANCEDANIMATIONGRAPH_API void MarkDirty() const;
};

template<typename T>
struct TGIAG_AnimNodePtr : FGIAG_AnimNodeRef
{
	T* NodePtr = nullptr;

	FGIAG_AnimNodeRef& operator=(const FGIAG_AnimNodeRef&) = delete;
	explicit operator bool() { return NodePtr != nullptr; }
	T* operator->() { return NodePtr; }
};

/**
 * Base interface for a node meta.
 */
struct GAMEINSTANCEDANIMATIONGRAPH_API IGIAG_AnimNodeMeta : FNoncopyable
{
	IGIAG_AnimNodeMeta();
	virtual ~IGIAG_AnimNodeMeta();

	virtual UScriptStruct* GetStruct() const = 0;

	/** Optional: whether this node type participates in culling. */
	virtual bool HasCullLogic() const = 0;

	/** Optional: Node-cull input need-mask (CPU): bit i => input pin i is needed. */
	virtual uint32 ComputeCullNeedMaskCPU(uint32 NumInputs, const void* NodeData) const = 0;

	/**
	 * Optional: emit HLSL function body that computes an input need-mask for this node.
	 *
	 * Contract:
	 * - Called only when HasCullLogic() is true.
	 * - Must write the full function body (including `return ...;`).
	 * - Must provide cull param metadata:
	 *   - OutHlslElementType: element type of StructuredBuffer NodeData (e.g. "float")
	 *   - OutMemberName: suffix for symbol generation (e.g. "Alpha" => Foo_Alpha_Params)
	 */
	virtual void EmitCullNeedMaskHlslBody(FString& Out, const TCHAR*& OutHlslElementType, const TCHAR*& OutMemberName) const = 0;

	virtual int32 GetNumInputPins() const = 0;
	virtual int32 GetNumOutputPins() const = 0;
	virtual EGIAG_AnimPinType GetInputPinType(int32 PinIndex) const = 0;
	virtual EGIAG_AnimPinType GetOutputPinType(int32 PinIndex) const = 0;

	virtual FString GetInputPinName(int32 PinIndex) const = 0;
	virtual FString GetOutputPinName(int32 PinIndex) const = 0;

	virtual void InitInstanceData(void* NodeData) const = 0;
	virtual const void* GatherUploadsGPU(const void* NodeData, uint32& OutUploadStrideBytes) const = 0;
	virtual void AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context) const = 0;
	virtual void AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context) const = 0;

	/** Optional: declare extra GPU resources needed by this node type (shared via ShareKey). */
	virtual void EnumerateResourceRequests(FConstStructView Settings, const USkeleton* Skeleton, EGIAG_AnimResourceTarget Target, TArray<FGIAG_AnimResourceRequest>& Out) const = 0;

	/** Optional: build CPU-side bytes for the requested resource. Return false if not supported. */
	virtual bool BuildResourceForGPU(const FGIAG_AnimResourceRequest& Req, FConstStructView Settings, const USkeleton* Skeleton, TArray<uint8>& OutBytes) const = 0;

	/** Optional: build CPU-side resource for the requested layout/key. Return false if not supported. */
	virtual bool BuildResourceForCPU(const FGIAG_AnimResourceRequest& Req, FConstStructView Settings, const USkeleton* Skeleton, TSharedPtr<void>& OutResource) const = 0;
	
	/** Optional: runtime control hook (e.g. ClipPlayer). Return false if unsupported by this node type. */
	virtual void PlayAnimation(void* NodeInstance, const FGIAG_AnimNodeRef& NodeRef, const UAnimSequence* AnimSequence, float BlendDurationSeconds, float StartSeconds, bool bLoop, float PlayRate) const = 0;

	/** Optional: enumerate referenced clip indices for this node instance (for cleanup / CPU->GPU bake). */
	virtual void EnumerateClips(const void* NodeInstance, TArray<int32>& OutClipIndices) const = 0;
};

namespace GIAG::Detail
{
	consteval bool GIAG_IsIdentStart(char C)
	{
		return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || C == '_';
	}

	consteval bool GIAG_IsIdentChar(char C)
	{
		return GIAG_IsIdentStart(C) || (C >= '0' && C <= '9');
	}

	consteval bool GIAG_IsValidEnumValueIdent(std::string_view S)
	{
		if (S.empty())
		{
			return false;
		}
		if (!GIAG_IsIdentStart(S.front()))
		{
			return false;
		}
		for (char C : S)
		{
			if (!GIAG_IsIdentChar(C))
			{
				return false;
			}
		}
		return true;
	}

	consteval std::string_view GIAG_Trim(std::string_view S)
	{
		while (!S.empty() && (S.front() == ' ' || S.front() == '\t' || S.front() == '\n' || S.front() == '\r'))
		{
			S.remove_prefix(1);
		}
		while (!S.empty() && (S.back() == ' ' || S.back() == '\t' || S.back() == '\n' || S.back() == '\r'))
		{
			S.remove_suffix(1);
		}
		return S;
	}

	consteval std::string_view GIAG_StripEnumClassPrefix(std::string_view S)
	{
		S = GIAG_Trim(S);
		if (S.rfind("enum ", 0) == 0)
		{
			S.remove_prefix(5);
		}
		else if (S.rfind("class ", 0) == 0)
		{
			S.remove_prefix(6);
		}
		return GIAG_Trim(S);
	}

	consteval std::string_view GIAG_ExtractEnumValueNameFromTemplateArg(std::string_view Arg)
	{
		Arg = GIAG_StripEnumClassPrefix(Arg);

		// Prefer the last scoped token (e.g. "Foo::Bar::Baz" -> "Baz").
		if (const size_t Pos = Arg.rfind("::"); Pos != std::string_view::npos)
		{
			Arg = Arg.substr(Pos + 2);
		}
		Arg = GIAG_Trim(Arg);

		// Trim any non-identifier suffix (defensive against compiler formatting).
		while (!Arg.empty() && !GIAG_IsIdentChar(Arg.back()))
		{
			Arg.remove_suffix(1);
		}

		return Arg;
	}

	template<auto V>
	consteval std::string_view GIAG_EnumValueName()
	{
#if defined(_MSC_VER)
		// MSVC: "...GIAG_EnumValueName<FGIAG_Foo::EInputPin::Bar>(void)"
		constexpr std::string_view Sig = __FUNCSIG__;
		const size_t L = Sig.find('<');
		const size_t R = Sig.rfind('>');
		static_assert(L != std::string_view::npos && R != std::string_view::npos && R > L, "GIAG: unexpected __FUNCSIG__ format.");
		constexpr std::string_view Name = GIAG_ExtractEnumValueNameFromTemplateArg(Sig.substr(L + 1, R - L - 1));
		static_assert(GIAG_IsValidEnumValueIdent(Name), "GIAG: failed to extract a valid enum value identifier name.");
		return Name;
#else
		// Clang/GCC: "...GIAG_EnumValueName() [V = FGIAG_Foo::EInputPin::Bar]"
		constexpr std::string_view Sig = __PRETTY_FUNCTION__;
		constexpr std::string_view Key = "V = ";
		const size_t K = Sig.find(Key);
		static_assert(K != std::string_view::npos, "GIAG: unexpected __PRETTY_FUNCTION__ format.");
		const size_t Start = K + Key.size();
		const size_t End = Sig.find(']', Start);
		static_assert(End != std::string_view::npos && End > Start, "GIAG: unexpected __PRETTY_FUNCTION__ format (missing closing ']').");
		constexpr std::string_view Name = GIAG_ExtractEnumValueNameFromTemplateArg(Sig.substr(Start, End - Start));
		static_assert(GIAG_IsValidEnumValueIdent(Name), "GIAG: failed to extract a valid enum value identifier name.");
		return Name;
#endif
	}

	template<typename E, size_t N>
	consteval auto GIAG_MakeEnumIndexNames()
	{
		static_assert(std::is_enum_v<E>, "GIAG: E must be an enum type.");

		return []<size_t... Is>(std::index_sequence<Is...>)
		{
			return std::array<std::string_view, N>{ GIAG_EnumValueName<static_cast<E>(Is)>()... };
		}(std::make_index_sequence<N>{});
	}

	FORCEINLINE FString GIAG_ToFString(std::string_view Sv)
	{
		// Enum value names are ASCII identifiers; build a TCHAR string without relying on null-termination.
		FString Out;
		Out.Reserve((int32)Sv.size());
		for (char C : Sv)
		{
			Out.AppendChar((TCHAR)C);
		}
		return Out;
	}
}

template<typename T>
struct TGIAG_AnimNodeMeta : IGIAG_AnimNodeMeta
{
	UScriptStruct* GetStruct() const override { return T::StaticStruct(); }

	bool HasCullLogic() const override
	{
		static constexpr bool bHasCpuCull = requires(const T& Self, uint32 N)
		{
			Self.ComputeCullNeedMaskCPU(N);
		};
		static constexpr bool bHasHlslCull = requires(FString& Out, const TCHAR*& ElemType, const TCHAR*& MemberName)
		{
			T::EmitCullNeedMaskHlslBody(Out, ElemType, MemberName);
		};
		static_assert(bHasCpuCull == bHasHlslCull, "GIAG: node cull contract violation: EmitCullNeedMaskHlslBody and ComputeCullNeedMaskCPU must either both exist or both not exist.");
		return bHasCpuCull && bHasHlslCull;
	}

	uint32 ComputeCullNeedMaskCPU(uint32 NumInputs, const void* NodeData) const override
	{
		check(NodeData != nullptr);
		if constexpr (requires(const T& Self, uint32 N)
		{
			Self.ComputeCullNeedMaskCPU(N);
		})
		{
			return (uint32)((const T*)NodeData)->ComputeCullNeedMaskCPU(NumInputs);
		}
		else
		{
			checkNoEntry();
			return 0u;
		}
	}

	void EmitCullNeedMaskHlslBody(FString& Out, const TCHAR*& OutHlslElementType, const TCHAR*& OutMemberName) const override
	{
		if constexpr (requires { T::EmitCullNeedMaskHlslBody(Out, OutHlslElementType, OutMemberName); })
		{
			T::EmitCullNeedMaskHlslBody(Out, OutHlslElementType, OutMemberName);
		}
		else
		{
			checkNoEntry();
		}
	}

	// Legacy overload removed (contract now requires element type + member name).
	
	int32 GetNumInputPins() const override
	{
		if constexpr (requires { T::GetNumInputPins(); })
		{
			return (int32)T::GetNumInputPins();
		}
		else
		{
			return (int32)T::EInputPin::Num;
		}
	}
	int32 GetNumOutputPins() const override
	{
		if constexpr (requires { T::GetNumOutputPins(); })
		{
			return (int32)T::GetNumOutputPins();
		}
		else
		{
			return (int32)T::EOutputPin::Num;
		}
	}
	EGIAG_AnimPinType GetInputPinType(int32 PinIndex) const override
	{
		if constexpr (requires { T::GetInputPinType; })
		{
			return T::GetInputPinType(PinIndex);
		}
		else
		{
			return EGIAG_AnimPinType::Pose;
		}
	}
	EGIAG_AnimPinType GetOutputPinType(int32 PinIndex) const override
	{
		if constexpr (requires { T::GetOutputPinType; })
		{
			return T::GetOutputPinType(PinIndex);
		}
		else
		{
			return EGIAG_AnimPinType::Pose;
		}
	}

	FString GetInputPinName(int32 PinIndex) const override
	{
		check(PinIndex >= 0 && PinIndex < GetNumInputPins());

		if constexpr (requires(int32 I) { T::GetInputPinName(I); })
		{
			return T::GetInputPinName(PinIndex);
		}
		else
		{
			static_assert(requires { typename T::EInputPin; }, "GIAG: Node must declare EInputPin or provide GetInputPinName(int32).");

			using E = typename T::EInputPin;
			static constexpr auto Names = GIAG::Detail::GIAG_MakeEnumIndexNames<E, (size_t)E::Num>();
			return GIAG::Detail::GIAG_ToFString(Names[(size_t)PinIndex]);
		}
	}

	FString GetOutputPinName(int32 PinIndex) const override
	{
		check(PinIndex >= 0 && PinIndex < GetNumOutputPins());

		if constexpr (requires(int32 I) { T::GetOutputPinName(I); })
		{
			return T::GetOutputPinName(PinIndex);
		}
		else
		{
			static_assert(requires { typename T::EOutputPin; }, "GIAG: Node must declare EOutputPin or provide GetOutputPinName(int32).");

			using E = typename T::EOutputPin;
			static constexpr auto Names = GIAG::Detail::GIAG_MakeEnumIndexNames<E, (size_t)E::Num>();
			return GIAG::Detail::GIAG_ToFString(Names[(size_t)PinIndex]);
		}
	}

	void InitInstanceData(void* NodeData) const override
	{
		if constexpr (requires(T& Self) { Self.InitInstanceData(); })
		{
			((T*)NodeData)->InitInstanceData();
		}
	}
	const void* GatherUploadsGPU(const void* NodeData, uint32& OutUploadStrideBytes) const override
	{
		if constexpr (requires(const T& Self, uint32& Stride) { Self.GatherUploadsGPU(Stride); })
		{
			return ((T*)NodeData)->GatherUploadsGPU(OutUploadStrideBytes);
		}
		else
		{
			return nullptr;
		}
	}
	void AddPassesGPU(const FGIAG_AnimNodeDispatchContext& Context) const override
	{
		T::AddPassesGPU(Context);
	}

	void AddPassesCPU(const FGIAG_AnimNodeCpuDispatchContext& Context) const override
	{
		T::AddPassesCPU(Context);
	}

	void EnumerateResourceRequests(
		FConstStructView Settings,
		const USkeleton* Skeleton,
		EGIAG_AnimResourceTarget Target,
		TArray<FGIAG_AnimResourceRequest>& Out) const override
	{
		if constexpr (requires(FConstStructView S, const USkeleton* Skel, EGIAG_AnimResourceTarget Tgt, TArray<FGIAG_AnimResourceRequest>& O)
		{
			T::EnumerateResourceRequests(S, Skel, Tgt, O);
		})
		{
			T::EnumerateResourceRequests(Settings, Skeleton, Target, Out);
		}
		else if constexpr (requires(FConstStructView S, const USkeleton* Skel, TArray<FGIAG_AnimResourceRequest>& O)
		{
			T::EnumerateResourceRequests(S, Skel, O);
		})
		{
			// Backward-compatible overload (legacy signature without Target).
			T::EnumerateResourceRequests(Settings, Skeleton, Out);
		}
	}

	bool BuildResourceForGPU(const FGIAG_AnimResourceRequest& Req, FConstStructView Settings, const USkeleton* Skeleton, TArray<uint8>& OutBytes) const override
	{
		if constexpr (requires(const FGIAG_AnimResourceRequest& R, FConstStructView S, const USkeleton* Skel, TArray<uint8>& Bytes)
		{
			T::BuildResourceForGPU(R, S, Skel, Bytes);
		})
		{
			return (bool)T::BuildResourceForGPU(Req, Settings, Skeleton, OutBytes);
		}
		else
		{
			return false;
		}
	}

	bool BuildResourceForCPU(
		const FGIAG_AnimResourceRequest& Req,
		FConstStructView Settings,
		const USkeleton* Skeleton,
		TSharedPtr<void>& OutResource) const override
	{
		if constexpr (requires(const FGIAG_AnimResourceRequest& R, FConstStructView S, const USkeleton* Skel, TSharedPtr<void>& Res)
		{
			T::BuildResourceForCPU(R, S, Skel, Res);
		})
		{
			return (bool)T::BuildResourceForCPU(Req, Settings, Skeleton, OutResource);
		}
		else
		{
			return false;
		}
	}

	void PlayAnimation(
		void* NodeInstance,
		const FGIAG_AnimNodeRef& NodeRef,
		const UAnimSequence* AnimSequence,
		float BlendDurationSeconds,
		float StartSeconds,
		bool bLoop,
		float PlayRate) const override
	{
		if constexpr (requires(T& Self)
		{
			Self.PlayAnimation(NodeRef, AnimSequence, BlendDurationSeconds, StartSeconds, bLoop, PlayRate);
		})
		{
			((T*)NodeInstance)->PlayAnimation(NodeRef, AnimSequence, BlendDurationSeconds, StartSeconds, bLoop, PlayRate);
		}
	}

	void EnumerateClips(const void* NodeInstance, TArray<int32>& OutClipIndices) const override
	{
		if constexpr (requires(const T& Self, TArray<int32>& Out)
		{
			Self.EnumerateClips(Out);
		})
		{
			((const T*)NodeInstance)->EnumerateClips(OutClipIndices);
		}
	}
};

USTRUCT(BlueprintType)
struct GAMEINSTANCEDANIMATIONGRAPH_API FGIAG_AnimNodeBase
{
	GENERATED_BODY()
	
	// Defaults (used when a node type does not declare its own enums):
	// - 0 input pins
	// - 1 output pin named Out
	enum class EInputPin : uint8
	{
		Num,
	};

	enum class EOutputPin : uint8
	{
		Out = 0,
		Num,
	};
};
