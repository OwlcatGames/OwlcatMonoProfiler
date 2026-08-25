#pragma once
#include <qdialog.h>

class QListWidget;
class QPushButton;
class QLabel;

/*
    Editor for the runtime-frame pattern list (see frame_classifier.h): which
    frames the UI treats as allocator/runtime plumbing when it decides what a
    callstack row is about.

    Editable rather than baked in, because the answer is project-specific: a
    studio's own engine modules are game code, another team's plugin might not
    be. Changes apply immediately, and "Restore defaults" always brings back a
    working list.
*/
class runtime_frames_dialog : public QDialog
{
    Q_OBJECT
public:
    runtime_frames_dialog(QWidget* parent = nullptr);

    // Re-reads icons and colours after a theme switch
    void apply_theme();

signals:
    // The list changed; views showing callstacks should repaint
    void patternsChanged();

private slots:
    void addPattern();
    void removeSelected();
    void restoreDefaults();
    void applyList();

private:
    void fill(const QStringList& patterns);
    QStringList currentList() const;

    QListWidget* m_list;
    QPushButton* m_remove;
    QPushButton* m_defaults;
    QLabel*      m_hint;
    QLabel*      m_count;
};
