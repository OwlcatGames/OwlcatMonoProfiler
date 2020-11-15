#include "run_dialog.h"
#include "ui_rundialog.h"
#include <qfiledialog.h>
#include <qmessagebox.h>
#include <qsettings.h>

run_dialog::run_dialog(QWidget* parent) :
    QDialog(parent),
    m_ui(new Ui::RunDialog)
{
    QSettings settings;

    m_prev_run_settings.clear();

    int size = settings.beginReadArray("run/applications");
    for (int i = 0; i < size; ++i)
    {
        settings.setArrayIndex(i);
        auto path = settings.value("path").toString();
        auto args = settings.value("arguments").toString();
        auto port = settings.value("port").toString();
        auto lastTime = settings.value("lastTime").toLongLong();

        m_prev_run_settings.push_back({path, args, port, lastTime});
    }
    settings.endArray();

    trim_prev_settings();

    m_ui->setupUi(this);

    for (auto& s : m_prev_run_settings)
        m_ui->path->addItem(s.path);
}

std::string run_dialog::path()
{
    return m_ui->path->currentText().toStdString();
}

std::string run_dialog::arguments()
{
    return m_ui->arguments->text().toStdString();
}

int run_dialog::port()
{
    return m_ui->port->text().toInt();
}

void run_dialog::browseForApp()
{
    QString defaultPath = QDir::currentPath();
    if (!m_ui->path->currentText().isEmpty())
    {
        QDir d = QFileInfo(m_ui->path->currentText()).absoluteDir();

        auto path = d.absolutePath();
        if (!path.isEmpty())
            defaultPath = path;
    }

    QString file_name = QFileDialog::getOpenFileName(this, tr("Select executable"),
        defaultPath,
        tr("Executable files (*.exe)"));

    if (!file_name.isEmpty())
        m_ui->path->setCurrentText(file_name);    
}

void run_dialog::accept()
{
    auto path = m_ui->path->currentText();
    auto args = m_ui->arguments->text();
    auto port = m_ui->port->text();

    if (!QFileInfo::exists(path))
    {
        if (QMessageBox::critical(nullptr, "File not found", "Specified executable file not found", QMessageBox::Ok, QMessageBox::Close) == QMessageBox::Ok)
            reject();

        return;
    }

    QFileInfo fi(path);
    if (!fi.isExecutable())
    {
        if (QMessageBox::critical(nullptr, "File is not executable", "Specified file is not executable", QMessageBox::Ok, QMessageBox::Close) == QMessageBox::Ok)
            reject();

        return;
    }

    if (port.toUInt() == 0)
    {
        if (QMessageBox::critical(nullptr, "Port not specified", "Please specify port for profiler to use for communications. It must enabled in your firewall.", QMessageBox::Ok, QMessageBox::Close) == QMessageBox::Ok)
            reject();

        return;
    }

    auto iter = std::find_if(m_prev_run_settings.begin(), m_prev_run_settings.end(), [&](auto& s) {return s.path == path; });
    if (iter != m_prev_run_settings.end())
    {
        iter->time = time(0);
    }
    else
    {
        m_prev_run_settings.push_back({ path, args, port, time(0) });
    }

    trim_prev_settings();

    QSettings settings;
    settings.beginWriteArray("run/applications", (int)m_prev_run_settings.size());
    int index = 0;
    for (auto& s : m_prev_run_settings)
    {
        settings.setArrayIndex(index++);
        settings.setValue("path", s.path);
        settings.setValue("arguments", s.args);
        settings.setValue("port", s.port);
        settings.setValue("lastTime", s.time);
    }
    settings.endArray();

    QDialog::accept();
}

void run_dialog::onPathSelected(QString path)
{
    if (path.isEmpty())
        return;

    auto iter = std::find_if(m_prev_run_settings.begin(), m_prev_run_settings.end(), [&](auto& s) {return s.path == path; });
    if (iter != m_prev_run_settings.end())
    {
        m_ui->arguments->setText(iter->args);
        m_ui->port->setText(iter->port);
    }
}

void run_dialog::trim_prev_settings()
{
    std::sort(m_prev_run_settings.begin(), m_prev_run_settings.end(), [](auto& s1, auto& s2) {return s1.time > s2.time; });
    if (m_prev_run_settings.size() > MAX_PREV_RUN_SETTINGS)
        m_prev_run_settings.resize(MAX_PREV_RUN_SETTINGS);
}
