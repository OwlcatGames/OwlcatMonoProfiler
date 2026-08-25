#pragma once
#include <qdialog.h>
#include <qstringlist.h>

class QListWidget;
class QTableWidget;
class QLabel;
class QPushButton;

namespace owlcat { class mono_profiler_client; }

/*
    The Symbols window: search paths on top, per-module resolution status below.

    Supersedes the old pair of dialogs (symbol_paths_dialog + symbol_status_dialog):
    setting a path and watching the modules go green is one task, so it belongs in
    one modeless window. Path edits are applied immediately (and persisted to
    QSettings), which re-resolves every callstack on the client's background thread;
    the module table refreshes on a timer while the window is visible.
*/
class symbols_dialog : public QDialog
{
    Q_OBJECT
public:
    symbols_dialog(owlcat::mono_profiler_client* client, QWidget* parent = nullptr);

    // Counts for the toolbar badge / status bar: modules with usable symbols
    // out of modules referenced by native frames (system modules excluded).
    static void module_counts(owlcat::mono_profiler_client* client, int& resolved, int& total);

signals:
    // Emitted when the search path changed, so the caller can refresh its badge
    void symbolPathsChanged();

private slots:
    void addPath();
    void removePath();
    void movePathUp();
    void movePathDown();
    void reResolve();
    void refresh();

private:
    void showEvent(QShowEvent* ev) override;
    void applyPaths();
    QStringList paths() const;

    owlcat::mono_profiler_client* m_client;
    QListWidget*  m_paths;
    QTableWidget* m_modules;
    QLabel*       m_summary;
    QLabel*       m_pending;
    QPushButton*  m_remove;
    QPushButton*  m_up;
    QPushButton*  m_down;
};
