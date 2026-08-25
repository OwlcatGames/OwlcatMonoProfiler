#include "callstack_delegate.h"
#include "theme.h"
#include "frame_classifier.h"

#include <qpainter.h>
#include <qapplication.h>

namespace
{
    // Same split as the inspector: dim what is context, bright what is the method
    void draw_frame_line(QPainter* painter, const QRect& rect, const QString& line,
                         const QColor& dim, const QColor& bright, const QFont& font)
    {
        QString head = line;
        QString suffix;

        const int paren = head.indexOf(" (");
        if (paren > 0)
        {
            suffix = head.mid(paren);
            head = head.left(paren);
        }

        QString prefix;
        QString method = head;
        const int bang = head.indexOf('!');
        if (bang > 0)
        {
            prefix = head.left(bang + 1);
            method = head.mid(bang + 1);
        }
        else
        {
            const int dot = head.lastIndexOf('.');
            if (dot > 0)
            {
                prefix = head.left(dot + 1);
                method = head.mid(dot + 1);
            }
        }

        painter->setFont(font);
        const QFontMetrics metrics(font);

        int x = rect.left();
        const int max_x = rect.right();

        const auto draw_part = [&](const QString& text, const QColor& color)
        {
            if (text.isEmpty() || x >= max_x)
                return;

            const int available = max_x - x;
            const QString shown = metrics.horizontalAdvance(text) <= available
                ? text
                : metrics.elidedText(text, Qt::ElideRight, available);

            painter->setPen(color);
            painter->drawText(QRect(x, rect.top(), available, rect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, shown);
            x += metrics.horizontalAdvance(shown);
        };

        draw_part(prefix, dim);
        draw_part(method, bright);
        draw_part(suffix, dim);
    }
}

callstack_delegate::callstack_delegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

QSize callstack_delegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const QFontMetrics metrics(option.font);
    return QSize(option.rect.width(), metrics.height() * 2 + 6);
}

void callstack_delegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const auto& t = theme::tokens();

    // Background, selection and hover exactly as the style would draw them
    QStyleOptionViewItem background_option = option;
    background_option.text.clear();
    QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &background_option, painter, option.widget);

    const QString full = index.data(Qt::DisplayRole).toString();
    if (full.isEmpty())
        return;

    const QStringList lines = full.split('\n', Qt::SkipEmptyParts);
    if (lines.isEmpty())
        return;

    // Which frame this row is about: the allocator leaf says nothing, since every
    // managed allocation ends in an icall wrapper and every native one in
    // MemoryManager::Allocate. Attribute to the first frame of game code instead.
    const bool attribute = frame_class::attribution_enabled();
    const int leaf_index = attribute ? frame_class::attribution_index(lines) : 0;
    const int next_index = attribute
        ? frame_class::next_game_index(lines, leaf_index)
        : (lines.size() > 1 ? 1 : -1);

    const bool selected = option.state & QStyle::State_Selected;
    const QColor bright = selected ? t.text_bright : t.text;

    QFont leaf_font = option.font;
    QFont tail_font = option.font;
    tail_font.setPixelSize(qMax(9, (option.font.pixelSize() > 0 ? option.font.pixelSize() : 13) - 1));

    const QFontMetrics leaf_metrics(leaf_font);
    const QFontMetrics tail_metrics(tail_font);

    const QRect content = option.rect.adjusted(6, 2, -6, -2);
    const QRect leaf_rect(content.left(), content.top(), content.width(), leaf_metrics.height());

    painter->save();

    QRect leaf_text_rect = leaf_rect;

    // How many runtime frames were skipped to get here. Shown, never hidden: the
    // tooltip and the inspector still carry every frame.
    if (leaf_index > 0)
    {
        const QString badge = QString::fromUtf8("\xE2\x86\x91") + QString::number(leaf_index);
        painter->setFont(tail_font);
        painter->setPen(t.text_faint);
        const int badge_width = tail_metrics.horizontalAdvance(badge) + 6;
        painter->drawText(QRect(leaf_rect.left(), leaf_rect.top(), badge_width, leaf_rect.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, badge);
        leaf_text_rect.setLeft(leaf_rect.left() + badge_width);
    }

    draw_frame_line(painter, leaf_text_rect, lines[leaf_index].trimmed(), t.text_dim, bright, leaf_font);

    // Second line: the caller, so the row says where this allocation came from
    if (next_index >= 0)
    {
        QString tail = QString::fromUtf8("\xE2\x86\x90 ") + lines[next_index].trimmed();
        const int remaining = lines.size() - next_index - 1;
        if (remaining > 0)
            tail += QString::fromUtf8(" \xE2\x86\x90 ") + QObject::tr("%1 more").arg(remaining);

        const QRect tail_rect(content.left(), leaf_rect.bottom() + 1, content.width(), tail_metrics.height());
        painter->setFont(tail_font);
        painter->setPen(t.text_faint);
        painter->drawText(tail_rect, Qt::AlignLeft | Qt::AlignVCenter,
                          tail_metrics.elidedText(tail, Qt::ElideRight, tail_rect.width()));
    }
    painter->restore();
}
