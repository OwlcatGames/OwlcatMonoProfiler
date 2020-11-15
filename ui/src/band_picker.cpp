#include "band_picker.h"

band_picker::band_picker(QWidget* widget)
        : QwtPlotPicker(widget)
{}

void band_picker::drawRubberBand(QPainter* painter) const
{
    auto sel = selection().boundingRect();
    auto area = pickArea().boundingRect();
    auto mask = rubberBandMask().begin();
    int w = sel.width();
    sel.setY(area.top());
    sel.setBottom(area.height());
    const QRect rect = QRect(sel.topLeft(), sel.bottomRight()).normalized();
    QwtPainter::drawRect(painter, rect);
}

QRegion band_picker::rubberBandMask() const
{
    const QPolygon pa = adjustedPoints(pickedPoints());

    const int pw = rubberBandPen().width();

    auto sel = selection().boundingRect();
    auto area = pickArea().boundingRect();
    sel.setY(area.top());
    sel.setBottom(area.height());
    const QRect rect = QRect(sel.topLeft(), sel.bottomRight());

    QRegion mask = qwtMaskRegion(rect.normalized(), pw);

    return mask;
}
