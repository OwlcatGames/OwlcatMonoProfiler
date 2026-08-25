#pragma once
#include <qstring.h>
#include <qstringlist.h>

/*
    Splits a callstack into "runtime plumbing" and "your code".

    Every allocation stack ends in the allocator, so the leaf frame is almost
    never the interesting one: managed allocations bottom out in
    Object.__icall_wrapper_ves_icall_array_new_specific or Array.Resize, and
    native ones in MemoryManager::Allocate called from malloc_internal. Showing
    the leaf makes thousands of distinct stacks look identical.

    The frame worth showing is the first one, walking up from the leaf, that is
    not part of the runtime - the line where the game asked for the memory. This
    is display-only: nothing is dropped, the inspector still shows every frame,
    and the whole behaviour can be switched off (Settings -> "Attribute
    allocations to game code").

    Which frames count as runtime is a user-editable pattern list (Settings ->
    "Runtime frames..."), so nothing is baked in. A frame is runtime if it comes
    from a runtime MODULE or matches a framework NAMESPACE root or a wrapper
    MARKER; everything else counts as game code, so a studio's own namespaces
    never need listing.

    Prior art in this codebase: worker_thread's m_stopwords, which drops
    allocations whose callstack contains an uninteresting frame. Same idea, but
    for presentation.
*/
namespace frame_class
{
    enum class kind
    {
        game,      // the product's own code - what an allocation is attributed to
        runtime,   // allocator, runtime, engine, BCL, OS
    };

    /*
        The pattern list. Three forms, distinguished by shape so one flat list
        covers every case:

          "unityplayer.dll", "mono_profiler*.dll"  a module name, '*' and '?' allowed;
                                                   matched against the module of a
                                                   native frame ("Module.dll!Sym")
          "System.", "Array."                      a prefix of a managed frame
          "__icall_wrapper", "(wrapper"            a substring anywhere in the frame

        Matching is case-insensitive throughout.
    */
    QStringList default_patterns();
    QStringList patterns();
    void set_patterns(const QStringList& patterns);

    // Whether the UI attributes an allocation to the first game frame rather than
    // to the raw leaf. Persisted; Settings -> "Attribute allocations to game code".
    bool attribution_enabled();
    void set_attribution_enabled(bool enabled);

    kind classify(const QString& frame_line);

    // Index of the frame an allocation should be attributed to: the first game
    // frame counting from the leaf (index 0). Returns 0 when every frame is
    // runtime, so a stack always has something to show.
    int attribution_index(const QStringList& frames);

    // Index of the next game frame above `from` (exclusive), or -1
    int next_game_index(const QStringList& frames, int from);
}
