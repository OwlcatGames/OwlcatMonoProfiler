#include "dll_exports.h"
#include "mono_profiler_test.h"
#include "mono_profiler_server.h"
#include "mono_profiler_client.h"
#include "persistent_storage.h"

#include <Windows.h>
#include <thread>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cstdio>

using namespace owlcat;

/*
    This is a VERY basic test which does little, but starts client and server and makes them connect
    to each other, exercises the native hook path, and checks capture save/open. If you're looking
    for a test project on which the profiler can be run, check the unity folder.
*/

// Exported so the native hook engine can resolve and detour them by name. noinline keeps
// them as real, hookable functions in optimized builds.
extern "C" __declspec(dllexport) __declspec(noinline) void* test_alloc(size_t size)
{
    void* p = malloc(size);
    if (p != nullptr)
        memset(p, 0, 1);
    return p;
}

extern "C" __declspec(dllexport) __declspec(noinline) void test_free(void* p)
{
    free(p);
}

static int run_native_hook_test()
{
    // Hook config (text) for the two exported functions above. This is delivered to the
    // server over the connection (CMD_CONFIGURE), the same way the UI does it.
    const std::string config =
        "owlcat_mono_profiler_test.exe | test_alloc | alloc | size=a1, ptr=ret | \"Test Alloc\"\n"
        "owlcat_mono_profiler_test.exe | test_free | free | ptr=a1 | \"Test Free\"\n";

    // Server just listens; the client selects native-only capture and supplies the config
    mono_profiler_server server;
    server.start(false, 8890);

    mono_profiler_client client;
    if (!client.start("127.0.0.1", 8890, "test_native.owl", owlcat::CAPTURE_NATIVE, config))
    {
        printf("native test: client failed to connect\n");
        return 2;
    }

    // Let the server receive CMD_CONFIGURE and install the hooks before we allocate
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const int count = 10;
    std::vector<void*> pointers;

    // Frame 1: allocate
    server.on_frame();
    for (int i = 0; i < count; ++i)
        pointers.push_back(test_alloc(128));

    // Frame 2: free (a frame change flushes the previous frame's events on the client)
    server.on_frame();
    for (void* p : pointers)
        test_free(p);

    // Frame 3: nothing, just advance so frame 2 can flush
    server.on_frame();

    // Give the worker + network + client time to process the events
    std::this_thread::sleep_for(std::chrono::seconds(1));

    server.stop();
    client.stop(); // final flush of the last frame

    uint64_t inserted = client.get_db_inserted_events_count();
    printf("native test: %llu events stored (expected >= %d)\n", (unsigned long long)inserted, count * 2);

    if (inserted < (uint64_t)(count * 2))
    {
        printf("native test FAILED: too few events - native hooks likely did not fire\n");
        return 2;
    }

    printf("native test OK\n");
    return 0;
}

int main()
{
    // Load library so that server can start
    auto module = LoadLibrary(owlcat::MONO_DLL_PATH);

    // --- Basic connect + save/open smoke test ---
    {
        mono_profiler_server server;
        server.start(false, 8888);

        mono_profiler_client client;
        client.start("127.0.0.1", 8888, "test.owl");

        // Take some time to exchange initial messages
        std::this_thread::sleep_for(std::chrono::seconds(1));

        server.stop();
        client.stop();

        // Check that packing a capture into a container and re-opening it works
        if (!client.save_db("test_saved.owl", false))
        {
            printf("save_db failed\n");
            return 1;
        }

        if (!client.open_data("test_saved.owl"))
        {
            printf("open_data failed\n");
            return 1;
        }
    }

    // --- Native hook round-trip test ---
    int native_result = run_native_hook_test();
    if (native_result != 0)
        return native_result;

    return 0;
}
