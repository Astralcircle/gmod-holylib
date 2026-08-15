#define PHYSENV_C 1
#if CUSTOM_VPHYSICS_BUILD
#include "physics_holylib.h"
#endif
#include "LuaInterface.h"
#include "module.h"
#include "lua.h"
#include <vphysics_interface.h>
#include <detouring/classproxy.hpp>
#include "tier1/tier1.h"
#define DLL_TOOLS
#include "detours.h"

#if CUSTOM_VPHYSICS_BUILD
#define private public
#include "physics_environment.h"
#undef private
#include "physics_object.h"
#endif

#if !defined(CUSTOM_VPHYSICS_BUILD)
#undef IVP_IF
#include "ivp_old/ivp_classes.h"
#include "ivp_old/ivp_types.h"
#include "ivp_old/cphysicsenvironment.h"
#include "ivp_old/cphysicsobject.h"
#endif

#if !defined(CUSTOM_VPHYSICS_BUILD)
using CPhysicsEnvironment = GMODSDK::CPhysicsEnvironment;
using CPhysicsObject = GMODSDK::CPhysicsObject;
#endif

#include "tier0/memdbgon.h"

class CPhysEnvModule : public IModule
{
public:
	void Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn) override;
	void LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit) override;
	void LuaShutdown(GarrysMod::Lua::ILuaInterface* pLua) override;
	void InitDetour(bool bPreServer) override;
	void Shutdown() override;
	const char* Name() override { return "physenv"; };
	int Compatibility() override { return LINUX32 | LINUX64; };
};

static CPhysEnvModule g_pPhysEnvModule;
IModule* pPhysEnvModule = &g_pPhysEnvModule;

IPhysics* g_pPhysics = nullptr;

class CPhysicsCollisionSet : public IPhysicsCollisionSet
{
	~CPhysicsCollisionSet() {}
private:
	unsigned int m_bits[32];
};

class IVP_VHash_Store;
class CPhysicsInterface : public CTier1AppSystem<IPhysics>
{
public:
	CUtlVector<IPhysicsEnvironment *>	m_envList;
	CUtlVector<CPhysicsCollisionSet>	m_collisionSets;
	IVP_VHash_Store						*m_pCollisionSetHash;
};

// ------------------------------------------------------------------
// Object tracking for IsPhysicsObjectValid optimization
// ------------------------------------------------------------------

struct ILuaPhysicsEnvironment;

static unordered_map<IPhysicsEnvironment*, ILuaPhysicsEnvironment*> g_pEnvironmentToLua;
static unordered_map<IPhysicsObject*, ILuaPhysicsEnvironment*> g_pObjects;

struct ILuaPhysicsEnvironment
{
	ILuaPhysicsEnvironment(IPhysicsEnvironment* env)
	{
		pEnvironment = env;

		int iCount;
		IPhysicsObject** pList = (IPhysicsObject**)pEnvironment->GetObjectList(&iCount);
		for (int i = 0; i < iCount; ++i)
		{
			RegisterObject(pList[i]);
		}
	}

	~ILuaPhysicsEnvironment()
	{
		for (IPhysicsObject* pObject : pObjects)
		{
			auto it = g_pObjects.find(pObject);
			if (it != g_pObjects.end())
				g_pObjects.erase(it);
		}
		pObjects.clear();
	}

	inline void RegisterObject(IPhysicsObject* pObject)
	{
		auto it = pObjects.find(pObject);
		if (it == pObjects.end())
			pObjects.insert(pObject);

		auto it2 = g_pObjects.find(pObject);
		if (it2 == g_pObjects.end())
			g_pObjects[pObject] = this;
	}

	inline void UnregisterObject(IPhysicsObject* pObject)
	{
		auto it = pObjects.find(pObject);
		if (it != pObjects.end())
			pObjects.erase(it);

		auto it2 = g_pObjects.find(pObject);
		if (it2 != g_pObjects.end())
			g_pObjects.erase(it2);
	}

	unordered_set<IPhysicsObject*> pObjects;
	IPhysicsEnvironment* pEnvironment = nullptr;
};

static inline ILuaPhysicsEnvironment* RegisterPhysicsEnvironment(IPhysicsEnvironment* pEnv)
{
	auto it = g_pEnvironmentToLua.find(pEnv);
	if (it == g_pEnvironmentToLua.end())
	{
		ILuaPhysicsEnvironment* pLuaEnv = new ILuaPhysicsEnvironment(pEnv);
		g_pEnvironmentToLua[pEnv] = pLuaEnv;
		return pLuaEnv;
	}
	return it->second;
}

static inline void UnregisterPhysicsEnvironment(IPhysicsEnvironment* pEnv)
{
	auto it = g_pEnvironmentToLua.find(pEnv);
	if (it != g_pEnvironmentToLua.end())
	{
		delete it->second;
		g_pEnvironmentToLua.erase(it);
	}
}

static inline bool IsRegisteredPhysicsObject(IPhysicsObject* pObject)
{
	auto it = g_pObjects.find(pObject);
	return it != g_pObjects.end();
}

static inline ILuaPhysicsEnvironment* GetPhysicsEnvironment(IPhysicsEnvironment* pEnv)
{
	auto it = g_pEnvironmentToLua.find(pEnv);
	if (it != g_pEnvironmentToLua.end())
		return it->second;
	return nullptr;
}

// ------------------------------------------------------------------
// CPhysicsEnvironment detours to keep g_pObjects in sync
// ------------------------------------------------------------------

static Detouring::Hook detour_CPhysicsEnvironment_DestroyObject;
void hook_CPhysicsEnvironment_DestroyObject(GMODSDK::CPhysicsEnvironment* pEnvironment, IPhysicsObject* pObject)
{
	ILuaPhysicsEnvironment* pLuaEnvironment = nullptr;
	{
		auto it = g_pObjects.find(pObject);
		if (it != g_pObjects.end())
			pLuaEnvironment = it->second;
	}

	if (!pLuaEnvironment || !pLuaEnvironment->pEnvironment)
	{
		detour_CPhysicsEnvironment_DestroyObject.GetTrampoline<Symbols::CPhysicsEnvironment_DestroyObject>()(pEnvironment, pObject);
		return;
	}

	CPhysicsEnvironment* pEnv = (CPhysicsEnvironment*)pLuaEnvironment->pEnvironment;
	int foundIndex = -1;
	for (int i = pEnv->m_objects.Count(); --i >= 0; )
		if (pEnv->m_objects[i] == pObject)
			foundIndex = i;

	if (foundIndex == -1)
	{
		detour_CPhysicsEnvironment_DestroyObject.GetTrampoline<Symbols::CPhysicsEnvironment_DestroyObject>()(pEnvironment, pObject);
		return;
	}

#if ARCHITECTURE_X86
	pEnv->m_objects.FastRemove(foundIndex);
	pLuaEnvironment->UnregisterObject(pObject);

	GMODSDK::CPhysicsObject* pPhysics = static_cast<GMODSDK::CPhysicsObject*>(pObject);
	pPhysics->AddCallbackFlags(CALLBACK_MARKED_FOR_DELETE);

	if (foundIndex > pEnv->m_lastObjectThisTick)
		pPhysics->ForceSilentDelete();

	if (pEnv->m_inSimulation || pEnv->m_queueDeleteObject)
	{
		pEnv->m_deadObjects.AddToTail(pObject);
	}
	else
	{
		pEnv->m_pSleepEvents->DeleteObject(pPhysics);
		delete pObject;
	}
#else
	detour_CPhysicsEnvironment_DestroyObject.GetTrampoline<Symbols::CPhysicsEnvironment_DestroyObject>()(pEnv, pObject);
	pLuaEnvironment->UnregisterObject(pObject);
#endif
}

static Detouring::Hook detour_CPhysicsEnvironment_Restore;
bool hook_CPhysicsEnvironment_Restore(IPhysicsEnvironment* pEnv, physrestoreparams_t const& params)
{
	bool bSuccess = detour_CPhysicsEnvironment_Restore.GetTrampoline<Symbols::CPhysicsEnvironment_Restore>()(pEnv, params);
	if (bSuccess && params.type == PIID_IPHYSICSOBJECT)
	{
		ILuaPhysicsEnvironment* pLuaEnv = GetPhysicsEnvironment(pEnv);
		if (pLuaEnv)
			pLuaEnv->RegisterObject((IPhysicsObject*)(*params.ppObject));
	}
	return bSuccess;
}

static Detouring::Hook detour_CPhysicsEnvironment_TransferObject;
bool hook_CPhysicsEnvironment_TransferObject(IPhysicsEnvironment* pEnv, IPhysicsObject* pObject, IPhysicsEnvironment* pDestinationEnvironment)
{
	bool bSuccess = detour_CPhysicsEnvironment_TransferObject.GetTrampoline<Symbols::CPhysicsEnvironment_TransferObject>()(pEnv, pObject, pDestinationEnvironment);
	if (bSuccess)
	{
		ILuaPhysicsEnvironment* pLuaEnv = GetPhysicsEnvironment(pEnv);
		ILuaPhysicsEnvironment* pDestLuaEnv = GetPhysicsEnvironment(pDestinationEnvironment);
		if (pLuaEnv && pDestLuaEnv)
		{
			pLuaEnv->UnregisterObject(pObject);
			pDestLuaEnv->RegisterObject(pObject);
		}
	}
	return bSuccess;
}

static Detouring::Hook detour_CPhysicsEnvironment_CreateSphereObject;
IPhysicsObject* hook_CPhysicsEnvironment_CreateSphereObject(IPhysicsEnvironment* pEnv, float radius, int materialIndex, const Vector& position, const QAngle& angles, objectparams_t* pParams, bool isStatic)
{
	IPhysicsObject* pObject = detour_CPhysicsEnvironment_CreateSphereObject.GetTrampoline<Symbols::CPhysicsEnvironment_CreateSphereObject>()(pEnv, radius, materialIndex, position, angles, pParams, isStatic);
	if (pObject)
	{
		ILuaPhysicsEnvironment* pLuaEnv = GetPhysicsEnvironment(pEnv);
		if (pLuaEnv)
			pLuaEnv->RegisterObject(pObject);
	}
	return pObject;
}

static Detouring::Hook detour_CPhysicsEnvironment_UnserializeObjectFromBuffer;
IPhysicsObject* hook_CPhysicsEnvironment_UnserializeObjectFromBuffer(IPhysicsEnvironment* pEnv, void* pGameData, unsigned char* pBuffer, unsigned int bufferSize, bool enableCollisions)
{
	IPhysicsObject* pObject = detour_CPhysicsEnvironment_UnserializeObjectFromBuffer.GetTrampoline<Symbols::CPhysicsEnvironment_UnserializeObjectFromBuffer>()(pEnv, pGameData, pBuffer, bufferSize, enableCollisions);
	if (pObject)
	{
		ILuaPhysicsEnvironment* pLuaEnv = GetPhysicsEnvironment(pEnv);
		if (pLuaEnv)
			pLuaEnv->RegisterObject(pObject);
	}
	return pObject;
}

static Detouring::Hook detour_CPhysicsEnvironment_CreatePolyObjectStatic;
IPhysicsObject* hook_CPhysicsEnvironment_CreatePolyObjectStatic(IPhysicsEnvironment* pEnv, const CPhysCollide* pCollisionModel, int materialIndex, const Vector& position, const QAngle& angles, objectparams_t* pParams)
{
	IPhysicsObject* pObject = detour_CPhysicsEnvironment_CreatePolyObjectStatic.GetTrampoline<Symbols::CPhysicsEnvironment_CreatePolyObjectStatic>()(pEnv, pCollisionModel, materialIndex, position, angles, pParams);
	if (pObject)
	{
		ILuaPhysicsEnvironment* pLuaEnv = GetPhysicsEnvironment(pEnv);
		if (pLuaEnv)
			pLuaEnv->RegisterObject(pObject);
	}
	return pObject;
}

static Detouring::Hook detour_CPhysicsEnvironment_CreatePolyObject;
IPhysicsObject* hook_CPhysicsEnvironment_CreatePolyObject(IPhysicsEnvironment* pEnv, const CPhysCollide* pCollisionModel, int materialIndex, const Vector& position, const QAngle& angles, objectparams_t* pParams)
{
	IPhysicsObject* pObject = detour_CPhysicsEnvironment_CreatePolyObject.GetTrampoline<Symbols::CPhysicsEnvironment_CreatePolyObject>()(pEnv, pCollisionModel, materialIndex, position, angles, pParams);
	if (pObject)
	{
		ILuaPhysicsEnvironment* pLuaEnv = GetPhysicsEnvironment(pEnv);
		if (pLuaEnv)
			pLuaEnv->RegisterObject(pObject);
	}
	return pObject;
}

static Detouring::Hook detour_CPhysicsEnvironment_D2;
void hook_CPhysicsEnvironment_D2(IPhysicsEnvironment* pEnv)
{
	UnregisterPhysicsEnvironment(pEnv);
	detour_CPhysicsEnvironment_D2.GetTrampoline<Symbols::CPhysicsEnvironment_D2>()(pEnv);
}

static Detouring::Hook detour_CPhysicsEnvironment_C2;
void hook_CPhysicsEnvironment_C2(IPhysicsEnvironment* pEnv)
{
	detour_CPhysicsEnvironment_C2.GetTrampoline<Symbols::CPhysicsEnvironment_C2>()(pEnv);
	RegisterPhysicsEnvironment(pEnv);
}

// ------------------------------------------------------------------
// IsPhysicsObjectValid optimization
// ------------------------------------------------------------------

static Detouring::Hook detour_GMod_Util_IsPhysicsObjectValid;
static bool hook_GMod_Util_IsPhysicsObjectValid(IPhysicsObject* pObject)
{
	if (!pObject)
		return false;

#if CUSTOM_VPHYSICS_BUILD
	if (g_pPhysicsHolyLib)
	{
		return g_pPhysicsHolyLib->IsValidObject(pObject);
	}
	else
#endif
	{
		return IsRegisteredPhysicsObject(pObject);
	}
}

// ------------------------------------------------------------------
// Module interface
// ------------------------------------------------------------------

void CPhysEnvModule::Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn)
{
	if (appfn[0])
	{
		g_pPhysics = (IPhysics*)appfn[0](VPHYSICS_INTERFACE_VERSION, nullptr);
	}
	else
	{
		SourceSDK::FactoryLoader vphysics_loader("vphysics");
		g_pPhysics = vphysics_loader.GetInterface<IPhysics>(VPHYSICS_INTERFACE_VERSION);
	}

	Detour::CheckValue("get interface", "g_pPhysics", g_pPhysics != nullptr);
}

void CPhysEnvModule::LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit)
{
	if (bServerInit)
		return;

	if (pLua == g_Lua)
	{
		CPhysicsInterface* pPhys = (CPhysicsInterface*)g_pPhysics;
		FOR_EACH_VEC(pPhys->m_envList, i)
		{
			RegisterPhysicsEnvironment(pPhys->m_envList[i]);
		}
	}
}

void CPhysEnvModule::LuaShutdown(GarrysMod::Lua::ILuaInterface* pLua)
{
}

void CPhysEnvModule::InitDetour(bool bPreServer)
{
	if (bPreServer)
		return;

	if (!g_pModuleManager.IsUsingGhostInj())
	{
		Warning(PROJECT_NAME " - physenv: we weren't loaded early enough! use the ghostinj and ensure that holylib is loaded properly!\n");
		return;
	}

	SourceSDK::FactoryLoader vphysics_loader("vphysics");

	Detour::Create(
		&detour_CPhysicsEnvironment_DestroyObject, "CPhysicsEnvironment::DestroyObject",
		vphysics_loader.GetModule(), Symbols::CPhysicsEnvironment_DestroyObjectSym,
		(void*)hook_CPhysicsEnvironment_DestroyObject, m_pID
	);

	Detour::Create(
		&detour_CPhysicsEnvironment_Restore, "CPhysicsEnvironment::Restore",
		vphysics_loader.GetModule(), Symbols::CPhysicsEnvironment_RestoreSym,
		(void*)hook_CPhysicsEnvironment_Restore, m_pID
	);

	Detour::Create(
		&detour_CPhysicsEnvironment_TransferObject, "CPhysicsEnvironment::TransferObject",
		vphysics_loader.GetModule(), Symbols::CPhysicsEnvironment_TransferObjectSym,
		(void*)hook_CPhysicsEnvironment_TransferObject, m_pID
	);

	Detour::Create(
		&detour_CPhysicsEnvironment_CreateSphereObject, "CPhysicsEnvironment::CreateSphereObject",
		vphysics_loader.GetModule(), Symbols::CPhysicsEnvironment_CreateSphereObjectSym,
		(void*)hook_CPhysicsEnvironment_CreateSphereObject, m_pID
	);

	Detour::Create(
		&detour_CPhysicsEnvironment_UnserializeObjectFromBuffer, "CPhysicsEnvironment::UnserializeObjectFromBuffer",
		vphysics_loader.GetModule(), Symbols::CPhysicsEnvironment_UnserializeObjectFromBufferSym,
		(void*)hook_CPhysicsEnvironment_UnserializeObjectFromBuffer, m_pID
	);

	Detour::Create(
		&detour_CPhysicsEnvironment_CreatePolyObjectStatic, "CPhysicsEnvironment::CreatePolyObjectStatic",
		vphysics_loader.GetModule(), Symbols::CPhysicsEnvironment_CreatePolyObjectStaticSym,
		(void*)hook_CPhysicsEnvironment_CreatePolyObjectStatic, m_pID
	);

	Detour::Create(
		&detour_CPhysicsEnvironment_CreatePolyObject, "CPhysicsEnvironment::CreatePolyObject",
		vphysics_loader.GetModule(), Symbols::CPhysicsEnvironment_CreatePolyObjectSym,
		(void*)hook_CPhysicsEnvironment_CreatePolyObject, m_pID
	);

	Detour::Create(
		&detour_CPhysicsEnvironment_D2, "CPhysicsEnvironment::~CPhysicsEnvironment",
		vphysics_loader.GetModule(), Symbols::CPhysicsEnvironment_D2Sym,
		(void*)hook_CPhysicsEnvironment_D2, m_pID
	);

	Detour::Create(
		&detour_CPhysicsEnvironment_C2, "CPhysicsEnvironment::CPhysicsEnvironment",
		vphysics_loader.GetModule(), Symbols::CPhysicsEnvironment_C2Sym,
		(void*)hook_CPhysicsEnvironment_C2, m_pID
	);

	SourceSDK::FactoryLoader server_loader("server");
	Detour::Create(
		&detour_GMod_Util_IsPhysicsObjectValid, "GMod::Util::IsPhysicsObjectValid",
		server_loader.GetModule(), Symbols::GMod_Util_IsPhysicsObjectValidSym,
		(void*)hook_GMod_Util_IsPhysicsObjectValid, m_pID
	);

	if (!detour_CPhysicsEnvironment_DestroyObject.IsValid() || !detour_CPhysicsEnvironment_CreatePolyObject.IsValid() || !detour_CPhysicsEnvironment_CreatePolyObjectStatic.IsValid())
	{
		detour_GMod_Util_IsPhysicsObjectValid.Disable();
		detour_GMod_Util_IsPhysicsObjectValid.Destroy();
		Warning(PROJECT_NAME " - physenv: Removed GMod::Util::IsPhysicsObjectValid due to other detours failing to hook!\n");
	}
}

void CPhysEnvModule::Shutdown()
{
	for (auto it = g_pEnvironmentToLua.begin(); it != g_pEnvironmentToLua.end(); ++it)
	{
		Msg(PROJECT_NAME " - physenv: Found remaining environment! (%p - %p)\n", it->first, it->second);
	}
}
