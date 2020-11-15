## Rationale and comparison with other profiling methods

When we began to develop the console version of Pathfinder: Kingmaker, we encountered a problem of memory leaks. They were present on PC, but never very noticeable, since most PCs now have more memory than PlayStation 4 or XBox One, and it is also backed by a swap file, which is absent on consoles. We began to search for a way to find those leaks. Some of them we were able to trace using platform tools, but since those tools are geared toward native development, they had a hard time dealing with Mono's managed memory.

Unfortunately, Unity's own tools also proved to be almost useless in our case: neither built-in profiler, nor Memory Analyzer package could take a snapshot of our game in reasonable time (it took them more than 12 hours). Using some tricks, we were able to track down a few more suspicious places, but in the end, the limitations of the tools made it hard to make any further progress. Which made us look for alternatives.

## Why is it hard to write a memory profiler for Unity?

When it comes to profiling CPU usage of Untiy games, JetBrains' dotTrace takes the prize. It provides a much more accurate measurement of game's performance than built-in profiler, even with Deep Profiling enabled, and also works faster. However, memory profiling counterpart of dotTrace, dotMemory, still does not support Unity. There are good reasons for that: you can'r profile memory usage of Unity game without resorting to some dirty tricks.

As you know, Mono uses Garbage Collector to manage memory allocations. To be more exact, the version of Mono that is used by Unity runs on BoehmGC, an old and venerable garbage collector, which has a somewhat limited interface for profiling. Mono itself provides a profiling interface, and even has a built-in profiler called 'log'. Unfortunately, Unity does not provide any access to that interface, and also Unity's version of Mono has that profiler removed. Even if it wasn't removed, we're not sure if it would be usable in all cases, since it needs to create an output file on the same system where the game is running, which may have limited space (and profiling traces can grow quite big) or access restrictions. Also, from a quick overview, it seems that most tools designed to work with Mono's log files are either abandoned, unfinished or obsolete.

It leaves us with an option to access Mono's profiling interface directly, for example, from native Unity plugin (theorhetically, you can acess it from C# code via DllImport's, but this looks problematic: your own profiling code would be allocating managed memory then, skewing the results). However, the interface only provides a callback for object allocation, which is not enough if we want to find "leaked" objects (i.e. objects that are still references when we thought they shouldn't be anymore), although it is useful by itself, for example, for finding allocation hot-spots.

One option we tried early is to add a finalizer to every allocated object. Finalizers are called when the object is finally garbage collected, thus providing us with a chance to register de-allocation. However, we found it impossible to implement without breaking things. Therefore, another approach was needed. It was suggested by an old and obsolete [heap-prof](https://github.com/mono/heap-prof/blob/master/src/runtime-profiler/gc-profiler.c) profiler for Mono. It works by keeping a record of all allocations, and, when GC's Collect function is called, it iterates over stored allocations and checks if they are still alive. If the allocation is no longer referenced, a "free" event is reported for it.

This approach has both CPU and memory overhead, but it's the only way to profile memory usage with BoehmGC, as described in this e-mail to Mono e-mailing list from 2009: https://mono.github.io/mail-archives/mono-devel-list/2009-November/033387.html. There is a proposal for better profiling interface, but only for the newer sgen GC, which Unity doesn't use for some reason, even though Mono has long since moved away from BoehmGC.

The same e-mail explains why we couldn't simply copy heap-prof's code. The crux of the problem is "mono_object_is_alive" function which is used to determine is the object is still alive. Basically, it's prone to crashes and unreliable even when it doesn't crash. After a few attempts to fix or work around issues surrounding it, we had to try something else, at the cost of even more CPU time.

## Our methodology

What worked for us in the end was a re-implementation of the whole garbage collection process, though a bit more simplified. Therefore, Owlcat Mono Profiler works this way:

1. Whenever an allocation happens, we store it and report it via network to client software, which stores is in file
2. Whenever a GC root (i.e. an object which doesn't have any references to it, but should never be deleted) is registred, we add it to our own list of roots. If Unity was using sgen GC, we could just obtain a list of GC roots when we needed it, but with BoehmGC, this is not supported, as the GC itself has no way of getting roots, only adding and removing them.
3. Whenever a GC collection happens, we traverse our object graph, starting from the registred roots, and mark all reachable objects. After that, we report all unreachable non-root objects as freed.

And that's it. It took quite a few tries to get it right, because at the beginning, we didn't know much about how Unity's GC worked. The code might still be wrong in some cases, but we tested it on our own games, and it provides information that is accurate enough to be usable.

## Goals of Owlcat Mono Profiler project

Our main goal is to provide Unity developers with a way to profile Mono real-time memory usage for their games and analyze the profiling data for leaked objects. The profiler should be compatible with all recent versions of Unity, and ideally should run on all supported platforms, including mobile and console ones. Profiler UI should run on all major desktop platforms: Windows 10, macOS and Linux.

The profiler also should support both Mono and IL2CPP scripting backend if possible (IL2CPP has at least some counterpart to Mono's profiling interface, but it needs some more research before we can support it).

Going further, we also would like to support profiling Unity's heap alongside Mono Heap, but this requires even more time and research, and might not even be possible. But we are reasonably sure that one can find most memory leaks by looking at Mono heap, since most of the time the leaks happens in user's C# code which allocates objects on Mono heap.

The last goal of this project is to allow developers to provide a simple command-line or one-button profiler to their users for creating a trace for a problem that can't be reproduced locally.

## Significant issues

* Can we profile Mono heap with IL2CPP? Do we have to change a lot of code to do that?
* Can we profile Mono heap on consoles and mobiles (this would require us to access Mono's or IL2CPP's profiling interface via dlopen)?
* Can we move symbol resolution for callstacks to client side? This would reduce the CPU overhead and network traffic a lot, but requires research into ways to translate addresses into symbols via Mono PDB/MDB. How will this work on consoles (prx/so files)?