#include "symbol_paths_dialog.h"

#include <qlistwidget.h>
#include <qpushbutton.h>
#include <qboxlayout.h>
#include <qlabel.h>
#include <qfiledialog.h>
#include <qdir.h>

symbol_paths_dialog::symbol_paths_dialog(const QStringList& paths, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Symbol paths"));
    resize(500, 300);

    auto* layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(tr("Directories searched for PDBs / DLLs when resolving native callstack frames:"), this));

    m_list = new QListWidget(this);
    for (const auto& p : paths)
        if (!p.isEmpty())
            m_list->addItem(p);
    layout->addWidget(m_list);

    auto* buttons = new QHBoxLayout();
    auto* addButton = new QPushButton(tr("Add..."), this);
    auto* removeButton = new QPushButton(tr("Remove"), this);
    buttons->addWidget(addButton);
    buttons->addWidget(removeButton);
    buttons->addStretch();
    auto* okButton = new QPushButton(tr("OK"), this);
    auto* cancelButton = new QPushButton(tr("Cancel"), this);
    buttons->addWidget(okButton);
    buttons->addWidget(cancelButton);
    layout->addLayout(buttons);

    connect(addButton, &QPushButton::clicked, this, &symbol_paths_dialog::addPath);
    connect(removeButton, &QPushButton::clicked, this, &symbol_paths_dialog::removePath);
    connect(okButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
}

QStringList symbol_paths_dialog::paths() const
{
    QStringList result;
    for (int i = 0; i < m_list->count(); ++i)
        result << m_list->item(i)->text();
    return result;
}

void symbol_paths_dialog::addPath()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr("Select symbol directory"), QDir::currentPath());
    if (!dir.isEmpty())
        m_list->addItem(dir);
}

void symbol_paths_dialog::removePath()
{
    qDeleteAll(m_list->selectedItems());
}
