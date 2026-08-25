#pragma once
#include <qcolor.h>
#include <qstring.h>
#include <qfont.h>

/*
    Single owner of every colour in the UI. One token table produces both the
    QPalette and the application stylesheet, so widgets never carry their own
    colours (per-widget palette overrides survive a theme switch and break it).

    Qwt does not read stylesheets, so the plots pull their pens straight from
    tokens() - that is why the series colours live here too.
*/
namespace theme
{
    enum class mode { dark, light };

    struct tokens_t
    {
        bool    dark;

        QColor  window;         // application background
        QColor  panel;          // panel / table background
        QColor  panel_alt;      // toolbar, status bar, table header
        QColor  panel_hi;       // hover / raised control
        QColor  border;
        QColor  border_strong;

        QColor  text;
        QColor  text_bright;
        QColor  text_dim;
        QColor  text_faint;

        QColor  accent;         // primary action, selection, tracked series
        QColor  accent_bg;      // accent-tinted button background
        QColor  accent_border;
        QColor  accent_text;

        QColor  ok;
        QColor  warn;
        QColor  danger;

        QColor  grid;             // plot grid lines
        QColor  plot_bg;          // plot canvas
        QColor  series_tracked;
        QColor  series_committed;
        QColor  series_native;
        QColor  series_allocs;
        QColor  series_gc;
        QColor  selection_band;   // frame-range selection fill
    };

    const tokens_t& tokens();

    // Applies style, palette and stylesheet to the running QApplication
    void apply(mode m);
    mode current();

    // Stored preference: "dark", "light" or "system" (default). preferred()
    // resolves "system" against the OS setting.
    QString preference();
    mode     preferred();
    void     set_preference(const QString& pref);

    // A monospace family that actually exists on this machine, for numbers
    // and callstack text (tabular figures, so columns stop jittering).
    QString mono_family();
    // Size is in PIXELS, matching the 13px base in the stylesheet: a point-size
    // delta lands at 7-8pt on a 9pt system font, which is unreadable in a table
    QFont   mono_font(int pixel_size_delta = 0);
}
