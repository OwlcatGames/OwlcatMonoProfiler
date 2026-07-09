#pragma once
#include <qdialog.h>
#include <qwidget.h>

namespace Ui {
    class ConnectDialog;
}

/*
    Dialog for connecting to a running profiler server
*/
class connect_dialog : public QDialog
{
    struct prev_connect_settings
    {
        QString ip;
        QString port;
        time_t time;
    };
    std::vector<prev_connect_settings> m_prev_connect_settings;
    static const int MAX_PREV_CONNECT_SETTINGS = 10;

    Q_OBJECT
public slots:
    void accept();
    void onIPSelected(QString ip);
    void browseForHookConfig();

public:
    connect_dialog(QWidget* parent = 0);

    std::string ip();
    int port();
    // True if managed (Mono/IL2CPP) heap tracking is enabled
    bool trackManaged();
    // True if native heap tracking is enabled
    bool trackNative();
    // Path to the native-hook config file (empty if native tracking is off)
    std::string hookConfigPath();

private:
    void trim_prev_settings();

    Ui::ConnectDialog* m_ui;
};
