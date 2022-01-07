#pragma once
#include <qdialog.h>
#include <qwidget.h>

namespace Ui {
    class RunDialog;
}

enum ProfileMode
{
    PROFILE_MODE_MONO,
    PROFILE_MODE_IL2CPP
};

/*
    Dialog for running an app on the current computer and profiling it using Detouring
*/
class run_dialog : public QDialog
{
    struct prev_run_settings
    {
        QString path;
        QString args;
        QString port;
        QString mode;
        time_t time;
    };
    std::vector<prev_run_settings> m_prev_run_settings;
    static const int MAX_PREV_RUN_SETTINGS = 10;

    Q_OBJECT
public slots:
    void browseForApp();
    void accept();
    void onPathSelected(QString path);

public:
    run_dialog(QWidget* parent = 0);

    std::string path();
    std::string arguments();
    int port();
    ProfileMode mode();

private:
    void trim_prev_settings();

    Ui::RunDialog* m_ui;
};
