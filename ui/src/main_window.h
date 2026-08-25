#pragma once
#include <qmainwindow.h>
#include <qitemselectionmodel.h>
#include <qsortfilterproxymodel.h>
#include <qevent.h>
#include <qprogressbar.h>
#include <qboxlayout.h>
#include <qtableview.h>
#include <qthread.h>
#include <qelapsedtimer.h>
#include <qtimer.h>
#include "qwt_plot_histogram.h"
#include "qwt_plot_curve.h"
#include "qwt_plot_zoneitem.h"
#include "qwt_plot_marker.h"

#include "mono_profiler_client.h"
#include "band_picker.h"
#include "byte_scale_draw.h"
#include "graphs_data.h"
#include "live_objects_data.h"
#include "search_results_data.h"
#include "object_references_model.h"

namespace Ui {
    class MainWindow;
}

class symbols_dialog;
class QToolButton;
class QLabel;

/*
    Thread class that calls a callback on main thread when finished
*/
class ui_thread : public QThread
{
    Q_OBJECT

    std::function<void()> m_loop;
public:
    ui_thread(QObject* parent, std::function<void()> loop, std::function<void()> end)
    {
        m_loop = loop;
        
        connect(this, &QThread::finished, this, end);
    }

    void run() override
    {
        m_loop();
    }
};

class markers
{
private:
    std::vector<std::shared_ptr<QwtPlotMarker>> m_pool;
    std::vector<std::shared_ptr<QwtPlotMarker>> m_active_markers;

public:
    void update(std::vector<QPointF> points)
    {

    }
};

class main_window : public QMainWindow
{
    Q_OBJECT
public:
    main_window(QWidget* parent = 0);
    ~main_window();

signals:
    void onLiveObjectTypesProgressInitiated(int min, int max);
    void onLiveObjectTypesProgressChanged(int value);
    // Capture-open progress in permille (the open runs on a worker thread,
    // the modal progress dialog lives on the GUI thread)
    void onOpenDataProgress(int permille);
    void onObjectReferencesFound(std::string error, std::vector<uint64_t> addresses, std::vector<owlcat::object_references_t> result);

public slots:
    void onOpenData();
    void onSaveData();
    void onStartProfiling();
    void onRunUnityApp();
    void onStopProfiling();
    // Both Settings menu items and the toolbar button open the one Symbols window
    void onShowSymbols();
    void onThemeChanged();
    void onAllocationsScrolled(int);    
    void onLiveObjectTypeSelected(const QItemSelection& selected, const QItemSelection& deselected);
    void onSearchTypeSelected(const QItemSelection& selected, const QItemSelection& deselected);
    void onPickerChanged(const QPolygon& selection);
    void onLiveObjectsCallstacksContextMenuRequested(QPoint point);
    void onReferencesContextMenuRequested(QPoint point);
    void onLiveObjectsMenuRequested(QPoint point);
    void onCallstackMenuAction(bool state);
    void onReferencesMenuAction(bool state);
    void onTypesMenuAction(bool state);
    void onPauseApp(bool);
    void onTypeFilterChanged(QString filter);
    void onCallstackFilterChanged(QString filter);
    void onReferenceExpanded(QModelIndex index);

private:
    /*
        Per-callstacks-table state for populating rows and applying row heights
        on the GUI thread in chunks, with a progress bar and event pumping in
        between (the work cannot move to a thread: row heights and model
        population belong to the GUI thread). `generation` implements
        cancellation: any newer populate/heights pass, or a live-objects
        recalculation, bumps it, and older passes notice and abort at their
        next chunk boundary.
    */
    struct callstacks_view_state
    {
        QTableView* view = nullptr;
        QProgressBar* bar = nullptr;
        uint64_t generation = 0;
        bool heights_scheduled = false;
        bool populating = false;
        // Non-zero while applyRowHeights is looping (a counter, since passes
        // pump events and can nest); filter application is deferred while set
        int heights_running = 0;
    };

    bool eventFilter(QObject* object, QEvent* event) override;
    void timerEvent(QTimerEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;
    void setZoom(float new_zoom);
    void calculateLiveObjects(int from_frame, int to_frame);
    void updateLiveObjectsSelectedSize();
    void updateCallstacksFilteredSize();
    bool trySaveUnsavedData();
    void findObjectsReferences(const std::vector<uint64_t>& addresses);
    void export_types_to_csv();
    void finishOpenData();
    // Sets every row's height from the model's Lines role (chunked, pumps
    // events, drives s.bar when manage_bar). Returns false if cancelled.
    bool applyRowHeights(callstacks_view_state& s, uint64_t generation, int progress_offset, bool manage_bar);
    // Toolbar extras, status-bar indicators and plot styling; the .ui only
    // carries the actions and the widgets themselves
    void setupChrome();
    void setupPlotStyle();
    void setupTableStyle();
    // Keeps the two plots' canvases starting at the same x (shared X axis)
    void syncAxisExtents();
    // Compact legend row above the plots, replacing Qwt's own top legend
    void buildLegend();
    void updateLegendColors();
    // Refreshes the Symbols button badge and the status-bar entry from the client
    void refreshSymbolsIndicator();
    // Coalesces height re-application (sort/filter changes) into one deferred pass
    void scheduleRowHeights(callstacks_view_state& s);

    Ui::MainWindow* m_ui;

    QwtPlotHistogram m_allocations_chart;
    QwtPlotCurve m_size_chart;
    QwtPlotCurve m_committed_chart;
    std::shared_ptr<band_picker> m_allocations_picker;
    std::shared_ptr<band_picker> m_size_picker;

    QwtPlotZoneItem m_allocationsZone;
    QwtPlotZoneItem m_sizeZone;

    markers m_markers;

    QScopedPointer<QHBoxLayout> m_live_objects_types_progress_layout;
    QScopedPointer<QProgressBar> m_live_objects_types_progress;
    QScopedPointer<QHBoxLayout> m_live_objects_callstacks_progress_layout;
    QScopedPointer<QProgressBar> m_live_objects_callstacks_progress;
    QScopedPointer<QHBoxLayout> m_search_callstacks_progress_layout;
    QScopedPointer<QProgressBar> m_search_callstacks_progress;

    callstacks_view_state m_live_callstacks_view;
    callstacks_view_state m_search_callstacks_view;

    // Debounce for the callstack filter box: refiltering the proxy is expensive
    // (it queries every source row's text), so apply only after typing pauses
    QTimer m_callstack_filter_timer;
    QString m_pending_callstack_filter;

    live_objects_data m_live_objects_data;
    live_objects_by_type_model m_live_objects_by_type_model;
    filter_proxy_model m_live_object_by_type_filter_model;
    live_callstacks_by_type_model m_live_callstack_by_type_model;    
    filter_proxy_model m_live_callstack_by_type_filter_model;
    object_references_tree_model m_object_references_tree_model;

    search_results_data m_search_results_data;
    search_results_types_model m_search_results_types_model;
    filter_proxy_model m_search_results_types_filter_model;
    search_results_callstacks_model m_search_results_callstacks_model;
    filter_proxy_model m_search_results_callstacks_filter_model;

    owlcat::mono_profiler_client m_client;
    std::shared_ptr<graphs_data> m_data;

    QScopedPointer<QThread> m_types_worker;
    QScopedPointer<QThread> m_open_worker;

    QScopedPointer<QMenu> m_callstacksMenu;
    QScopedPointer<QMenu> m_referencesMenu;
    QScopedPointer<QMenu> m_liveObjectsMenu;

    // Modeless Symbols window (paths + per-module status); created on first use
    symbols_dialog* m_symbols_dialog = nullptr;

    // Toolbar/status-bar chrome created in setupChrome()
    QToolButton* m_symbols_button = nullptr;
    QLabel*      m_symbols_badge = nullptr;
    QLabel*      m_capture_mode_label = nullptr;
    QLabel*      m_status_symbols = nullptr;
    QWidget*     m_legend = nullptr;
    QLabel*      m_selection_label = nullptr;
    // Drives refreshSymbolsIndicator() while the app is idle as well as while
    // profiling (symbolication runs on a background thread either way)
    QTimer       m_chrome_timer;

    // Left-axis extent currently forced on both plots (see syncAxisExtents)
    double m_axis_extent = 0.0;

    float m_zoom = 1.0f;
    int m_pos = 0;

    int m_updateTimer = -1;

    // For measuring the database insert rate shown in the status bar
    QElapsedTimer m_db_rate_timer;
    uint64_t m_last_db_inserted_count = 0;
    uint64_t m_db_inserts_per_second = 0;

    std::string m_db_file_name;
    bool m_is_db_temporary = false;
};
