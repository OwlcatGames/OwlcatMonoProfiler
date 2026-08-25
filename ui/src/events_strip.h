#pragma once
#include <qwidget.h>
#include <qcolor.h>
#include <qstring.h>

#include <vector>

class QwtPlot;

/*
    The events lane: a 20px strip between the memory and allocations plots that
    marks *when* things happened, on the same X axis as both plots.

    Today it carries GC events. Instrumented markers ("started loading area")
    land here too once the server can report them - they are plain shapes in a
    colour the calling code picks, never labels: text drawn in a 20px strip
    collides with itself and covers the data at any zoom. Names live in the
    legend and in the hover tooltip.

    X positions come from a reference QwtPlot, so the strip is aligned with the
    canvases by construction (see main_window::syncAxisExtents).
*/
class events_strip : public QWidget
{
    Q_OBJECT
public:
    enum class shape { diamond, circle, square, triangle };

    struct event_mark
    {
        int64_t frame;
        QString name;
        QColor  color;
        shape   form;
    };

    events_strip(QwtPlot* reference, QWidget* parent = nullptr);

    // Everything drawn in the strip: inferred collections today, instrumented
    // markers once the server can report them. Shape and colour per entry.
    void set_events(std::vector<event_mark> events);
    // Highlighted frame range, matching the plots' selection band ([first, last])
    void set_selection(int64_t first_frame, int64_t last_frame);

    QSize sizeHint() const override;

signals:
    void frameClicked(qint64 frame);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    bool event(QEvent* ev) override;

private:
    // Canvas-relative x for an axis value, shifted into strip coordinates
    int frame_to_x(double frame) const;
    // Nearest event within a few pixels of x, or nullptr
    const event_mark* event_at(int x) const;

    QwtPlot* m_reference;
    std::vector<event_mark> m_events;
    // Every drawn event in strip coordinates, rebuilt on paint for hit-testing
    mutable std::vector<std::pair<int, event_mark>> m_hit_test;

    int64_t m_selection_first = -1;
    int64_t m_selection_last = -1;
};
