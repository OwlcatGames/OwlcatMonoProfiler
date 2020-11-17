#include <Windows.h>

#include "mono_profiler_server.h"
#include "functor.h"
#include "detours.h"
#include "network.h"
#include <string>

using namespace owlcat;

// A global pointer to an instance of profiler server
mono_profiler_server* server = nullptr;

// Support detouring the profiled app using profiler DLL

// True if this DLL was loaded via Detours
bool g_is_detoured = false;
// True if ANOTHER DLL was loaded via Detours (it is possible to Detour an application which already was instrumented using Unity Plugin method)
bool g_is_detoured_by_another_dll = false;

// A way to return a detailed error to the profiler client
HANDLE pipe = INVALID_HANDLE_VALUE;
void SendErrorToPipe(const std::string& error)
{
    WriteFile(pipe, error.c_str(), (DWORD)error.size()+1, NULL, NULL);
}

// A pointer to the original PlayerInitEngineGraphics function
using PlayerInitEngineGraphicsType = bool(*)(bool);
PlayerInitEngineGraphicsType g_original_player_init_engine_graphics = nullptr;

// A pointer to the original EndOfFrameCallbacks::Dequeue method
using DequeCallbacksType = void(*)();
DequeCallbacksType g_deque_callbacks_original = nullptr;

bool wait_for_object(HANDLE object)
{
    DWORD dw;
    MSG msg;

    for (;;)
    {
        dw = MsgWaitForMultipleObjectsEx(1, &object, INFINITE, QS_ALLINPUT, 0);

        if (dw == WAIT_OBJECT_0) break;
        if (dw == WAIT_OBJECT_0 + 1)
        {
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessage(&msg);
            continue;
        }
        return false;
    }

    return true;
}

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
    {
        SendErrorToPipe(owlcat::protocol::error_deque);
        return false;
    }
    
    DetourDetach(&(PVOID&)g_original_player_init_engine_graphics, player_init_engine_graphics_detour);
    DetourAttach(&(PVOID&)g_deque_callbacks_original, deque_callbacks_detour);

    if (DetourTransactionCommit() != NO_ERROR)
    {
        SendErrorToPipe(owlcat::protocol::error_detour_late);
        return false;
    }

    SendErrorToPipe(owlcat::protocol::error_ok);
    CloseHandle(pipe);
    pipe = INVALID_HANDLE_VALUE;

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
            pipe = CreateNamedPipe(owlcat::protocol::pipe_name, PIPE_ACCESS_OUTBOUND|FILE_FLAG_OVERLAPPED, PIPE_TYPE_MESSAGE|PIPE_ACCEPT_REMOTE_CLIENTS, 1, 1024, 1024, 0, NULL);
            if (pipe == INVALID_HANDLE_VALUE)
                return false;

            auto io_event = CreateEvent(NULL, FALSE, FALSE, NULL);
            if (io_event == NULL)
                return false;

            bool pipe_ok = false;
            OVERLAPPED overlapped;
            for (;;)
            {
                SecureZeroMemory(&overlapped, sizeof(overlapped));
                overlapped.hEvent = io_event;
                DWORD dw;

                if (!ConnectNamedPipe(pipe, &overlapped))
                {
                    auto err = GetLastError();
                    if (err == ERROR_PIPE_CONNECTED)
                    {
                        pipe_ok = true;
                        break;
                    }
                    else if (err == ERROR_IO_PENDING)
                    {                        
                        wait_for_object(io_event);
                        if (!GetOverlappedResult(pipe, &overlapped, &dw, FALSE))
                        {
                            return false;
                        }
                        pipe_ok = true;
                        break;
                    }
                }
                else
                {
                    if (!GetOverlappedResult(pipe, &overlapped, &dw, FALSE))
                    {
                        return false;
                    }
                    pipe_ok = true;
                }
            }

            g_original_player_init_engine_graphics = (PlayerInitEngineGraphicsType)DetourFindFunction("UnityPlayer.dll", "?PlayerInitEngineGraphics@@YA_N_N@Z");
            if (g_original_player_init_engine_graphics == nullptr)
            {
                SendErrorToPipe(owlcat::protocol::error_symbols);
                return false;
            }

            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)g_original_player_init_engine_graphics, player_init_engine_graphics_detour);

            if (DetourTransactionCommit() != NO_ERROR)
            {
                SendErrorToPipe(owlcat::protocol::error_detour);
                return false;
            }
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
        if (pipe != INVALID_HANDLE_VALUE)
            CloseHandle(pipe);
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
