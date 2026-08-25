#include "band_picker.h"
#include "common_ui.h"
#include "theme.h"

#include <qpainter.h>

band_picker::band_picker(QWidget* widget, y_format format)
        : QwtPlotPicker(widget)
        , m_y_format(format)
{
    apply_theme();
}

/*
    The cursor readout and the selection band are painted on the plot canvas,
    which is nearly black in the dark theme - Qwt's default black pen makes both
    invisible there. Everything visual here comes from the token table instead.
*/
void band_picker::apply_theme()
{
    const auto& t = theme::tokens();

    setTrackerPen(QPen(t.text));
    setTrackerFont(theme::mono_font(-1));

    QPen band(t.selection_band, 1.0);
    setRubberBandPen(band);
}

QwtText band_picker::trackerTextF(const QPointF& pos) const
{
    const qint64 frame = qRound64(pos.x());

    // Prefer showing the graph's value at the cursor's frame: the cursor's own
    // Y coordinate is rarely exactly on the curve, and misleads
    if (m_value_text)
    {
        QString text = m_value_text(frame);
        if (!text.isEmpty())
            return make_tracker_text(text);
    }

    QString value;
    if (m_y_format == y_format::bytes)
        value = size_to_string(pos.y());
    else
        value = QString::number(qRound64(pos.y()));

    return make_tracker_text(QString("frame %1, %2").arg(frame).arg(value));
}

// A small panel-coloured chip behind the readout, so it stays legible over the
// curve, the selection band and the plot background alike
QwtText band_picker::make_tracker_text(const QString& text) const
{
    const auto& t = theme::tokens();

    QwtText result(text);
    result.setColor(t.text);
    result.setFont(theme::mono_font(-1));

    QColor background = t.panel_hi;
    background.setAlpha(232);
    result.setBackgroundBrush(QBrush(background));
    result.setBorderPen(QPen(t.border_strong));
    result.setBorderRadius(3.0);
    result.setPaintAttribute(QwtText::PaintBackground, true);
    result.setRenderFlags(Qt::AlignLeft | Qt::AlignVCenter);
    return result;
}

/*
    The band spans the full height of the pick area, from the drag's left x to
    its right x. Both the painting and the overlay mask derive from this one
    rect - if they disagree, part of the band is clipped away invisibly.
*/
QRect band_picker::band_rect() const
{
    auto sel = selection().boundingRect();
    auto area = pickArea().boundingRect();
    sel.setY(area.top());
    sel.setBottom(area.height());
    return QRect(sel.topLeft(), sel.bottomRight()).normalized();
}

void band_picker::drawRubberBand(QPainter* painter) const
{
    const QRect rect = band_rect();

    // Fill first, then the two edges: a 10% accent wash reads as "this range",
    // where a bare outline was easy to miss
    const auto& t = theme::tokens();
    QColor fill = t.selection_band;
    fill.setAlphaF(0.10);
    painter->fillRect(rect, fill);

    painter->setPen(QPen(t.selection_band, 1.0));
    painter->drawLine(rect.topLeft(), rect.bottomLeft());
    painter->drawLine(rect.topRight(), rect.bottomRight());
}

QRegion band_picker::rubberBandMask() const
{
    // The whole rect, not just its outline: the overlay only paints inside this
    // mask, so an outline-shaped mask (what qwtMaskRegion builds) threw away the
    // translucent fill and clipped the right edge one pixel out of view.
    // One pixel of slack on each side leaves room for the edge strokes.
    return QRegion(band_rect().adjusted(-1, -1, 2, 2));
}
