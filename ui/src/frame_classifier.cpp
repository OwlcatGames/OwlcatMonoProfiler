#include "frame_classifier.h"

#include <qsettings.h>
#include <qregularexpression.h>
#include <qvector.h>

namespace
{
    /*
        A compiled pattern. classify() runs once per painted row, so the list is
        parsed and compiled once and cached until the user edits it.
    */
    struct rule
    {
        enum class form { module, prefix, substring };

        form                 kind = form::substring;
        QString              text;      // module name or literal, lower-cased
        QRegularExpression   wildcard;  // only for module patterns containing * or ?
        bool                 is_wildcard = false;
    };

    QVector<rule>  g_rules;
    QStringList    g_compiled_from;
    bool           g_compiled = false;

    rule compile_one(const QString& pattern)
    {
        rule r;
        const QString trimmed = pattern.trimmed();
        r.text = trimmed.toLower();

        const bool has_wildcard = trimmed.contains('*') || trimmed.contains('?');
        if (trimmed.contains(QLatin1String(".dll"), Qt::CaseInsensitive) ||
            trimmed.contains(QLatin1String(".exe"), Qt::CaseInsensitive) ||
            has_wildcard)
        {
            r.kind = rule::form::module;
            if (has_wildcard)
            {
                r.is_wildcard = true;
                r.wildcard = QRegularExpression(
                    QRegularExpression::anchoredPattern(QRegularExpression::wildcardToRegularExpression(r.text)),
                    QRegularExpression::CaseInsensitiveOption);
            }
            return r;
        }

        // A trailing dot means "everything under this namespace or type"
        r.kind = trimmed.endsWith('.') ? rule::form::prefix : rule::form::substring;
        return r;
    }

    const QVector<rule>& rules()
    {
        const QStringList current = frame_class::patterns();
        if (!g_compiled || current != g_compiled_from)
        {
            g_rules.clear();
            for (const QString& pattern : current)
            {
                if (!pattern.trimmed().isEmpty())
                    g_rules.push_back(compile_one(pattern));
            }
            g_compiled_from = current;
            g_compiled = true;
        }

        return g_rules;
    }

    // The module part of a native frame ("Module.dll!Symbol", "Module.dll+0x1234"),
    // or an empty string for a managed frame
    QString module_of(const QString& line)
    {
        int end = line.indexOf('!');
        if (end < 0)
        {
            const int plus = line.indexOf(QLatin1String("+0x"));
            if (plus > 0)
                end = plus;
        }

        if (end <= 0)
            return QString();

        QString module = line.left(end);
        const int slash = qMax(module.lastIndexOf('\\'), module.lastIndexOf('/'));
        if (slash >= 0)
            module = module.mid(slash + 1);

        return module.toLower();
    }
}

namespace frame_class
{
    QStringList default_patterns()
    {
        return QStringList
        {
            // The profiler itself: its own allocations and its own frames are
            // never what you are looking for
            "mono_profiler*.dll",

            // Runtime, engine, CRT, OS, GPU drivers
            "mono-2.0-bdwgc.dll", "mono-2.0-sgen.dll", "gameassembly.dll",
            "unityplayer.dll", "unity.dll", "baselib.dll",
            "ucrtbase.dll", "msvcrt.dll", "msvcp140.dll", "msvcp_win.dll",
            "vcruntime140.dll", "vcruntime140_1.dll",
            "ntdll.dll", "kernel32.dll", "kernelbase.dll", "user32.dll",
            "gdi32.dll", "gdi32full.dll", "win32u.dll", "shcore.dll",
            "combase.dll", "ole32.dll", "oleaut32.dll", "rpcrt4.dll", "sechost.dll",
            "advapi32.dll", "bcryptprimitives.dll",
            "d3d11.dll", "d3d12.dll", "dxgi.dll", "nvwgf2umx.dll", "amdxc64.dll", "atidxx64.dll",

            // Framework namespaces
            "System.", "Microsoft.", "Mono.", "Internal.", "Interop.",
            "UnityEngine.", "Unity.", "UnityEditor.", "TMPro.",
            "Newtonsoft.", "JetBrains.",

            // Mono resolves some frames without their namespace, so a bare BCL
            // type is all that reaches us ("Array.Resize")
            "Array.", "String.", "StringBuilder.", "Object.", "Enum.", "Type.", "Buffer.",
            "List`1.", "Dictionary`2.", "HashSet`1.", "Queue`1.", "Stack`1.", "Enumerable.",
            "Marshal.", "GC.", "Monitor.", "Interlocked.", "RuntimeHelpers.",

            // Mono's wrapper frames, plumbing whatever they wrap
            "(wrapper", "__icall_wrapper", "ves_icall", "runtime_invoke",
        };
    }

    QStringList patterns()
    {
        QSettings settings;
        if (!settings.contains("callstacks/runtime_patterns"))
            return default_patterns();

        const QStringList stored = settings.value("callstacks/runtime_patterns").toStringList();
        // An empty stored list is a deliberate "classify nothing as runtime"
        return stored;
    }

    void set_patterns(const QStringList& list)
    {
        QSettings().setValue("callstacks/runtime_patterns", list);
        g_compiled = false;
    }

    bool attribution_enabled()
    {
        // Default on: for essentially every stack the leaf frame is plumbing
        return QSettings().value("callstacks/attribute_to_game_code", true).toBool();
    }

    void set_attribution_enabled(bool enabled)
    {
        QSettings().setValue("callstacks/attribute_to_game_code", enabled);
    }

    kind classify(const QString& frame_line)
    {
        const QString line = frame_line.trimmed();
        if (line.isEmpty())
            return kind::runtime;

        const QString lower = line.toLower();
        const QString module = module_of(line);

        for (const rule& r : rules())
        {
            switch (r.kind)
            {
            case rule::form::module:
                if (module.isEmpty())
                    break;
                if (r.is_wildcard ? r.wildcard.match(module).hasMatch() : module == r.text)
                    return kind::runtime;
                break;

            case rule::form::prefix:
                // Namespace rules only apply to managed frames
                if (module.isEmpty() && lower.startsWith(r.text))
                    return kind::runtime;
                break;

            case rule::form::substring:
                if (lower.contains(r.text))
                    return kind::runtime;
                break;
            }
        }

        return kind::game;
    }

    int attribution_index(const QStringList& frames)
    {
        for (int i = 0; i < frames.size(); ++i)
            if (classify(frames[i]) == kind::game)
                return i;

        // An all-runtime stack (e.g. an engine-internal allocation): the leaf is
        // all there is to show
        return 0;
    }

    int next_game_index(const QStringList& frames, int from)
    {
        for (int i = from + 1; i < frames.size(); ++i)
            if (classify(frames[i]) == kind::game)
                return i;

        return -1;
    }
}
