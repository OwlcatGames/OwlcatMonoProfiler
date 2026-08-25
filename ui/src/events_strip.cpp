#include "events_strip.h"
#include "theme.h"

#include "qwt_plot.h"
#include "qwt_plot_canvas.h"

#include <qpainter.h>
#include <qpainterpath.h>
#include <qevent.h>
#include <qtooltip.h>

#include <algorithm>

namespace
{
    const int kStripHeight = 20;
    // How close the cursor has to be to an event to get its tooltip
    const int kHitSlack = 5;
}

events_strip::events_strip(QwtPlot* reference, QWidget* parent)
    : QWidget(parent)
    , m_reference(reference)
{
    setFixedHeight(kStripHeight);
    setMouseTracking(true);
    setToolTip(QString());   // enables the ToolTip event without a static text
}

QSize events_strip::sizeHint() const
{
    return QSize(0, kStripHeight);
}

void events_strip::set_events(std::vector<event_mark> events)
{
    m_events = std::move(events);
    update();
}

void events_strip::set_selection(int64_t first_frame, int64_t last_frame)
{
    if (m_selection_first == first_frame && m_selection_last == last_frame)
        return;

    m_selection_first = first_frame;
    m_selection_last = last_frame;
    update();
}

int events_strip::frame_to_x(double frame) const
{
    if (m_reference == nullptr || m_reference->canvas() == nullptr)
        return -1;

    // transform() is canvas-relative; the canvas is inset from the plot widget
    // by the left axis, and the strip shares the plot's column in the layout
    return m_reference->canvas()->x() + (int)qRound(m_reference->transform(QwtPlot::xBottom, frame));
}

void events_strip::paintEvent(QPaintEvent*)
{
    const auto& t = theme::tokens();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRect area = rect();
    p.fillRect(area, t.plot_bg);
    p.setPen(QPen(t.border, 1.0));
    p.drawLine(area.topLeft(), area.topRight());
    p.drawLine(area.bottomLeft(), area.bottomRight());

    if (m_reference == nullptr || m_reference->canvas() == nullptr)
        return;

    // The selection band, so range, GC and spike line up vertically
    if (m_selection_first >= 0 && m_selection_last >= m_selection_first)
    {
        const int x1 = frame_to_x((double)m_selection_first);
        const int x2 = frame_to_x((double)m_selection_last + 1.0);
        QColor fill = t.selection_band;
        fill.setAlphaF(0.10);
        p.fillRect(QRect(x1, area.top() + 1, qMax(1, x2 - x1), area.height() - 2), fill);
        p.setPen(QPen(t.selection_band, 1.0));
        p.drawLine(x1, area.top() + 1, x1, area.bottom() - 1);
        p.drawLine(x2, area.top() + 1, x2, area.bottom() - 1);
    }

    m_hit_test.clear();

    const int canvas_left = m_reference->canvas()->x();
    const int canvas_right = canvas_left + m_reference->canvas()->width();
    const int cy = area.center().y() + 1;

    // ---- one pass over every event: shape and colour come from the entry ----
    p.setPen(Qt::NoPen);
    // Far enough left to be outside any canvas, but NOT INT_MIN: "x - last_x"
    // overflowed signed int for the first visible event and wrapped negative,
    // so the collapse rule below swallowed it
    int last_x = -10000;
    int off_left = 0, off_right = 0;
    const event_mark* nearest_left = nullptr;
    const event_mark* nearest_right = nullptr;
    for (const auto& mark : m_events)
    {
        const int x = frame_to_x((double)mark.frame + 0.5);
        // Events are rare enough that "off screen" needs saying out loud: count
        // them and remember the nearest one on each side for the edge markers
        if (x < canvas_left)
        {
            ++off_left;
            nearest_left = &mark;
            continue;
        }
        if (x > canvas_right)
        {
            ++off_right;
            if (nearest_right == nullptr)
                nearest_right = &mark;
            continue;
        }
        // Zoomed out, several events land on the same pixel; one glyph is enough
        if (x - last_x < 3)
            continue;
        last_x = x;

        p.setBrush(QBrush(mark.color));
        switch (mark.form)
        {
        case shape::circle:
            p.drawEllipse(QPointF(x, cy), 4.2, 4.2);
            break;
        case shape::square:
            p.drawRect(QRectF(x - 4.0, cy - 4.0, 8.0, 8.0));
            break;
        case shape::triangle:
        {
            QPainterPath tri;
            tri.moveTo(x, cy - 4.8);
            tri.lineTo(x + 4.4, cy + 4.0);
            tri.lineTo(x - 4.4, cy + 4.0);
            tri.closeSubpath();
            p.drawPath(tri);
            break;
        }
        case shape::diamond:
        default:
        {
            QPainterPath diamond;
            diamond.moveTo(x, cy - 4.6);
            diamond.lineTo(x + 4.6, cy);
            diamond.lineTo(x, cy + 4.6);
            diamond.lineTo(x - 4.6, cy);
            diamond.closeSubpath();
            p.drawPath(diamond);
            break;
        }
        }

        m_hit_test.push_back({ x, mark });
    }

    // ---- edge markers: "there are collections outside this view" ----
    const auto draw_edge = [&](bool left, int count, const event_mark* nearest)
    {
        if (count <= 0)
            return;

        const int x = left ? canvas_left + 5 : canvas_right - 5;
        QColor color = nearest != nullptr ? nearest->color : t.series_gc;
        color.setAlphaF(0.55);
        p.setBrush(QBrush(color));

        QPainterPath arrow;
        if (left)
        {
            arrow.moveTo(x - 4, cy);
            arrow.lineTo(x + 2, cy - 4.5);
            arrow.lineTo(x + 2, cy + 4.5);
        }
        else
        {
            arrow.moveTo(x + 4, cy);
            arrow.lineTo(x - 2, cy - 4.5);
            arrow.lineTo(x - 2, cy + 4.5);
        }
        arrow.closeSubpath();
        p.drawPath(arrow);

        QString text = tr("%1 event(s) outside the view").arg(count);
        if (nearest != nullptr)
            text += "\n" + nearest->name;
        m_hit_test.push_back({ x, event_mark{ nearest != nullptr ? nearest->frame : -1, text, color, shape::triangle } });
    };

    draw_edge(true, off_left, nearest_left);
    draw_edge(false, off_right, nearest_right);
}

const events_strip::event_mark* events_strip::event_at(int x) const
{
    const event_mark* best = nullptr;
    int best_distance = kHitSlack + 1;

    for (const auto& entry : m_hit_test)
    {
        const int distance = qAbs(entry.first - x);
        if (distance < best_distance)
        {
            best_distance = distance;
            best = &entry.second;
        }
    }

    return best;
}

bool events_strip::event(QEvent* ev)
{
    if (ev->type() == QEvent::ToolTip)
    {
        auto* help = static_cast<QHelpEvent*>(ev);
        if (const event_mark* mark = event_at(help->pos().x()))
        {
            QToolTip::showText(help->globalPos(), mark->name, this);
        }
        else
        {
            QToolTip::hideText();
            ev->ignore();
        }
        return true;
    }

    return QWidget::event(ev);
}

void events_strip::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_reference == nullptr || m_reference->canvas() == nullptr)
        return;

    // Clicking the strip is a shortcut to "put the cursor on this frame"
    const double frame = m_reference->invTransform(QwtPlot::xBottom, ev->pos().x() - m_reference->canvas()->x());
    emit frameClicked((qint64)qRound(frame));
}
