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
