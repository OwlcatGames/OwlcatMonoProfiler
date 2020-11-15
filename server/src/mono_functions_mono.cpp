#pragma once

#include "mono_functions.h"

namespace owlcat
{
	/*
		Definitions of function used when we compile for Mono target
	*/
	namespace mono_functions
	{
		mono_func<InstallAllocationsType> install_allocations_proc("mono_profiler_install_allocation");
		mono_func<InstallGCType> install_gc_proc("mono_profiler_install_gc");
		mono_func<CreateProfilerType> create_profiler_handle("mono_profiler_create");
		mono_func<SetEventsType> set_events_proc("mono_profiler_set_events");
		mono_func<SetGCRootRegisterProc> set_gc_register_root_proc("mono_profiler_set_gc_root_register_callback");
		mono_func<SetGCRootUnRegisterProc> set_gc_unregister_root_proc("mono_profiler_set_gc_root_unregister_callback");
		mono_func<ProfilerInstallType> profiler_install_proc("mono_profiler_install");
		mono_func<GetNameType> get_class_namespace("mono_class_get_namespace");
		mono_func<GetNameType> get_class_name("mono_class_get_name");
		mono_func<StackWalkType> stack_walk("mono_stack_walk_no_il");
		mono_func<GetMethodNameType> get_method_name("mono_method_get_name");
		mono_func<MethodGetClassType> method_get_class("mono_method_get_class");
		mono_func<ObjectGetClassType> object_get_class("mono_object_get_class");
		mono_func<ObjectGetSize> object_get_size("mono_object_get_size");
		
		mono_func<BeginLivenessCalculation> begin_liveness_calculation("mono_unity_liveness_calculation_begin");
		mono_func<EndLivenessCalculation> end_liveness_calculation("mono_unity_liveness_calculation_end");
		mono_func<CalculateLivenessFromStatics> calculate_liveness_from_statics("mono_unity_liveness_calculation_from_statics");
	}
}
