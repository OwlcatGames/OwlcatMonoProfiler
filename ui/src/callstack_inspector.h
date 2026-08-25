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
    // Re-reads icons and colours after a theme switch (both are baked in at
    // paint/render time: QIcon pixmaps and the frame list's HTML)
    void apply_theme();

signals:
    void findReferencesRequested();

private slots:
    void copyToClipboard();

private:
    QLabel*        m_header;
    QLabel*        m_stats;
    QTextBrowser*  m_frames;
    QPushButton*   m_find_references;
    QPushButton*   m_copy;
    QString        m_plain_text;
    // Kept so a theme switch can re-render the frame list
    quint64        m_objects = 0;
    quint64        m_bytes = 0;
};
