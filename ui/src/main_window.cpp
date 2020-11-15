#include "main_window.h"
#include "ui_mainwindow.h"
#include "connect_dialog.h"
#include "run_dialog.h"
#include "qwt_point_data.h"
#include "qwt_picker_machine.h"
#include "qwt_scale_widget.h"
#include <execution>
#include <algorithm>
#include <future>
#include <chrono>
#include <filesystem>
#include <qmessagebox.h>
#include <qfiledialog.h>
#include <qcheckbox.h>

class ui_data_allocations : public QwtSeriesData<QwtIntervalSample>
{
    std::shared_ptr<graphs_data> m_ui_data;
public:
    ui_data_allocations(std::shared_ptr<graphs_data> d)
    {
        m_ui_data = d;
    }

    virtual size_t size() const { return m_ui_data->get_allocations_count_size(); }
    virtual QwtIntervalSample sample(size_t i) const { return m_ui_data->get_allocations_count(i); }
    virtual QRectF boundingRect() const { return QRectF(m_ui_data->min_frame, 0, m_ui_data->max_frame - m_ui_data->min_frame, m_ui_data->max_allocs); }
};

class ui_data_frees : public QwtSeriesData<QwtIntervalSample>
{
    std::shared_ptr<graphs_data> m_ui_data;
public:
    ui_data_frees(std::shared_ptr<graphs_data> d)
    {
        m_ui_data = d;
    }

    virtual size_t size() const { return m_ui_data->get_frees_count_size(); }
    virtual QwtIntervalSample sample(size_t i) const { return m_ui_data->get_frees_count(i); }
    virtual QRectF boundingRect() const { return QRectF(m_ui_data->min_frame, 0, m_ui_data->max_frame - m_ui_data->min_frame, m_ui_data->max_frees); }
};

class ui_data_size : public QwtSeriesData<QPointF>
{
    std::shared_ptr<graphs_data> m_ui_data;
public:
    ui_data_size(std::shared_ptr<graphs_data> d)
    {
        m_ui_data = d;
    }

    virtual size_t size() const { return m_ui_data->get_sizes_size(); }
    virtual QPointF sample(size_t i) const { return m_ui_data->get_size(i); }
    virtual QRectF boundingRect() const { return QRectF(m_ui_data->min_frame, 0, m_ui_data->max_frame - m_ui_data->min_frame, m_ui_data->max_size); }
};

QString size_to_string(double value)
{
    if (value < 1024.0)
        return QString::number((uint)value, 10) + "b";
    else if (value < 1024.0 * 1024.0)
        return QString::number(value / 1024.0, 'f', 2) + "Kb";
    else
        return QString::number(value / 1024.0 / 1024.0, 'f', 2) + "Mb";
}

main_window::main_window(QWidget* parent) :
    QMainWindow(parent),
    m_ui(new Ui::MainWindow)
{
    m_data = std::make_shared<graphs_data>(&m_client);
    m_data->init();

    m_ui->setupUi(this);

    m_live_objects_types_progress.reset(new QProgressBar());
    m_live_objects_types_progress->setAlignment(Qt::AlignCenter);
    m_live_objects_types_progress_layout.reset(new QHBoxLayout(m_ui->liveObjectsList));
    m_live_objects_types_progress_layout->addSpacing(100);
    m_live_objects_types_progress_layout->addWidget(m_live_objects_types_progress.data());
    m_live_objects_types_progress_layout->addSpacing(100);
    m_live_objects_types_progress->setVisible(false);

    m_live_objects_callstacks_progress.reset(new QProgressBar());
    m_live_objects_callstacks_progress->setAlignment(Qt::AlignCenter);
    m_live_objects_callstacks_progress_layout.reset(new QHBoxLayout(m_ui->callstacksList));
    m_live_objects_callstacks_progress_layout->addSpacing(100);
    m_live_objects_callstacks_progress_layout->addWidget(m_live_objects_callstacks_progress.data());
    m_live_objects_callstacks_progress_layout->addSpacing(100);
    m_live_objects_callstacks_progress->setVisible(false);

    m_allocations_picker = std::make_shared<band_picker>(m_ui->allocationsGraph->canvas());
    m_size_picker = std::make_shared<band_picker>(m_ui->sizeGraph->canvas());

    m_allocations_picker->setStateMachine(new QwtPickerDragRectMachine());
    m_allocations_picker->setTrackerMode(QwtPicker::AlwaysOn);
    m_allocations_picker->setRubberBand(QwtPicker::RectRubberBand);
    m_allocationsZone.attach(m_ui->allocationsGraph);
    m_allocationsZone.setOrientation(Qt::Vertical);
    m_allocationsZone.setZ(100);

    m_size_picker->setStateMachine(new QwtPickerDragRectMachine());
    m_size_picker->setTrackerMode(QwtPicker::AlwaysOn);
    m_size_picker->setRubberBand(QwtPicker::RectRubberBand);
    m_sizeZone.attach(m_ui->sizeGraph);
    m_sizeZone.setOrientation(Qt::Vertical);
    m_sizeZone.setZ(100);

    m_ui->allocationsGraph->setAutoReplot(true);
    //TODO: Scale without scientific notation
    //m_ui->allocationsGraph->setAxisScaleDraw(QwtPlot::yLeft, &?);

    m_ui->sizeGraph->setAutoReplot(true);
    m_ui->sizeGraph->setAxisScaleDraw(QwtPlot::yLeft, new bytes_scale_draw());

    m_allocations_chart.attach(m_ui->allocationsGraph);
    m_allocations_chart.setData(new ui_data_allocations(m_data));
    m_allocations_chart.setStyle(QwtPlotHistogram::Columns);
    m_allocations_chart.setPen(QColor::fromRgb(255, 0, 0));
    m_allocations_chart.setBrush(QBrush(QColor::fromRgb(255, 0, 0)));

    m_size_chart.attach(m_ui->sizeGraph);
    m_size_chart.setData(new ui_data_size(m_data));
    m_size_chart.setStyle(QwtPlotCurve::Lines);
    m_size_chart.setPen(QColor::fromRgb(0, 0, 0));

    m_live_object_by_type_filter_model.setSourceModel(&m_live_objects_by_type_model);
    m_live_object_by_type_filter_model.setFilterKeyColumn(0);
    m_live_object_by_type_filter_model.setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_ui->liveObjectsList->setModel(&m_live_object_by_type_filter_model);

    m_live_callstack_by_type_filter_model.setSourceModel(&m_live_callstack_by_type_model);
    m_live_callstack_by_type_filter_model.setFilterKeyColumn(0);
    m_live_callstack_by_type_filter_model.setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_ui->callstacksList->setModel(&m_live_callstack_by_type_filter_model);

    m_ui->callstacksList->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    connect(m_ui->liveObjectsList->selectionModel(), SIGNAL(selectionChanged(QItemSelection, QItemSelection)), this, SLOT(onLiveObjectTypeSelected(QItemSelection, QItemSelection)));

    connect(m_allocations_picker.get(), SIGNAL(selected(const QPolygon)), this, SLOT(onPickerChanged(const QPolygon)));
    connect(m_size_picker.get(), SIGNAL(selected(const QPolygon)), this, SLOT(onPickerChanged(const QPolygon)));
    
    connect(this, SIGNAL(onLiveObjectTypesProgressChanged(int)), m_live_objects_types_progress.get(), SLOT(setValue(int)), Qt::ConnectionType::QueuedConnection);
    connect(this, SIGNAL(onLiveObjectTypesProgressInitiated(int, int)), m_live_objects_types_progress.get(), SLOT(setRange(int, int)), Qt::ConnectionType::QueuedConnection);
    connect(this, SIGNAL(onLiveObjectCallstacksProgressChanged(int)), m_live_objects_callstacks_progress.get(), SLOT(setValue(int)), Qt::ConnectionType::QueuedConnection);
    connect(this, SIGNAL(onLiveObjectCallstacksProgressInitiated(int, int)), m_live_objects_callstacks_progress.get(), SLOT(setRange(int, int)), Qt::ConnectionType::QueuedConnection);

    m_ui->allocationsGraph->installEventFilter(this);
    m_ui->sizeGraph->installEventFilter(this);

    m_ui->allocationsGraph->axisScaleDraw(QwtPlot::yLeft)->setMinimumExtent(100);
    m_ui->sizeGraph->axisScaleDraw(QwtPlot::yLeft)->setMinimumExtent(100);

    m_ui->callstacksList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_callstacksMenu.reset(new QMenu(this));
    m_callstacksMenu->addAction(new QAction("Find references", this));
    
    connect(m_callstacksMenu->actions()[0], SIGNAL(triggered(bool)), this, SLOT(onCallstackMenuAction(bool)));
    
    connect(this, SIGNAL(onObjectReferencesFound(std::string, std::vector<uint64_t>, std::vector<owlcat::object_references_t>)), &m_object_references_tree_model, SLOT(update(std::string, std::vector<uint64_t>, std::vector<owlcat::object_references_t>)));

//std::vector<uint64_t> test_addr = { 1,2,3 };
//std::vector<owlcat::object_references_t> test_refs =
//{
//    {1, "Type1", {}},
//    {2, "Type1", {{4}, {5}}},
//    {3, "Type1", {{4}, {6}}},
//    {4, "Type4", {}},
//    {5, "Type5", {}},
//    {6, "Type6", {{7}}},
//    {7, "Type7", {{1}}},
//};
//
//m_object_references_tree_model.update("", test_addr, test_refs);
    m_ui->objectReferences->setModel(&m_object_references_tree_model);
    
}

main_window::~main_window()
{
    m_client.stop();
    m_client.close_db();

    if (m_is_db_temporary)
    {
        std::filesystem::remove(m_db_file_name);
    }
}

void main_window::onOpenData()
{
    if (!trySaveUnsavedData())
        return;

    m_client.close_db();

    QString file_name = QFileDialog::getOpenFileName(this, tr("Open trace"),
        QDir::currentPath(),
        tr("Owl Traces (*.owl)"));

    if (!file_name.isEmpty())
    {
        if (!m_client.open_data(file_name.toStdString().c_str()))
        {
            return;
        }

        m_is_db_temporary = false;

        m_data->clear();
        m_data->update_boundaries();

        m_live_objects_data.init(m_client.get_data());
        m_live_objects_by_type_model.init(&m_live_objects_data);
        m_live_callstack_by_type_model.init(&m_live_objects_data);

        m_ui->horizontalScrollBar->setMinimum(m_data->min_frame);
        m_ui->horizontalScrollBar->setMaximum(m_data->max_frame);
        m_ui->horizontalScrollBar->setSliderPosition(m_data->min_frame);
        setZoom(1.0f);

        m_ui->allocationsGraph->replot();
        m_ui->sizeGraph->replot();
    }
}

void main_window::onSaveData()
{
    QString file_name = QFileDialog::getSaveFileName(this, tr("Save trace"),
        QDir::homePath(),
        tr("Owl Traces (*.owl)"));

    if (file_name.isEmpty())
        return;

    if (!m_client.save_db(file_name.toStdString().c_str(), m_is_db_temporary))
    {
        QMessageBox::critical(nullptr, "Error", "Failed to save trace", QMessageBox::Ok);
        return;
    }

    m_db_file_name = file_name.toStdString();
    m_is_db_temporary = false;
}

void main_window::onStartProfiling()
{
    if (!trySaveUnsavedData())
        return;

    connect_dialog dlg;
    if (dlg.exec() == QDialog::Rejected)
        return;

    auto ip = dlg.ip();
    auto port = dlg.port();

    m_data->clear();
    m_data->update_boundaries();

    // TODO: Learn to resume a profiling session?
    m_client.close_db();
    if (!m_db_file_name.empty() && std::filesystem::exists(m_db_file_name))
        std::filesystem::remove(m_db_file_name);

    m_db_file_name = std::tmpnam(0);
    
    if (!m_client.start(ip.c_str(), port, m_db_file_name.c_str()))
    {
        QMessageBox::critical(nullptr, "Netwok error", "Failed to connect to profiler server", QMessageBox::Ok);
        return;
    }

    m_is_db_temporary = true;

    m_live_objects_data.init(m_client.get_data());
    m_live_objects_by_type_model.init(&m_live_objects_data);
    m_live_callstack_by_type_model.init(&m_live_objects_data);

    m_ui->horizontalScrollBar->setMinimum(m_data->min_frame);
    m_ui->horizontalScrollBar->setMaximum(m_data->max_frame);
    m_ui->horizontalScrollBar->setSliderPosition(m_data->min_frame);
    setZoom(1.0f);

    m_ui->allocationsGraph->replot();
    m_ui->sizeGraph->replot();

    m_ui->actionStart_Profiling->setEnabled(false);
    m_ui->actionSave->setEnabled(false);
    m_ui->actionStop_Profiling->setEnabled(true);    

    m_updateTimer = startTimer(100);
}

void main_window::onRunUnityApp()
{
    if (!trySaveUnsavedData())
        return;

    run_dialog dlg;
    if (dlg.exec() == QDialog::Rejected)
        return;

    auto path = dlg.path();
    auto args = dlg.arguments();
    int port = dlg.port();

    m_data->clear();
    m_data->update_boundaries();

    // TODO: Learn to resume a profiling session?
    m_client.close_db();
    //if (!m_db_file_name.empty() && std::filesystem::exists(m_db_file_name))
    //    std::filesystem::remove(m_db_file_name);

    // Use "temporary" database, which stores part of content in memory for quicker writes
    m_db_file_name = "";// ":memory:"; std::tmpnam(0);

    auto dllPath = QApplication::applicationDirPath() + "\\mono_profiler_mono.dll";
    if (!m_client.launch_executable(path, args, port, m_db_file_name.c_str(), dllPath.toStdString()))
    {
        QMessageBox::critical(nullptr, "Error", "Failed to launch the app or to connect to profiler server", QMessageBox::Ok);
        return;
    }

    m_is_db_temporary = true;

    m_live_objects_data.init(m_client.get_data());
    m_live_objects_by_type_model.init(&m_live_objects_data);
    m_live_callstack_by_type_model.init(&m_live_objects_data);

    m_ui->horizontalScrollBar->setMinimum(m_data->min_frame);
    m_ui->horizontalScrollBar->setMaximum(m_data->max_frame);
    m_ui->horizontalScrollBar->setSliderPosition(m_data->min_frame);
    setZoom(1.0f);

    m_ui->allocationsGraph->replot();
    m_ui->sizeGraph->replot();

    m_ui->actionStart_Profiling->setEnabled(false);
    m_ui->actionSave->setEnabled(false);
    m_ui->actionStop_Profiling->setEnabled(true);

    m_updateTimer = startTimer(100);
}

void main_window::onStopProfiling()
{
    killTimer(m_updateTimer);

    m_client.stop();

    m_ui->actionStart_Profiling->setEnabled(true);
    m_ui->actionSave->setEnabled(true);
    m_ui->actionStop_Profiling->setEnabled(false);
}

void main_window::onAllocationsScrolled(int value)
{
    m_pos = value;
    setZoom(m_zoom);
}

void main_window::onLiveObjectTypeSelected(const QItemSelection& selected, const QItemSelection& deselected)
{
    auto currentIndex = m_ui->liveObjectsList->currentIndex();
    if (currentIndex == QModelIndex())
        return;

    uint64_t type_id = m_live_object_by_type_filter_model.data(currentIndex, Qt::UserRole).toInt(); //m_live_objects_by_type_model.data(currentIndex, Qt::UserRole).toInt();

    updateLiveObjectsSelectedSize();

    // TODO: Make update of list of callstack asynchronous somehow? But we can't do it easilty, because the slowdown is in calculating the height of list items, which is pefrormed
    // in one frame on main thread only.

    //m_ui->callstacksList->setEnabled(false);
    //m_live_objects_callstacks_progress->setVisible(true);

    //m_callstacks_worker.reset(new ui_thread(
    //    this,
    //    [this, type_id]()
    //    {
    //        bool first_time = true;
    //        size_t cnt = 0;

    //        m_live_callstack_by_type_model.update(type_id, [&](size_t cur, size_t max)
    //            {
    //                if (first_time)
    //                {
    //                    first_time = false;
    //                    emit onLiveObjectCallstacksProgressInitiated(0, max);
    //                }
    //                if (cur / 10 > cnt)
    //                {
    //                    cnt = cur / 10;
    //                    emit onLiveObjectCallstacksProgressChanged(cur);
    //                }
    //            });
    //    },
    //    [this]()
    //    {
    //        m_ui->callstacksList->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    //        m_ui->callstacksList->horizontalHeader()->resizeSection(1, 100);
    //        m_ui->callstacksList->horizontalHeader()->resizeSection(2, 100);
    //        m_ui->callstacksList->sortByColumn(2, Qt::SortOrder::DescendingOrder);                      

    //        m_ui->callstacksList->setEnabled(true);
    //        m_live_objects_callstacks_progress->setVisible(false);            
    //    }
    //    ));

    //m_callstacks_worker->start();

    m_live_callstack_by_type_model.update(type_id, nullptr);
    
    m_ui->callstacksList->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_ui->callstacksList->horizontalHeader()->resizeSection(1, 100);
    m_ui->callstacksList->horizontalHeader()->resizeSection(2, 100);
    m_ui->callstacksList->sortByColumn(2, Qt::SortOrder::DescendingOrder);
}

void main_window::onPickerChanged(const QPolygon& selection)
{    
    QRect rect(selection.first(), selection.last());
    rect = rect.normalized();
    auto x1 = floor(m_ui->allocationsGraph->invTransform(QwtPlot::xBottom, rect.left()));
    auto x2 = ceil(m_ui->allocationsGraph->invTransform(QwtPlot::xBottom, rect.right()));

    if (m_ui->snapToGC->checkState() == Qt::Checked)
    {
        x1 = m_data->get_closest_gc_frame(x1);
        x2 = m_data->get_closest_gc_frame(x2) + 1;
    }

    m_allocationsZone.setInterval(x1, x2);    
    m_sizeZone.setInterval(x1, x2);

    calculateLiveObjects(x1, x2-1);
}

void main_window::onLiveObjectsCallstacksContextMenuRequested(QPoint point)
{
    QModelIndex index = m_ui->callstacksList->indexAt(point);

    if (index == QModelIndex())
        return;

    m_callstacksMenu->popup(m_ui->callstacksList->viewport()->mapToGlobal(point));
}

void main_window::onCallstackMenuAction(bool)
{
    auto currentIndex = m_ui->callstacksList->currentIndex();
    if (currentIndex == QModelIndex())
        return;

    currentIndex = m_live_callstack_by_type_filter_model.mapToSource(currentIndex);

    uint64_t type_id = m_live_callstack_by_type_filter_model.data(currentIndex, Qt::UserRole).toInt(); //m_live_objects_by_type_model.data(currentIndex, Qt::UserRole).toInt();

    auto addresses = m_live_callstack_by_type_model.get_addresses(currentIndex);
    if (addresses == nullptr)
        return;
    findObjectsReferences(*addresses);
}

void main_window::onPauseApp(bool pause)
{
    if (pause)
        m_client.pause_app([](bool ok) {});
    else
        m_client.resume_app([](bool ok) {});
}

void main_window::onTypeFilterChanged(QString filter)
{
    m_live_object_by_type_filter_model.setFilterWildcard(filter);
}

void main_window::onCallstackFilterChanged(QString filter)
{
    m_live_callstack_by_type_filter_model.setFilterWildcard(filter);
}

bool main_window::eventFilter(QObject* object, QEvent* event)
{
    // We want wheel to act as zoom any time when the cursor is over allocation graph or size graph
    if (event->type() == QEvent::Wheel)
    {
        if (object == m_ui->allocationsGraph || object == m_ui->sizeGraph)
        {
            auto ev = (QWheelEvent*)event;

            setZoom(m_zoom + ev->delta() / 1000.0f);

            return true;
        }
    }
    else if (event->type() == QEvent::Resize)
    {
        setZoom(m_zoom);
    }

    return false;
}

void main_window::timerEvent(QTimerEvent* ev)
{
    if (!m_client.is_connected() && !m_client.is_connecting())
    {
        QMessageBox::critical(nullptr, "Network error", "Network disconnected.", QMessageBox::Ok);
        onStopProfiling();
        return;
    }

    bool can_see_edge = m_data->last_visible_frame >= m_data->max_frame;
    
    m_data->update_boundaries();
    m_ui->horizontalScrollBar->setMinimum(m_data->min_frame);
    m_ui->horizontalScrollBar->setMaximum(m_data->max_frame);

    // auto scrolling
    if (can_see_edge && m_data->last_visible_frame < m_data->max_frame)
    {
        const QwtScaleMap scaleMap = m_ui->sizeGraph->canvasMap(QwtPlot::xBottom);

        m_pos = m_data->max_frame - floorf(scaleMap.pDist() / m_zoom);
        m_ui->horizontalScrollBar->setSliderPosition(m_pos);

        m_ui->allocationsGraph->replot();
        m_ui->sizeGraph->replot();
    }
    else if (m_data->last_visible_frame >= m_data->max_frame || m_data->last_visible_frame == 0)
    {
        m_data->update_region(m_data->first_visible_frame, m_data->last_visible_frame);

        m_ui->allocationsGraph->replot();
        m_ui->sizeGraph->replot();
    }

    char tmp[64];
    sprintf(tmp, "Network buffer: %I64u", m_client.get_network_messages_count());
    m_ui->statusbar->showMessage(tmp);
}

void main_window::closeEvent(QCloseEvent* ev)
{
    if (!trySaveUnsavedData())
        ev->ignore();
    else
        ev->accept();
}

void main_window::setZoom(float new_zoom)
{
    m_zoom = new_zoom;
    if (m_zoom <= 0.1f)
        m_zoom = 0.1f;
    else if (m_zoom > 10.0f)
        m_zoom = 10.0f;

    const QwtScaleMap scaleMap = m_ui->sizeGraph->canvasMap(QwtPlot::xBottom);

    int pos2 = m_pos + scaleMap.pDist() / m_zoom;
    m_data->update_region(m_pos, pos2);
    m_ui->allocationsGraph->setAxisScale(QwtPlot::xBottom, m_pos, pos2);
    m_ui->sizeGraph->setAxisScale(QwtPlot::xBottom, m_pos, pos2);    
}

void main_window::calculateLiveObjects(int from_frame, int to_frame)
{
    if (m_types_worker != nullptr && m_types_worker->isRunning())
    {
        m_types_worker->requestInterruption();
        m_types_worker->wait();
    }

    m_ui->liveObjectsList->setEnabled(false);
    m_ui->callstacksList->setEnabled(false);
    m_live_objects_types_progress->setVisible(true);

    m_types_worker.reset(new ui_thread(
        this, 
        [this, from_frame, to_frame]()
        {
            bool first_time = true;
            size_t cnt = 0;

            m_live_objects_data.update(from_frame, to_frame, [&](size_t cur, size_t max)
                {
                    if (QThread::currentThread()->isInterruptionRequested())
                        return false;

                    if (first_time)
                    {
                        first_time = false;
                        emit onLiveObjectTypesProgressInitiated(0, (int)max);
                    }
                    if (cur / 10 > cnt)
                    {
                        cnt = cur / 10;
                        emit onLiveObjectTypesProgressChanged((int)cur);
                    }

                    return true;
                });
        },
        [this]()
        {
            m_live_objects_by_type_model.update();
            m_live_callstack_by_type_model.clear();

            m_ui->liveObjectsList->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
            m_ui->liveObjectsList->horizontalHeader()->resizeSection(1, 100);
            m_ui->liveObjectsList->horizontalHeader()->resizeSection(2, 100);
            m_ui->liveObjectsList->sortByColumn(2, Qt::SortOrder::DescendingOrder);

            m_ui->liveObjectsList->setEnabled(true);
            m_ui->callstacksList->setEnabled(true);
            m_live_objects_types_progress->setVisible(false);

            updateLiveObjectsSelectedSize();
        }
    ));

    m_types_worker->start();
}

void main_window::updateLiveObjectsSelectedSize()
{
    auto rows = m_ui->liveObjectsList->selectionModel()->selectedRows();
    uint64_t selected_size = 0;
    for (auto& index : rows)
    {
        selected_size += m_live_objects_by_type_model.data(index, live_objects_by_type_model::role::Size).toULongLong();
    }
    QString text = QString("Selected %1 out of %2").arg(size_to_string(selected_size), size_to_string(m_live_objects_by_type_model.get_total_size()));
    m_ui->liveObjectsSizeLabel->setText(text);
}

bool main_window::trySaveUnsavedData()
{
    if (!m_is_db_temporary || !m_client.is_data_open())
        return true;
    
    auto res = QMessageBox::question(nullptr, "Save trace?", "Previous session trace not saved. Save now?", QMessageBox::Yes, QMessageBox::No, QMessageBox::Cancel);
    switch (res)
    {
        case QMessageBox::Yes:
            onSaveData();
            return true;

        case QMessageBox::No:
            return true;

        case QMessageBox::Cancel:
        default:
            return false;
    }
}

void main_window::findObjectsReferences(const std::vector<uint64_t>& addresses)
{
    m_ui->tabWidget->setCurrentIndex(1);
    m_client.find_objects_references(addresses, [this](const std::vector<uint64_t>& addresses, std::string error, const std::vector<owlcat::object_references_t>& result) -> void
        {
            m_object_references_tree_model.update(error, addresses, result);
        });
}
