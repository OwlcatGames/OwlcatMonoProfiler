#pragma once
#include "qwt_plot_picker.h"
#include "qwt_painter.h"

/*
    A class that allows to select a timeframe on the graph.

    TODO: Allow selection that spans more than one screen.
*/
class band_picker : public QwtPlotPicker
{
    // Copied from qwt_picker.cpp, because why it is not public?!
    inline QRegion qwtMaskRegion(const QRect& r, int penWidth) const
    {
        const int pw = qMax(penWidth, 1);
        const int pw2 = penWidth / 2;

        int x1 = r.left() - pw2;
        int x2 = r.right() + 1 + pw2 + (pw % 2);

        int y1 = r.top() - pw2;
        int y2 = r.bottom() + 1 + pw2 + (pw % 2);

        QRegion region;

        region += QRect(x1, y1, x2 - x1, pw);
        region += QRect(x1, y1, pw, y2 - y1);
        region += QRect(x1, y2 - pw, x2 - x1, pw);
        region += QRect(x2 - pw, y1, pw, y2 - y1);

        return region;
    }

public:
    band_picker(QWidget* widget);
    virtual void drawRubberBand(QPainter* painter) const override;
    virtual QRegion rubberBandMask() const override;
};
