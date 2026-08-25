#include "size_bar_delegate.h"
#include "theme.h"

#include <qpainter.h>

namespace
{
    // Bar height and the gap between it and the text baseline
    const int kBarHeight = 3;
    const int kBarGap = 2;
}

size_bar_delegate::size_bar_delegate(int size_role, QObject* parent)
    : QStyledItemDelegate(parent)
    , m_size_role(size_role)
{
}

void size_bar_delegate::set_total_provider(std::function<quint64()> provider)
{
    m_total_provider = std::move(provider);
}

QSize size_bar_delegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QSize hint = QStyledItemDelegate::sizeHint(option, index);
    hint.setHeight(hint.height() + kBarHeight + kBarGap);
    return hint;
}

void size_bar_delegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    // Text first, in the cell minus the strip the bar occupies
    QStyleOptionViewItem text_option = option;
    text_option.rect = option.rect.adjusted(0, 0, 0, -(kBarHeight + kBarGap));
    QStyledItemDelegate::paint(painter, text_option, index);

    if (!m_total_provider)
        return;

    const quint64 total = m_total_provider();
    if (total == 0)
        return;

    const QVariant value = index.data(m_size_role);
    if (!value.isValid())
        return;

    const quint64 size = value.toULongLong();
    if (size == 0)
        return;

    const int track_width = option.rect.width() - 8;
    if (track_width <= 0)
        return;

    // Share of the total in view. At least one pixel, so a small row still reads
    // as present rather than as an empty cell
    const double share = qMin(1.0, (double)size / (double)total);
    const int width = qMax(1, (int)((double)track_width * share));

    const auto& t = theme::tokens();
    const bool selected = option.state & QStyle::State_Selected;
    QColor color = selected ? t.accent : t.series_allocs;
    color.setAlphaF(selected ? 0.95 : 0.75);

    const QRect bar(option.rect.left() + 4,
                    option.rect.bottom() - kBarHeight - 1,
                    width,
                    kBarHeight);

    painter->save();
    painter->setPen(Qt::NoPen);
    painter->fillRect(QRect(bar.left(), bar.top(), track_width, kBarHeight), t.grid);
    painter->fillRect(bar, color);
    painter->restore();
}
