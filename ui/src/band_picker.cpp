#include "band_picker.h"
#include "common_ui.h"

band_picker::band_picker(QWidget* widget, y_format format)
        : QwtPlotPicker(widget)
        , m_y_format(format)
{}

QwtText band_picker::trackerTextF(const QPointF& pos) const
{
    const qint64 frame = qRound64(pos.x());

    // Prefer showing the graph's value at the cursor's frame: the cursor's own
    // Y coordinate is rarely exactly on the curve, and misleads
    if (m_value_text)
    {
        QString text = m_value_text(frame);
        if (!text.isEmpty())
            return QwtText(text);
    }

    QString value;
    if (m_y_format == y_format::bytes)
        value = size_to_string(pos.y());
    else
        value = QString::number(qRound64(pos.y()));

    return QwtText(QString("frame %1, %2").arg(frame).arg(value));
}

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
