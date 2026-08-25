#pragma once
#include <qdialog.h>

class QTableWidget;
class QLabel;

namespace owlcat { class mono_profiler_client; }

/*
    Modeless window showing per-module symbol resolution status: for which modules
    referenced by native callstack frames a PDB was found and parsed, and which are
    missing symbols. Refreshes itself periodically while open, since symbolication
    runs on a background thread. Built programmatically (no .ui).
*/
class symbol_status_dialog : public QDialog
{
    Q_OBJECT
public:
    symbol_status_dialog(owlcat::mono_profiler_client* client, QWidget* parent = nullptr);

private slots:
    void refresh();

private:
    void showEvent(QShowEvent* ev) override;

    owlcat::mono_profiler_client* m_client;
    QTableWidget* m_table;
    QLabel* m_summary;
};
