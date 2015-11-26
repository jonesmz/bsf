#include "BsCoreObjectManager.h"
#include "BsCoreObject.h"
#include "BsCoreObjectCore.h"
#include "BsException.h"
#include "BsMath.h"
#include "BsFrameAlloc.h"
#include "BsCoreThread.h"

namespace BansheeEngine
{
	CoreObjectManager::CoreObjectManager()
		:mNextAvailableID(1)
	{

	} 

	CoreObjectManager::~CoreObjectManager()
	{
#if BS_DEBUG_MODE
		BS_LOCK_MUTEX(mObjectsMutex);

		if(mObjects.size() > 0)
		{
			// All objects MUST be destroyed at this point, otherwise there might be memory corruption.
			// (Reason: This is called on application shutdown and at that point we also unload any dynamic libraries, 
			// which will invalidate any pointers to objects created from those libraries. Therefore we require of the user to 
			// clean up all objects manually before shutting down the application).
			BS_EXCEPT(InternalErrorException, "Core object manager shut down, but not all objects were released. Application must release ALL " \
				"engine objects before shutdown.");
		}
#endif
	}

	UINT64 CoreObjectManager::registerObject(CoreObject* object)
	{
		assert(object != nullptr);

		BS_LOCK_MUTEX(mObjectsMutex);

		mObjects[mNextAvailableID] = object;
		mDirtyObjects[mNextAvailableID] = { object, -1 };

		return mNextAvailableID++;
	}

	void CoreObjectManager::unregisterObject(CoreObject* object)
	{
		assert(object != nullptr);

		UINT64 internalId = object->getInternalID();

		// If dirty, we generate sync data before it is destroyed
		{
			BS_LOCK_MUTEX(mObjectsMutex);
			bool isDirty = object->isCoreDirty() || (mDirtyObjects.find(internalId) != mDirtyObjects.end());

			if (isDirty)
			{
				SPtr<CoreObjectCore> coreObject = object->getCore();
				if (coreObject != nullptr)
				{
					CoreSyncData objSyncData = object->syncToCore(gCoreThread().getFrameAlloc());
				
					mDestroyedSyncData.push_back(CoreStoredSyncObjData(coreObject, internalId, objSyncData));

					DirtyObjectData& dirtyObjData = mDirtyObjects[internalId];
					dirtyObjData.syncDataId = (INT32)mDestroyedSyncData.size() - 1;
					dirtyObjData.object = nullptr;
				}
				else
				{
					DirtyObjectData& dirtyObjData = mDirtyObjects[internalId];
					dirtyObjData.syncDataId = -1;
					dirtyObjData.object = nullptr;
				}
			}

			mObjects.erase(internalId);
		}

		updateDependencies(object, nullptr);
	}

	void CoreObjectManager::notifyCoreDirty(CoreObject* object)
	{
		UINT64 id = object->getInternalID();

		BS_LOCK_MUTEX(mObjectsMutex);

		mDirtyObjects[id] = { object, -1 };
	}

	void CoreObjectManager::notifyDependenciesDirty(CoreObject* object)
	{
		Vector<CoreObject*> dependencies;
		object->getCoreDependencies(dependencies);

		updateDependencies(object, &dependencies);
	}

	void CoreObjectManager::updateDependencies(CoreObject* object, Vector<CoreObject*>* dependencies)
	{
		UINT64 id = object->getInternalID();

		bs_frame_mark();
		{
			FrameVector<CoreObject*> toRemove;
			FrameVector<CoreObject*> toAdd;

			BS_LOCK_MUTEX(mObjectsMutex);

			// Add dependencies and clear old dependencies from dependants
			{
				if (dependencies != nullptr)
					std::sort(dependencies->begin(), dependencies->end());

				auto iterFind = mDependencies.find(id);
				if (iterFind != mDependencies.end())
				{
					const Vector<CoreObject*>& oldDependencies = iterFind->second;

					if (dependencies != nullptr)
					{
						std::set_difference(dependencies->begin(), dependencies->end(),
							dependencies->begin(), dependencies->end(), toRemove.begin());

						std::set_difference(oldDependencies.begin(), oldDependencies.end(),
							oldDependencies.begin(), oldDependencies.end(), toAdd.begin());
					}
					else
					{
						for (auto& dependency : oldDependencies)
							toRemove.push_back(dependency);
					}

					for (auto& dependency : toRemove)
					{
						UINT64 dependencyId = dependency->getInternalID();
						auto iterFind2 = mDependants.find(dependencyId);

						if (iterFind2 != mDependants.end())
						{
							Vector<CoreObject*>& dependants = iterFind2->second;
							auto findIter3 = std::find(dependants.begin(), dependants.end(), object);
							dependants.erase(findIter3);

							if (dependants.size() == 0)
								mDependants.erase(iterFind2);
						}
					}
				}
				else
				{
					if (dependencies != nullptr)
					{
						for (auto& dependency : *dependencies)
							toAdd.push_back(dependency);
					}
				}

				if (dependencies != nullptr)
					mDependencies[id] = *dependencies;
			}

			// Register dependants
			{
				for (auto& dependency : toAdd)
				{
					UINT64 dependencyId = dependency->getInternalID();
					Vector<CoreObject*>& dependants = mDependants[dependencyId];
					dependants.push_back(object);
				}
			}
		}
		bs_frame_clear();
	}

	void CoreObjectManager::syncToCore(CoreAccessor& accessor)
	{
		syncDownload(gCoreThread().getFrameAlloc());
		accessor.queueCommand(std::bind(&CoreObjectManager::syncUpload, this));
	}

	void CoreObjectManager::syncToCore(CoreObject* object, CoreAccessor& accessor)
	{
		struct IndividualCoreSyncData
		{
			SPtr<CoreObjectCore> destination;
			CoreSyncData syncData;
			FrameAlloc* allocator;
		};

		BS_LOCK_MUTEX(mObjectsMutex);

		FrameAlloc* allocator = gCoreThread().getFrameAlloc();
		Vector<IndividualCoreSyncData> syncData;

		std::function<void(CoreObject*)> syncObject = [&](CoreObject* curObj)
		{
			if (!curObj->isCoreDirty())
				return; // We already processed it as some other object's dependency

			// Sync dependencies before dependants
			// Note: I don't check for recursion. Possible infinite loop if two objects
			// are dependent on one another.

			UINT64 id = curObj->getInternalID();
			auto iterFind = mDependencies.find(id);

			if (iterFind != mDependencies.end())
			{
				const Vector<CoreObject*>& dependencies = iterFind->second;
				for (auto& dependency : dependencies)
					syncObject(dependency);
			}

			SPtr<CoreObjectCore> objectCore = curObj->getCore();
			if (objectCore == nullptr)
			{
				curObj->markCoreClean();
				mDirtyObjects.erase(id);
				return;
			}

			syncData.push_back(IndividualCoreSyncData());
			IndividualCoreSyncData& data = syncData.back();
			data.allocator = allocator;
			data.destination = objectCore;
			data.syncData = curObj->syncToCore(allocator);

			curObj->markCoreClean();
			mDirtyObjects.erase(id);
		};

		syncObject(object);

		std::function<void(const Vector<IndividualCoreSyncData>&)> callback =
			[](const Vector<IndividualCoreSyncData>& data)
		{
			// Traverse in reverse to sync dependencies before dependants
			for (auto& riter = data.rbegin(); riter != data.rend(); ++riter)
			{
				const IndividualCoreSyncData& entry = *riter;
				entry.destination->syncToCore(entry.syncData);

				UINT8* dataPtr = entry.syncData.getBuffer();

				if (dataPtr != nullptr)
					entry.allocator->dealloc(dataPtr);
			}
		};

		if (syncData.size() > 0)
			accessor.queueCommand(std::bind(callback, syncData));
	}

	void CoreObjectManager::syncDownload(FrameAlloc* allocator)
	{
		BS_LOCK_MUTEX(mObjectsMutex);

		mCoreSyncData.push_back(CoreStoredSyncData());
		CoreStoredSyncData& syncData = mCoreSyncData.back();

		syncData.alloc = allocator;
		
		// Add all objects dependant on the dirty objects
		bs_frame_mark();
		{
			FrameSet<CoreObject*> dirtyDependants;
			for (auto& objectData : mDirtyObjects)
			{
				auto iterFind = mDependants.find(objectData.first);
				if (iterFind != mDependants.end())
				{
					const Vector<CoreObject*>& dependants = iterFind->second;
					for (auto& dependant : dependants)
					{
						if (!dependant->isCoreDirty())
							dirtyDependants.insert(dependant);
					}
				}
			}

			for (auto& dirtyDependant : dirtyDependants)
			{
				UINT64 id = dirtyDependant->getInternalID();

				mDirtyObjects[id] = { dirtyDependant, -1 };
			}
		}

		bs_frame_clear();
		
		// Order in which objects are recursed in matters, ones with lower ID will have been created before
		// ones with higher ones and should be updated first.
		for (auto& objectData : mDirtyObjects)
		{
			std::function<void(CoreObject*)> syncObject = [&](CoreObject* curObj)
			{
				if (!curObj->isCoreDirty())
					return; // We already processed it as some other object's dependency

				// Sync dependencies before dependants
				// Note: I don't check for recursion. Possible infinite loop if two objects
				// are dependent on one another.
				
				UINT64 id = curObj->getInternalID();
				auto iterFind = mDependencies.find(id);

				if (iterFind != mDependencies.end())
				{
					const Vector<CoreObject*>& dependencies = iterFind->second;
					for (auto& dependency : dependencies)
						syncObject(dependency);
				}

				SPtr<CoreObjectCore> objectCore = curObj->getCore();
				if (objectCore == nullptr)
				{
					curObj->markCoreClean();
					return;
				}

				CoreSyncData objSyncData = curObj->syncToCore(allocator);
				curObj->markCoreClean();

				syncData.entries.push_back(CoreStoredSyncObjData(objectCore,
					curObj->getInternalID(), objSyncData));
			};

			CoreObject* object = objectData.second.object;
			if (object != nullptr)
				syncObject(object);
			else
			{
				// Object was destroyed but we still need to sync its modifications before it was destroyed
				if (objectData.second.syncDataId != -1)
					syncData.entries.push_back(mDestroyedSyncData[objectData.second.syncDataId]);
			}
		}

		mDirtyObjects.clear();
		mDestroyedSyncData.clear();
	}

	void CoreObjectManager::syncUpload()
	{
		BS_LOCK_MUTEX(mObjectsMutex);

		if (mCoreSyncData.size() == 0)
			return;

		CoreStoredSyncData& syncData = mCoreSyncData.front();

		for (auto& iter = syncData.entries.begin(); iter != syncData.entries.end(); ++iter)
		{
			const CoreStoredSyncObjData& objSyncData = *iter;

			SPtr<CoreObjectCore> destinationObj = objSyncData.destinationObj.lock();
			if (destinationObj != nullptr)
				destinationObj->syncToCore(objSyncData.syncData);

			UINT8* data = objSyncData.syncData.getBuffer();

			if (data != nullptr)
				syncData.alloc->dealloc(data);
		}

		syncData.entries.clear();
		mCoreSyncData.pop_front();
	}

	void CoreObjectManager::clearDirty()
	{
		BS_LOCK_MUTEX(mObjectsMutex);

		FrameAlloc* allocator = gCoreThread().getFrameAlloc();
		for (auto& objectData : mDirtyObjects)
		{
			if (objectData.second.syncDataId != -1)
			{
				CoreStoredSyncObjData& objSyncData = mDestroyedSyncData[objectData.second.syncDataId];

				UINT8* data = objSyncData.syncData.getBuffer();

				if (data != nullptr)
					allocator->dealloc(data);
			}
		}

		mDirtyObjects.clear();
		mDestroyedSyncData.clear();
	}
}