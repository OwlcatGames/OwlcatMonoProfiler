#pragma once

#include "mono_functions.h"

namespace owlcat
{
	/*
		Definitions of function used when we compile for Mono target
	*/
	namespace mono_functions
	{
		mono_func<InstallAllocationsType> install_allocations_proc("il2cpp_profiler_install_allocation");
		mono_func<InstallGCType> install_gc_proc("il2cpp_profiler_install_gc");
		//mono_func<CreateProfilerType> create_profiler_handle("il2cpp_profiler_create");
		mono_func<SetEventsType> set_events_proc("il2cpp_profiler_set_events");
		mono_func<SetGCRootRegisterProc> set_gc_register_root_proc("il2cpp_profiler_set_gc_root_register_callback");
		mono_func<SetGCRootUnRegisterProc> set_gc_unregister_root_proc("il2cpp_profiler_set_gc_root_unregister_callback");
		mono_func<ProfilerInstallType> profiler_install_proc("il2cpp_profiler_install");
		mono_func<GetNameType> get_class_namespace("il2cpp_class_get_namespace");
		mono_func<GetNameType> get_class_name("il2cpp_class_get_name");
		mono_func<StackWalkType> stack_walk("il2cpp_current_thread_walk_frame_stack");
		mono_func<GetMethodNameType> get_method_name("il2cpp_method_get_name");
		mono_func<MethodGetClassType> method_get_class("il2cpp_method_get_class");
		mono_func<ObjectGetClassType> object_get_class("il2cpp_object_get_class");
		mono_func<ObjectGetSize> object_get_size("il2cpp_object_get_size");
		
		mono_func<BeginLivenessCalculation> begin_liveness_calculation("il2cpp_unity_liveness_calculation_begin");
		mono_func<EndLivenessCalculation> end_liveness_calculation("il2cpp_unity_liveness_calculation_end");
		mono_func<CalculateLivenessFromStatics> calculate_liveness_from_statics("il2cpp_unity_liveness_calculation_from_statics");
	}
}
