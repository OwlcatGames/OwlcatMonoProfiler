# Profiling IL2CPP builds

Profiling IL2CPP builds requires a bit of modification to IL2CPP source code, which is shipped with Unity Editor (in Data/il2cpp folder). By default, IL2CPP (unlike Mono) provides no way to get a set of root objects, which is necessary for pseudo-GC which Owlcat Mono Profiler performs.

IL2CPP source code may differ between Unity versions, so providing a simple patch file to add GC roots callbacks doesn't seem safe. For now, you will have to follow instructions in this file and modify IL2CPP files by hand, making corrections where necessary.

After making these changes and rebuilding your project, the profiler will be able to profile it just as well as a Mono-based build. If you're using "Run App" command to start your application, choose the new IL2CPP option in dialog.

Don't forget to copy relevant Unity pdb to the target folder.

## Changes to IL2CPP:

### libil2cpp\il2cpp-api-functions.h

Find a section that begins with

```c++
// profiler
#if IL2CPP_ENABLE_PROFILER
```

and insert these lines at the end, before #endif:

```c++
// Owlcat Mono Profiler
DO_API(void, il2cpp_profiler_set_gc_root_register_callback, (Il2CppRegisterRootCallback callback));
DO_API(void, il2cpp_profiler_set_gc_root_unregister_callback, (Il2CppUnregisterRootCallback callback));
```

### libil2cpp\il2cpp-api-types.h

Insert these lines at the end of file:

```c++
typedef void (*Il2CppRegisterRootCallback) (Il2CppProfiler* prof, char* start, size_t size);
typedef void (*Il2CppUnregisterRootCallback) (Il2CppProfiler* prof, char* start);
```

### libil2cpp\vm\Profiler.h

Add these lines to //exported section:

```c++
        static void InstallGCRootRegister(Il2CppRegisterRootCallback callback);
        static void InstallGCRootUnregister(Il2CppUnregisterRootCallback callback);
```

and these lines to //internal section:

```c++
        static void RegisterRoot(char* start, size_t size);
        static void UnregisterRoot(char* start);
```

### libil2cpp\vm\Profiler.cpp

Add implementations for the above functions:

```c++
void Profiler::InstallGCRootRegister(Il2CppRegisterRootCallback callback)
    {
        if (!s_profilers.size())
            return;
        s_profilers.back()->registerRootCallback = callback;
    }

    void Profiler::InstallGCRootUnregister(Il2CppUnregisterRootCallback callback)
    {
        if (!s_profilers.size())
            return;
        s_profilers.back()->unregisterRootCallback = callback;
    }

    void Profiler::RegisterRoot(char* start, size_t size)
    {
        for (ProfilersVec::const_iterator iter = s_profilers.begin(); iter != s_profilers.end(); iter++)
        {
            if ((*iter)->registerRootCallback)
                (*iter)->registerRootCallback((*iter)->profiler, start, size);
        }
    }

    void Profiler::UnregisterRoot(char* start)
    {
        for (ProfilersVec::const_iterator iter = s_profilers.begin(); iter != s_profilers.end(); iter++)
        {
            if ((*iter)->unregisterRootCallback)
                (*iter)->unregisterRootCallback((*iter)->profiler, start);
        }
    }
```

Add two new fields to ProfilerDesc struct:

```
       Il2CppRegisterRootCallback registerRootCallback;
       Il2CppUnregisterRootCallback unregisterRootCallback;
```

### libil2cpp\gc\BoehmGC.cpp

Replace function il2cpp::gc::GarbageCollector::AllocateFixed(size_t size, void *descr) with

```c++
void*
il2cpp::gc::GarbageCollector::AllocateFixed(size_t size, void *descr)
{
    // Note that we changed the implementation from mono.
    // In our case, we expect that
    // a) This memory will never be moved
    // b) This memory will be scanned for references
    // c) This memory will remain 'alive' until explicitly freed
    // GC_MALLOC_UNCOLLECTABLE fulfills all these requirements
    // It does not accept a descriptor, but there was only one
    // or two places in mono that pass a descriptor to this routine
    // and we can or will support those use cases in a different manner.
    IL2CPP_ASSERT(!descr);

    void* res = GC_MALLOC_UNCOLLECTABLE(size);
    // Il2Cpp never calls BoehmGC::RegisterRoot, but we need to call Profiler::RegisterRoot
    Profiler::RegisterRoot((char*)res, size);
    return res;
}
```

Replace function il2cpp::gc::GarbageCollector::FreeFixed(void* addr) with

```c++
void
il2cpp::gc::GarbageCollector::FreeFixed(void* addr)
{
    GC_FREE(addr);
    // Il2Cpp never calls BoehmGC::UnregisterRoot, but we need to call Profiler::UnrregisterRoot
    Profiler::UnregisterRoot((char*)addr);
}
```