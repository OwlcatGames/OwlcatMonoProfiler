#pragma once
#include <qwidget.h>
#include <qstring.h>

class QLabel;
class QTextBrowser;
class QPushButton;

/*
    The full callstack of the selected row, shown beside the callstacks table
    rather than inside it.

    A stack is six to twelve lines; a table cell is one row. Putting the whole
    thing in the cell is what forced variable row heights (and the chunked
    row-height pass in main_window) and still left the text truncated. The table
    now shows two lines per row (see callstack_delegate) and the full,
    symbolicated stack lives here, where it has the width to be read.
*/
class callstack_inspector : public QWidget
{
    Q_OBJECT
public:
    callstack_inspector(QWidget* parent = nullptr);

    // full_text: the callstack as the model provides it, one frame per line
    void set_callstack(const QString& full_text, quint64 objects, quint64 bytes);
    void clear_callstack();

signals:
    void findReferencesRequested();

private slots:
    void copyToClipboard();

private:
    void apply_theme();

    QLabel*        m_header;
    QLabel*        m_stats;
    QTextBrowser*  m_frames;
    QPushButton*   m_find_references;
    QPushButton*   m_copy;
    QString        m_plain_text;
};
