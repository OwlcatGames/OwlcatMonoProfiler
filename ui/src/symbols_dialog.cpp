#include "symbols_dialog.h"
#include "theme.h"
#include "icons.h"

#include "mono_profiler_client.h"

#include <qlistwidget.h>
#include <qtablewidget.h>
#include <qheaderview.h>
#include <qboxlayout.h>
#include <qlabel.h>
#include <qpushbutton.h>
#include <qtoolbutton.h>
#include <qfiledialog.h>
#include <qsettings.h>
#include <qtimer.h>
#include <qdir.h>
#include <qframe.h>

#include <algorithm>

namespace
{
    // Modules the resolver never looks for symbols for; they are not failures
    // and should not drag the "resolved" count down.
    bool is_system_module(const std::string& name)
    {
        static const char* system_modules[] = {
            "ntdll.dll", "kernel32.dll", "kernelbase.dll", "user32.dll", "gdi32.dll",
            "gdi32full.dll", "msvcrt.dll", "ucrtbase.dll", "combase.dll", "ole32.dll",
            "oleaut32.dll", "rpcrt4.dll", "sechost.dll", "advapi32.dll", "shell32.dll",
            "shcore.dll", "win32u.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "nvwgf2umx.dll",
            "amdxc64.dll", "wintypes.dll", "bcryptprimitives.dll", "msvcp_win.dll",
        };

        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)::tolower(c); });
        const size_t slash = lower.find_last_of("\\/");
        if (slash != std::string::npos)
            lower = lower.substr(slash + 1);

        for (const char* sys : system_modules)
            if (lower == sys)
                return true;
        return false;
    }

    QLabel* section_label(const QString& text, QWidget* parent)
    {
        auto* label = new QLabel(text.toUpper(), parent);
        QFont f = label->font();
        f.setPointSizeF(f.pointSizeF() - 1.0);
        f.setLetterSpacing(QFont::PercentageSpacing, 106);
        label->setFont(f);
        label->setStyleSheet(QString("color: %1;").arg(theme::tokens().text_dim.name()));
        return label;
    }
}

symbols_dialog::symbols_dialog(owlcat::mono_profiler_client* client, QWidget* parent)
    : QDialog(parent)
    , m_client(client)
{
    setWindowTitle(tr("Symbols"));
    setWindowIcon(icons::symbols());
    resize(880, 620);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    // ---------------- header ----------------

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(tr("Symbols"), this);
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    header->addWidget(title);

    m_summary = new QLabel(this);
    header->addWidget(m_summary);
    header->addStretch();
    m_pending = new QLabel(this);
    m_pending->setStyleSheet(QString("color: %1;").arg(theme::tokens().text_dim.name()));
    header->addWidget(m_pending);
    layout->addLayout(header);

    // ---------------- search paths ----------------

    layout->addWidget(section_label(tr("Search paths"), this));
    m_hint = new QLabel(tr("Searched in order. A PDB must match the module's GUID and age, not just its name, "
                               "so several same-named PDBs on the path are fine."), this);
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);

    auto* paths_row = new QHBoxLayout();
    m_paths = new QListWidget(this);
    m_paths->setFont(theme::mono_font(0));
    m_paths->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_paths->setMinimumHeight(96);
    m_paths->setMaximumHeight(150);
    for (const auto& p : QSettings().value("symbols/paths").toStringList())
        if (!p.isEmpty())
            m_paths->addItem(p);
    paths_row->addWidget(m_paths, 1);

    auto* path_buttons = new QVBoxLayout();
    m_add = new QPushButton(icons::add_folder(), tr("Add folder..."), this);
    auto* add = m_add;
    m_remove = new QPushButton(icons::remove(), tr("Remove"), this);
    m_up = new QPushButton(icons::move_up(), QString(), this);
    m_up->setToolTip(tr("Search this folder earlier"));
    m_down = new QPushButton(icons::move_down(), QString(), this);
    m_down->setToolTip(tr("Search this folder later"));
    path_buttons->addWidget(add);
    path_buttons->addWidget(m_remove);
    auto* order_row = new QHBoxLayout();
    order_row->addWidget(m_up);
    order_row->addWidget(m_down);
    path_buttons->addLayout(order_row);
    path_buttons->addStretch();
    paths_row->addLayout(path_buttons);
    layout->addLayout(paths_row);

    connect(add,      &QPushButton::clicked, this, &symbols_dialog::addPath);
    connect(m_remove, &QPushButton::clicked, this, &symbols_dialog::removePath);
    connect(m_up,     &QPushButton::clicked, this, &symbols_dialog::movePathUp);
    connect(m_down,   &QPushButton::clicked, this, &symbols_dialog::movePathDown);

    auto update_buttons = [this]()
    {
        const bool has_selection = !m_paths->selectedItems().isEmpty();
        m_remove->setEnabled(has_selection);
        m_up->setEnabled(has_selection);
        m_down->setEnabled(has_selection);
    };
    connect(m_paths, &QListWidget::itemSelectionChanged, this, update_buttons);
    update_buttons();

    // ---------------- modules ----------------

    auto* modules_header = new QHBoxLayout();
    modules_header->addWidget(section_label(tr("Modules in this capture"), this));
    modules_header->addStretch();
    layout->addLayout(modules_header);

    m_modules = new QTableWidget(0, 3, this);
    m_modules->setHorizontalHeaderLabels({ tr("Module"), tr("Status"), tr("Symbol file") });
    m_modules->horizontalHeader()->resizeSection(0, 230);
    m_modules->horizontalHeader()->resizeSection(1, 150);
    m_modules->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_modules->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_modules->verticalHeader()->setVisible(false);
    m_modules->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_modules->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_modules->setAlternatingRowColors(false);
    m_modules->setShowGrid(false);
    m_modules->setWordWrap(false);
    m_modules->setFont(theme::mono_font(0));
    layout->addWidget(m_modules, 1);

    // ---------------- footer ----------------

    auto* footer = new QHBoxLayout();
    m_re_resolve = new QPushButton(icons::refresh(), tr("Re-resolve all callstacks"), this);
    connect(m_re_resolve, &QPushButton::clicked, this, &symbols_dialog::reResolve);
    footer->addWidget(m_re_resolve);
    footer->addStretch();
    auto* close = new QPushButton(tr("Close"), this);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    footer->addWidget(close);
    layout->addLayout(footer);

    // Symbolication runs on a background thread and modules appear as their
    // frames are first resolved, so keep the view current while visible
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &symbols_dialog::refresh);
    timer->start(1000);

    apply_theme();
}

void symbols_dialog::apply_theme()
{
    const auto& t = theme::tokens();

    setWindowIcon(icons::symbols());
    m_add->setIcon(icons::add_folder());
    m_remove->setIcon(icons::remove());
    m_up->setIcon(icons::move_up());
    m_down->setIcon(icons::move_down());
    m_re_resolve->setIcon(icons::refresh());

    m_hint->setStyleSheet(QString("color: %1;").arg(t.text_faint.name()));
    m_pending->setStyleSheet(QString("color: %1;").arg(t.text_dim.name()));
    m_paths->setFont(theme::mono_font(0));
    m_modules->setFont(theme::mono_font(0));

    // The module rows carry per-status colours
    refresh();
}

void symbols_dialog::showEvent(QShowEvent* ev)
{
    QDialog::showEvent(ev);
    refresh();
}

QStringList symbols_dialog::paths() const
{
    QStringList result;
    for (int i = 0; i < m_paths->count(); ++i)
        result << m_paths->item(i)->text();
    return result;
}

void symbols_dialog::applyPaths()
{
    const QStringList list = paths();
    QSettings().setValue("symbols/paths", list);
    m_client->set_symbol_paths(list.join(';').toStdString());
    emit symbolPathsChanged();
    refresh();
}

void symbols_dialog::addPath()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select symbol directory"), QDir::currentPath());
    if (dir.isEmpty())
        return;

    m_paths->addItem(QDir::toNativeSeparators(dir));
    applyPaths();
}

void symbols_dialog::removePath()
{
    qDeleteAll(m_paths->selectedItems());
    applyPaths();
}

void symbols_dialog::movePathUp()
{
    const int row = m_paths->currentRow();
    if (row <= 0)
        return;

    m_paths->insertItem(row - 1, m_paths->takeItem(row));
    m_paths->setCurrentRow(row - 1);
    applyPaths();
}

void symbols_dialog::movePathDown()
{
    const int row = m_paths->currentRow();
    if (row < 0 || row >= m_paths->count() - 1)
        return;

    m_paths->insertItem(row + 1, m_paths->takeItem(row));
    m_paths->setCurrentRow(row + 1);
    applyPaths();
}

void symbols_dialog::reResolve()
{
    // Re-applying the same path is what triggers a full re-resolve pass
    applyPaths();
}

void symbols_dialog::module_counts(owlcat::mono_profiler_client* client, int& resolved, int& total)
{
    using state = owlcat::symbol_module_status::state;

    resolved = 0;
    total = 0;
    for (const auto& m : client->get_symbol_module_status())
    {
        if (is_system_module(m.module))
            continue;

        ++total;
        if (m.status == state::loaded || m.status == state::exports)
            ++resolved;
    }
}

void symbols_dialog::refresh()
{
    // The timer keeps running while the window is hidden; don't waste work
    if (!isVisible())
        return;

    auto modules = m_client->get_symbol_module_status();
    std::sort(modules.begin(), modules.end(), [](const auto& a, const auto& b) { return a.module < b.module; });

    using state = owlcat::symbol_module_status::state;
    const auto& t = theme::tokens();

    int resolved = 0, total = 0;
    module_counts(m_client, resolved, total);

    const bool all_good = (total == 0) || (resolved == total);
    m_summary->setText(tr("%1 of %2 modules resolved").arg(resolved).arg(total));
    m_summary->setStyleSheet(QString("color: %1;").arg((all_good ? t.ok : t.warn).name()));

    const size_t pending = m_client->get_symbolication_pending();
    m_pending->setText(pending > 0 ? tr("resolving callstacks (%1 left)...").arg(pending) : QString());

    m_modules->setRowCount((int)modules.size());
    for (int i = 0; i < (int)modules.size(); ++i)
    {
        const auto& m = modules[i];
        const bool system = is_system_module(m.module);

        QString status_text;
        QColor  status_color;
        switch (m.status)
        {
        case state::loaded:   status_text = tr("resolved");        status_color = t.ok;        break;
        case state::exports:  status_text = tr("exports only");    status_color = t.warn;      break;
        case state::mismatch: status_text = tr("PDB mismatch");    status_color = t.danger;    break;
        case state::none:     status_text = tr("not found");       status_color = t.warn;      break;
        case state::failed:   status_text = tr("load failed");     status_color = t.danger;    break;
        case state::pending:  status_text = tr("pending");         status_color = t.text_dim;  break;
        }
        if (system)
        {
            status_text = tr("skipped");
            status_color = t.text_faint;
        }

        // Reuse existing items so periodic refreshes don't drop the selection
        auto set_item = [&](int col, const QString& text) -> QTableWidgetItem*
        {
            auto* item = m_modules->item(i, col);
            if (item == nullptr)
            {
                item = new QTableWidgetItem();
                m_modules->setItem(i, col, item);
            }
            item->setText(text);
            return item;
        };

        auto* module_item = set_item(0, QString::fromStdString(m.module));
        module_item->setForeground(system ? t.text_dim : t.text);
        set_item(1, status_text)->setForeground(status_color);

        QString file = QString::fromStdString(m.symbol_file);
        if (system && file.isEmpty())
            file = tr("system module - not searched");
        auto* file_item = set_item(2, file);
        file_item->setForeground(system ? t.text_faint : t.text_dim);
        file_item->setToolTip(file);
    }
}
