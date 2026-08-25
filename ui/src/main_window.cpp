#include "main_window.h"
#include "ui_mainwindow.h"
#include "connect_dialog.h"
#include "run_dialog.h"
#include "symbols_dialog.h"
#include "theme.h"
#include "icons.h"
#include "size_bar_delegate.h"
#include "callstack_delegate.h"
#include "frame_classifier.h"
#include "runtime_frames_dialog.h"
#include <qsettings.h>
#include "qwt_point_data.h"
#include "qwt_picker_machine.h"
#include "qwt_scale_widget.h"
#include "qwt_legend.h"
#include <execution>
#include <algorithm>
#include <future>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <qmessagebox.h>
#include <qfiledialog.h>
#include <qcheckbox.h>
#include <qtimer.h>
#include <qprogressdialog.h>
#include <qfileinfo.h>
#include <qtoolbutton.h>
#include <qtoolbar.h>
#include <qstatusbar.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qlocale.h>
#include <qmenu.h>
#include <qactiongroup.h>
#include <qshortcut.h>
#include <qheaderview.h>
#include <qframe.h>
#include <qpainter.h>
#include <qevent.h>
#include "qwt_plot_canvas.h"
#include "qwt_plot_grid.h"
#include <atomic>

// Callstacks-table population/row-height passes: rows per event-loop pump, and
// the row count above which the progress bar is shown (below it the pass is
// quick enough that the bar would only flash)
static const size_t kCallstacksChunk = 2048;
static const size_t kCallstacksProgressThreshold = 2000;

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

// Whole-process committed memory, overlaid on the size graph. The gap between this and the
// tracked-size line is native allocator pool overhead (committed pages not handed out).
class ui_data_committed : public QwtSeriesData<QPointF>
{
    std::shared_ptr<graphs_data> m_ui_data;
public:
    ui_data_committed(std::shared_ptr<graphs_data> d)
    {
        m_ui_data = d;
    }

    virtual size_t size() const { return m_ui_data->get_committed_size(); }
    virtual QPointF sample(size_t i) const { return m_ui_data->get_committed(i); }
    virtual QRectF boundingRect() const { return QRectF(m_ui_data->min_frame, 0, m_ui_data->max_frame - m_ui_data->min_frame, m_ui_data->max_committed); }
};

// Reads a whole file into a string. Used to load the native-hook config, which the client
// sends to the (possibly remote) server over the connection rather than as a path.
static std::string read_text_file(const std::string& path)
{
    if (path.empty())
        return std::string();
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

QString size_to_string(double value)
{
    if (value < 1024.0)
        return QString::number((uint)value, 10) + "b";
    else if (value < 1024.0 * 1024.0)
        return QString::number(value / 1024.0, 'f', 2) + "Kb";
    else
        return QString::number(value / 1024.0 / 1024.0, 'f', 2) + "Mb";
}

void test_references_tree(object_references_tree_model& model)
{
    FILE* f = fopen("test.fr", "r");
    if (f == nullptr)
        return;

    uint64_t addr_count = 0;
    fscanf(f, "%I64u", &addr_count);
    if (addr_count <= 0)
        return;

    std::vector<uint64_t> test_addr;
    for (auto i = 0; i < addr_count; ++i)
    {
        uint64_t addr;
        fscanf(f, "\t%I64u", &addr);
        test_addr.push_back(addr);
    }

    uint64_t ref_count = 0;
    fscanf(f, "%I64u", &ref_count);
    if (ref_count <= 0)
        return;

    std::vector<owlcat::object_references_t> test_refs;
    for (auto i = 0; i < ref_count; ++i)
    {
        uint64_t addr = 0;
        char type[1024];
        uint64_t parent_count = 0;

        fscanf(f, "\t%I64u \"%[^\"]\"", &addr, type);

        owlcat::object_references_t ref{ addr, type };

        fscanf(f, "\t\t\t%I64u", &parent_count);
        for (auto p = 0; p < parent_count; ++p)
        {
            uint64_t parent_addr;
            fscanf(f, "\t\t\t\t%I64u", &parent_addr);
            ref.parents.push_back({ parent_addr });
        }
        test_refs.push_back(ref);
    }

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
    model.update("", test_addr, test_refs);
}

main_window::main_window(QWidget* parent) :
    QMainWindow(parent),
    m_ui(new Ui::MainWindow)
{
    m_data = std::make_shared<graphs_data>(&m_client);
    m_data->init();

    m_ui->setupUi(this);

    // --------- Symbol paths ---------

    connect(m_ui->actionSymbolPaths, &QAction::triggered, this, &main_window::onShowSymbols);
    connect(m_ui->actionSymbolStatus, &QAction::triggered, this, &main_window::onShowSymbols);
    // Apply the saved symbol search path so native callstacks resolve from the start
    {
        QStringList paths = QSettings().value("symbols/paths").toStringList();
        m_client.set_symbol_paths(paths.join(';').toStdString());
    }

    // --------- Progress bars ---------

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

    m_search_callstacks_progress.reset(new QProgressBar());
    m_search_callstacks_progress->setAlignment(Qt::AlignCenter);
    m_search_callstacks_progress_layout.reset(new QHBoxLayout(m_ui->searchCallstacksList));
    m_search_callstacks_progress_layout->addSpacing(100);
    m_search_callstacks_progress_layout->addWidget(m_search_callstacks_progress.data());
    m_search_callstacks_progress_layout->addSpacing(100);
    m_search_callstacks_progress->setVisible(false);

    m_live_callstacks_view.view = m_ui->callstacksList;
    m_live_callstacks_view.bar = m_live_objects_callstacks_progress.data();
    m_search_callstacks_view.view = m_ui->searchCallstacksList;
    m_search_callstacks_view.bar = m_search_callstacks_progress.data();

    // Apply the callstack filter only after the user stops typing: each
    // application refilters the whole proxy synchronously. If a populate or
    // heights pass is mid-flight (they pump events, so the timer can fire
    // inside one), retry later instead of changing rows under the pass.
    m_callstack_filter_timer.setSingleShot(true);
    m_callstack_filter_timer.setInterval(300);
    connect(&m_callstack_filter_timer, &QTimer::timeout, this, [this]()
        {
            auto& s = m_live_callstacks_view;
            if (s.populating || s.heights_running)
            {
                m_callstack_filter_timer.start();
                return;
            }
            m_live_callstack_by_type_filter_model.setFilterWildcard(m_pending_callstack_filter);
            updateCallstacksFilteredSize();
        });

    connect(this, SIGNAL(onLiveObjectTypesProgressChanged(int)), m_live_objects_types_progress.data(), SLOT(setValue(int)), Qt::ConnectionType::QueuedConnection);
    connect(this, SIGNAL(onLiveObjectTypesProgressInitiated(int, int)), m_live_objects_types_progress.data(), SLOT(setRange(int, int)), Qt::ConnectionType::QueuedConnection);

    // --------- Pickers ---------

    m_allocations_picker = std::make_shared<band_picker>(m_ui->allocationsGraph->canvas(), band_picker::y_format::count);
    m_size_picker = std::make_shared<band_picker>(m_ui->sizeGraph->canvas(), band_picker::y_format::bytes);

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

    // Show the graphs' actual values at the cursor's frame in the trackers.
    // The frame is clamped to the loaded data, so hovering past the end of the
    // capture shows the last frame's values.
    m_allocations_picker->set_value_text([this](qint64 frame) -> QString
    {
        if (!m_data || m_data->get_allocations_count_size() == 0)
            return QString();

        const qint64 first = m_data->first_visible_frame;
        const qint64 last = first + (qint64)m_data->get_allocations_count_size() - 1;
        frame = qBound(first, frame, last);
        const uint64_t index = frame - first;

        const qint64 allocs = (qint64)m_data->get_allocations_count(index).value;
        const qint64 frees = index < m_data->get_frees_count_size() ? (qint64)m_data->get_frees_count(index).value : 0;
        return QString("frame %1: %2 allocs, %3 frees").arg(frame).arg(allocs).arg(frees);
    });

    m_size_picker->set_value_text([this](qint64 frame) -> QString
    {
        if (!m_data || m_data->get_sizes_size() == 0)
            return QString();

        const qint64 first = m_data->first_visible_frame;
        const qint64 last = first + (qint64)m_data->get_sizes_size() - 1;
        frame = qBound(first, frame, last);
        const uint64_t index = frame - first;

        return QString("frame %1: %2").arg(frame).arg(size_to_string(m_data->get_size(index).y()));
    });

    connect(m_allocations_picker.get(), SIGNAL(selected(const QPolygon)), this, SLOT(onPickerChanged(const QPolygon)));
    connect(m_size_picker.get(), SIGNAL(selected(const QPolygon)), this, SLOT(onPickerChanged(const QPolygon)));

    // --------- Graphs ---------

    m_ui->allocationsGraph->setAutoReplot(true);
    m_ui->allocationsGraph->setAxisScaleDraw(QwtPlot::yLeft, new counts_scale_draw());

    m_ui->sizeGraph->setAutoReplot(true);
    m_ui->sizeGraph->setAxisScaleDraw(QwtPlot::yLeft, new bytes_scale_draw());

    m_allocations_chart.attach(m_ui->allocationsGraph);
    m_allocations_chart.setData(new ui_data_allocations(m_data));
    m_allocations_chart.setStyle(QwtPlotHistogram::Columns);

    // Committed-memory line first, drawn under the tracked line, so the
    // overhead gap between the two reads clearly
    m_committed_chart.attach(m_ui->sizeGraph);
    m_committed_chart.setData(new ui_data_committed(m_data));
    m_committed_chart.setStyle(QwtPlotCurve::Lines);
    m_committed_chart.setTitle("Committed (process)");

    m_size_chart.attach(m_ui->sizeGraph);
    m_size_chart.setData(new ui_data_size(m_data));
    m_size_chart.setStyle(QwtPlotCurve::Lines);
    m_size_chart.setPen(QColor::fromRgb(0, 0, 0));
    m_size_chart.setTitle("Tracked allocations");


    // --------- Live objects tab ---------

    m_live_object_by_type_filter_model.setSourceModel(&m_live_objects_by_type_model);
    m_live_object_by_type_filter_model.setFilterKeyColumn(0);
    m_live_object_by_type_filter_model.setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_ui->liveObjectsList->setModel(&m_live_object_by_type_filter_model);

    m_live_callstack_by_type_filter_model.setSourceModel(&m_live_callstack_by_type_model);
    m_live_callstack_by_type_filter_model.setFilterKeyColumn(0);
    m_live_callstack_by_type_filter_model.setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_ui->callstacksList->setModel(&m_live_callstack_by_type_filter_model);

    // Row heights are set explicitly from cached line counts (see applyRowHeights):
    // ResizeToContents would synchronously lay out every row's multi-line text on
    // the GUI thread on every insert/sort/filter pass
    m_ui->callstacksList->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    connect(m_ui->liveObjectsList->selectionModel(), SIGNAL(selectionChanged(QItemSelection, QItemSelection)), this, SLOT(onLiveObjectTypeSelected(QItemSelection, QItemSelection)));

    // Re-apply row heights whenever the proxy rearranges rows (header-click sort,
    // filter changes); populate paths apply heights themselves (see `populating`)
    auto reapply_live_heights = [this]() { scheduleRowHeights(m_live_callstacks_view); };
    connect(&m_live_callstack_by_type_filter_model, &QAbstractItemModel::layoutChanged, this, reapply_live_heights);
    connect(&m_live_callstack_by_type_filter_model, &QAbstractItemModel::rowsInserted, this, reapply_live_heights);
    connect(&m_live_callstack_by_type_filter_model, &QAbstractItemModel::rowsRemoved, this, reapply_live_heights);

    // --------- Object references tab ---------

    test_references_tree(m_object_references_tree_model);
    m_ui->objectReferences->setModel(&m_object_references_tree_model);

    // --------- Search results tab ---------

    m_search_results_types_filter_model.setSourceModel(&m_search_results_types_model);
    m_search_results_types_filter_model.setFilterKeyColumn(0);
    m_search_results_types_filter_model.setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_ui->searchTypesList->setModel(&m_search_results_types_filter_model);

    m_search_results_callstacks_filter_model.setSourceModel(&m_search_results_callstacks_model);
    m_search_results_callstacks_filter_model.setFilterKeyColumn(0);
    m_search_results_callstacks_filter_model.setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_ui->searchCallstacksList->setModel(&m_search_results_callstacks_filter_model);

    m_ui->searchCallstacksList->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    connect(m_ui->searchTypesList->selectionModel(), SIGNAL(selectionChanged(QItemSelection, QItemSelection)), this, SLOT(onSearchTypeSelected(QItemSelection, QItemSelection)));

    auto reapply_search_heights = [this]() { scheduleRowHeights(m_search_callstacks_view); };
    connect(&m_search_results_callstacks_filter_model, &QAbstractItemModel::layoutChanged, this, reapply_search_heights);
    connect(&m_search_results_callstacks_filter_model, &QAbstractItemModel::rowsInserted, this, reapply_search_heights);
    connect(&m_search_results_callstacks_filter_model, &QAbstractItemModel::rowsRemoved, this, reapply_search_heights);

    // --------- ---------
    
    m_ui->allocationsGraph->installEventFilter(this);
    m_ui->sizeGraph->installEventFilter(this);

    // Both plots share one X axis, so their canvases must start at the same x;
    // syncAxisExtents() keeps them there as the labels change width
    syncAxisExtents();

    // --------- Context menus ---------

    m_ui->callstacksList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_callstacksMenu.reset(new QMenu(this));
    m_callstacksMenu->addAction(new QAction("Find references", this));
    
    connect(m_callstacksMenu->actions()[0], SIGNAL(triggered(bool)), this, SLOT(onCallstackMenuAction(bool)));
    
    connect(this, SIGNAL(onObjectReferencesFound(std::string, std::vector<uint64_t>, std::vector<owlcat::object_references_t>)), &m_object_references_tree_model, SLOT(update(std::string, std::vector<uint64_t>, std::vector<owlcat::object_references_t>)));

    m_ui->objectReferences->setContextMenuPolicy(Qt::CustomContextMenu);
    m_referencesMenu.reset(new QMenu(this));
    m_referencesMenu->addAction(new QAction("Go to callstack", this));

    connect(m_referencesMenu->actions()[0], SIGNAL(triggered(bool)), this, SLOT(onReferencesMenuAction(bool)));

    m_ui->liveObjectsList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_liveObjectsMenu.reset(new QMenu(this));
    m_liveObjectsMenu->addAction(new QAction("Export to CSV", this));

    connect(m_liveObjectsMenu->actions()[0], SIGNAL(triggered(bool)), this, SLOT(onTypesMenuAction(bool)));

    // --------- Look and feel ---------

    setupChrome();
    setupPlotStyle();
    setupTableStyle();

    // A tool that reopens exactly as you left it feels finished
    {
        QSettings settings;
        restoreGeometry(settings.value("window/geometry").toByteArray());
        restoreState(settings.value("window/state").toByteArray());
        const QByteArray splitter = settings.value("window/splitter").toByteArray();
        if (!splitter.isEmpty())
            m_ui->splitter->restoreState(splitter);
        const QByteArray panes = settings.value("window/live_panes").toByteArray();
        if (!panes.isEmpty() && m_live_splitter != nullptr)
            m_live_splitter->restoreState(panes);
    }
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

    if (file_name.isEmpty())
        return;

    if (m_open_worker != nullptr && m_open_worker->isRunning())
        return;

    // Opening unpacks the (potentially huge) container into the temp directory,
    // so it runs on a worker thread behind an application-modal progress dialog
    auto* dialog = new QProgressDialog(tr("Opening %1...").arg(QFileInfo(file_name).fileName()), tr("Cancel"), 0, 1000, this);
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->setMinimumDuration(500);
    // Keep the dialog up for the whole open: the bar reaches its maximum when
    // the unpack finishes, but the database is still being loaded after that
    dialog->setAutoClose(false);
    dialog->setAutoReset(false);

    connect(this, &main_window::onOpenDataProgress, dialog, &QProgressDialog::setValue, Qt::ConnectionType::QueuedConnection);

    auto open_cancelled = std::make_shared<std::atomic<bool>>(false);
    auto open_succeeded = std::make_shared<std::atomic<bool>>(false);
    connect(dialog, &QProgressDialog::canceled, this, [open_cancelled]() { open_cancelled->store(true); });

    m_open_worker.reset(new ui_thread(
        this,
        [this, path = std::string(file_name.toStdString()), open_cancelled, open_succeeded]()
        {
            size_t last_permille = (size_t)-1;
            bool ok = m_client.open_data(path, [&](size_t cur, size_t max)
                {
                    if (open_cancelled->load())
                        return false;
                    size_t permille = max > 0 ? cur * 1000 / max : 0;
                    if (permille != last_permille)
                    {
                        last_permille = permille;
                        emit onOpenDataProgress((int)permille);
                    }
                    return true;
                });
            open_succeeded->store(ok);
        },
        [this, dialog, open_succeeded]()
        {
            dialog->close();
            dialog->deleteLater();

            if (!open_succeeded->load())
                return;

            finishOpenData();
        }));

    m_open_worker->start();
}

// The GUI-thread part of opening a capture: hooks the freshly opened data up
// to the models and graphs. Split out of onOpenData because the open itself
// runs on a worker thread.
void main_window::finishOpenData()
{
    syncAxisExtents();
    refreshEventsStrip();
    m_is_db_temporary = false;

    m_data->clear();
    m_data->update_boundaries();

    m_live_objects_data.init(m_client.get_data());        
    m_live_objects_by_type_model.init(&m_live_objects_data);
    m_live_callstack_by_type_model.init(&m_live_objects_data);

    m_search_results_data.init(m_client.get_data());
    m_search_results_types_model.init(&m_search_results_data);
    m_search_results_callstacks_model.init(&m_search_results_data);

    m_ui->horizontalScrollBar->setMinimum(m_data->min_frame);
    m_ui->horizontalScrollBar->setMaximum(m_data->max_frame);
    m_ui->horizontalScrollBar->setSliderPosition(m_data->min_frame);
    setZoom(1.0f);

    m_ui->allocationsGraph->replot();
    m_ui->sizeGraph->replot();
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

    uint32_t capture_flags = (dlg.trackManaged() ? owlcat::CAPTURE_MANAGED : 0) | (dlg.trackNative() ? owlcat::CAPTURE_NATIVE : 0);
    std::string native_config = dlg.trackNative() ? read_text_file(dlg.hookConfigPath()) : std::string();

    m_data->clear();
    m_data->update_boundaries();

    // TODO: Learn to resume a profiling session?
    m_client.close_db();
    if (!m_db_file_name.empty() && std::filesystem::exists(m_db_file_name))
        std::filesystem::remove(m_db_file_name);

    m_db_file_name = std::tmpnam(0);
    
    if (!m_client.start(ip.c_str(), port, m_db_file_name.c_str(), capture_flags, native_config))
    {
        QMessageBox::critical(nullptr, "Netwok error", "Failed to connect to profiler server", QMessageBox::Ok);
        return;
    }

    m_is_db_temporary = true;

    m_live_objects_data.init(m_client.get_data());
    m_live_objects_by_type_model.init(&m_live_objects_data);
    m_live_callstack_by_type_model.init(&m_live_objects_data);

    m_search_results_data.init(m_client.get_data());
    m_search_results_types_model.init(&m_search_results_data);
    m_search_results_callstacks_model.init(&m_search_results_data);

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
    auto mode = dlg.mode();

    uint32_t capture_flags = (dlg.trackManaged() ? owlcat::CAPTURE_MANAGED : 0) | (dlg.trackNative() ? owlcat::CAPTURE_NATIVE : 0);
    std::string native_config = dlg.trackNative() ? read_text_file(dlg.hookConfigPath()) : std::string();

    m_data->clear();
    m_data->update_boundaries();

    // TODO: Learn to resume a profiling session?
    m_client.close_db();
    //if (!m_db_file_name.empty() && std::filesystem::exists(m_db_file_name))
    //    std::filesystem::remove(m_db_file_name);

    // Use "temporary" database, which stores part of content in memory for quicker writes
    m_db_file_name = "";// ":memory:"; std::tmpnam(0);

    QString dllPath;
    if (mode == PROFILE_MODE_MONO)
        dllPath = QApplication::applicationDirPath() + "\\mono_profiler_mono.dll";
    else
        dllPath = QApplication::applicationDirPath() + "\\mono_profiler_il2cpp.dll";

    auto result = m_client.launch_executable(path, args, port, m_db_file_name.c_str(), dllPath.toStdString(), capture_flags, native_config);
    if (result != owlcat::mono_profiler_client::OK)
    {
        const char* error = "Unknown error";
        switch (result)        
        {
            case owlcat::mono_profiler_client::DETOUR_CREATE_WITH_DLL_FAILED:
                error = "Failed to launch target executable with profiler DLL. Profiler DLL is missing or the executable is protected somehow. mono_profiler_server.dll should be placed in the same folder where mono_profiler_ui.exe resided";
                break;
            case owlcat::mono_profiler_client::DETOUR_PIPE_TIMEOUT:
                error = "Failed to establish named pipe connection to profiler";
                break;
            case owlcat::mono_profiler_client::DETOUR_PIPE_FAILED:
                error = "Pipe failed to enter message mode";
                break;
            case owlcat::mono_profiler_client::DETOUR_PIPE_READ_FAILED:
                error = "Failed to read error code from named pipe";
                break;
            case owlcat::mono_profiler_client::PDB_NOT_FOUND:
                error = "Unable to find necessary symbols. Probably PDB file is missing. Place PDB file for UnityPlayer.dll into the same folder where the target executable file resides";
                break;
            case owlcat::mono_profiler_client::DETOUR_FAILED:
                error = "Failed to detour target executable";
                break;
            case owlcat::mono_profiler_client::BAD_VERSION:
                error = "Failed to find necessary symbols. It is possible that the target application uses unsupported Unity version";
                break;
            case owlcat::mono_profiler_client::DETOUR_FAILED_LATE:
                error = "Failed to detour target executable in player_init_engine_graphics_detour. This shouldn't really happen";
                break;
            case owlcat::mono_profiler_client::CONNECT_FAILED:
                error = "Failed to connect to profiler server. Prossibly app failed to start in time";
                break;
        }

        QString text = QString("Failed to launch app: %1").arg(error);
        QMessageBox::critical(nullptr, "Error", text, QMessageBox::Ok);
        return;
    }

    m_is_db_temporary = true;

    m_live_objects_data.init(m_client.get_data());
    m_live_objects_by_type_model.init(&m_live_objects_data);
    m_live_callstack_by_type_model.init(&m_live_objects_data);

    m_search_results_data.init(m_client.get_data());
    m_search_results_types_model.init(&m_search_results_data);
    m_search_results_callstacks_model.init(&m_search_results_data);

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

void main_window::onShowSymbols()
{
    if (m_symbols_dialog == nullptr)
    {
        m_symbols_dialog = new symbols_dialog(&m_client, this);
        connect(m_symbols_dialog, &symbols_dialog::symbolPathsChanged, this, &main_window::refreshSymbolsIndicator);
    }

    m_symbols_dialog->show();
    m_symbols_dialog->raise();
    m_symbols_dialog->activateWindow();
}

void main_window::onShowRuntimeFrames()
{
    if (m_runtime_frames_dialog == nullptr)
    {
        m_runtime_frames_dialog = new runtime_frames_dialog(this);
        connect(m_runtime_frames_dialog, &runtime_frames_dialog::patternsChanged, this, [this]()
        {
            // Both the rows and the inspector are drawn from the classification
            m_ui->callstacksList->viewport()->update();
            onLiveObjectCallstackSelected(QItemSelection(), QItemSelection());
        });
    }

    m_runtime_frames_dialog->show();
    m_runtime_frames_dialog->raise();
    m_runtime_frames_dialog->activateWindow();
}

void main_window::onThemeChanged()
{
    auto* action = qobject_cast<QAction*>(sender());
    if (action == nullptr)
        return;

    theme::set_preference(action->data().toString());
    icons::invalidate();

    // Re-apply everything that reads the token table directly
    setupPlotStyle();
    setupTableStyle();
    m_ui->actionOpen->setIcon(icons::open());
    m_ui->actionSave->setIcon(icons::save());
    m_ui->actionStart_Profiling->setIcon(icons::connect_app());
    m_ui->actionRunUnityApp->setIcon(icons::run_app());
    m_ui->actionStop_Profiling->setIcon(icons::stop());
    m_ui->actionPause_App->setIcon(icons::pause());
    if (m_symbols_button != nullptr)
        m_symbols_button->setIcon(icons::symbols());
    if (m_settings_button != nullptr)
        m_settings_button->setIcon(icons::settings());
    if (m_settings_symbols_action != nullptr)
        m_settings_symbols_action->setIcon(icons::symbols());

    // Painted icons and rendered HTML both bake in the colour they were made
    // with, so every widget that owns either has to be told
    if (m_callstack_inspector != nullptr)
        m_callstack_inspector->apply_theme();
    if (m_symbols_dialog != nullptr)
        m_symbols_dialog->apply_theme();
    if (m_runtime_frames_dialog != nullptr)
        m_runtime_frames_dialog->apply_theme();
    if (m_events_strip != nullptr)
        m_events_strip->update();

    refreshEventsStrip();
    refreshSymbolsIndicator();
}

/*
    The toolbar extras (capture mode, Symbols, Settings), the status-bar
    indicators and the filter-box affordances. Everything here is created in
    code because the .ui format cannot express a menu button or a status-bar
    permanent widget.
*/
void main_window::setupChrome()
{
    const auto& t = theme::tokens();

    setWindowIcon(icons::app());

    m_ui->actionOpen->setIcon(icons::open());
    m_ui->actionSave->setIcon(icons::save());
    m_ui->actionStart_Profiling->setIcon(icons::connect_app());
    m_ui->actionRunUnityApp->setIcon(icons::run_app());
    m_ui->actionStop_Profiling->setIcon(icons::stop());
    m_ui->actionPause_App->setIcon(icons::pause());

    // Shortcuts belong in the tooltip, not only in the menu
    for (QAction* action : { m_ui->actionOpen, m_ui->actionSave, m_ui->actionStart_Profiling, m_ui->actionStop_Profiling })
        if (!action->shortcut().isEmpty())
            action->setToolTip(QString("%1 (%2)").arg(action->toolTip().isEmpty() ? action->text() : action->toolTip(),
                                                      action->shortcut().toString(QKeySequence::NativeText)));

    m_ui->toolBar->insertSeparator(m_ui->actionStart_Profiling);

    // ---- right-hand side of the toolbar ----

    auto* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_ui->toolBar->addWidget(spacer);

    m_capture_mode_label = new QLabel(this);
    m_capture_mode_label->setStyleSheet(QString("color: %1; padding-right: 4px;").arg(t.text_dim.name()));
    m_ui->toolBar->addWidget(m_capture_mode_label);

    m_ui->toolBar->addSeparator();

    // Symbols is the one setting that changes what the callstack pane can show,
    // so it gets a button carrying its own state rather than a menu entry
    m_symbols_button = new QToolButton(this);
    m_symbols_button->setIcon(icons::symbols());
    m_symbols_button->setText(tr("Symbols"));
    m_symbols_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_symbols_button->setToolTip(tr("Symbol search paths and per-module resolution status"));
    connect(m_symbols_button, &QToolButton::clicked, this, &main_window::onShowSymbols);
    m_ui->toolBar->addWidget(m_symbols_button);

    m_symbols_badge = new QLabel(this);
    m_symbols_badge->setFont(theme::mono_font(-1));
    m_symbols_badge->setCursor(Qt::PointingHandCursor);
    m_symbols_badge->installEventFilter(this);
    m_ui->toolBar->addWidget(m_symbols_badge);

    // ---- settings menu (everything that is not symbols) ----

    auto* settings_menu = new QMenu(this);

    auto* theme_group = new QActionGroup(this);
    theme_group->setExclusive(true);
    const QString current_pref = theme::preference();
    struct { const char* label; const char* value; } themes[] = {
        { "Dark theme", "dark" }, { "Light theme", "light" }, { "Follow system", "system" } };
    for (const auto& entry : themes)
    {
        auto* action = settings_menu->addAction(tr(entry.label));
        action->setCheckable(true);
        action->setData(QString(entry.value));
        action->setChecked(current_pref == entry.value);
        theme_group->addAction(action);
        connect(action, &QAction::triggered, this, &main_window::onThemeChanged);
    }

    settings_menu->addSeparator();

    // Every allocation stack ends in the allocator, so by default a row is
    // labelled with the first frame of game code instead of the raw leaf
    auto* attribute_action = settings_menu->addAction(tr("Attribute allocations to game code"));
    attribute_action->setCheckable(true);
    attribute_action->setChecked(frame_class::attribution_enabled());
    attribute_action->setToolTip(tr("Label callstack rows with the first non-runtime frame "
                                    "instead of the allocator frame every stack ends in"));
    connect(attribute_action, &QAction::toggled, this, [this](bool on)
    {
        frame_class::set_attribution_enabled(on);
        m_ui->callstacksList->viewport()->update();
        onLiveObjectCallstackSelected(QItemSelection(), QItemSelection());
    });

    auto* runtime_frames_entry = settings_menu->addAction(tr("Runtime frames..."));
    runtime_frames_entry->setToolTip(tr("Which frames count as allocator, runtime or engine plumbing"));
    connect(runtime_frames_entry, &QAction::triggered, this, &main_window::onShowRuntimeFrames);

    settings_menu->addSeparator();
    m_settings_symbols_action = settings_menu->addAction(icons::symbols(), tr("Symbols..."));
    connect(m_settings_symbols_action, &QAction::triggered, this, &main_window::onShowSymbols);

    m_settings_button = new QToolButton(this);
    m_settings_button->setIcon(icons::settings());
    m_settings_button->setText(tr("Settings"));
    m_settings_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_settings_button->setPopupMode(QToolButton::InstantPopup);
    m_settings_button->setMenu(settings_menu);
    m_ui->toolBar->addWidget(m_settings_button);

    // ---- status bar ----

    m_status_symbols = new QLabel(this);
    m_status_symbols->setCursor(Qt::PointingHandCursor);
    m_status_symbols->setToolTip(tr("Open the Symbols window"));
    m_status_symbols->installEventFilter(this);
    statusBar()->addPermanentWidget(m_status_symbols);

    // ---- filter boxes ----

    for (QLineEdit* filter : { m_ui->typesFilter, m_ui->callstackFilter, m_ui->searchTypesFilter, m_ui->searchCallstackFilter })
    {
        filter->setClearButtonEnabled(true);
        filter->addAction(icons::search(), QLineEdit::LeadingPosition);
    }

    auto* find_shortcut = new QShortcut(QKeySequence::Find, this);
    connect(find_shortcut, &QShortcut::activated, this, [this]()
    {
        // Ctrl+F focuses the filter of whichever tab is in front
        QLineEdit* target = m_ui->tabWidget->currentWidget() == m_ui->searchResultsTab
            ? m_ui->searchTypesFilter : m_ui->typesFilter;
        target->setFocus(Qt::ShortcutFocusReason);
        target->selectAll();
    });

    buildLegend();
    setupPlotPanel();

    // Symbolication happens on a background thread whether or not a capture is
    // being recorded, so this ticks independently of the profiling timer
    connect(&m_chrome_timer, &QTimer::timeout, this, &main_window::refreshSymbolsIndicator);
    connect(&m_chrome_timer, &QTimer::timeout, this, &main_window::syncAxisExtents);
    connect(&m_chrome_timer, &QTimer::timeout, this, &main_window::refreshEventsStrip);
    m_chrome_timer.start(1500);
    refreshSymbolsIndicator();
}

void main_window::refreshSymbolsIndicator()
{
    const auto& t = theme::tokens();

    int resolved = 0, total = 0;
    symbols_dialog::module_counts(&m_client, resolved, total);
    const size_t pending = m_client.get_symbolication_pending();

    if (total == 0)
    {
        m_symbols_badge->clear();
        m_status_symbols->setText(QString());
        return;
    }

    const bool all_good = resolved == total;
    const QColor color = all_good ? t.ok : t.warn;

    m_symbols_badge->setText(QString("%1 / %2").arg(resolved).arg(total));
    m_symbols_badge->setStyleSheet(QString("color: %1; padding-right: 6px;").arg(color.name()));
    m_symbols_badge->setToolTip(all_good
        ? tr("Symbols found for every module")
        : tr("%1 module(s) without symbols - native frames stay as module+offset").arg(total - resolved));

    QString status = tr("symbols: %1 of %2").arg(resolved).arg(total);
    if (pending > 0)
        status += tr(" (resolving %1)").arg(pending);
    m_status_symbols->setText(status + "  \u203a");
    m_status_symbols->setStyleSheet(QString("color: %1; border-bottom: 1px dotted %2; padding: 0 4px;")
        .arg(color.name(), t.text_faint.name()));
}

void main_window::onAllocationsScrolled(int value)
{
    syncAxisExtents();
    refreshEventsStrip();
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

    // Populating the table and applying row heights both belong to the GUI
    // thread, so the work is chunked and the event loop is pumped in between:
    // the progress bar paints, and the user can still change the selection -
    // which re-enters this handler and cancels the older pass via `generation`.
    // A cancelled pass returns without restoring the UI: the newer pass that
    // cancelled it has already done so itself.
    auto& s = m_live_callstacks_view;
    const uint64_t generation = ++s.generation;
    auto cancelled = [&s, generation] { return s.generation != generation; };

    s.populating = true;
    m_ui->callstacksList->setEnabled(false);

    size_t total = 0;
    bool bar_shown = false;
    m_live_callstack_by_type_model.update(type_id, [&](size_t cur, size_t max)
        {
            if (cancelled())
                return false;
            if (cur == 0 && max > kCallstacksProgressThreshold)
            {
                total = max;
                // First half of the range: building rows; second half: row heights
                s.bar->setRange(0, (int)(2 * max));
                s.bar->setValue(0);
                s.bar->setVisible(true);
                bar_shown = true;
            }
            if (bar_shown && (cur + 1) % kCallstacksChunk == 0)
            {
                s.bar->setValue((int)cur);
                QCoreApplication::processEvents();
                if (cancelled())
                    return false;
            }
            return true;
        });

    if (cancelled())
        return;

    m_ui->callstacksList->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_ui->callstacksList->horizontalHeader()->resizeSection(1, 100);
    m_ui->callstacksList->horizontalHeader()->resizeSection(2, 100);
    m_ui->callstacksList->sortByColumn(2, Qt::SortOrder::DescendingOrder);

    if (!applyRowHeights(s, generation, (int)total, false))
        return;

    s.populating = false;
    s.bar->setVisible(false);
    m_ui->callstacksList->setEnabled(true);

    updateCallstacksFilteredSize();
}

void main_window::onSearchTypeSelected(const QItemSelection& selected, const QItemSelection& deselected)
{
    auto currentIndex = m_ui->searchTypesList->currentIndex();
    if (currentIndex == QModelIndex())
        return;

    uint64_t type_id = m_search_results_types_filter_model.data(currentIndex, Qt::UserRole).toInt();

    // Same chunked populate + row heights as onLiveObjectTypeSelected
    auto& s = m_search_callstacks_view;
    const uint64_t generation = ++s.generation;
    auto cancelled = [&s, generation] { return s.generation != generation; };

    s.populating = true;
    m_ui->searchCallstacksList->setEnabled(false);

    size_t total = 0;
    bool bar_shown = false;
    m_search_results_callstacks_model.update(type_id, [&](size_t cur, size_t max)
        {
            if (cancelled())
                return false;
            if (cur == 0 && max > kCallstacksProgressThreshold)
            {
                total = max;
                s.bar->setRange(0, (int)(2 * max));
                s.bar->setValue(0);
                s.bar->setVisible(true);
                bar_shown = true;
            }
            if (bar_shown && (cur + 1) % kCallstacksChunk == 0)
            {
                s.bar->setValue((int)cur);
                QCoreApplication::processEvents();
                if (cancelled())
                    return false;
            }
            return true;
        });

    if (cancelled())
        return;

    m_ui->searchCallstacksList->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_ui->searchCallstacksList->horizontalHeader()->resizeSection(1, 100);
    m_ui->searchCallstacksList->horizontalHeader()->resizeSection(2, 100);
    m_ui->searchCallstacksList->sortByColumn(2, Qt::SortOrder::DescendingOrder);

    if (!applyRowHeights(s, generation, (int)total, false))
        return;

    s.populating = false;
    s.bar->setVisible(false);
    m_ui->searchCallstacksList->setEnabled(true);
}

void main_window::onPickerChanged(const QPolygon& selection)
{
    QRect rect(selection.first(), selection.last());
    rect = rect.normalized();

    // The polygon is in the canvas pixels of the plot that was dragged, so it
    // must be mapped back through THAT plot: the two canvases can start at
    // different x (their left-axis labels differ in width), and using the wrong
    // transform shifted every selection made on the memory graph
    QwtPlot* source = m_ui->allocationsGraph;
    if (sender() == m_size_picker.get())
        source = m_ui->sizeGraph;

    const double left = source->invTransform(QwtPlot::xBottom, rect.left());
    const double right = source->invTransform(QwtPlot::xBottom, rect.right());

    // Frame f occupies [f, f+1) on the X axis. The selection is computed as an
    // INCLUSIVE frame range [first_frame, last_frame] - every frame whose column
    // the drag overlaps - and everything downstream takes inclusive ranges. The
    // only exclusive value is the highlight zone end, which is in axis coordinates.
    int first_frame = (int)floor(left);
    int last_frame = (int)ceil(right) - 1;

    // A drag that reaches the right edge of the canvas means "up to the end of
    // what is visible": include the boundary frame, whose data point can sit
    // exactly on the edge of the axis
    if (rect.right() >= source->canvas()->width() - 2)
        last_frame = (int)floor(right);

    // A click exactly on a frame boundary still selects one frame
    if (last_frame < first_frame)
        last_frame = first_frame;

    if (m_ui->snapToGC->checkState() == Qt::Checked)
    {
        first_frame = (int)m_data->get_closest_gc_frame(first_frame);
        last_frame = (int)m_data->get_closest_gc_frame(last_frame);
        if (last_frame < first_frame)
            last_frame = first_frame;
    }

    m_allocationsZone.setInterval(first_frame, last_frame + 1);
    m_sizeZone.setInterval(first_frame, last_frame + 1);
    if (m_events_strip != nullptr)
        m_events_strip->set_selection(first_frame, last_frame);
    updateSelectionSummary(first_frame, last_frame);

    calculateLiveObjects(first_frame, last_frame);
}

void main_window::onLiveObjectsCallstacksContextMenuRequested(QPoint point)
{
    QModelIndex index = m_ui->callstacksList->indexAt(point);

    if (index == QModelIndex())
        return;

    m_callstacksMenu->popup(m_ui->callstacksList->viewport()->mapToGlobal(point));
}

void main_window::onReferencesContextMenuRequested(QPoint point)
{
    QModelIndex index = m_ui->objectReferences->indexAt(point);

    if (index == QModelIndex())
        return;

    m_referencesMenu->popup(m_ui->objectReferences->viewport()->mapToGlobal(point));
}

void main_window::onLiveObjectsMenuRequested(QPoint point)
{
    m_liveObjectsMenu->popup(m_ui->liveObjectsList->viewport()->mapToGlobal(point));
}

void main_window::onCallstackMenuAction(bool state)
{
    findReferencesForCurrentCallstack();
}

void main_window::findReferencesForCurrentCallstack()
{
    auto currentIndex = m_ui->callstacksList->currentIndex();
    if (currentIndex == QModelIndex())
        return;

    currentIndex = m_live_callstack_by_type_filter_model.mapToSource(currentIndex);

    uint64_t type_id = m_live_objects_by_type_model.data(currentIndex, Qt::UserRole).toInt();

    auto addresses = m_live_callstack_by_type_model.get_addresses(currentIndex);
    if (addresses == nullptr)
        return;
    auto addresses_truncated = *addresses;
    findObjectsReferences(addresses_truncated);
}

void main_window::onReferencesMenuAction(bool state)
{
    auto currentIndex = m_ui->objectReferences->currentIndex();
    if (currentIndex == QModelIndex())
        return;

    // Cancel any in-flight search-callstacks population/height pass (this can
    // be reached from their processEvents pumping): search_address_list
    // rebuilds m_search_results_data, which such a pass iterates
    ++m_search_callstacks_view.generation;
    m_search_callstacks_view.populating = false;
    m_search_callstacks_view.bar->setVisible(false);

    auto addresses = m_object_references_tree_model.get_addresses(currentIndex);
    m_search_results_data.search_address_list(addresses, nullptr);
    m_ui->tabWidget->setCurrentIndex(2);

    m_search_results_types_model.update();
    m_search_results_callstacks_model.clear();
}

void main_window::onTypesMenuAction(bool state)
{
    export_types_to_csv();
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
    m_pending_callstack_filter = filter;
    m_callstack_filter_timer.start();
}

void main_window::onReferenceExpanded(QModelIndex index)
{
    m_object_references_tree_model.expand(index);
    m_ui->objectReferences->doItemsLayout();
}

bool main_window::eventFilter(QObject* object, QEvent* event)
{
    // The Symbols badge and the status-bar entry are plain QLabels acting as
    // shortcuts to the Symbols window (a QLabel has no clicked() signal)
    if ((object == m_symbols_badge || object == m_status_symbols) && event->type() == QEvent::MouseButtonRelease)
    {
        onShowSymbols();
        return true;
    }

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

    // Measure the database insert rate (events written per second), averaged over ~1 second windows
    uint64_t db_inserted = m_client.get_db_inserted_events_count();
    if (!m_db_rate_timer.isValid())
    {
        m_db_rate_timer.start();
        m_last_db_inserted_count = db_inserted;
    }
    else if (m_db_rate_timer.elapsed() >= 1000)
    {
        m_db_inserts_per_second = (db_inserted - m_last_db_inserted_count) * 1000 / (uint64_t)m_db_rate_timer.elapsed();
        m_last_db_inserted_count = db_inserted;
        m_db_rate_timer.restart();
    }

    char tmp[160];
    sprintf(tmp, "Network buffer: %I64u | Events stored/s: %I64u (total: %I64u)", m_client.get_network_messages_count(), m_db_inserts_per_second, db_inserted);
    m_ui->statusbar->showMessage(tmp);
}

void main_window::closeEvent(QCloseEvent* ev)
{
    if (!trySaveUnsavedData())
    {
        ev->ignore();
        return;
    }

    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
    settings.setValue("window/splitter", m_ui->splitter->saveState());
    if (m_live_splitter != nullptr)
        settings.setValue("window/live_panes", m_live_splitter->saveState());

    ev->accept();
}

/*
    Everything the plots take from the token table. Qwt does not read the
    application stylesheet, so this is where the theme reaches the charts -
    and it is re-run on a theme switch.
*/
void main_window::setupPlotStyle()
{
    const auto& t = theme::tokens();

    struct plot_entry { QwtPlot* plot; };
    for (QwtPlot* plot : { (QwtPlot*)m_ui->allocationsGraph, (QwtPlot*)m_ui->sizeGraph })
    {
        plot->setCanvasBackground(QBrush(t.plot_bg));

        // The sunken 3D frame Qwt draws by default is the single most dated
        // pixel in the window; the panel around the plot carries the border
        // static_cast, not qobject_cast: the Qwt DLL does not export
        // QwtPlotCanvas::staticMetaObject, and QwtPlot always creates a
        // QwtPlotCanvas here (we never swap in a different canvas)
        if (auto* canvas = static_cast<QwtPlotCanvas*>(plot->canvas()))
        {
            canvas->setFrameStyle(QFrame::NoFrame);
            canvas->setBorderRadius(4);
        }

        QPalette plot_palette = plot->palette();
        plot_palette.setColor(QPalette::WindowText, t.text_dim);
        plot_palette.setColor(QPalette::Text, t.text_dim);
        plot->setPalette(plot_palette);

        for (int axis : { QwtPlot::yLeft, QwtPlot::xBottom })
        {
            QFont axis_font = theme::mono_font(-1);
            plot->setAxisFont(axis, axis_font);
            if (auto* scale = plot->axisWidget(axis))
            {
                QPalette scale_palette = scale->palette();
                scale_palette.setColor(QPalette::WindowText, t.border_strong);  // ticks
                scale_palette.setColor(QPalette::Text, t.text_dim);             // labels
                scale->setPalette(scale_palette);
            }
        }

        // Horizontal rules only: the curve should stay the loudest thing here
        auto* grid = new QwtPlotGrid();
        grid->setPen(QPen(t.grid, 1.0, Qt::SolidLine));
        grid->enableX(false);
        grid->enableY(true);
        grid->enableXMin(false);
        grid->enableYMin(false);
        grid->setZ(0);
        // Replaces the grid attached by a previous call (Qwt deletes detached items it owns)
        for (QwtPlotItem* item : plot->itemList(QwtPlotItem::Rtti_PlotGrid))
        {
            item->detach();
            delete item;
        }
        grid->attach(plot);
    }

    // Allocation counts: thin muted-violet columns, so pure red is free again
    // for anomalies rather than meaning "allocations" and "committed" at once
    m_allocations_chart.setPen(QPen(t.series_allocs, 0.0));
    m_allocations_chart.setBrush(QBrush(t.series_allocs));
    m_allocations_chart.setRenderHint(QwtPlotItem::RenderAntialiased, true);

    QPen committed_pen(t.series_committed, 1.4, Qt::DashLine);
    m_committed_chart.setPen(committed_pen);
    m_committed_chart.setRenderHint(QwtPlotItem::RenderAntialiased, true);

    QPen tracked_pen(t.series_tracked, 1.6);
    m_size_chart.setPen(tracked_pen);
    m_size_chart.setRenderHint(QwtPlotItem::RenderAntialiased, true);

    // Area fill under the tracked line, fading to nothing at the baseline
    QLinearGradient fill(0, 0, 0, 1);
    fill.setCoordinateMode(QGradient::ObjectBoundingMode);
    QColor fill_top = t.series_tracked;
    fill_top.setAlphaF(0.22);
    QColor fill_bottom = t.series_tracked;
    fill_bottom.setAlphaF(0.02);
    fill.setColorAt(0.0, fill_top);
    fill.setColorAt(1.0, fill_bottom);
    m_size_chart.setBrush(QBrush(fill));

    // Selection band across both plots
    QColor band = t.selection_band;
    band.setAlphaF(0.10);
    m_allocationsZone.setBrush(QBrush(band));
    m_sizeZone.setBrush(QBrush(band));

    if (m_legend != nullptr)
        updateLegendColors();

    // The pickers paint the cursor readout and the selection band themselves
    if (m_allocations_picker)
        m_allocations_picker->apply_theme();
    if (m_size_picker)
        m_size_picker->apply_theme();

    m_ui->allocationsGraph->replot();
    m_ui->sizeGraph->replot();
}

/*
    Row/header presentation for the four data tables. Numbers are drawn in a
    monospace face with tabular figures so columns of sizes stop jittering;
    the alignment itself comes from the models (Qt::TextAlignmentRole).
*/
void main_window::setupTableStyle()
{
    const QFont mono = theme::mono_font(0);

    for (QTableView* view : { m_ui->liveObjectsList, m_ui->callstacksList,
                              m_ui->searchTypesList, m_ui->searchCallstacksList })
    {
        view->setFont(mono);
        view->setShowGrid(false);
        view->setAlternatingRowColors(false);
        view->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        view->horizontalHeader()->setHighlightSections(false);
        view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    }

    m_ui->objectReferences->setFont(mono);

    for (QLabel* label : { m_ui->liveObjectsSizeLabel, m_ui->callstacksSizeLabel })
        label->setStyleSheet(QString("color: %1;").arg(theme::tokens().text_dim.name()));
}

void main_window::setZoom(float new_zoom)
{
    syncAxisExtents();
    refreshEventsStrip();
    m_zoom = new_zoom;
    if (m_zoom <= 0.02f)
        m_zoom = 0.02f;
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
    // Cancel any in-flight callstacks population/height pass (this can be
    // reached from their processEvents pumping): the worker below rebuilds
    // m_live_objects_data, which a populate pass iterates
    ++m_live_callstacks_view.generation;
    m_live_callstacks_view.populating = false;
    m_live_callstacks_view.bar->setVisible(false);

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
            size_t last_percent = 0;

            m_live_objects_data.update(from_frame, to_frame, [&](size_t cur, size_t max)
                {
                    if (QThread::currentThread()->isInterruptionRequested())
                        return false;

                    if (first_time)
                    {
                        first_time = false;
                        emit onLiveObjectTypesProgressInitiated(0, (int)max);
                    }
                    size_t percent = 0;
                    if (cur < std::numeric_limits<size_t>::max() / 100)                        
                        percent = max > 0 ? cur * 100 / max : 0;
                    else
                        percent = max > 0 ? (size_t)(floor((double)cur/max * 100)) : 0;

                    if (percent > last_percent)
                    {
                        last_percent = percent;
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
            updateCallstacksFilteredSize();
        }
    ));

    m_types_worker->start();
}

/*
    Sets every row's height from the model's cached line count (role::Lines),
    in chunks, pumping the event loop in between so the progress bar paints and
    the pass can be cancelled. This replaces QHeaderView::ResizeToContents,
    which laid out every row's full multi-line text on the GUI thread on every
    insert/sort/filter pass.

    progress_offset shifts the bar values when the caller shares the bar with a
    preceding populate phase; manage_bar makes this pass own the bar's
    range/visibility (standalone re-application after sort/filter changes).

    Returns false if cancelled (s.generation moved on from inside processEvents).
*/
bool main_window::applyRowHeights(callstacks_view_state& s, uint64_t generation, int progress_offset, bool manage_bar)
{
    QAbstractItemModel* model = s.view->model();
    QHeaderView* header = s.view->verticalHeader();

    const int rows = model->rowCount();
    if (rows == 0)
        return true;

    // Calibrate against the delegate's real hint for one row, so heights match
    // what ResizeToContents used to produce
    const int line_spacing = s.view->fontMetrics().lineSpacing();
    const int first_lines = std::max(1, model->index(0, 0).data(live_callstacks_by_type_model::role::Lines).toInt());
    // sizeHintForRow is public on QAbstractItemView but re-declared protected
    // by QTableView; access is checked against the static type, so go via the base
    const int first_hint = static_cast<QAbstractItemView*>(s.view)->sizeHintForRow(0);
    const int padding = first_hint > 0 ? std::max(0, first_hint - first_lines * line_spacing) : 4;

    const bool show_bar = manage_bar && rows > (int)kCallstacksProgressThreshold;
    if (show_bar)
    {
        s.bar->setRange(0, rows);
        s.bar->setValue(0);
        s.bar->setVisible(true);
    }

    // The progress bar is a child of the view itself, not of the viewport, so
    // it keeps painting while viewport updates are suppressed
    s.view->viewport()->setUpdatesEnabled(false);
    ++s.heights_running;

    for (int i = 0; i < rows; ++i)
    {
        const int lines = std::max(1, model->index(i, 0).data(live_callstacks_by_type_model::role::Lines).toInt());
        header->resizeSection(i, padding + lines * line_spacing);

        if ((size_t)(i + 1) % kCallstacksChunk == 0)
        {
            s.bar->setValue(progress_offset + i + 1);
            QCoreApplication::processEvents();
            if (s.generation != generation)
            {
                --s.heights_running;
                s.view->viewport()->setUpdatesEnabled(true);
                // The successor pass may never show the bar (e.g. the filter
                // shrank the list below the progress threshold), so a
                // bar-owning pass must hide it itself on cancellation
                if (show_bar)
                    s.bar->setVisible(false);
                return false;
            }
        }
    }

    --s.heights_running;
    s.view->viewport()->setUpdatesEnabled(true);
    s.view->viewport()->update();

    if (show_bar)
        s.bar->setVisible(false);

    return true;
}

void main_window::scheduleRowHeights(callstacks_view_state& s)
{
    // Populate passes apply heights themselves; coalesce everything else
    // (sort clicks, filter changes) into one deferred pass per event-loop turn
    if (s.populating || s.heights_scheduled)
        return;

    s.heights_scheduled = true;
    QTimer::singleShot(0, this, [this, &s]()
        {
            s.heights_scheduled = false;
            if (s.populating)
                return;
            const uint64_t generation = ++s.generation;
            applyRowHeights(s, generation, 0, true);
        });
}

void main_window::updateLiveObjectsSelectedSize()
{
    auto rows = m_ui->liveObjectsList->selectionModel()->selectedRows();
    uint64_t selected_size = 0;
    for (auto& index : rows)
    {
        selected_size += m_live_object_by_type_filter_model.data(index, live_objects_by_type_model::role::Size).toULongLong();
    }
    QString text = QString("Selected %1 out of %2").arg(size_to_string(selected_size), size_to_string(m_live_objects_by_type_model.get_total_size()));
    m_ui->liveObjectsSizeLabel->setText(text);
}

void main_window::updateCallstacksFilteredSize()
{
    uint64_t filtered_size = 0;
    int rows = m_live_callstack_by_type_filter_model.rowCount();
    for (int i = 0; i < rows; ++i)
    {
        auto index = m_live_callstack_by_type_filter_model.index(i, 0);
        filtered_size += m_live_callstack_by_type_filter_model.data(index, live_callstacks_by_type_model::role::Size).toULongLong();
    }
    QString text = QString("Filtered %1 out of %2").arg(size_to_string(filtered_size), size_to_string(m_live_callstack_by_type_model.get_total_size()));
    m_ui->callstacksSizeLabel->setText(text);
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
/*FILE* f = fopen("test.fr", "w");
fprintf(f, "%I64u\n", addresses.size());
for(auto& a : addresses)
    fprintf(f, "\t%I64u\n", a);
fprintf(f, "%I64u\n", result.size());
for (auto& r : result)
{
    fprintf(f, "\t%I64u \"%s\"\n", r.address, r.type.c_str());
    fprintf(f, "\t\t%I64u\n", r.parents.size());
    for(auto& p : r.parents)
        fprintf(f, "\t\t\t%I64u\n", p.address);
}
fclose(f);*/
            m_object_references_tree_model.update(error, addresses, result);
        });
}

void main_window::export_types_to_csv()
{
    QString file_name = QFileDialog::getSaveFileName(this, tr("Export to CSV"),
        QDir::currentPath(),
        tr("CSV files (*.csv)"));

    if (file_name.isEmpty())
        return;

    if (!m_live_objects_data.export_to_csv(file_name.toStdString()))
    {
        QMessageBox::critical(nullptr, "Error", "Failed to save types to CSV", QMessageBox::Ok);
    }
}

/*
    A legend row above the plots. Qwt's own QwtLegend spent a full widget row
    on two centred entries; this is one 20px line that also leaves room for
    the GC / marker entries the events strip will add.
*/
void main_window::buildLegend()
{
    m_legend = new QWidget(this);
    auto* layout = new QHBoxLayout(m_legend);
    layout->setContentsMargins(56, 2, 6, 2);
    layout->setSpacing(14);

    struct entry { const char* object_name; const char* text; bool bar; };
    static const entry entries[] = {
        { "legend_tracked",   "Tracked allocations", false },
        { "legend_committed", "Committed (process)", false },
        { "legend_allocs",    "Allocations / frame", true  },
    };

    for (const auto& e : entries)
    {
        auto* item = new QWidget(m_legend);
        auto* item_layout = new QHBoxLayout(item);
        item_layout->setContentsMargins(0, 0, 0, 0);
        item_layout->setSpacing(6);

        auto* swatch = new QFrame(item);
        swatch->setObjectName(e.object_name);
        swatch->setFixedSize(e.bar ? QSize(4, 10) : QSize(11, 2));
        item_layout->addWidget(swatch, 0, Qt::AlignVCenter);

        auto* label = new QLabel(tr(e.text), item);
        item_layout->addWidget(label, 0, Qt::AlignVCenter);

        layout->addWidget(item);
    }

    // How many collections the capture holds at all - three in a 27 000-frame
    // session is normal, and without this the empty strip looks broken
    m_events_label = new QLabel(m_legend);
    layout->addWidget(m_events_label);

    layout->addStretch();

    // Selection summary: the frame range every table below is computed from,
    // which the UI never used to state anywhere
    m_selection_label = new QLabel(m_legend);
    m_selection_label->setFont(theme::mono_font(-1));
    layout->addWidget(m_selection_label);

    m_ui->verticalLayout->insertWidget(0, m_legend);
    m_ui->verticalLayout->setStretch(0, 0);   // legend
    m_ui->verticalLayout->setStretch(1, 1);   // allocations plot
    m_ui->verticalLayout->setStretch(2, 0);   // scrollbar
    m_ui->verticalLayout->setStretch(3, 1);   // size plot
    m_ui->verticalLayout->setStretch(4, 0);   // snap-to-GC row

    updateLegendColors();
}

void main_window::updateLegendColors()
{
    const auto& t = theme::tokens();

    struct swatch_color { const char* object_name; QColor color; };
    const swatch_color colors[] = {
        { "legend_tracked",   t.series_tracked },
        { "legend_committed", t.series_committed },
        { "legend_allocs",    t.series_allocs },
    };

    for (const auto& c : colors)
    {
        if (auto* swatch = m_legend->findChild<QFrame*>(c.object_name))
            swatch->setStyleSheet(QString("background: %1; border: none;").arg(c.color.name()));
    }

    for (QLabel* label : m_legend->findChildren<QLabel*>())
        label->setStyleSheet(QString("color: %1;").arg(t.text_dim.name()));
}

/*
    The allocations and memory plots stack vertically and share an X axis, so a
    selection dragged on one is drawn on the other - which only lines up if both
    canvases start at the same x. Their left-axis labels differ in width ("500k"
    against "667.57Mb") and change as you scroll, so measure both and give each
    the wider extent.
*/
void main_window::syncAxisExtents()
{
    auto measure = [](QwtPlot* plot) -> double
    {
        auto* scale = plot->axisScaleDraw(QwtPlot::yLeft);
        // extent() clamps to the current minimum, so clear it while measuring or
        // the value would only ever ratchet upwards
        const double saved = scale->minimumExtent();
        scale->setMinimumExtent(0.0);
        const double needed = scale->extent(plot->axisFont(QwtPlot::yLeft));
        scale->setMinimumExtent(saved);
        return needed;
    };

    const double extent = qMax(qMax(measure(m_ui->allocationsGraph), measure(m_ui->sizeGraph)), 44.0);
    if (qAbs(extent - m_axis_extent) < 0.5)
        return;

    m_axis_extent = extent;
    for (QwtPlot* plot : { (QwtPlot*)m_ui->allocationsGraph, (QwtPlot*)m_ui->sizeGraph })
    {
        plot->axisScaleDraw(QwtPlot::yLeft)->setMinimumExtent(extent);
        // updateLayout() is what re-places the canvas and the scale widgets;
        // replot() alone repaints inside the old geometry, which left the two
        // canvases misaligned until some later event forced a layout
        plot->updateLayout();
        plot->replot();
    }

    // The legend row sits above the plots; keep its text over the canvas, not
    // over the axis gutter
    if (m_legend != nullptr)
    {
        if (auto* legend_layout = qobject_cast<QHBoxLayout*>(m_legend->layout()))
            legend_layout->setContentsMargins((int)extent + 2, 2, 6, 2);
    }
}

/*
    One instrument instead of two charts: memory on top, the events strip, then
    allocations per frame, with a single X axis label row at the bottom. Both
    plots keep their own Y scale (see doc: the allocation bars must never be
    able to reach the heap curve), and syncAxisExtents keeps the canvases
    starting at the same x.
*/
void main_window::setupPlotPanel()
{
    auto* layout = m_ui->verticalLayout;

    // Memory is the primary signal, so it goes first; the legend is already at 0
    layout->removeWidget(m_ui->sizeGraph);
    layout->insertWidget(1, m_ui->sizeGraph);

    // The allocations plot keeps the visible X axis, so its scale map is the one
    // Qwt definitely keeps current; both canvases start at the same x anyway
    m_events_strip = new events_strip(m_ui->allocationsGraph, this);
    m_events_strip->setToolTip(tr("GC events and instrumented markers"));
    layout->insertWidget(2, m_events_strip);

    // Order is now: legend, memory, events, allocations, scrollbar, snap row
    layout->setStretch(0, 0);
    layout->setStretch(1, 1);
    layout->setStretch(2, 0);
    layout->setStretch(3, 1);
    layout->setStretch(4, 0);
    layout->setStretch(5, 0);

    // One X axis for the pair: the top plot keeps its scale (the pickers and the
    // strip map through it) but stops drawing a second row of tick labels
    m_ui->sizeGraph->enableAxis(QwtPlot::xBottom, false);

    // Clicking an event moves the selection to that frame
    connect(m_events_strip, &events_strip::frameClicked, this, [this](qint64 frame)
    {
        const int f = (int)frame;
        m_allocationsZone.setInterval(f, f + 1);
        m_sizeZone.setInterval(f, f + 1);
        m_events_strip->set_selection(f, f);
        updateSelectionSummary(f, f);
        calculateLiveObjects(f, f);
    });

    // ---- size bars in the Size column ----

    // The bar shows each row's share of the total in view, not its size relative
    // to the biggest row (which only ever said "this one is the largest")
    m_types_size_bar = new size_bar_delegate(live_objects_by_type_model::role::Size, this);
    m_types_size_bar->set_total_provider([this]() { return (quint64)m_live_objects_by_type_model.get_total_size(); });
    m_ui->liveObjectsList->setItemDelegateForColumn(2, m_types_size_bar);

    // Two fixed lines per callstack row; the whole stack goes to the inspector
    m_callstack_delegate = new callstack_delegate(this);
    m_ui->callstacksList->setItemDelegateForColumn(0, m_callstack_delegate);

    m_callstack_inspector = new callstack_inspector(this);

    connect(m_callstack_inspector, &callstack_inspector::findReferencesRequested,
            this, &main_window::findReferencesForCurrentCallstack);
    connect(m_ui->callstacksList->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &main_window::onLiveObjectCallstackSelected);

    m_callstacks_size_bar = new size_bar_delegate(live_callstacks_by_type_model::role::Size, this);
    m_callstacks_size_bar->set_total_provider([this]() { return (quint64)m_live_callstack_by_type_model.get_total_size(); });
    m_ui->callstacksList->setItemDelegateForColumn(2, m_callstacks_size_bar);

    setupLiveObjectsPanes();
}

/*
    The tab's three panes - types, callstacks, callstack inspector - as a
    QSplitter rather than fixed grid columns: how much room a callstack needs
    depends on the capture (native frames with file and line are long), so the
    ratio is the user's call. Sizes are remembered between sessions.
*/
void main_window::setupLiveObjectsPanes()
{
    const auto build_pane = [this](QLineEdit* filter, QTableView* table, QLabel* footer)
    {
        auto* pane = new QWidget(this);
        auto* layout = new QVBoxLayout(pane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        // Adding to this layout reparents each widget out of the old grid
        layout->addWidget(filter);
        layout->addWidget(table, 1);
        layout->addWidget(footer);
        return pane;
    };

    QWidget* types_pane = build_pane(m_ui->typesFilter, m_ui->liveObjectsList, m_ui->liveObjectsSizeLabel);
    QWidget* callstacks_pane = build_pane(m_ui->callstackFilter, m_ui->callstacksList, m_ui->callstacksSizeLabel);

    m_live_splitter = new QSplitter(Qt::Horizontal, m_ui->liveObjectsTab);
    m_live_splitter->setChildrenCollapsible(false);
    m_live_splitter->setHandleWidth(7);
    m_live_splitter->addWidget(types_pane);
    m_live_splitter->addWidget(callstacks_pane);
    m_live_splitter->addWidget(m_callstack_inspector);

    // A callstack row and the inspector both need width; the type list needs least
    types_pane->setMinimumWidth(220);
    callstacks_pane->setMinimumWidth(280);
    m_callstack_inspector->setMinimumWidth(260);
    m_live_splitter->setStretchFactor(0, 26);
    m_live_splitter->setStretchFactor(1, 44);
    m_live_splitter->setStretchFactor(2, 30);

    // setSizes() is in pixels, and the splitter has no width yet, so the first
    // proportional split has to wait until it has been laid out. A saved state
    // (restored in the constructor) overrides this.
    QTimer::singleShot(0, this, [this]()
    {
        if (m_live_splitter == nullptr || m_live_splitter->width() <= 0)
            return;
        if (!QSettings().value("window/live_panes").toByteArray().isEmpty())
            return;

        const int width = m_live_splitter->width();
        m_live_splitter->setSizes({ width * 26 / 100, width * 44 / 100, width * 30 / 100 });
    });

    // The grid is empty now that its children live in the panes
    delete m_ui->liveObjectsTab->layout();
    auto* tab_layout = new QVBoxLayout(m_ui->liveObjectsTab);
    tab_layout->setContentsMargins(6, 6, 6, 6);
    tab_layout->addWidget(m_live_splitter);
}

void main_window::refreshEventsStrip()
{
    if (m_events_strip == nullptr || !m_data)
        return;

    const auto& t = theme::tokens();

    std::vector<events_strip::event_mark> events;
    for (const auto& gc : m_data->get_gc_events())
    {
        QString name = tr("GC - frame %1 - %2 objects, %3 freed")
            .arg(gc.frame)
            .arg(QLocale().toString((qulonglong)gc.freed_objects))
            .arg(size_to_string((double)gc.freed_bytes));

        events.push_back({ (int64_t)gc.frame, name, t.series_gc, events_strip::shape::diamond });
    }

    // Instrumented markers (SRV_MARKER) will be appended here with their own
    // shape and colour once the server can report them

    std::vector<double> gc_frames;
    gc_frames.reserve(events.size());
    for (const auto& mark : events)
        gc_frames.push_back((double)mark.frame + 0.5);

    m_size_markers.update(m_ui->sizeGraph, gc_frames, t.series_gc);
    m_allocations_markers.update(m_ui->allocationsGraph, gc_frames, t.series_gc);

    if (m_events_label != nullptr)
    {
        m_events_label->setText(events.empty()
            ? tr("no GC events in this capture")
            : tr("%1 GC event(s)").arg(events.size()));
    }

    m_events_strip->set_events(std::move(events));
}

void main_window::updateSelectionSummary(int first_frame, int last_frame)
{
    if (m_selection_label == nullptr)
        return;

    QString summary = tr("frames %1-%2  %3 frames")
        .arg(first_frame).arg(last_frame).arg(last_frame - first_frame + 1);

    // How much the tracked total moved across the selection - the number the
    // range was probably selected to find out
    const size_t sizes = m_data ? m_data->get_sizes_size() : 0;
    if (sizes > 0)
    {
        const int64_t base = m_data->first_visible_frame;
        const auto index_of = [&](int frame) -> uint64_t
        {
            return (uint64_t)qBound<int64_t>(0, (int64_t)frame - base, (int64_t)sizes - 1);
        };

        const double delta = m_data->get_size(index_of(last_frame)).y() - m_data->get_size(index_of(first_frame)).y();
        summary += QString("  %1%2").arg(delta < 0 ? "-" : "+").arg(size_to_string(qAbs(delta)));
    }

    m_selection_label->setText(summary);
}

void main_window::onLiveObjectCallstackSelected(const QItemSelection& selected, const QItemSelection& deselected)
{
    if (m_callstack_inspector == nullptr)
        return;

    const QModelIndex current = m_ui->callstacksList->currentIndex();
    if (!current.isValid())
    {
        m_callstack_inspector->clear_callstack();
        return;
    }

    const QModelIndex source = m_live_callstack_by_type_filter_model.mapToSource(current);
    const QModelIndex text_index = m_live_callstack_by_type_model.index(source.row(), 0);

    const QString text = m_live_callstack_by_type_model.data(text_index, Qt::DisplayRole).toString();
    const quint64 objects = m_live_callstack_by_type_model
        .data(m_live_callstack_by_type_model.index(source.row(), 1), Qt::DisplayRole).toString().remove(QLocale().groupSeparator()).toULongLong();
    const quint64 bytes = m_live_callstack_by_type_model
        .data(text_index, live_callstacks_by_type_model::role::Size).toULongLong();

    m_callstack_inspector->set_callstack(text, objects, bytes);
}
