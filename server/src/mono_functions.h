#pragma once

#include "functor.h"
#include "mono/metadata/profiler.h"

#if OWLCAT_IL2CPP
typedef struct Il2CppStackFrameInfo
{
	MonoMethod* method;
} Il2CppStackFrameInfo;
#endif

#if WIN32
    #define CALLING_CONV __stdcall
#else
    #define CALLING_CONV
#endif

namespace owlcat
{
	/*
	    A set of function exported from Mono dll that the profiler uses
	*/
	namespace mono_functions
	{
		typedef MonoProfilerHandle(CALLING_CONV* CreateProfilerType)(MonoProfiler* prof);
		extern mono_func<CreateProfilerType> create_profiler_handle;	 

		typedef void(CALLING_CONV* WriteAllocationProc)(MonoProfiler* p, MonoObject* obj, MonoClass* klass);
		typedef void(CALLING_CONV* InstallAllocationsType)(WriteAllocationProc);
		extern mono_func<InstallAllocationsType> install_allocations_proc;

		typedef void(CALLING_CONV* GCCallProc)(MonoProfiler* p, MonoProfilerGCEvent e, int gen);
		typedef void(CALLING_CONV* HeapResizeProc)(MonoProfiler* p, int64_t new_size);
		typedef void(CALLING_CONV* InstallGCType)(GCCallProc, HeapResizeProc);
		extern mono_func<InstallGCType> install_gc_proc;

		typedef void(CALLING_CONV* SetEventsType)(int flags);
		extern mono_func<SetEventsType> set_events_proc;

#if OWLCAT_MONO
		typedef void(CALLING_CONV* GCRegisterRootProc)(MonoProfiler* prof, const mono_byte* start, size_t size, MonoGCRootSource source, const void* key, const char* name);
		typedef void(CALLING_CONV* GCUnRegisterRootProc)(MonoProfiler* prof, const mono_byte* start);
		typedef void(CALLING_CONV* SetGCRootRegisterProc)(_MonoProfilerDesc* handle, GCRegisterRootProc);
		typedef void(CALLING_CONV* SetGCRootUnRegisterProc)(_MonoProfilerDesc* handle, GCUnRegisterRootProc);
#else
		typedef void(CALLING_CONV* GCRegisterRootProc)(MonoProfiler* prof, char* start, size_t size);
		typedef void(CALLING_CONV* GCUnRegisterRootProc)(MonoProfiler* prof, char* start);
		typedef void(CALLING_CONV* SetGCRootRegisterProc)(GCRegisterRootProc);
		typedef void(CALLING_CONV* SetGCRootUnRegisterProc)(GCUnRegisterRootProc);
#endif
		extern mono_func<SetGCRootRegisterProc> set_gc_register_root_proc;
		extern mono_func<SetGCRootUnRegisterProc> set_gc_unregister_root_proc;

		typedef void(CALLING_CONV* ShutdownProc)(MonoProfiler*);
		typedef void(CALLING_CONV* ProfilerInstallType)(MonoProfiler*, ShutdownProc);
		extern mono_func<ProfilerInstallType> profiler_install_proc;

		typedef const char* (CALLING_CONV* GetNameType)(MonoClass*);
		extern mono_func<GetNameType> get_class_namespace;
		extern mono_func<GetNameType> get_class_name;
#if OWLCAT_MONO
		typedef void(CALLING_CONV* StackWalkType)(MonoStackWalk func, void* user_data);
#else
		typedef void (*Il2CppFrameWalkFunc) (const Il2CppStackFrameInfo* info, void* user_data);
		typedef void(CALLING_CONV* StackWalkType)(Il2CppFrameWalkFunc func, void* user_data);
#endif
		extern mono_func<StackWalkType> stack_walk;
		typedef const char* (CALLING_CONV* GetMethodNameType)(MonoMethod*);
		extern mono_func<GetMethodNameType> get_method_name;
		typedef MonoClass* (CALLING_CONV* MethodGetClassType)(MonoMethod*);
		extern mono_func<MethodGetClassType> method_get_class;
		typedef MonoClass* (CALLING_CONV* ObjectGetClassType)(MonoObject*);
		extern mono_func<ObjectGetClassType> object_get_class;
		typedef unsigned int (CALLING_CONV* ObjectGetSize)(MonoObject*);
		extern mono_func<ObjectGetSize> object_get_size;

		typedef void (*register_object_callback)(void* arr, int size, void* callback_userdata);
		typedef void (*WorldStateChanged)();
		struct LivenessState;
		typedef LivenessState* (CALLING_CONV* BeginLivenessCalculation)(MonoClass* filter, uint32_t max_count, register_object_callback callback, void* callback_userdata, WorldStateChanged onWorldStartCallback, WorldStateChanged onWorldStopCallback);
		extern mono_func<BeginLivenessCalculation> begin_liveness_calculation;

		typedef void (CALLING_CONV* EndLivenessCalculation)(LivenessState* state);
		extern mono_func<EndLivenessCalculation> end_liveness_calculation;

		typedef void(CALLING_CONV* CalculateLivenessFromStatics)(LivenessState* state);
		extern mono_func<CalculateLivenessFromStatics> calculate_liveness_from_statics;
	};
}
