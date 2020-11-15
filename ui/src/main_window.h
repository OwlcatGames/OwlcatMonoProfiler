#pragma once
#include <qmainwindow.h>
#include <qitemselectionmodel.h>
#include <qsortfilterproxymodel.h>
#include <qevent.h>
#include <qprogressbar.h>
#include <qboxlayout.h>
#include <qthread.h>
#include "qwt_plot_histogram.h"
#include "qwt_plot_curve.h"
#include "qwt_plot_zoneitem.h"

#include "mono_profiler_client.h"
#include "band_picker.h"
#include "byte_scale_draw.h"
#include "graphs_data.h"
#include "live_objects_data.h"
#include "object_references_model.h"

namespace Ui {
    class MainWindow;
}

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

class main_window : public QMainWindow
{
    Q_OBJECT
public:
    main_window(QWidget* parent = 0);
    ~main_window();

signals:
    void onLiveObjectTypesProgressInitiated(int min, int max);
    void onLiveObjectTypesProgressChanged(int value);
    void onLiveObjectCallstacksProgressInitiated(int min, int max);
    void onLiveObjectCallstacksProgressChanged(int value);
    void onObjectReferencesFound(std::string error, std::vector<uint64_t> addresses, std::vector<owlcat::object_references_t> result);

public slots:
    void onOpenData();
    void onSaveData();
    void onStartProfiling();
    void onRunUnityApp();
    void onStopProfiling();
    void onAllocationsScrolled(int);    
    void onLiveObjectTypeSelected(const QItemSelection& selected, const QItemSelection& deselected);
    void onPickerChanged(const QPolygon& selection);
    void onLiveObjectsCallstacksContextMenuRequested(QPoint point);
    void onCallstackMenuAction(bool);
    void onPauseApp(bool);
    void onTypeFilterChanged(QString filter);
    void onCallstackFilterChanged(QString filter);

private:
    bool eventFilter(QObject* object, QEvent* event) override;
    void timerEvent(QTimerEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;
    void setZoom(float new_zoom);
    void calculateLiveObjects(int from_frame, int to_frame);
    void updateLiveObjectsSelectedSize();
    bool trySaveUnsavedData();
    void findObjectsReferences(const std::vector<uint64_t>& addresses);
    
    Ui::MainWindow* m_ui;

    QwtPlotHistogram m_allocations_chart;
    QwtPlotCurve m_size_chart;
    std::shared_ptr<band_picker> m_allocations_picker;
    std::shared_ptr<band_picker> m_size_picker;

    QwtPlotZoneItem m_allocationsZone;
    QwtPlotZoneItem m_sizeZone;

    QScopedPointer<QHBoxLayout> m_live_objects_types_progress_layout;
    QScopedPointer<QProgressBar> m_live_objects_types_progress;
    QScopedPointer<QHBoxLayout> m_live_objects_callstacks_progress_layout;
    QScopedPointer<QProgressBar> m_live_objects_callstacks_progress;

    live_objects_data m_live_objects_data;
    live_objects_by_type_model m_live_objects_by_type_model;
    filter_proxy_model m_live_object_by_type_filter_model;
    live_callstacks_by_type_model m_live_callstack_by_type_model;    
    filter_proxy_model m_live_callstack_by_type_filter_model;
    object_references_tree_model m_object_references_tree_model;

    owlcat::mono_profiler_client m_client;
    std::shared_ptr<graphs_data> m_data;

    QScopedPointer<QThread> m_types_worker;
    QScopedPointer<QThread> m_callstacks_worker;

    QScopedPointer<QMenu> m_callstacksMenu;

    float m_zoom = 1.0f;
    int m_pos = 0;

    int m_updateTimer = -1;

    std::string m_db_file_name;
    bool m_is_db_temporary = false;
};
