#pragma once
#include "qwt_plot_picker.h"
#include "qwt_painter.h"

#include <functional>

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
    // How the tracker (the text at the cursor) formats the Y coordinate
    enum class y_format
    {
        // A plain integer (e.g. a number of allocations)
        count,
        // A size in bytes, shown as B/Kb/Mb/Gb - same as the axis labels
        bytes,
    };

    band_picker(QWidget* widget, y_format format);
    virtual void drawRubberBand(QPainter* painter) const override;
    virtual QRegion rubberBandMask() const override;
    virtual QwtText trackerTextF(const QPointF& pos) const override;

    // Formats the tracker text for the given frame from the graph's actual data
    // (rather than from the cursor position). If it returns an empty string (e.g.
    // no data for that frame), the tracker falls back to the cursor coordinates.
    using value_text_func = std::function<QString(qint64 frame)>;
    void set_value_text(value_text_func func) { m_value_text = func; }

private:
    y_format m_y_format;
    value_text_func m_value_text;
};
