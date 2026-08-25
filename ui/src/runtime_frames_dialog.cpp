#include "runtime_frames_dialog.h"
#include "frame_classifier.h"
#include "theme.h"
#include "icons.h"

#include <qlistwidget.h>
#include <qpushbutton.h>
#include <qboxlayout.h>
#include <qlabel.h>
#include <qmessagebox.h>

runtime_frames_dialog::runtime_frames_dialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Runtime frames"));
    resize(520, 560);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto* hint = new QLabel(tr(
        "Frames matching these patterns are treated as allocator, runtime or engine "
        "plumbing, so a callstack row is labelled with the first frame that is not one "
        "of them. Nothing is hidden: the row shows how many frames were skipped and the "
        "inspector still lists every frame.\n\n"
        "Three forms, by shape:\n"
        "  unityplayer.dll, mono_profiler*.dll   module of a native frame (* and ? allowed)\n"
        "  System.,  Array.                      prefix of a managed frame\n"
        "  __icall_wrapper                       substring anywhere in the frame"), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: %1;").arg(theme::tokens().text_dim.name()));
    layout->addWidget(hint);

    auto* row = new QHBoxLayout();

    m_list = new QListWidget(this);
    m_list->setFont(theme::mono_font(0));
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    row->addWidget(m_list, 1);

    auto* buttons = new QVBoxLayout();
    auto* add = new QPushButton(tr("Add"), this);
    m_remove = new QPushButton(icons::remove(), tr("Remove"), this);
    auto* defaults = new QPushButton(icons::refresh(), tr("Restore defaults"), this);
    buttons->addWidget(add);
    buttons->addWidget(m_remove);
    buttons->addSpacing(12);
    buttons->addWidget(defaults);
    buttons->addStretch();
    row->addLayout(buttons);
    layout->addLayout(row);

    m_count = new QLabel(this);
    m_count->setStyleSheet(QString("color: %1;").arg(theme::tokens().text_faint.name()));
    layout->addWidget(m_count);

    auto* footer = new QHBoxLayout();
    footer->addStretch();
    auto* close = new QPushButton(tr("Close"), this);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    footer->addWidget(close);
    layout->addLayout(footer);

    connect(add, &QPushButton::clicked, this, &runtime_frames_dialog::addPattern);
    connect(m_remove, &QPushButton::clicked, this, &runtime_frames_dialog::removeSelected);
    connect(defaults, &QPushButton::clicked, this, &runtime_frames_dialog::restoreDefaults);
    // Editing an item in place is the common case, so apply on every change
    connect(m_list, &QListWidget::itemChanged, this, &runtime_frames_dialog::applyList);
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this]()
    {
        m_remove->setEnabled(!m_list->selectedItems().isEmpty());
    });

    fill(frame_class::patterns());
    m_remove->setEnabled(false);
}

void runtime_frames_dialog::fill(const QStringList& patterns)
{
    // Repopulating fires itemChanged for every row; don't write back mid-fill
    const bool blocked = m_list->blockSignals(true);
    m_list->clear();
    for (const QString& pattern : patterns)
    {
        auto* item = new QListWidgetItem(pattern, m_list);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    m_list->blockSignals(blocked);

    m_count->setText(tr("%1 pattern(s)").arg(m_list->count()));
}

QStringList runtime_frames_dialog::currentList() const
{
    QStringList result;
    for (int i = 0; i < m_list->count(); ++i)
    {
        const QString text = m_list->item(i)->text().trimmed();
        if (!text.isEmpty())
            result << text;
    }
    return result;
}

void runtime_frames_dialog::applyList()
{
    frame_class::set_patterns(currentList());
    m_count->setText(tr("%1 pattern(s)").arg(m_list->count()));
    emit patternsChanged();
}

void runtime_frames_dialog::addPattern()
{
    auto* item = new QListWidgetItem(tr("module.dll"), m_list);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    m_list->setCurrentItem(item);
    m_list->editItem(item);
}

void runtime_frames_dialog::removeSelected()
{
    qDeleteAll(m_list->selectedItems());
    applyList();
}

void runtime_frames_dialog::restoreDefaults()
{
    fill(frame_class::default_patterns());
    applyList();
}
