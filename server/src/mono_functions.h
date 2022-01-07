#pragma once

#include "functor.h"
#include "mono\metadata\profiler.h"

#if OWLCAT_IL2CPP
typedef struct Il2CppStackFrameInfo
{
	MonoMethod* method;
} Il2CppStackFrameInfo;
#endif

namespace owlcat
{
	/*
	    A set of function exported from Mono dll that the profiler uses
	*/
	namespace mono_functions
	{
		typedef MonoProfilerHandle(__stdcall* CreateProfilerType)(MonoProfiler* prof);
		extern mono_func<CreateProfilerType> create_profiler_handle;	 

		typedef void(__stdcall* WriteAllocationProc)(MonoProfiler* p, MonoObject* obj, MonoClass* klass);
		typedef void(__stdcall* InstallAllocationsType)(WriteAllocationProc);
		extern mono_func<InstallAllocationsType> install_allocations_proc;

		typedef void(__stdcall* GCCallProc)(MonoProfiler* p, MonoProfilerGCEvent e, int gen);
		typedef void(__stdcall* HeapResizeProc)(MonoProfiler* p, int64_t new_size);
		typedef void(__stdcall* InstallGCType)(GCCallProc, HeapResizeProc);
		extern mono_func<InstallGCType> install_gc_proc;

		typedef void(__stdcall* SetEventsType)(int flags);
		extern mono_func<SetEventsType> set_events_proc;

#if OWLCAT_MONO
		typedef void(__stdcall* GCRegisterRootProc)(MonoProfiler* prof, const mono_byte* start, size_t size, MonoGCRootSource source, const void* key, const char* name);
		typedef void(__stdcall* GCUnRegisterRootProc)(MonoProfiler* prof, const mono_byte* start);
		typedef void(__stdcall* SetGCRootRegisterProc)(_MonoProfilerDesc* handle, GCRegisterRootProc);
		typedef void(__stdcall* SetGCRootUnRegisterProc)(_MonoProfilerDesc* handle, GCUnRegisterRootProc);
#else
		typedef void(__stdcall* GCRegisterRootProc)(MonoProfiler* prof, char* start, size_t size);
		typedef void(__stdcall* GCUnRegisterRootProc)(MonoProfiler* prof, char* start);
		typedef void(__stdcall* SetGCRootRegisterProc)(GCRegisterRootProc);
		typedef void(__stdcall* SetGCRootUnRegisterProc)(GCUnRegisterRootProc);
#endif
		extern mono_func<SetGCRootRegisterProc> set_gc_register_root_proc;
		extern mono_func<SetGCRootUnRegisterProc> set_gc_unregister_root_proc;

		typedef void(__stdcall* ShutdownProc)(MonoProfiler*);
		typedef void(__stdcall* ProfilerInstallType)(MonoProfiler*, ShutdownProc);
		extern mono_func<ProfilerInstallType> profiler_install_proc;

		typedef const char* (__stdcall* GetNameType)(MonoClass*);
		extern mono_func<GetNameType> get_class_namespace;
		extern mono_func<GetNameType> get_class_name;
#if OWLCAT_MONO
		typedef void(__stdcall* StackWalkType)(MonoStackWalk func, void* user_data);
#else
		typedef void (*Il2CppFrameWalkFunc) (const Il2CppStackFrameInfo* info, void* user_data);
		typedef void(__stdcall* StackWalkType)(Il2CppFrameWalkFunc func, void* user_data);
#endif
		extern mono_func<StackWalkType> stack_walk;
		typedef const char* (__stdcall* GetMethodNameType)(MonoMethod*);
		extern mono_func<GetMethodNameType> get_method_name;
		typedef MonoClass* (__stdcall* MethodGetClassType)(MonoMethod*);
		extern mono_func<MethodGetClassType> method_get_class;
		typedef MonoClass* (__stdcall* ObjectGetClassType)(MonoObject*);
		extern mono_func<ObjectGetClassType> object_get_class;
		typedef unsigned int (__stdcall* ObjectGetSize)(MonoObject*);
		extern mono_func<ObjectGetSize> object_get_size;

		typedef void (*register_object_callback)(void* arr, int size, void* callback_userdata);
		typedef void (*WorldStateChanged)();
		struct LivenessState;
		typedef LivenessState* (__stdcall* BeginLivenessCalculation)(MonoClass* filter, uint32_t max_count, register_object_callback callback, void* callback_userdata, WorldStateChanged onWorldStartCallback, WorldStateChanged onWorldStopCallback);
		extern mono_func<BeginLivenessCalculation> begin_liveness_calculation;

		typedef void (__stdcall* EndLivenessCalculation)(LivenessState* state);
		extern mono_func<EndLivenessCalculation> end_liveness_calculation;

		typedef void(__stdcall* CalculateLivenessFromStatics)(LivenessState* state);
		extern mono_func<CalculateLivenessFromStatics> calculate_liveness_from_statics;
	};
}
