// Minimal version: only optimize IsPhysicsObjectValid using hash table
#include "module.h"
#include <unordered_map>
#include <unordered_set>
#include <vphysics_interface.h>
#include <detouring/classproxy.hpp>
#include "detours.h"
#include "tier1/tier1.h"

#if CUSTOM_VPHYSICS_BUILD
#define private public
#include "physics_environment.h"
#undef private
#include "physics_object.h"
#endif

#if defined(PHYSENV_INCLUDEIVPFALLBACK) || !defined(CUSTOM_VPHYSICS_BUILD)
#include "ivp_old/ivp_classes.h"
#include "ivp_old/ivp_types.h"
#include "ivp_old/cphysicsenvironment.h"
#include "ivp_old/cphysicsobject.h"
#endif

#if !defined(CUSTOM_VPHYSICS_BUILD)
using CPhysicsEnvironment = GMODSDK::CPhysicsEnvironment;
using CPhysicsObject = GMODSDK::CPhysicsObject;
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// ------------------------------------------------------------------
// Minimal module to set up the hook
// ------------------------------------------------------------------
class CPhysEnvModule : public IModule
{
public:
	void Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn) override;
	void LuaInit(GarrysMod::Lua::ILuaInterface* pLua, bool bServerInit) override {}
	void LuaShutdown(GarrysMod::Lua::ILuaInterface* pLua) override {}
	void InitDetour(bool bPreServer) override;
	void Shutdown() override;
	const char* Name() override { return "physenv_opt"; };
	int Compatibility() override { return LINUX32 | LINUX64; };
};

static CPhysEnvModule g_pPhysEnvModule;
IModule* pPhysEnvModule = &g_pPhysEnvModule;

// ------------------------------------------------------------------
// Data structures to track physics objects and environments
// ------------------------------------------------------------------
struct ILuaPhysicsEnvironment;

static std::unordered_map<IPhysicsEnvironment*, ILuaPhysicsEnvironment*> g_pEnvironmentToLua;
static std::unordered_map<IPhysicsObject*, ILuaPhysicsEnvironment*> g_pObjects; // all objects

struct ILuaPhysicsEnvironment
{
	ILuaPhysicsEnvironment(IPhysicsEnvironment* env) : pEnvironment(env) {}

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
		if (pObjects.insert(pObject).second)
			g_pObjects[pObject] = this;
	}

	inline void UnregisterObject(IPhysicsObject* pObject)
	{
		auto it = pObjects.find(pObject);
		if (it != pObjects.end())
		{
			pObjects.erase(it);
			g_pObjects.erase(pObject);
		}
	}

	inline bool HasObject(IPhysicsObject* pObject) const
	{
		return pObjects.find(pObject) != pObjects.end();
	}

	IPhysicsEnvironment* pEnvironment = nullptr;
	std::unordered_set<IPhysicsObject*> pObjects;
};

static inline ILuaPhysicsEnvironment* GetPhysicsEnvironment(IPhysicsEnvironment* pEnv)
{
	auto it = g_pEnvironmentToLua.find(pEnv);
	return (it != g_pEnvironmentToLua.end()) ? it->second : nullptr;
}

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

static inline ILuaPhysicsEnvironment* GetPhysicsObjectLuaEnvironment(IPhysicsObject* pObject)
{
	auto it = g_pObjects.find(pObject);
	return (it != g_pObjects.end()) ? it->second : nullptr;
}

static inline bool IsRegisteredPhysicsObject(IPhysicsObject* pObject)
{
	return g_pObjects.find(pObject) != g_pObjects.end();
}

// ------------------------------------------------------------------
// Hooks to track object creation / destruction
// ------------------------------------------------------------------
#if PHYSENV_INCLUDEIVPFALLBACK

static Detouring::Hook detour_CPhysicsEnvironment_DestroyObject;

void hook_CPhysicsEnvironment_DestroyObject(GMODSDK::CPhysicsEnvironment* pEnvironment, IPhysicsObject* pObject)
{
	ILuaPhysicsEnvironment* pLuaEnv = GetPhysicsObjectLuaEnvironment(pObject);
	if (pLuaEnv && pLuaEnv->pEnvironment)
	{
		// Remove from tracking
		pLuaEnv->UnregisterObject(pObject);
	}

	// Call original
	detour_CPhysicsEnvironment_DestroyObject.GetTrampoline<Symbols::CPhysicsEnvironment_DestroyObject>()(pEnvironment, pObject);
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

#endif // PHYSENV_INCLUDEIVPFALLBACK

// ------------------------------------------------------------------
// Hook for GMod::Util::IsPhysicsObjectValid
// ------------------------------------------------------------------
static Detouring::Hook detour_GMod_Util_IsPhysicsObjectValid;
static bool hook_GMod_Util_IsPhysicsObjectValid(IPhysicsObject* pObject)
{
	if (!pObject)
		return false;

#if CUSTOM_VPHYSICS_BUILD
	if (g_pPhysicsHolyLib && g_pPhysicsHolyLib->IsValidObject(pObject))
		return true;
#endif

	return IsRegisteredPhysicsObject(pObject);
}

// ------------------------------------------------------------------
// Module implementation
// ------------------------------------------------------------------
void CPhysEnvModule::Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn)
{
	// Minimal initialization - just get needed interfaces if any
}

void CPhysEnvModule::InitDetour(bool bPreServer)
{
	if (bPreServer)
		return; // nothing to do pre-server

#if PHYSENV_INCLUDEIVPFALLBACK
	SourceSDK::FactoryLoader vphysics_loader("vphysics");

	// Hook object creation/destruction to track objects
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
#endif

	// Hook GMod's IsPhysicsObjectValid
	SourceSDK::FactoryLoader server_loader("server");
	Detour::Create(
		&detour_GMod_Util_IsPhysicsObjectValid, "GMod::Util::IsPhysicsObjectValid",
		server_loader.GetModule(), Symbols::GMod_Util_IsPhysicsObjectValidSym,
		(void*)hook_GMod_Util_IsPhysicsObjectValid, m_pID
	);
}

void CPhysEnvModule::Shutdown()
{
	// Clean up detours
	detour_GMod_Util_IsPhysicsObjectValid.Disable();
	detour_GMod_Util_IsPhysicsObjectValid.Destroy();

#if PHYSENV_INCLUDEIVPFALLBACK
	detour_CPhysicsEnvironment_DestroyObject.Disable();
	detour_CPhysicsEnvironment_DestroyObject.Destroy();
	detour_CPhysicsEnvironment_Restore.Disable();
	detour_CPhysicsEnvironment_Restore.Destroy();
	detour_CPhysicsEnvironment_TransferObject.Disable();
	detour_CPhysicsEnvironment_TransferObject.Destroy();
	detour_CPhysicsEnvironment_CreateSphereObject.Disable();
	detour_CPhysicsEnvironment_CreateSphereObject.Destroy();
	detour_CPhysicsEnvironment_UnserializeObjectFromBuffer.Disable();
	detour_CPhysicsEnvironment_UnserializeObjectFromBuffer.Destroy();
	detour_CPhysicsEnvironment_CreatePolyObjectStatic.Disable();
	detour_CPhysicsEnvironment_CreatePolyObjectStatic.Destroy();
	detour_CPhysicsEnvironment_CreatePolyObject.Disable();
	detour_CPhysicsEnvironment_CreatePolyObject.Destroy();
	detour_CPhysicsEnvironment_D2.Disable();
	detour_CPhysicsEnvironment_D2.Destroy();
	detour_CPhysicsEnvironment_C2.Disable();
	detour_CPhysicsEnvironment_C2.Destroy();
#endif

	// Clean up any remaining environments
	for (auto& pair : g_pEnvironmentToLua)
	{
		delete pair.second;
	}
	g_pEnvironmentToLua.clear();
	g_pObjects.clear();
}
