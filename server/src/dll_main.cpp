#include <Windows.h>

#include "mono_profiler_server.h"
#include "functor.h"
#include "detours.h"

using namespace owlcat;

// A global pointer to an instance of profiler server
mono_profiler_server* server = nullptr;

// Support detouring the profiled app using profiler DLL

// True if this DLL was loaded via Detours
bool g_is_detoured = false;
// True if ANOTHER DLL was loaded via Detours (it is possible to Detour an application which already was instrumented using Unity Plugin method)
bool g_is_detoured_by_another_dll = false;

// A pointer to the original PlayerInitEngineGraphics function
using PlayerInitEngineGraphicsType = bool(*)(bool);
PlayerInitEngineGraphicsType g_original_player_init_engine_graphics = nullptr;

// A pointer to the original EndOfFrameCallbacks::Dequeue method
using DequeCallbacksType = void(*)();
DequeCallbacksType g_deque_callbacks_original = nullptr;

// The only reliable method to notice frame change in standalone non-development player without the help of C#-side instrumentation I have found.
void deque_callbacks_detour()
{
    g_deque_callbacks_original();
    if (server)
        server->on_frame();
}

// The place where we start the profiler and detour EndOfFrameCallbacks::Dequeue to get frame end events
bool __cdecl player_init_engine_graphics_detour(bool arg)
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    // Find EndOfFrameCallbacks::Dequeue (need PDB for that!)
    g_deque_callbacks_original = (DequeCallbacksType)DetourFindFunction("UnityPlayer.dll", "?DequeAll@EndOfFrameCallbacks@@SAXXZ");
    if (g_deque_callbacks_original == 0)
        return false;
    
    DetourDetach(&(PVOID&)g_original_player_init_engine_graphics, player_init_engine_graphics_detour);
    DetourAttach(&(PVOID&)g_deque_callbacks_original, deque_callbacks_detour);

    if (DetourTransactionCommit() != NO_ERROR)
        return false;

    // Start server. We really should pass the port here, somehow
    server->start(true, 8888);

    // Return control to the original function
    return g_original_player_init_engine_graphics(arg);
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:        
        // If the profiler dll is loaded via Detour, schedule automatic server activation
        g_is_detoured = DetourRestoreAfterWith();
        if (g_is_detoured)
        {
            g_original_player_init_engine_graphics = (PlayerInitEngineGraphicsType)DetourFindFunction("UnityPlayer.dll", "?PlayerInitEngineGraphics@@YA_N_N@Z");
            if (g_original_player_init_engine_graphics == nullptr)
                return false;

            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)g_original_player_init_engine_graphics, player_init_engine_graphics_detour);

            if (DetourTransactionCommit() != NO_ERROR)
                return false;
        }
        // Enumerate modules to check if we have two copies of profiler dll. This will mean that process've been detoured and we should do nothing 
        else
        {
            int profiler_dll_count = 0;
            HMODULE module = nullptr;
            while (module = DetourEnumerateModules(module))
            {
                char moduleName[1024];
                GetModuleFileNameA(module, moduleName, sizeof(moduleName));
                moduleName[sizeof(moduleName) - 1] = 0;
                if (strstr(moduleName, "mono_profiler") != nullptr)
                    ++profiler_dll_count;
            }

            if (profiler_dll_count > 1)
            {
                g_is_detoured_by_another_dll = true;
                return true;
            }
        }
        server = new mono_profiler_server();
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;

    case DLL_PROCESS_DETACH:
        delete server;
        server = nullptr;
        break;
    }
    return TRUE;
}

extern "C"
{
    __declspec(dllexport) void StartProfiling()
    {
        // Avoid second call to start if already started from detour, even if the game wants it
        if (!g_is_detoured && !g_is_detoured_by_another_dll)
            server->start(true, 8888);
    }

    __declspec(dllexport) void EndProfilingFrame()
    {
        if (!g_is_detoured && !g_is_detoured_by_another_dll)
            server->on_frame();
    }
}
