#include "symbol_status_dialog.h"

#include "mono_profiler_client.h"

#include <qtablewidget.h>
#include <qheaderview.h>
#include <qboxlayout.h>
#include <qlabel.h>
#include <qpushbutton.h>
#include <qtimer.h>

#include <algorithm>

symbol_status_dialog::symbol_status_dialog(owlcat::mono_profiler_client* client, QWidget* parent)
    : QDialog(parent)
    , m_client(client)
{
    setWindowTitle(tr("Symbol resolution status"));
    resize(750, 400);

    auto* layout = new QVBoxLayout(this);

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);

    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({ tr("Module"), tr("Status"), tr("Symbol file") });
    m_table->horizontalHeader()->resizeSection(0, 220);
    m_table->horizontalHeader()->resizeSection(1, 140);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(false);
    layout->addWidget(m_table);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    auto* closeButton = new QPushButton(tr("Close"), this);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);

    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

    // Symbolication runs on a background thread and modules appear as their frames
    // are first resolved, so keep the view current while the window is open
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &symbol_status_dialog::refresh);
    timer->start(1000);
}

void symbol_status_dialog::showEvent(QShowEvent* ev)
{
    QDialog::showEvent(ev);
    refresh();
}

void symbol_status_dialog::refresh()
{
    // The timer keeps running while the window is closed (hidden); don't waste work
    if (!isVisible())
        return;

    auto modules = m_client->get_symbol_module_status();
    std::sort(modules.begin(), modules.end(), [](const auto& a, const auto& b) { return a.module < b.module; });

    using state = owlcat::symbol_module_status::state;

    size_t with_symbols = 0;
    for (const auto& m : modules)
        if (m.status == state::loaded || m.status == state::exports)
            ++with_symbols;

    QString summary = tr("%1 modules referenced by native callstacks, symbols found for %2.")
        .arg(modules.size()).arg(with_symbols);
    const size_t pending = m_client->get_symbolication_pending();
    if (pending > 0)
        summary += tr(" Resolving callstacks (%1 left)...").arg(pending);
    m_summary->setText(summary);

    m_table->setRowCount((int)modules.size());
    for (int i = 0; i < (int)modules.size(); ++i)
    {
        const auto& m = modules[i];

        QString status_text;
        QColor status_color;
        switch (m.status)
        {
        case state::loaded:   status_text = tr("PDB loaded");            status_color = QColor(0, 160, 0);     break;
        case state::mismatch: status_text = tr("PDB mismatch (not used)"); status_color = QColor(200, 0, 0);   break;
        case state::exports:  status_text = tr("Exports only (no PDB)"); status_color = QColor(220, 130, 0);   break;
        case state::none:     status_text = tr("No symbols found");      status_color = QColor(200, 0, 0);     break;
        case state::failed:   status_text = tr("Failed to load module"); status_color = QColor(200, 0, 0);     break;
        case state::pending:  status_text = tr("Pending");               status_color = QColor(128, 128, 128); break;
        }

        // Reuse existing items so periodic refreshes don't drop the selection
        auto set_item = [&](int col, const QString& text) -> QTableWidgetItem*
        {
            auto* item = m_table->item(i, col);
            if (item == nullptr)
            {
                item = new QTableWidgetItem();
                m_table->setItem(i, col, item);
            }
            item->setText(text);
            return item;
        };

        set_item(0, QString::fromStdString(m.module));
        set_item(1, status_text)->setForeground(status_color);
        auto* file_item = set_item(2, QString::fromStdString(m.symbol_file));
        file_item->setToolTip(file_item->text());
    }
}
