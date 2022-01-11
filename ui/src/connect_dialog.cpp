#include "connect_dialog.h"
#include "ui_connectdialog.h"
#include <qsettings.h>
#include <qmessagebox.h>

connect_dialog::connect_dialog(QWidget* parent) :
    QDialog(parent),
    m_ui(new Ui::ConnectDialog)
{
    QSettings settings;

    m_prev_connect_settings.clear();

    bool contains_default = false;

    int size = settings.beginReadArray("connect/ips");
    for (int i = 0; i < size; ++i)
    {
        settings.setArrayIndex(i);
        auto ip = settings.value("ip").toString();
        auto port = settings.value("port").toString();
        auto lastTime = settings.value("lastTime").toLongLong();

        if (ip == "127.0.0.1")
            contains_default = true;

        m_prev_connect_settings.push_back({ ip, port, lastTime });
    }
    settings.endArray();

    trim_prev_settings();

    m_ui->setupUi(this);

    for (auto& s : m_prev_connect_settings)
        m_ui->ip->addItem(s.ip);

    if (!contains_default)
        m_ui->ip->addItem("127.0.0.1");
}

std::string connect_dialog::ip()
{
    return m_ui->ip->currentText().toStdString();
}

int connect_dialog::port()
{
    return m_ui->port->text().toInt();
}

void connect_dialog::accept()
{
    auto ip = m_ui->ip->currentText();
    auto port = m_ui->port->text();

    if (ip.isEmpty())
    {
        if (QMessageBox::critical(nullptr, "Wrong IP", "Please specify IP", QMessageBox::Ok, QMessageBox::Close) == QMessageBox::Ok)
            reject();

        return;
    }

    if (port.toUInt() == 0)
    {
        if (QMessageBox::critical(nullptr, "Port not specified", "Please specify port for profiler to use for communications. It must enabled in your firewall.", QMessageBox::Ok, QMessageBox::Close) == QMessageBox::Ok)
            reject();

        return;
    }

    auto iter = std::find_if(m_prev_connect_settings.begin(), m_prev_connect_settings.end(), [&](auto& s) {return s.ip == ip; });
    if (iter != m_prev_connect_settings.end())
    {
        iter->time = time(0);
    }
    else
    {
        m_prev_connect_settings.push_back({ ip, port, time(0) });
    }

    trim_prev_settings();

    QSettings settings;
    settings.beginWriteArray("run/connect", (int)m_prev_connect_settings.size());
    int index = 0;
    for (auto& s : m_prev_connect_settings)
    {
        settings.setArrayIndex(index++);
        settings.setValue("ip", s.ip);
        settings.setValue("port", s.port);
        settings.setValue("lastTime", (qulonglong)s.time);
    }
    settings.endArray();

    QDialog::accept();
}


void connect_dialog::onIPSelected(QString ip)
{
    if (ip.isEmpty())
        return;

    auto iter = std::find_if(m_prev_connect_settings.begin(), m_prev_connect_settings.end(), [&](auto& s) {return s.ip == ip; });
    if (iter != m_prev_connect_settings.end())
    {
        m_ui->port->setText(iter->port);
    }
}

void connect_dialog::trim_prev_settings()
{
    std::sort(m_prev_connect_settings.begin(), m_prev_connect_settings.end(), [](auto& s1, auto& s2) {return s1.time > s2.time; });
    if (m_prev_connect_settings.size() > MAX_PREV_CONNECT_SETTINGS)
        m_prev_connect_settings.resize(MAX_PREV_CONNECT_SETTINGS);
}
