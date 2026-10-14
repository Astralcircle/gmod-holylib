// Optimized-only physenv module.
// Keeps the IsPhysicsObjectValid fast path and the hooks required to maintain
// the live physics-object registry. All other physenv functionality is removed.

#include "LuaInterface.h"
#include "module.h"
#include "lua.h"
#include <vphysics_interface.h>
#include "vcollide_parse.h"
#include "detours.h"
#include "tier1/tier1.h"

#if CUSTOM_VPHYSICS_BUILD
#define private public
#include "physics_environment.h"
#undef private
#include "physics_object.h"
#endif

#if defined(SYSTEM_LINUX) || !defined(CUSTOM_VPHYSICS_BUILD)
#include "ivp_old/cphysicsenvironment.h"
#endif

#if !defined(CUSTOM_VPHYSICS_BUILD)
using CPhysicsEnvironment = GMODSDK::CPhysicsEnvironment;
#endif

#include <unordered_map>
#include <unordered_set>

// memdbgon must be the last include file in a .cpp file.
#include "tier0/memdbgon.h"

class CPhysEnvModule : public IModule
{
public:
	void Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn) override;
	void LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit) override;
	void LuaShutdown(GarrysMod::Lua::ILuaInterface* pLua) override;
	void InitDetour(bool bPreServer) override;
	void Shutdown() override;
	const char* Name() override { return "physenv"; }
	int Compatibility() override { return LINUX32 | LINUX64; }
};

static CPhysEnvModule g_pPhysEnvModule;
IModule* pPhysEnvModule = &g_pPhysEnvModule;

static IPhysics* g_pPhysics = nullptr;
static std::unordered_set<IPhysicsObject*> g_pValidObjects;
static std::unordered_map<IPhysicsEnvironment*, std::unordered_set<IPhysicsObject*>> g_pObjectsByEnvironment;
static std::unordered_map<IPhysicsObject*, IPhysicsEnvironment*> g_pEnvironmentByObject;

static void RegisterObject(IPhysicsEnvironment* pEnvironment, IPhysicsObject* pObject)
{
	if (!pEnvironment || !pObject)
		return;

	auto itPrevious = g_pEnvironmentByObject.find(pObject);
	if (itPrevious != g_pEnvironmentByObject.end() && itPrevious->second != pEnvironment)
	{
		auto itPreviousEnvironment = g_pObjectsByEnvironment.find(itPrevious->second);
		if (itPreviousEnvironment != g_pObjectsByEnvironment.end())
			itPreviousEnvironment->second.erase(pObject);
	}

	g_pObjectsByEnvironment[pEnvironment].insert(pObject);
	g_pEnvironmentByObject[pObject] = pEnvironment;
	g_pValidObjects.insert(pObject);
}

static void UnregisterObject(IPhysicsEnvironment* pEnvironment, IPhysicsObject* pObject)
{
	if (!pObject)
		return;

	auto itOwner = g_pEnvironmentByObject.find(pObject);
	IPhysicsEnvironment* pOwner = itOwner != g_pEnvironmentByObject.end() ? itOwner->second : pEnvironment;
	auto itEnvironment = g_pObjectsByEnvironment.find(pOwner);
	if (itEnvironment != g_pObjectsByEnvironment.end())
		itEnvironment->second.erase(pObject);

	if (itOwner != g_pEnvironmentByObject.end())
		g_pEnvironmentByObject.erase(itOwner);
	g_pValidObjects.erase(pObject);
}

static void RegisterEnvironment(IPhysicsEnvironment* pEnvironment)
{
	if (!pEnvironment)
		return;

	int iCount = 0;
	IPhysicsObject** pObjects = static_cast<IPhysicsObject**>(pEnvironment->GetObjectList(&iCount));
	for (int i = 0; pObjects && i < iCount; ++i)
		RegisterObject(pEnvironment, pObjects[i]);
}

static void UnregisterEnvironment(IPhysicsEnvironment* pEnvironment)
{
	auto it = g_pObjectsByEnvironment.find(pEnvironment);
	if (it == g_pObjectsByEnvironment.end())
		return;

	for (IPhysicsObject* pObject : it->second)
	{
		g_pValidObjects.erase(pObject);
		g_pEnvironmentByObject.erase(pObject);
	}

	g_pObjectsByEnvironment.erase(it);
}

static void RegisterExistingEnvironments()
{
	if (!g_pPhysics)
		return;

	for (int i = 0; ; ++i)
	{
		IPhysicsEnvironment* pEnvironment = g_pPhysics->GetActiveEnvironmentByIndex(i);
		if (!pEnvironment)
			break;

		RegisterEnvironment(pEnvironment);
	}
}

// This replaces GMod's linear search of the main environment's object list
// with an O(1) membership test. The registry is updated by the hooks below.
static Detouring::Hook detour_GMod_Util_IsPhysicsObjectValid;
static bool hook_GMod_Util_IsPhysicsObjectValid(IPhysicsObject* pObject)
{
	return pObject && g_pValidObjects.find(pObject) != g_pValidObjects.end();
}

static Detouring::Hook detour_CPhysicsEnvironment_C2;
static void hook_CPhysicsEnvironment_C2(IPhysicsEnvironment* pEnvironment)
{
	detour_CPhysicsEnvironment_C2.GetTrampoline<Symbols::CPhysicsEnvironment_C2>()(pEnvironment);
	RegisterEnvironment(pEnvironment);
}

static Detouring::Hook detour_CPhysicsEnvironment_D2;
static void hook_CPhysicsEnvironment_D2(IPhysicsEnvironment* pEnvironment)
{
	UnregisterEnvironment(pEnvironment);
	detour_CPhysicsEnvironment_D2.GetTrampoline<Symbols::CPhysicsEnvironment_D2>()(pEnvironment);
}

static Detouring::Hook detour_CPhysicsEnvironment_DestroyObject;
static void hook_CPhysicsEnvironment_DestroyObject(GMODSDK::CPhysicsEnvironment* pEnvironment, IPhysicsObject* pObject)
{
	auto itOwner = g_pEnvironmentByObject.find(pObject);
	GMODSDK::CPhysicsEnvironment* pOwner = itOwner != g_pEnvironmentByObject.end()
		? (GMODSDK::CPhysicsEnvironment*)itOwner->second
		: pEnvironment;

	int iFoundIndex = -1;
	for (int i = pOwner->m_objects.Count(); --i >= 0; )
	{
		if (pOwner->m_objects[i] == pObject)
		{
			iFoundIndex = i;
			break;
		}
	}

	if (iFoundIndex == -1)
		return;

#if ARCHITECTURE_X86
	// Preserve GMod's required deferred-delete behavior while unregistering the
	// object from the fast validity registry.
	pOwner->m_objects.FastRemove(iFoundIndex);
	UnregisterObject(pOwner, pObject);

	GMODSDK::CPhysicsObject* pPhysicsObject = static_cast<GMODSDK::CPhysicsObject*>(pObject);
	pPhysicsObject->AddCallbackFlags(CALLBACK_MARKED_FOR_DELETE);

	if (iFoundIndex > pOwner->m_lastObjectThisTick)
		pPhysicsObject->ForceSilentDelete();

	if (pOwner->m_inSimulation || pOwner->m_queueDeleteObject)
	{
		pOwner->m_deadObjects.AddToTail(pObject);
	}
	else
	{
		pOwner->m_pSleepEvents->DeleteObject(pPhysicsObject);
		delete pObject;
	}
#else
	// GMod may call DestroyObject on the wrong environment; use the actual owner.
	detour_CPhysicsEnvironment_DestroyObject.GetTrampoline<Symbols::CPhysicsEnvironment_DestroyObject>()(pOwner, pObject);
	UnregisterObject(pOwner, pObject);
#endif
}

static Detouring::Hook detour_CPhysicsEnvironment_CreateSphereObject;
static IPhysicsObject* hook_CPhysicsEnvironment_CreateSphereObject(IPhysicsEnvironment* pEnvironment, float radius, int materialIndex, const Vector& position, const QAngle& angles, objectparams_t* pParams, bool isStatic)
{
	IPhysicsObject* pObject = detour_CPhysicsEnvironment_CreateSphereObject.GetTrampoline<Symbols::CPhysicsEnvironment_CreateSphereObject>()(pEnvironment, radius, materialIndex, position, angles, pParams, isStatic);
	RegisterObject(pEnvironment, pObject);
	return pObject;
}

static Detouring::Hook detour_CPhysicsEnvironment_UnserializeObjectFromBuffer;
static IPhysicsObject* hook_CPhysicsEnvironment_UnserializeObjectFromBuffer(IPhysicsEnvironment* pEnvironment, void* pGameData, unsigned char* pBuffer, unsigned int bufferSize, bool enableCollisions)
{
	IPhysicsObject* pObject = detour_CPhysicsEnvironment_UnserializeObjectFromBuffer.GetTrampoline<Symbols::CPhysicsEnvironment_UnserializeObjectFromBuffer>()(pEnvironment, pGameData, pBuffer, bufferSize, enableCollisions);
	RegisterObject(pEnvironment, pObject);
	return pObject;
}

static Detouring::Hook detour_CPhysicsEnvironment_CreatePolyObjectStatic;
static IPhysicsObject* hook_CPhysicsEnvironment_CreatePolyObjectStatic(IPhysicsEnvironment* pEnvironment, const CPhysCollide* pCollisionModel, int materialIndex, const Vector& position, const QAngle& angles, objectparams_t* pParams)
{
	IPhysicsObject* pObject = detour_CPhysicsEnvironment_CreatePolyObjectStatic.GetTrampoline<Symbols::CPhysicsEnvironment_CreatePolyObjectStatic>()(pEnvironment, pCollisionModel, materialIndex, position, angles, pParams);
	RegisterObject(pEnvironment, pObject);
	return pObject;
}

static Detouring::Hook detour_CPhysicsEnvironment_CreatePolyObject;
static IPhysicsObject* hook_CPhysicsEnvironment_CreatePolyObject(IPhysicsEnvironment* pEnvironment, const CPhysCollide* pCollisionModel, int materialIndex, const Vector& position, const QAngle& angles, objectparams_t* pParams)
{
	IPhysicsObject* pObject = detour_CPhysicsEnvironment_CreatePolyObject.GetTrampoline<Symbols::CPhysicsEnvironment_CreatePolyObject>()(pEnvironment, pCollisionModel, materialIndex, position, angles, pParams);
	RegisterObject(pEnvironment, pObject);
	return pObject;
}

static Detouring::Hook detour_CPhysicsEnvironment_Restore;
static bool hook_CPhysicsEnvironment_Restore(IPhysicsEnvironment* pEnvironment, physrestoreparams_t const& params)
{
	bool bSuccess = detour_CPhysicsEnvironment_Restore.GetTrampoline<Symbols::CPhysicsEnvironment_Restore>()(pEnvironment, params);
	if (bSuccess && params.type == PIID_IPHYSICSOBJECT && params.ppObject)
		RegisterObject(pEnvironment, static_cast<IPhysicsObject*>(*params.ppObject));

	return bSuccess;
}

static Detouring::Hook detour_CPhysicsEnvironment_TransferObject;
static bool hook_CPhysicsEnvironment_TransferObject(IPhysicsEnvironment* pEnvironment, IPhysicsObject* pObject, IPhysicsEnvironment* pDestinationEnvironment)
{
	bool bSuccess = detour_CPhysicsEnvironment_TransferObject.GetTrampoline<Symbols::CPhysicsEnvironment_TransferObject>()(pEnvironment, pObject, pDestinationEnvironment);
	if (bSuccess)
	{
		UnregisterObject(pEnvironment, pObject);
		RegisterObject(pDestinationEnvironment, pObject);
	}

	return bSuccess;
}

void CPhysEnvModule::Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn)
{
	if (appfn && appfn[0])
	{
		g_pPhysics = static_cast<IPhysics*>(appfn[0](VPHYSICS_INTERFACE_VERSION, nullptr));
	}
	else
	{
		SourceSDK::FactoryLoader vphysicsLoader("vphysics");
		g_pPhysics = vphysicsLoader.GetInterface<IPhysics>(VPHYSICS_INTERFACE_VERSION);
	}
}

void CPhysEnvModule::LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit)
{
	// This module intentionally exposes no Lua API.
}

void CPhysEnvModule::LuaShutdown(GarrysMod::Lua::ILuaInterface* pLua)
{
	// This module intentionally exposes no Lua API.
}

void CPhysEnvModule::InitDetour(bool bPreServer)
{
	if (bPreServer)
		return;

	if (!g_pModuleManager.IsUsingGhostInj())
	{
		Warning(PROJECT_NAME " - physenv: IsPhysicsObjectValid optimization requires GhostInj loading early enough.\n");
		return;
	}

	DETOUR_PREPARE_THISCALL();
	RegisterExistingEnvironments();

	SourceSDK::FactoryLoader vphysicsLoader("vphysics");
	Detour::Create(&detour_CPhysicsEnvironment_C2, "CPhysicsEnvironment::CPhysicsEnvironment", vphysicsLoader.GetModule(), Symbols::CPhysicsEnvironment_C2Sym, (void*)hook_CPhysicsEnvironment_C2, m_pID);
	Detour::Create(&detour_CPhysicsEnvironment_D2, "CPhysicsEnvironment::~CPhysicsEnvironment", vphysicsLoader.GetModule(), Symbols::CPhysicsEnvironment_D2Sym, (void*)hook_CPhysicsEnvironment_D2, m_pID);
	Detour::Create(&detour_CPhysicsEnvironment_DestroyObject, "CPhysicsEnvironment::DestroyObject", vphysicsLoader.GetModule(), Symbols::CPhysicsEnvironment_DestroyObjectSym, (void*)hook_CPhysicsEnvironment_DestroyObject, m_pID);
	Detour::Create(&detour_CPhysicsEnvironment_CreateSphereObject, "CPhysicsEnvironment::CreateSphereObject", vphysicsLoader.GetModule(), Symbols::CPhysicsEnvironment_CreateSphereObjectSym, (void*)hook_CPhysicsEnvironment_CreateSphereObject, m_pID);
	Detour::Create(&detour_CPhysicsEnvironment_UnserializeObjectFromBuffer, "CPhysicsEnvironment::UnserializeObjectFromBuffer", vphysicsLoader.GetModule(), Symbols::CPhysicsEnvironment_UnserializeObjectFromBufferSym, (void*)hook_CPhysicsEnvironment_UnserializeObjectFromBuffer, m_pID);
	Detour::Create(&detour_CPhysicsEnvironment_CreatePolyObjectStatic, "CPhysicsEnvironment::CreatePolyObjectStatic", vphysicsLoader.GetModule(), Symbols::CPhysicsEnvironment_CreatePolyObjectStaticSym, (void*)hook_CPhysicsEnvironment_CreatePolyObjectStatic, m_pID);
	Detour::Create(&detour_CPhysicsEnvironment_CreatePolyObject, "CPhysicsEnvironment::CreatePolyObject", vphysicsLoader.GetModule(), Symbols::CPhysicsEnvironment_CreatePolyObjectSym, (void*)hook_CPhysicsEnvironment_CreatePolyObject, m_pID);
	Detour::Create(&detour_CPhysicsEnvironment_Restore, "CPhysicsEnvironment::Restore", vphysicsLoader.GetModule(), Symbols::CPhysicsEnvironment_RestoreSym, (void*)hook_CPhysicsEnvironment_Restore, m_pID);
	Detour::Create(&detour_CPhysicsEnvironment_TransferObject, "CPhysicsEnvironment::TransferObject", vphysicsLoader.GetModule(), Symbols::CPhysicsEnvironment_TransferObjectSym, (void*)hook_CPhysicsEnvironment_TransferObject, m_pID);

	// Fail closed: stale or unregistered objects must never be reported valid.
	if (!detour_CPhysicsEnvironment_C2.IsValid() ||
		!detour_CPhysicsEnvironment_D2.IsValid() ||
		!detour_CPhysicsEnvironment_DestroyObject.IsValid() ||
		!detour_CPhysicsEnvironment_CreateSphereObject.IsValid() ||
		!detour_CPhysicsEnvironment_UnserializeObjectFromBuffer.IsValid() ||
		!detour_CPhysicsEnvironment_CreatePolyObjectStatic.IsValid() ||
		!detour_CPhysicsEnvironment_CreatePolyObject.IsValid() ||
		!detour_CPhysicsEnvironment_Restore.IsValid() ||
		!detour_CPhysicsEnvironment_TransferObject.IsValid())
	{
		Warning(PROJECT_NAME " - physenv: Required physics-object tracking hooks failed; IsPhysicsObjectValid optimization was not enabled.\n");
		return;
	}

	SourceSDK::FactoryLoader serverLoader("server");
	Detour::Create(&detour_GMod_Util_IsPhysicsObjectValid, "GMod::Util::IsPhysicsObjectValid", serverLoader.GetModule(), Symbols::GMod_Util_IsPhysicsObjectValidSym, (void*)hook_GMod_Util_IsPhysicsObjectValid, m_pID);
}

void CPhysEnvModule::Shutdown()
{
	g_pValidObjects.clear();
	g_pObjectsByEnvironment.clear();
	g_pEnvironmentByObject.clear();
	g_pPhysics = nullptr;
}
