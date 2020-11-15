#pragma once
/*
    Functions that can be used from C# are defined here
*/
extern "C"
{
    __declspec(dllexport) void StartProfiling();
    __declspec(dllexport) void EndProfilingFrame();
}
