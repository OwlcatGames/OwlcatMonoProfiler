#pragma once
#include <qstyleditemdelegate.h>

/*
    Two fixed lines per callstack row: the leaf frame, then a summary of the
    tail. The full stack is in the inspector beside the table.

    Why not just show the whole stack in the cell (what the app did before):
    every row then needs its own height, measured from the text, which is the
    reason for the chunked row-height pass in main_window - and the text was
    still clipped. Two lines make every row the same height and put the frame
    that matters, the leaf, in full contrast.
*/
class callstack_delegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    callstack_delegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};
