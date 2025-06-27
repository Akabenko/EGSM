#include "chromium_fix.h"
#include <mathlib/ssemath.h>
#include "f_mathlib.h"
#include "shaderlib.h"
//#include "bass.h"
#include <GarrysMod/Lua/LuaBase.h>
#include <lua.h>
#include <GarrysMod/Lua/Interface.h>
#include <lua_utils.h>
#include <GarrysMod/FactoryLoader.hpp>
#include <scanning/symbolfinder.hpp>
#include <detouring/hook.hpp>
#include "f_igamesystem.h"
#include <e_utils.h>
#include <Color.h>
#include "version.h"
#include "Windows.h"

#include <scanning/symbolfinder.hpp>

static SymbolFinder SymFinder;

using namespace GarrysMod::Lua;

void ExposeVersion(ILuaBase* LUA)
{
	LUA->PushSpecial(SPECIAL_GLOB);
	LUA->CreateTable();
	LUA->PushNumber(EGSM_VERSION);
	LUA->SetField(-2, "Version");
	LUA->SetField(-2, "EGSM");
	LUA->Pop();
}

typedef int			(*luaL__loadbufferex)(lua_State* L, const char* buff, size_t sz, const char* name, const char* mode);
#include "version_check.lua.inc"
#include "depthpass.lua.inc"

void Menu_Init(ILuaBase* LUA)
{
	ExposeVersion(LUA);
	ShaderLib::MenuInit(LUA);
	//Bass::MenuInit(LUA);

	auto lua_shared = GetModuleHandle("lua_shared.dll");
	if (!lua_shared) { ShaderLibError("lua_shared.dll == NULL\n"); }

	luaL__loadbufferex luaL__loadbufferfex = (luaL__loadbufferex)GetProcAddress(lua_shared, "luaL_loadbufferx");
	if (luaL__loadbufferfex(LUA->GetState(), version_check_lua, sizeof(version_check_lua) - 1, "", NULL))
	{
		Msg("%s\n", LUA->GetString());
		LUA->Pop();
		return;
	}
	if (LUA->PCall(0, 0, 0))
	{
		Msg("%s\n", LUA->GetString());
		LUA->Pop();
	}
}

void CL_Init(ILuaBase* LUA)
{
	ExposeVersion(LUA);
	ShaderLib::LuaInit(LUA);

	auto lua_shared = GetModuleHandle("lua_shared.dll");
	if (!lua_shared) { ShaderLibError("lua_shared.dll == NULL\n"); }

	luaL__loadbufferex luaL__loadbufferfex = (luaL__loadbufferex)GetProcAddress(lua_shared, "luaL_loadbufferx");
	if (luaL__loadbufferfex(LUA->GetState(), depthpass_lua, sizeof(depthpass_lua) - 1, "", NULL))
	{
		Msg("%s\n", LUA->GetString());
		LUA->Pop();
		return;
	}
	if (LUA->PCall(0, 0, 0))
	{
		Msg("%s\n", LUA->GetString());
		LUA->Pop();
	}

	//Bass::LuaInit(LUA);
}

void CL_PostInit(ILuaBase* LUA)
{
	ShaderLib::LuaPostInit(LUA);

	LUA->PushSpecial(SPECIAL_GLOB);
	LUA->GetField(-1, "shaderlib");
	LUA->GetField(-1, "__INIT");
	if (LUA->PCall(0, 0, 0))
	{
		LUA->Pop();
	}
	LUA->Pop(2);
	//Bass::LuaInit(LUA);
}

void CL_Deinit(ILuaBase* LUA)
{
	ShaderLib::LuaDeinit(LUA);
}

// Обновлено соглашение о вызове для корректной работы хука
typedef int32_t(__thiscall* lua_initcl)(void*);
typedef lua_State* (*luaL_newstate)();
typedef void		(*luaL_closestate)(lua_State*);

Detouring::Hook lua_initcl_hk;
Detouring::Hook luaL_newstate_hk;
Detouring::Hook lua_loadbufferex_hk;
Detouring::Hook luaL_closestate_hk;

extern lua_State* g_pClientLua = NULL;
extern lua_State* g_pMenuLua = NULL;

/*
* Обновлено соглашение о вызове для корректной передачи указателя this, так как данный метод является членом класса.
* Добавлен второй аргумент для совместимости с x86 архитектурой
*/
int32_t __fastcall lua_initcl_detour(
	void* __this
#ifndef _WIN64
	, void*
#endif
)
{
	luaL_newstate_hk.Enable();
	int32_t ret = lua_initcl_hk.GetTrampoline<lua_initcl>()(__this);
	luaL_newstate_hk.Disable();
	lua_loadbufferex_hk.Disable();

	GarrysMod::Lua::ILuaBase* LUA = g_pClientLua->luabase;
	LUA->SetState(g_pClientLua);
	CL_PostInit(LUA);

	return ret;
};

lua_State* luaL_newstate_detour()
{
	g_pClientLua = luaL_newstate_hk.GetTrampoline<luaL_newstate>()();
	luaL_newstate_hk.Disable();
	lua_loadbufferex_hk.Enable();
	return g_pClientLua;
}

int luaL_loadbufferex_detour(lua_State* L, const char* buff, size_t sz, const char* name, const char* mode)
{
	if (L == g_pClientLua)
	{
		GarrysMod::Lua::ILuaBase* LUA = L->luabase;
		LUA->SetState(L);
		lua_loadbufferex_hk.Disable();
		CL_Init(LUA);
	}
	int ret = lua_loadbufferex_hk.GetTrampoline<luaL__loadbufferex>()(L, buff, sz, name, mode);

	return ret;
}

void luaL_closestate_detour(lua_State* L)
{
	if (L == g_pClientLua)
	{
		GarrysMod::Lua::ILuaBase* LUA = g_pClientLua->luabase;
		LUA->SetState(g_pClientLua);
		CL_Deinit(LUA);
		g_pClientLua = NULL;
	}
	luaL_closestate_hk.GetTrampoline<luaL_closestate>()(L);
}

GMOD_MODULE_OPEN()
{
	g_pMenuLua = LUA->GetState();

	Color msgc(100, 255, 100, 255);
	ConColorMsg(msgc, "-----EGSM Loading-----\n");
	Menu_Init(LUA);

	/*
	* Реализация фабричных загрузчиков для упрощения работы с модулями игры.
	* Удобно и упрощает управление ресурсами.
	*/
	SourceSDK::FactoryLoader lua_shared_loader("lua_shared");
	SourceSDK::FactoryLoader client_loader("client");

	/*
	* Добавлена логика для безопасного завершения работы модуля в случае сбоя, чтобы избежать критических ошибок в игре.
	* Убраны излишние локальные области видимости и заменены на более стабильное решение, упрощающее управление ресурсами.
	*/
	auto luaL_newstate_p = SymFinder.FindSymbol(lua_shared_loader.GetModule(), "luaL_newstate");
	if (!luaL_newstate_hk.Create(
		reinterpret_cast<void*>(luaL_newstate_p),
		reinterpret_cast<void*>(&luaL_newstate_detour)
	))
	{
		LUA->ThrowError("unable to create detour for luaL_newstate");
	};

	auto luaL_loadbufferx_p = SymFinder.FindSymbol(lua_shared_loader.GetModule(), "luaL_loadbufferx");
	if (!lua_loadbufferex_hk.Create(
		reinterpret_cast<void*>(luaL_loadbufferx_p),
		reinterpret_cast<void*>(&luaL_loadbufferex_detour)
	))
	{
		LUA->ThrowError("unable to create detour for luaL_loadbufferx");
	};

	auto lua_close_p = SymFinder.FindSymbol(lua_shared_loader.GetModule(), "lua_close");
	if (!luaL_closestate_hk.Create(
		reinterpret_cast<void*>(lua_close_p),
		reinterpret_cast<void*>(&luaL_closestate_detour)
	))
	{
		LUA->ThrowError("unable to create detour for lua_close");
	};

	if (!luaL_closestate_hk.Enable())
	{
		LUA->ThrowError("unable to enable detour for lua_close");
	};

	/*
	* Использование современного STL-контейнера для удобства работы с сигнатурами в связке с SymbolFinder.
	* Контейнер автоматически освобождает память по завершению жизненного цикла функции.
	*/
	const std::vector<uint8_t> lua_initclf_sign
#ifdef CHROMIUM
#ifdef ARCHITECTURE_X86_64
	{
		0x48, 0x89, 0x5C, 0x24, 0x2A, 0x48, 0x89, 0x74, 0x24, 0x2A, 0x57, 0x48,
		0x83, 0xEC, 0x2A, 0x48, 0x8B, 0x05, 0x2A, 0x2A, 0x2A, 0x2A, 0x48, 0x33,
		0xC4, 0x48, 0x89, 0x44, 0x24, 0x2A, 0x48, 0x8B, 0xF1
	};
#elif ARCHITECTURE_X86
	{
		0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2A, 0x53, 0x68, 0x2A, 0x2A, 0x2A, 0x2A,
			0x8B, 0xD9, 0xFF, 0x15
	};
#endif
#else
	{
		;
	};
#endif

	auto lua_initclf = SymFinder.FindPattern(client_loader.GetModule(), lua_initclf_sign.data(), lua_initclf_sign.size());
	if (!lua_initclf)
	{
		LUA->ThrowError("failed to dereference lua_initclf");
	};

	if (!lua_initcl_hk.Create(reinterpret_cast<void*>(lua_initclf), reinterpret_cast<void*>(&lua_initcl_detour)))
	{
		LUA->ThrowError("unable to create detour for lua_initclf");
	};

	if (!lua_initcl_hk.Enable())
	{
		LUA->ThrowError("unable to enable detour for lua_initclf");
	};

	ConColorMsg(msgc, "----------------------\n");

	return 0;
};

GMOD_MODULE_CLOSE()
{
	ShaderLib::MenuDeinit(LUA);
	return 0;
};

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		HMODULE phModule;
		char l[MAX_PATH];
		GetModuleFileName(hinstDLL, l, MAX_PATH);
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, l, &phModule);
		break;
	}
	return TRUE;
}

//#include <cdll_client_int.h>
//#include "vgui/ISurface.h"
//
//#include "iclientmode.h"
//IVEngineClient* cengine = NULL;
//vgui::ISurface* g_pVGuiSurface = NULL;
//
/*class CLockCursor : CAutoGameSystemPerFrame
{
public:
	CLockCursor()
	{

	}
	virtual bool Init()
	{
		return true;
	}
	int IsLocked = true;

	void Lock()
	{
		if (!IsLocked)
		{
			IsLocked = true;
			auto mWindow = GetActiveWindow();

			RECT rect;
			GetClientRect(mWindow, &rect);

			POINT ul;
			ul.x = rect.left;
			ul.y = rect.top;

			POINT lr;
			lr.x = rect.right;
			lr.y = rect.bottom;

			MapWindowPoints(mWindow, nullptr, &ul, 1);
			MapWindowPoints(mWindow, nullptr, &lr, 1);

			rect.left = ul.x;
			rect.top = ul.y;

			rect.right = lr.x;
			rect.bottom = lr.y;

			ClipCursor(&rect);
		}
	}

	void Unlock()
	{
		if (IsLocked)
		{
			IsLocked = false;
			ClipCursor(NULL);
		}
	}

	virtual void Update(float frametime)
	{
		Msg("HasFocus() %i IsCursorVisible() %i IsInGame() %i\n", g_pVGuiSurface->HasFocus(), g_pVGuiSurface->IsCursorVisible(), cengine->IsInGame());

		if (!g_pVGuiSurface->HasFocus() || g_pVGuiSurface->IsCursorVisible()) { Unlock(); return; }
		if (cengine->IsInGame())
		{
			Lock();
		}
		else
		{
			Unlock();
		}
	}
};
*/
//static CLockCursor test;

//auto clientdll = GetModuleHandle("client.dll");
//if (!clientdll) { Msg("client.dll == NULL\n"); return 0; }

//typedef void (IGameSystem_AddDecl)(IGameSystem* pSys);
//
//if (!Sys_LoadInterface("engine", VENGINE_CLIENT_INTERFACE_VERSION, NULL, (void**)&cengine))
//{
//	ShaderLibError("IVEngineClient == NULL"); return 0;
//}
//
//if (!Sys_LoadInterface("vgui2", VGUI_SURFACE_INTERFACE_VERSION, NULL, (void**)&g_pVGuiSurface))
//{
//	ShaderLibError("ISurface == NULL"); return 0;
//}

//const char sign2[] = "55 8B EC 8B ? ? ? ? ? A1 ? ? ? ? 53 56 57 8B FA BB 08 00 00 00 8D 4F 01 3B C8 0F 8E ? ? ? ? 8B ? ? ? ? ? 85 F6 78 7C 74 0D 8B C7 99 F7 FE 8B C7 2B C2 03 C6 EB 0F 85 C0 0F 44 C3 3B C1 7D 26 03 C0 3B C1 7C FA 3B C1 7D 1C";
//IGameSystem_AddDecl* IGameSystem_Add = (IGameSystem_AddDecl*)ScanSign(clientdll, sign2, sizeof(sign2) - 1);
//if (!IGameSystem_Add) { Msg("IGameSystem::Add == NULL\n"); return 0; }
//IGameSystem_Add((IGameSystem*)&test);
