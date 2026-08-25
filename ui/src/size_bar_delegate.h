#pragma once
#include <qstyleditemdelegate.h>

#include <functional>

/*
    Draws a thin proportional bar under the Size column's text: the row's share
    of the total currently in view, so "how much of the heap is this?" is
    readable without arithmetic.

    Cheaper than it looks: one extra fillRect per visible row, and no text
    layout - which is why this is the right place to spend paint time in a table
    that can hold hundreds of thousands of rows.
*/
class size_bar_delegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    // size_role: the model role carrying the raw byte count for a row
    size_bar_delegate(int size_role, QObject* parent = nullptr);

    // The total the bar is drawn relative to (all rows in the model). A callback
    // rather than a value, because the model is repopulated often.
    void set_total_provider(std::function<quint64()> provider);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    int m_size_role;
    std::function<quint64()> m_total_provider;
};
