// If set it will include code to fallback when it couldn't replace IVP, in cases where for example holylib wasn't loaded by the ghostinj.dll
#define PHYSENV_INCLUDEIVPFALLBACK 1
#define PHYSENV_C 1
#if CUSTOM_VPHYSICS_BUILD
#include "physics_holylib.h"
#endif
#include "LuaInterface.h"
#include "module.h"
#include "lua.h"
#include <chrono>
//#include "sourcesdk/ivp_classes.h"
#include <vphysics_interface.h>
//#include "sourcesdk/cphysicsobject.h"
//#include "sourcesdk/cphysicsenvironment.h"
#include <detouring/classproxy.hpp>
#include "player.h"
#include "tier1/tier1.h"
#define DLL_TOOLS
#include "detours.h"
#include "tier0/icommandline.h"

#if CUSTOM_VPHYSICS_BUILD
#define private public
#include "physics_environment.h" // has to be before physics_object.h
#undef private
#include "physics_object.h"
#endif

#if defined(PHYSENV_INCLUDEIVPFALLBACK) || !defined(CUSTOM_VPHYSICS_BUILD)
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

// memdbgon must be the last include file in a .cpp file!!!
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

enum class IVP_SkipType {
	//IVP_NoCall = -2, // Were not in our expected simulation, so don't handle anything.
	IVP_None = -1, // We didn't ask lua yet. So make the lua call.

	// Lua Enums
	IVP_NoSkip,
	IVP_SkipImpact,
	IVP_SkipSimulation,
	IVP_LastSkipType,
};
static IVP_SkipType pCurrentSkipType = IVP_SkipType::IVP_None;

struct CPhysicsEnvironmentData;
static inline CPhysicsEnvironmentData* RegisterPhysicsEnvironment(IPhysicsEnvironment* pEnv);
static inline void UnregisterPhysicsEnvironment(IPhysicsEnvironment* pEnv);
void CheckPhysicsLag(CPhysicsObject* pObject1, CPhysicsObject* pObject2);

static bool g_bReplacedIVP = false;
static thread_local auto pCurrentTime = std::chrono::high_resolution_clock::now();
static double pCurrentLagThreadshold = 100; // 100 ms
static thread_local bool g_pIsInPhysicsLagCall = false;

// RaphaelIT7 (ToDo): We should probably use a std::shared_mutex
static unordered_map<IPhysicsEnvironment*, CPhysicsEnvironmentData*> g_pEnvironmentData;
static unordered_map<IPhysicsObject*, CPhysicsEnvironmentData*> g_pObjects; // contains all IPhysicsObject that exist

// RaphaelIT7 (ToDo):
// It's driving me insane, somehow it's at 160 but there is A LOT that is supposed to be before it....
// The entire layout of IVP_Real_Object is a mess and I don't want to figure that out rn
static bool g_bIsValidClientData = false;
static inline CPhysicsObject* GetClientDataFromGModObject(GMODSDK::IVP_Real_Object* pObj)
{
	if (!pObj)
		return nullptr;

	// Offset can easily be found in IVP_Real_Object constructor (It's the very last instruction load & store)
	CPhysicsObject* pPhys = (CPhysicsObject*)*(void**)((char*)pObj + 0x0A0);

	// We only check ones and then pray its fine
	// It's highly unlikely that we read the wrong offset yet still find a entry from a garbage pointer
	// This is as the first object is highly unlikely to trigger all the way down to trigger CheckPhysicsLag
	// We cannot verify from inside CheckPhysicsLag as it may be called while in CreatePhysicsObject setting up the fresh CPhysicsObject
	if (!g_bIsValidClientData)
	{
		// If the offsets in IVP_Real_Object change then client_data access may return junk!
		if (g_pObjects.find(pPhys) == g_pObjects.end())
			Error("Garbage CPhysicsObject! This should NEVER happen! (%p)\n", pPhys);
		else
			g_bIsValidClientData = true;
	}

	return pPhys;
}

#if CUSTOM_VPHYSICS_BUILD
#define IVPHolyLib_OVERRIDE override
#else
#define IVPHolyLib_OVERRIDE
#endif
class IVPHolyLib
#if CUSTOM_VPHYSICS_BUILD
	: public IVP_HolyLib_Callbacks
#endif
{
public:
	~IVPHolyLib() IVPHolyLib_OVERRIDE {};

	bool CheckLag(const char*, void* pObject1, void* pObject2) IVPHolyLib_OVERRIDE
	{
		CheckPhysicsLag(static_cast<CPhysicsObject*>(pObject1), static_cast<CPhysicsObject*>(pObject2));

		return ShouldSkip();
	}

	void OnEnvironmentCreated(IPhysicsEnvironment* pEnvironment) IVPHolyLib_OVERRIDE
	{
		RegisterPhysicsEnvironment(pEnvironment);
	}

	void OnEnvironmentDestroyed(IPhysicsEnvironment* pEnvironment) IVPHolyLib_OVERRIDE
	{
		UnregisterPhysicsEnvironment(pEnvironment);
	}

	void SimulationBegin() IVPHolyLib_OVERRIDE
	{
		pCurrentTime = std::chrono::high_resolution_clock::now();
		pCurrentSkipType = IVP_SkipType::IVP_None;
	}

	void SimulationFinish() IVPHolyLib_OVERRIDE
	{
		pCurrentSkipType = IVP_SkipType::IVP_None;
	}

	bool ShouldSkip() IVPHolyLib_OVERRIDE
	{
		return ((int)pCurrentSkipType) > 0;
	}

	// Preserve the callback interface while allowing normal recheck_ov_element processing.
	void ThrowRecheckOVWarning() IVPHolyLib_OVERRIDE {}
	bool ShouldSkipRecheck_ov_element() IVPHolyLib_OVERRIDE { return false; }
};

static IVPHolyLib g_pIVPHolyLib;

GMODPush_LuaClass(IPhysicsObject, GarrysMod::Lua::Type::PhysObj);

void CheckPhysicsLag(CPhysicsObject* pObject1, CPhysicsObject* pObject2)
{
	if (pCurrentSkipType != IVP_SkipType::IVP_None || g_pIsInPhysicsLagCall) // We already have a skip type.
		return;

	auto pTime = std::chrono::high_resolution_clock::now();
	auto pSimulationTime = std::chrono::duration_cast<std::chrono::milliseconds>(pTime - pCurrentTime).count();
	if (pSimulationTime > pCurrentLagThreadshold)
	{
		if (Lua::PushHook("HolyLib:OnPhysicsLag"))
		{
			g_Lua->PushNumber((double)pSimulationTime);

			if (pObject1)
			{
				Push_IPhysicsObject(g_Lua, pObject1);
			} else {
				g_Lua->PushNil();
			}

			if (pObject2)
			{
				Push_IPhysicsObject(g_Lua, pObject2);
			} else {
				g_Lua->PushNil();
			}


			g_pIsInPhysicsLagCall = true;
			pCurrentTime = std::chrono::high_resolution_clock::now(); // Update timer.
			if (g_Lua->CallFunctionProtected(4, 1, true))
			{
				int pType = (int)g_Lua->GetNumber(-1);
				if (pType >= (int)IVP_SkipType::IVP_LastSkipType || pType < -1)
					pType = (int)IVP_SkipType::IVP_NoSkip; // Invalid value. So we won't do shit.


				pCurrentSkipType = (IVP_SkipType)pType;
				g_Lua->Pop(1);

				if (g_pPhysEnvModule.InDebug() > 2)
					Msg(PROJECT_NAME " - physenv: HolyLib:OnPhysicsLag hook called (%i)\n", (int)pCurrentSkipType);

				//g_pIVPHolyLib.SetShouldSkip(pCurrentSkipType != IVP_SkipType::IVP_None);
			}
			g_pIsInPhysicsLagCall = false;
		}
	}
}

IPhysics* g_pPhysics = nullptr;
static thread_local bool g_pIsInPostPhysicsLagCall = false;
void PostPhysicsLag()
{
	if (pCurrentSkipType != IVP_SkipType::IVP_None && !g_pIsInPostPhysicsLagCall && Lua::PushHook("HolyLib:PostPhysicsLag"))
	{
		auto pTime = std::chrono::high_resolution_clock::now();
		auto pSimulationTime = std::chrono::duration_cast<std::chrono::milliseconds>(pTime - pCurrentTime).count();
		g_Lua->PushNumber((double)pSimulationTime);

		g_pIsInPostPhysicsLagCall = true;
		// VERIFY: What if someone calls IPhysicsEnvironment:Simulate? I mean... it should kinda be fine... right?
		// BUG: I hate that this is hard coded for the main environment... how the heck could one know which environment is lagging+
		bool bPrevious = false;
		CPhysicsEnvironment* pEnvironment = (CPhysicsEnvironment*)g_pPhysics->GetActiveEnvironmentByIndex(0);
		if (pEnvironment)
		{
			bPrevious = pEnvironment->IsInSimulation();
			pEnvironment->m_inSimulation = false; // ToDo: Jolt needs support
		}

		pCurrentTime = std::chrono::high_resolution_clock::now(); // Update timer.
		if (g_Lua->CallFunctionProtected(2, 0, true))
		{
			if (g_pPhysEnvModule.InDebug() > 2)
				Msg(PROJECT_NAME " - physenv: HolyLib:PostPhysicsLag hook called!\n");
		}

		if (pEnvironment)
			pEnvironment->m_inSimulation = bPrevious;

		g_pIsInPostPhysicsLagCall = false;
	}
}

#if PHYSENV_INCLUDEIVPFALLBACK
static bool bIsJoltPhysics = false; // if were using jolt, a lot of things need to change.
class IVP_Mindist;

static Detouring::Hook detour_IVP_Mindist_do_impact;
static void hook_IVP_Mindist_do_impact(IVP_Mindist* mindist)
{
	if (g_pPhysEnvModule.InDebug() > 2)
		Msg(PROJECT_NAME " - physenv: IVP_Mindist::do_impact called! (%i)\n", (int)pCurrentSkipType);

	if (pCurrentSkipType == IVP_SkipType::IVP_SkipImpact)
		return;

	detour_IVP_Mindist_do_impact.GetTrampoline<Symbols::IVP_Mindist_do_impact>()(mindist);
}

static Detouring::Hook detour_IVP_Event_Manager_Standard_simulate_time_events;
static void hook_IVP_Event_Manager_Standard_simulate_time_events(void* eventmanager, void* timemanager, void* environment, GMODSDK::IVP_Time time)
{
	pCurrentTime = std::chrono::high_resolution_clock::now();
	pCurrentSkipType = IVP_SkipType::IVP_None; // Idk if it can happen that something else sets it in the mean time but let's just be sure...

	if (g_pPhysEnvModule.InDebug() > 2)
		Msg(PROJECT_NAME " - physenv: IVP_Event_Manager_Standard::simulate_time_events called!\n");

	detour_IVP_Event_Manager_Standard_simulate_time_events.GetTrampoline<Symbols::IVP_Event_Manager_Standard_simulate_time_events>()(eventmanager, timemanager, environment, time);

	PostPhysicsLag();
	pCurrentSkipType = IVP_SkipType::IVP_None; // Reset it.
}

static Symbols::IVP_Mindist_Base_get_objects func_IVP_Mindist_Base_get_objects;
static Detouring::Hook detour_IVP_Mindist_simulate_time_event;
static void hook_IVP_Mindist_simulate_time_event(GMODSDK::IVP_Mindist* mindist, void* environment)
{
	if (g_pPhysEnvModule.InDebug() > 2)
		Msg(PROJECT_NAME " - physenv: IVP_Mindist::simulate_time_event called! (%i)\n", (int)pCurrentSkipType);


	GMODSDK::IVP_Real_Object* pObjs[2] = {nullptr, nullptr};
	if (func_IVP_Mindist_Base_get_objects)
	{
		func_IVP_Mindist_Base_get_objects(mindist, pObjs);

		CheckPhysicsLag(GetClientDataFromGModObject(pObjs[0]), GetClientDataFromGModObject(pObjs[1]));
	}

	if (pCurrentSkipType == IVP_SkipType::IVP_SkipSimulation)
		return;

	detour_IVP_Mindist_simulate_time_event.GetTrampoline<Symbols::IVP_Mindist_simulate_time_event>()(mindist, environment);
}

static Detouring::Hook detour_IVP_Mindist_update_exact_mindist_events;
static void hook_IVP_Mindist_update_exact_mindist_events(GMODSDK::IVP_Mindist* mindist, GMODSDK::IVP_BOOL allow_hull_conversion, GMODSDK::IVP_MINDIST_EVENT_HINT event_hint)
{
	if (g_pPhysEnvModule.InDebug() > 2)
		Msg(PROJECT_NAME " - physenv: IVP_Mindist::update_exact_mindist_events called! (%i)\n", (int)pCurrentSkipType);

	GMODSDK::IVP_Real_Object* pObjs[2] = {nullptr, nullptr};
	if (func_IVP_Mindist_Base_get_objects)
	{
		func_IVP_Mindist_Base_get_objects(mindist, pObjs);

		CheckPhysicsLag(GetClientDataFromGModObject(pObjs[0]), GetClientDataFromGModObject(pObjs[1]));
	}

	if (pCurrentSkipType == IVP_SkipType::IVP_SkipSimulation)
		return;

	//Msg("Debug3 start %i\n", (int)pCurrentSkipType);
	detour_IVP_Mindist_update_exact_mindist_events.GetTrampoline<Symbols::IVP_Mindist_update_exact_mindist_events>()(mindist, allow_hull_conversion, event_hint);
	//Msg("Debug3 end %i\n", (int)pCurrentSkipType);
}

static Detouring::Hook detour_IVP_Mindist_Minimize_Solver_p_minimize_PP;
static GMODSDK::IVP_MRC_TYPE hook_IVP_Mindist_Minimize_Solver_p_minimize_PP(GMODSDK::IVP_Mindist_Minimize_Solver* mindistMinimizeSolver, const GMODSDK::IVP_Compact_Edge *A, const GMODSDK::IVP_Compact_Edge *B, IVP_Cache_Ledge_Point *m_cache_A, IVP_Cache_Ledge_Point *m_cache_B)
{
	GMODSDK::IVP_Real_Object* pObjs[2] = {nullptr, nullptr};
	if (func_IVP_Mindist_Base_get_objects)
	{
		func_IVP_Mindist_Base_get_objects(mindistMinimizeSolver->mindist, pObjs);

		CheckPhysicsLag(GetClientDataFromGModObject(pObjs[0]), GetClientDataFromGModObject(pObjs[1]));
	}

	return detour_IVP_Mindist_Minimize_Solver_p_minimize_PP.GetTrampoline<Symbols::IVP_Mindist_Minimize_Solver_p_minimize_PP>()(mindistMinimizeSolver, A, B, m_cache_A, m_cache_B);
}

static Detouring::Hook detour_IVP_OV_Element_add_oo_collision;
static void hook_IVP_OV_Element_add_oo_collision(void* ovElement, GMODSDK::IVP_Collision* connector)
{
	if (!connector || !ovElement) // Simple NULL check missing. Yes ovElement can be NULL. Somehow.
		return;

	detour_IVP_OV_Element_add_oo_collision.GetTrampoline<Symbols::IVP_OV_Element_add_oo_collision>()(ovElement, connector);
}

static Detouring::Hook detour_IVP_OV_Element_remove_oo_collision;
static void hook_IVP_OV_Element_remove_oo_collision(void* ovElement, GMODSDK::IVP_Collision* connector)
{
	if (!connector || !ovElement) // Simple NULL check missing. Yes ovElement can be NULL. Somehow.
		return;

	detour_IVP_OV_Element_remove_oo_collision.GetTrampoline<Symbols::IVP_OV_Element_remove_oo_collision>()(ovElement, connector);
}


#endif

bool g_pForceOriginalIVP = false;
#if CUSTOM_VPHYSICS_BUILD
static Detouring::Hook detour_CreateInterface;
static void* hook_CreateInterface(const char *pName, int *pReturnCode)
{
	Msg("vphysics - CreateInterface: %s\n", pName);
	if (g_pForceOriginalIVP)
	{
		Msg("vphysics(original) - CreateInterface: %s\n", pName);
		return detour_CreateInterface.GetTrampoline<CreateInterfaceFn>()(pName, pReturnCode);
	}

	return Sys_GetFactoryThis()(pName, pReturnCode);
}
#endif

LUA_FUNCTION_STATIC(physenv_SetLagThreshold)
{
	pCurrentLagThreadshold = LUA->CheckNumber(1);

	return 0;
}

class CPhysicsInterface : public CTier1AppSystem<IPhysics>
{
public:
	CUtlVector<IPhysicsEnvironment*> m_envList;
};

void CPhysEnvModule::Init(CreateInterfaceFn* appfn, CreateInterfaceFn* gamefn)
{
	if (appfn[0])
	{
		g_pPhysics = (IPhysics*)appfn[0](VPHYSICS_INTERFACE_VERSION, nullptr);
	} else {
		SourceSDK::FactoryLoader vphysics_loader("vphysics");
		g_pPhysics = vphysics_loader.GetInterface<IPhysics>(VPHYSICS_INTERFACE_VERSION);
	}

	Detour::CheckValue("get interface", "g_pPhysics", g_pPhysics != nullptr);
}

#if defined(SYSTEM_WINDOWS) && PHYSENV_INCLUDEIVPFALLBACK
extern void hook_CPhysicsEnvironment_DestroyObject(GMODSDK::CPhysicsEnvironment* pEnvironment, IPhysicsObject* pObject);
class CPhysicsEnvironmentProxy : public Detouring::ClassProxy<IPhysicsEnvironment, CPhysicsEnvironmentProxy> {
public:
	CPhysicsEnvironmentProxy(IPhysicsEnvironment* env) {
		if (Detour::CheckValue("initialize", "CPhysicsEnvironmentProxy", Initialize(env)))
			Detour::CheckValue("CPhysicsEnvironment::DestroyObject", Hook(&IPhysicsEnvironment::DestroyObject, &CPhysicsEnvironmentProxy::DestroyObject));
	}

	void DeInit()
	{
		UnHook(&IPhysicsEnvironment::DestroyObject);
	}

	void DestroyObject(IPhysicsObject* pObject)
	{
		hook_CPhysicsEnvironment_DestroyObject((GMODSDK::CPhysicsEnvironment*)This(), pObject);
	}
};
#endif

#if PHYSENV_INCLUDEIVPFALLBACK
static inline void RegisterPhysicsObject(CPhysicsEnvironmentData* pEnv, IPhysicsObject* pObject);
#endif
struct CPhysicsEnvironmentData
{
	CPhysicsEnvironmentData(IPhysicsEnvironment* env)
	{
		pEnvironment = env;
#if defined(SYSTEM_WINDOWS) && PHYSENV_INCLUDEIVPFALLBACK
		pEnvironmentProxy = std::make_unique<CPhysicsEnvironmentProxy>(env);
#endif

#if PHYSENV_INCLUDEIVPFALLBACK
		int iCount;
		IPhysicsObject** pList = (IPhysicsObject**)pEnvironment->GetObjectList(&iCount);
		for (int i = 0; i < iCount; ++i)
		{
			RegisterPhysicsObject(this, pList[i]);
		}
#endif
	}

	~CPhysicsEnvironmentData()
	{

#if defined(SYSTEM_WINDOWS) && PHYSENV_INCLUDEIVPFALLBACK
		pEnvironmentProxy->DeInit();
#endif
		pEnvironment = nullptr;

#if PHYSENV_INCLUDEIVPFALLBACK
		for (IPhysicsObject* pObject : pObjects)
		{
			auto it = g_pObjects.find(pObject);
			if (it != g_pObjects.end())
				g_pObjects.erase(it);
		}

		pObjects.clear();
#endif

	}

#if PHYSENV_INCLUDEIVPFALLBACK
	inline void RegisterObject(IPhysicsObject* pObject)
	{
		auto it = pObjects.find(pObject);
		if (it == pObjects.end())
			pObjects.insert(pObject);
	}

	inline void UnregisterObject(IPhysicsObject* pObject)
	{
		auto it = pObjects.find(pObject);
		if (it != pObjects.end())
			pObjects.erase(it);
	}
#endif

#if PHYSENV_INCLUDEIVPFALLBACK
	unordered_set<IPhysicsObject*> pObjects;
#endif

	IPhysicsEnvironment* pEnvironment = nullptr;
#if defined(SYSTEM_WINDOWS) && PHYSENV_INCLUDEIVPFALLBACK
	std::unique_ptr<CPhysicsEnvironmentProxy> pEnvironmentProxy = nullptr;
#endif
};

/*
 * If it already exists it won't register it & just returns the existing one.
 */
static inline CPhysicsEnvironmentData* RegisterPhysicsEnvironment(IPhysicsEnvironment* pEnv)
{
	auto it = g_pEnvironmentData.find(pEnv);
	if (it == g_pEnvironmentData.end())
	{
		CPhysicsEnvironmentData* pEnvData = new CPhysicsEnvironmentData(pEnv);
		g_pEnvironmentData[pEnv] = pEnvData;
		return pEnvData;
	}

	return it->second;
}

static inline void UnregisterPhysicsEnvironment(IPhysicsEnvironment* pEnv)
{
	auto it = g_pEnvironmentData.find(pEnv);
	if (it != g_pEnvironmentData.end())
	{
		delete it->second;
		g_pEnvironmentData.erase(it);
	}
}

static inline CPhysicsEnvironmentData* GetPhysicsEnvironment(IPhysicsEnvironment* pEnv)
{
	auto it = g_pEnvironmentData.find(pEnv);
	if (it != g_pEnvironmentData.end())
		return it->second;

	return nullptr;
}

#if PHYSENV_INCLUDEIVPFALLBACK
static inline void RegisterPhysicsObject(CPhysicsEnvironmentData* pEnv, IPhysicsObject* pObject)
{
	auto it = g_pObjects.find(pObject);
	if (it == g_pObjects.end())
		g_pObjects[pObject] = pEnv;

	pEnv->RegisterObject(pObject);
}

static inline void UnregisterPhysicsObject(CPhysicsEnvironmentData* pEnv, IPhysicsObject* pObject)
{
	auto it = g_pObjects.find(pObject);
	if (it != g_pObjects.end())
		g_pObjects.erase(it);

	pEnv->UnregisterObject(pObject);
}

static inline bool IsRegisteredPhysicsObject(IPhysicsObject* pObject)
{
	auto it = g_pObjects.find(pObject);
	return it != g_pObjects.end();
}
#endif

static inline CPhysicsEnvironmentData* GetPhysicsObjectEnvironmentData(IPhysicsObject* pObject)
{
	auto it = g_pObjects.find(pObject);
	if (it != g_pObjects.end())
		return it->second;

	return nullptr;
}

/*
 * BUG: The engine likes to use PhysDestroyObject which will call DestroyObject on the wrong environment now.
 * Solution: If it was called on the main Environment, we loop thru all environments until we find our object and delete it.
 *
 * Real Solution: The real soulution would be to let the IPhysicsObject store it's environment and have a method ::GetEnvironment and use that inside PhysDestroyObject to delete it properly.
 *				It would most likely require one to edit the vphysics dll for a clean and proper solution.
 *				Inside the engine there most likely would also be a CPhysicsObject::SetEnvironment method that would need to be added and called at all points, were it uses m_objects.AddToTail()
 */
#if PHYSENV_INCLUDEIVPFALLBACK
static Detouring::Hook detour_CPhysicsEnvironment_DestroyObject;
void hook_CPhysicsEnvironment_DestroyObject(GMODSDK::CPhysicsEnvironment* pEnvironment, IPhysicsObject* pObject)
{
	int foundIndex = -1;
	CPhysicsEnvironmentData* pEnvData = GetPhysicsObjectEnvironmentData(pObject);
	if (!pEnvData || !pEnvData->pEnvironment)
	{
		CBaseEntity* pEntity = (CBaseEntity*)pObject->GetGameData();
		Warning(PROJECT_NAME " - physenv: Failed to find environment of physics object.... How?!? (%p, %i, %s)\n", pEntity, pEntity ? pEntity->entindex() : -1, pEntity ? pEntity->edict()->GetClassName() : "NULL");
		return;
	}

	GMODSDK::CPhysicsEnvironment* pEnv = (GMODSDK::CPhysicsEnvironment*)pEnvData->pEnvironment;
	for (int i = pEnv->m_objects.Count(); --i >= 0; )
		if (pEnv->m_objects[i] == pObject)
			foundIndex = i;

	if (foundIndex == -1)
	{
		if (g_pPhysEnvModule.InDebug())
			Warning(PROJECT_NAME " - physenv: Failed to find object on environment (%p)!\n", pEnvironment);
		return;
	}

#if ARCHITECTURE_X86
	pEnv->m_objects.FastRemove(foundIndex);

	UnregisterPhysicsObject(pEnvData, pObject);
	//pEnv->DestroyObject(pObject);

	GMODSDK::CPhysicsObject* pPhysics = static_cast<GMODSDK::CPhysicsObject*>(pObject);
	// add this flag so we can optimize some cases
	pPhysics->AddCallbackFlags(CALLBACK_MARKED_FOR_DELETE);

	// was created/destroyed without simulating.  No need to wake the neighbors!
	if (foundIndex > pEnv->m_lastObjectThisTick)
		pPhysics->ForceSilentDelete();

	if (pEnv->m_inSimulation || pEnv->m_queueDeleteObject)
	{
		// don't delete while simulating
		pEnv->m_deadObjects.AddToTail(pObject);
	}
	else
	{
		pEnv->m_pSleepEvents->DeleteObject(pPhysics);
		delete pObject;
	}
#else // Required since 64x uses a different vphysics build and we don't want to corrupt member variables of the CPhysicsEnvironment
	detour_CPhysicsEnvironment_DestroyObject.GetTrampoline<Symbols::CPhysicsEnvironment_DestroyObject>()(pEnv, pObject);

	UnregisterPhysicsObject(pEnvData, pObject);
#endif
}

static Detouring::Hook detour_CPhysicsEnvironment_Restore;
bool hook_CPhysicsEnvironment_Restore(IPhysicsEnvironment* pEnv, physrestoreparams_t const& params)
{
	bool bSuccess = detour_CPhysicsEnvironment_Restore.GetTrampoline<Symbols::CPhysicsEnvironment_Restore>()(pEnv, params);
	if (bSuccess && params.type == PIID_IPHYSICSOBJECT)
	{
		CPhysicsEnvironmentData* pEnvData = GetPhysicsEnvironment(pEnv);
		if (pEnvData)
			RegisterPhysicsObject(pEnvData, (IPhysicsObject*)(*params.ppObject));
	}

	return bSuccess;
}

static Detouring::Hook detour_CPhysicsEnvironment_TransferObject;
bool hook_CPhysicsEnvironment_TransferObject(IPhysicsEnvironment* pEnv, IPhysicsObject* pObject, IPhysicsEnvironment* pDestinationEnvironment)
{
	bool bSuccess = detour_CPhysicsEnvironment_TransferObject.GetTrampoline<Symbols::CPhysicsEnvironment_TransferObject>()(pEnv, pObject, pDestinationEnvironment);
	if (bSuccess)
	{
		CPhysicsEnvironmentData* pEnvData = GetPhysicsEnvironment(pEnv);
		CPhysicsEnvironmentData* pDestEnvData = GetPhysicsEnvironment(pDestinationEnvironment);
		if (pEnvData && pDestEnvData)
		{
			UnregisterPhysicsObject(pEnvData, pObject);
			RegisterPhysicsObject(pDestEnvData, pObject);
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
		CPhysicsEnvironmentData* pEnvData = GetPhysicsEnvironment(pEnv);
		if (pEnvData)
			RegisterPhysicsObject(pEnvData, pObject);
	}

	return pObject;
}

static Detouring::Hook detour_CPhysicsEnvironment_UnserializeObjectFromBuffer;
IPhysicsObject* hook_CPhysicsEnvironment_UnserializeObjectFromBuffer(IPhysicsEnvironment* pEnv, void* pGameData, unsigned char* pBuffer, unsigned int bufferSize, bool enableCollisions)
{
	IPhysicsObject* pObject = detour_CPhysicsEnvironment_UnserializeObjectFromBuffer.GetTrampoline<Symbols::CPhysicsEnvironment_UnserializeObjectFromBuffer>()(pEnv, pGameData, pBuffer, bufferSize, enableCollisions);
	if (pObject)
	{
		CPhysicsEnvironmentData* pEnvData = GetPhysicsEnvironment(pEnv);
		if (pEnvData)
			RegisterPhysicsObject(pEnvData, pObject);
	}

	return pObject;
}

static Detouring::Hook detour_CPhysicsEnvironment_CreatePolyObjectStatic;
IPhysicsObject* hook_CPhysicsEnvironment_CreatePolyObjectStatic(IPhysicsEnvironment* pEnv, const CPhysCollide* pCollisionModel, int materialIndex, const Vector& position, const QAngle& angles, objectparams_t* pParams)
{
	IPhysicsObject* pObject = detour_CPhysicsEnvironment_CreatePolyObjectStatic.GetTrampoline<Symbols::CPhysicsEnvironment_CreatePolyObjectStatic>()(pEnv, pCollisionModel, materialIndex, position, angles, pParams);
	if (pObject)
	{
		CPhysicsEnvironmentData* pEnvData = GetPhysicsEnvironment(pEnv);
		if (pEnvData)
			RegisterPhysicsObject(pEnvData, pObject);
	}

	return pObject;
}

static Detouring::Hook detour_CPhysicsEnvironment_CreatePolyObject;
IPhysicsObject* hook_CPhysicsEnvironment_CreatePolyObject(IPhysicsEnvironment* pEnv, const CPhysCollide* pCollisionModel, int materialIndex, const Vector& position, const QAngle& angles, objectparams_t* pParams)
{
	IPhysicsObject* pObject = detour_CPhysicsEnvironment_CreatePolyObject.GetTrampoline<Symbols::CPhysicsEnvironment_CreatePolyObject>()(pEnv, pCollisionModel, materialIndex, position, angles, pParams);
	if (pObject)
	{
		CPhysicsEnvironmentData* pEnvData = GetPhysicsEnvironment(pEnv);
		if (pEnvData)
			RegisterPhysicsObject(pEnvData, pObject);
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
#endif

static Detouring::Hook detour_GMod_Util_IsPhysicsObjectValid;
static bool hook_GMod_Util_IsPhysicsObjectValid(IPhysicsObject* pObject)
{
	if (!pObject)
		return false;

	// This is what Gmod does. Gmod loops thru all physics objects of the main environment to check if the specific IPhysicsObject is part of it.
	/*int iCount = 0;
	IPhysicsObject** pList = (IPhysicsObject**)physenv->GetObjectList(&iCount);
	for (int i = 0; i < iCount; ++i)
	{
		if (pList[i] == pObject)
			return true;
	}*/

	// Should be O(1) now since were using a hash / unordered_map.
#if CUSTOM_VPHYSICS_BUILD
	if (g_bReplacedIVP)
	{
		return g_pPhysicsHolyLib->IsValidObject(pObject);
	} else
#endif
	{
		return IsRegisteredPhysicsObject(pObject);
	}
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
			// Register environments that existed before this module was initialized.
			RegisterPhysicsEnvironment(pPhys->m_envList[i]);
		}
	}

	if (Util::PushTable(pLua, "physenv"))
	{
		Util::AddFunc(pLua, physenv_SetLagThreshold, "SetLagThreshold");
		Util::AddValue(pLua, (int)IVP_SkipType::IVP_SkipSimulation, "IVP_SkipSimulation");
		Util::PopTable(pLua);
	}
}

void CPhysEnvModule::LuaShutdown(GarrysMod::Lua::ILuaInterface* pLua)
{
	if (Util::PushTable(pLua, "physenv"))
	{
		Util::RemoveField(pLua, "SetLagThreshold");
		Util::RemoveField(pLua, "IVP_SkipSimulation");
		Util::PopTable(pLua);
	}
}

static DLL_Handle g_pPhysicsModule = nullptr;
void CPhysEnvModule::InitDetour(bool bPreServer)
{
	if (bPreServer)
	{
#if CUSTOM_VPHYSICS_BUILD
		if (CommandLine()->FindParm("-holylib_replaceivp"))
		{
			g_pPhysicsModule = DLL_LoadModule("vphysics" DLL_EXTENSION, RTLD_LAZY); // Load it manually since rn it wasn't loaded yet.
			SourceSDK::FactoryLoader vphysics_loader("vphysics");
			Detour::Create(
				&detour_CreateInterface, CREATEINTERFACE_PROCNAME,
				vphysics_loader.GetModule(), Symbol::FromName(CREATEINTERFACE_PROCNAME),
				(void*)hook_CreateInterface, m_pID
			);

			SetIVPHolyLibCallbacks(&g_pIVPHolyLib);
			g_bReplacedIVP = true;
		} else {
			g_pPhysicsHolyLib = nullptr;
		}
#endif
		return;
	}

	if (g_pPhysicsModule)
	{
		DLL_UnloadModule(g_pPhysicsModule);
		g_pPhysicsModule = nullptr;
	}

	DETOUR_PREPARE_THISCALL();
#if PHYSENV_INCLUDEIVPFALLBACK
	if (g_pFullFileSystem)
	{
		FileHandle_t fileHandle = g_pFullFileSystem->Open("bin/vphysics_srv.so", "rb", "BASE_PATH");
		if (fileHandle)
		{
			int size = g_pFullFileSystem->Size(fileHandle);
			g_pFullFileSystem->Close(fileHandle);

			if (size < (500 * 1024)) // It's smaller than 500kb? Sus. Let's check for jolt.
			{
				FileFindHandle_t findHandle;
				const char *pFilename = g_pFullFileSystem->FindFirstEx("bin/vphysics_jolt_*.*", "BASE_PATH", &findHandle);
				bIsJoltPhysics = pFilename != nullptr;
				g_pFullFileSystem->FindClose(findHandle);
			}
		}

		if (bIsJoltPhysics)
		{
			Msg(PROJECT_NAME " - physenv: Detected vphysics-jolt usage.\n");
		}
	}

#if defined(SYSTEM_LINUX) && PHYSENV_INCLUDEIVPFALLBACK
	if (!bIsJoltPhysics && !g_bReplacedIVP)
	{
		SourceSDK::FactoryLoader vphysics_loader("vphysics");
		Detour::Create(
			&detour_IVP_Mindist_do_impact, "IVP_Mindist::do_impact",
			vphysics_loader.GetModule(), Symbols::IVP_Mindist_do_impactSym,
			(void*)hook_IVP_Mindist_do_impact, m_pID
		);

		Detour::Create(
			&detour_IVP_Event_Manager_Standard_simulate_time_events, "IVP_Event_Manager_Standard::simulate_time_events",
			vphysics_loader.GetModule(), Symbols::IVP_Event_Manager_Standard_simulate_time_eventsSym,
			(void*)hook_IVP_Event_Manager_Standard_simulate_time_events, m_pID
		);

		Detour::Create(
			&detour_IVP_Mindist_simulate_time_event, "IVP_Mindist::simulate_time_event",
			vphysics_loader.GetModule(), Symbols::IVP_Mindist_simulate_time_eventSym,
			(void*)hook_IVP_Mindist_simulate_time_event, m_pID
		);

		Detour::Create(
			&detour_IVP_Mindist_update_exact_mindist_events, "IVP_Mindist::update_exact_mindist_events",
			vphysics_loader.GetModule(), Symbols::IVP_Mindist_update_exact_mindist_eventsSym,
			(void*)hook_IVP_Mindist_update_exact_mindist_events, m_pID
		);

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

		Detour::Create(
			&detour_IVP_Mindist_Minimize_Solver_p_minimize_PP, "IVP_Mindist_Minimize_Solver::p_minimize_PP",
			vphysics_loader.GetModule(), Symbols::IVP_Mindist_Minimize_Solver_p_minimize_PPSym,
			(void*)hook_IVP_Mindist_Minimize_Solver_p_minimize_PP, m_pID
		);

		Detour::Create(
			&detour_IVP_OV_Element_add_oo_collision, "IVP_OV_Element::add_oo_collision",
			vphysics_loader.GetModule(), Symbols::IVP_OV_Element_add_oo_collisionSym,
			(void*)hook_IVP_OV_Element_add_oo_collision, m_pID
		);

		Detour::Create(
			&detour_IVP_OV_Element_remove_oo_collision, "IVP_OV_Element::remove_oo_collision",
			vphysics_loader.GetModule(), Symbols::IVP_OV_Element_remove_oo_collisionSym,
			(void*)hook_IVP_OV_Element_remove_oo_collision, m_pID
		);


		func_IVP_Mindist_Base_get_objects = (Symbols::IVP_Mindist_Base_get_objects)Detour::GetFunction(vphysics_loader.GetModule(), Symbols::IVP_Mindist_Base_get_objectsSym);
		Detour::CheckFunction((void*)func_IVP_Mindist_Base_get_objects, "IVP_Mindist_Base::get_objects");
	}
#endif
#endif

	if (!g_pModuleManager.IsUsingGhostInj()) // We return as else we'll break stuff.
	{
		Warning(PROJECT_NAME " - physenv: we weren't loaded early enough! use the ghostinj and ensure that holylib is loaded properly!\n");
		return;
	}

	SourceSDK::FactoryLoader server_loader("server");
	Detour::Create(
		&detour_GMod_Util_IsPhysicsObjectValid, "GMod::Util::IsPhysicsObjectValid",
		server_loader.GetModule(), Symbols::GMod_Util_IsPhysicsObjectValidSym,
		(void*)hook_GMod_Util_IsPhysicsObjectValid, m_pID
	);

	if (!detour_CPhysicsEnvironment_DestroyObject.IsValid() || !detour_CPhysicsEnvironment_CreatePolyObject.IsValid() || !detour_CPhysicsEnvironment_CreatePolyObjectStatic.IsValid())
	{
		Detour::DisableHook(&detour_GMod_Util_IsPhysicsObjectValid);
		Warning(PROJECT_NAME " - physenv: Removed GMod::Util::IsPhysicsObjectValid due to other detours failing to hook!\n");
	}

}

void CPhysEnvModule::Shutdown()
{
	if (g_pPhysicsModule)
	{
		DLL_UnloadModule(g_pPhysicsModule);
		g_pPhysicsModule = nullptr;
		g_bReplacedIVP = false;
	}

	for (auto it = g_pEnvironmentData.begin(); it != g_pEnvironmentData.end(); ++it)
	{
		Msg(PROJECT_NAME " - physenv: Found remaining environment! (%p - %p)\n", it->first, it->second);
	}
}