#include "GIAG_DebugReadback.h"

#include "Containers/Queue.h"

namespace
{
	static TQueue<FGIAG_LocalPoseReadbackResult, EQueueMode::Mpsc> GQueue;
	static TQueue<FGIAG_AttachFxTransformReadbackResult, EQueueMode::Mpsc> GAttachQueue;
	static TQueue<FGIAG_AttachInstanceBuffersReadbackResult, EQueueMode::Mpsc> GAttachInstanceQueue;
	static TQueue<FGIAG_NeedNodeBitsReadbackResult, EQueueMode::Mpsc> GNeedNodeBitsQueue;
}

namespace GIAG::DebugReadback
{
	void EnqueueLocalPose(FGIAG_LocalPoseReadbackResult&& Result)
	{
		GQueue.Enqueue(MoveTemp(Result));
	}

	bool DequeueLocalPose(FGIAG_LocalPoseReadbackResult& OutResult)
	{
		return GQueue.Dequeue(OutResult);
	}

	void EnqueueAttachFxTransform(FGIAG_AttachFxTransformReadbackResult&& Result)
	{
		GAttachQueue.Enqueue(MoveTemp(Result));
	}

	bool DequeueAttachFxTransform(FGIAG_AttachFxTransformReadbackResult& OutResult)
	{
		return GAttachQueue.Dequeue(OutResult);
	}

	void EnqueueAttachInstanceBuffers(FGIAG_AttachInstanceBuffersReadbackResult&& Result)
	{
		GAttachInstanceQueue.Enqueue(MoveTemp(Result));
	}

	bool DequeueAttachInstanceBuffers(FGIAG_AttachInstanceBuffersReadbackResult& OutResult)
	{
		return GAttachInstanceQueue.Dequeue(OutResult);
	}

	void EnqueueNeedNodeBits(FGIAG_NeedNodeBitsReadbackResult&& Result)
	{
		GNeedNodeBitsQueue.Enqueue(MoveTemp(Result));
	}

	bool DequeueNeedNodeBits(FGIAG_NeedNodeBitsReadbackResult& OutResult)
	{
		return GNeedNodeBitsQueue.Dequeue(OutResult);
	}
}

