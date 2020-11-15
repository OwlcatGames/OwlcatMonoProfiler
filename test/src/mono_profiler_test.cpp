#include "dll_exports.h"
#include "mono_profiler_test.h"
#include "mono_profiler_server.h"
#include "mono_profiler_client.h"
#include "persistent_storage.h"

#include <Windows.h>
#include <thread>

using namespace owlcat;

/*
    This is a VERY basic test which does little, but starts client and server and makes them connect
    to each other. If you're looking for a test project on which the profiler can be run, check
    unity folder.
*/

int main()
{
    // Load library so that server can start
    auto module = LoadLibrary(owlcat::MONO_DLL_PATH);

    mono_profiler_server server;
    // Start server without blocking
    server.start(false, 8888);

    mono_profiler_client client;
    // Connect to server and create database file
    client.start("127.0.0.1", 8888, "test.owl");

    // Take some time to exchange initial messages
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Stop everything
    server.stop();
    client.stop();

    return 0;
}
