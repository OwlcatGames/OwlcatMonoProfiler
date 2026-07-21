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

// A calloc-style allocator: its allocation size is count * elem_size (two args), used to
// verify the product-size hook mode ("size=a1*a2").
extern "C" __declspec(dllexport) __declspec(noinline) void* test_calloc(size_t count, size_t elem_size)
{
    void* p = malloc(count * elem_size);
    if (p != nullptr)
        memset(p, 0, count * elem_size);
    return p;
}

// A pool-style allocator that commits a region via VirtualAlloc (reservation plane) and hands
// out an interior pointer (allocation plane), like Unity's DynamicHeapAllocator. Used to verify
// the reservation plane: when both this and the VirtualAlloc hook are installed, BOTH must be
// recorded (at distinct addresses) - the inner reservation is not suppressed by the outer
// allocation hook.
extern "C" __declspec(dllexport) __declspec(noinline) void* test_pool_alloc(size_t size)
{
    // Commit a region larger than the handed-out size, so the reservation-plane record (the
    // region) and the allocation-plane record (the interior pointer) have distinct sizes.
    char* region = (char*)VirtualAlloc(nullptr, size + 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return region != nullptr ? region + 0x1000 : nullptr;
}

static int run_native_hook_test()
{
    // Hook config (text) for the two exported functions above. This is delivered to the
    // server over the connection (CMD_CONFIGURE), the same way the UI does it.
    const std::string config =
        "owlcat_mono_profiler_test.exe | test_alloc | alloc | size=a1, ptr=ret | \"Test Alloc\"\n"
        "owlcat_mono_profiler_test.exe | test_free | free | ptr=a1 | \"Test Free\"\n"
        "owlcat_mono_profiler_test.exe | test_calloc | alloc | size=a1*a2, ptr=ret | \"Test Calloc\"\n"
        "owlcat_mono_profiler_test.exe | test_pool_alloc | alloc | size=a1, ptr=ret | \"Test Pool\"\n"
        // VirtualAlloc on the reservation plane, recording only commits (MEM_COMMIT=0x1000).
        // Resolved by name from kernel32.dll; size is the 2nd arg, allocation type the 3rd.
        "kernel32.dll | VirtualAlloc | alloc | size=a2, ptr=ret, if=a3&0x1000, plane=reserve | \"Test VirtualAlloc\"\n";

    // Distinctive sizes (multiples of 64KB, so VirtualAlloc doesn't round them) for the blocks
    // we leave alive and check. All distinct from each other and from the calloc/alloc sizes.
    const uint64_t calloc_count = 7;
    const uint64_t calloc_elem = 16;
    const uint64_t calloc_size = calloc_count * calloc_elem; // 112
    const uint64_t valloc_size = 0x30000;  // 192 KB - committed VirtualAlloc, must be tracked
    const uint64_t pool_size = 0x40000;    // 256 KB - handed out by test_pool_alloc (alloc plane)
    // test_pool_alloc commits pool_size + 0x10000 via VirtualAlloc -> the reservation plane must
    // record that region (at a distinct address and size from the handed-out pointer)
    const uint64_t pool_region_size = pool_size + 0x10000; // 320 KB committed region
    const uint64_t reserve_size = 0x70000; // 448 KB - RESERVE-only, must NOT be tracked (flag filter)

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

    // Frame 1: allocate. The calloc and VirtualAlloc blocks are left alive to check sizes.
    server.on_frame();
    for (int i = 0; i < count; ++i)
        pointers.push_back(test_alloc(128));
    test_calloc((size_t)calloc_count, (size_t)calloc_elem);
    VirtualAlloc(nullptr, (SIZE_T)valloc_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    // RESERVE-only: no MEM_COMMIT, so the flag filter must skip it (never tracked)
    VirtualAlloc(nullptr, (SIZE_T)reserve_size, MEM_RESERVE, PAGE_READWRITE);
    // Pool allocator: records both a handed-out block (alloc plane) and its committed region
    // (reservation plane, firing inside this allocation-plane hook)
    test_pool_alloc((size_t)pool_size);

    // Frame 2: free the test_alloc blocks (a frame change flushes the previous frame)
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

    // Verify the product-size hook: the still-alive calloc block should be a live object
    // of size count*elem_size.
    auto* data = client.get_data();
    uint64_t min_frame = 0, max_frame = 0;
    data->get_frame_boundaries(min_frame, max_frame);
    std::vector<owlcat::live_object> live;
    data->get_live_objects(live, (int)min_frame, (int)max_frame, nullptr);

    bool found_calloc = false;
    bool found_valloc = false;
    bool found_pool = false;
    bool found_pool_region = false;
    bool found_reserve = false;
    for (auto& o : live)
    {
        if (o.size == calloc_size) found_calloc = true;
        if (o.size == valloc_size) found_valloc = true;
        if (o.size == pool_size) found_pool = true;
        if (o.size == pool_region_size) found_pool_region = true;
        if (o.size == reserve_size) found_reserve = true;
    }

    if (!found_calloc)
    {
        printf("native test FAILED: no live object of size %llu - product-size (a1*a2) hook didn't work\n", (unsigned long long)calloc_size);
        return 2;
    }

    if (!found_valloc)
    {
        printf("native test FAILED: no live object of size %llu - committed VirtualAlloc not tracked\n", (unsigned long long)valloc_size);
        return 2;
    }

    if (!found_pool)
    {
        printf("native test FAILED: no live object of size %llu - allocation-plane hook broken alongside reservation plane\n", (unsigned long long)pool_size);
        return 2;
    }

    if (!found_pool_region)
    {
        printf("native test FAILED: no live object of size %llu - reservation plane didn't record a VirtualAlloc made inside an allocation-plane hook\n", (unsigned long long)pool_region_size);
        return 2;
    }

    if (found_reserve)
    {
        printf("native test FAILED: a MEM_RESERVE-only block of size %llu was tracked - the flag filter (if=a3&0x1000) didn't work\n", (unsigned long long)reserve_size);
        return 2;
    }

    printf("native test OK (calloc/committed-VirtualAlloc/pool tracked; reservation-plane and flag-filter verified)\n");
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
