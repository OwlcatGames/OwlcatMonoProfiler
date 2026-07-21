#pragma once
#include <qdialog.h>
#include <qstringlist.h>

class QListWidget;

/*
    Dialog for configuring the local directories searched for PDBs/DLLs when resolving
    native callstack frames to function names. Built programmatically (no .ui).
*/
class symbol_paths_dialog : public QDialog
{
    Q_OBJECT
public:
    symbol_paths_dialog(const QStringList& paths, QWidget* parent = nullptr);

    // The configured directories
    QStringList paths() const;

private slots:
    void addPath();
    void removePath();

private:
    QListWidget* m_list;
};
