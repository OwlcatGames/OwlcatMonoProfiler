#pragma once
#include "qwt_scale_draw.h"
#include "common_ui.h"

/*
    Class that supplies the axis of graph of allocated memory with correct labels
*/
class bytes_scale_draw : public QwtScaleDraw
{
public:
    virtual QwtText label(double value) const override
    {
        return QwtText(size_to_string(value));
    }
};

/*
    Axis labels for counts (allocations per frame): plain 1.2M / 450k / 900
    instead of Qwt's default scientific notation.
*/
class counts_scale_draw : public QwtScaleDraw
{
public:
    virtual QwtText label(double value) const override
    {
        const double v = qAbs(value);
        if (v >= 1000000.0)
            return QwtText(QString::number(value / 1000000.0, 'f', v >= 10000000.0 ? 0 : 1) + "M");
        if (v >= 1000.0)
            return QwtText(QString::number(value / 1000.0, 'f', v >= 10000.0 ? 0 : 1) + "k");
        return QwtText(QString::number(value, 'f', 0));
    }
};
