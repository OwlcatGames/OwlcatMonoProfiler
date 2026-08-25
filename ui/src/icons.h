#pragma once
#include <qicon.h>

/*
    The toolbar icon set, drawn with QPainter rather than shipped as files:
    no .qrc, no Qt5::Svg dependency, and every icon picks up the current
    theme colour (so it follows a dark/light switch for free).

    All icons are stroke drawings on a 16x16 grid, rendered at several sizes
    so they stay crisp on high-DPI screens. Call icons::invalidate() after a
    theme change.
*/
namespace icons
{
    QIcon open();
    QIcon save();
    QIcon connect_app();
    QIcon run_app();
    QIcon stop();
    QIcon pause();
    QIcon settings();
    QIcon symbols();
    QIcon search();
    QIcon copy();
    QIcon export_csv();
    QIcon refresh();
    QIcon add_folder();
    QIcon remove();
    QIcon move_up();
    QIcon move_down();
    QIcon references();

    // Application / window icon (the same mark as the design canvas)
    QIcon app();

    // A filled dot in an arbitrary colour, for status indicators
    QIcon dot(const QColor& color);

    // Drops the cache; call after theme::apply()
    void invalidate();
}
