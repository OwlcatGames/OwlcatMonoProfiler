#include "callstack_inspector.h"
#include "theme.h"
#include "icons.h"
#include "common_ui.h"
#include "frame_classifier.h"

#include <qlabel.h>
#include <qtextbrowser.h>
#include <qpushbutton.h>
#include <qboxlayout.h>
#include <qclipboard.h>
#include <qguiapplication.h>
#include <qlocale.h>

namespace
{
    // Splits a frame line into the part that carries no information at a glance
    // (namespace, module, source location) and the part that does (the method),
    // so a stack can be skimmed by its method names.
    struct frame_parts
    {
        QString dim_prefix;
        QString method;
        QString dim_suffix;
    };

    frame_parts split_frame(const QString& line)
    {
        frame_parts parts;

        QString head = line;

        // "UnityPlayer.dll!Foo::Bar (file.cpp:214)" - the source location is a suffix
        const int paren = head.indexOf(" (");
        if (paren > 0)
        {
            parts.dim_suffix = head.mid(paren);
            head = head.left(paren);
        }

        // Native frames: everything up to and including "!" is the module
        const int bang = head.indexOf('!');
        if (bang > 0)
        {
            parts.dim_prefix = head.left(bang + 1);
            parts.method = head.mid(bang + 1);
            return parts;
        }

        // Managed frames: everything up to the last '.' is the namespace + type
        const int dot = head.lastIndexOf('.');
        if (dot > 0)
        {
            parts.dim_prefix = head.left(dot + 1);
            parts.method = head.mid(dot + 1);
            return parts;
        }

        parts.method = head;
        return parts;
    }

    QString escape(const QString& text)
    {
        QString out = text;
        out.replace('&', "&amp;");
        out.replace('<', "&lt;");
        out.replace('>', "&gt;");
        return out;
    }
}

callstack_inspector::callstack_inspector(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* header_row = new QHBoxLayout();
    m_header = new QLabel(tr("CALLSTACK"), this);
    header_row->addWidget(m_header);
    header_row->addStretch();
    m_copy = new QPushButton(icons::copy(), QString(), this);
    m_copy->setToolTip(tr("Copy the whole callstack"));
    m_copy->setFlat(true);
    connect(m_copy, &QPushButton::clicked, this, &callstack_inspector::copyToClipboard);
    header_row->addWidget(m_copy);
    layout->addLayout(header_row);

    m_frames = new QTextBrowser(this);
    m_frames->setReadOnly(true);
    m_frames->setOpenExternalLinks(false);
    m_frames->setLineWrapMode(QTextEdit::NoWrap);
    m_frames->setFont(theme::mono_font(-1));
    layout->addWidget(m_frames, 1);

    m_stats = new QLabel(this);
    m_stats->setFont(theme::mono_font(-1));
    layout->addWidget(m_stats);

    m_find_references = new QPushButton(icons::references(), tr("Find references to these objects"), this);
    connect(m_find_references, &QPushButton::clicked, this, &callstack_inspector::findReferencesRequested);
    layout->addWidget(m_find_references);

    apply_theme();
    clear_callstack();
}

void callstack_inspector::apply_theme()
{
    const auto& t = theme::tokens();

    QFont header_font = m_header->font();
    header_font.setPointSizeF(header_font.pointSizeF() - 1.0);
    header_font.setLetterSpacing(QFont::PercentageSpacing, 106);
    m_header->setFont(header_font);
    m_header->setStyleSheet(QString("color: %1;").arg(t.text_dim.name()));
    m_stats->setStyleSheet(QString("color: %1;").arg(t.text_dim.name()));
}

void callstack_inspector::clear_callstack()
{
    const auto& t = theme::tokens();

    m_plain_text.clear();
    m_header->setText(tr("CALLSTACK"));
    m_stats->clear();
    m_frames->setHtml(QString("<div style='color:%1'>%2</div>")
        .arg(t.text_faint.name(), tr("Select a callstack to see all of its frames here.")));
    m_find_references->setEnabled(false);
    m_copy->setEnabled(false);
}

void callstack_inspector::set_callstack(const QString& full_text, quint64 objects, quint64 bytes)
{
    const auto& t = theme::tokens();

    m_plain_text = full_text;

    const QStringList lines = full_text.split('\n', Qt::SkipEmptyParts);
    m_header->setText(tr("CALLSTACK - %n FRAME(S)", "", lines.size()));

    // The frame the row is attributed to: the first one that is the game's own
    // code, since every allocation stack bottoms out in the allocator
    const int attributed = frame_class::attribution_enabled()
        ? frame_class::attribution_index(lines)
        : 0;

    QString html = QString("<div style='white-space:pre; font-family:\"%1\";'>").arg(theme::mono_family());
    for (int i = 0; i < lines.size(); ++i)
    {
        const QString raw = lines[i].trimmed();
        const frame_parts parts = split_frame(raw);
        const bool runtime = frame_class::classify(raw) == frame_class::kind::runtime;

        // Runtime plumbing stays visible but recedes; the attributed frame is
        // marked, so it is obvious which line the table row was showing
        const bool marked = (i == attributed);
        const QString method_color = marked ? t.accent_text.name()
                                            : (runtime ? t.text_dim.name() : t.text.name());
        const QString prefix_color = runtime ? t.text_faint.name() : t.text_dim.name();
        const QString marker = marked
            ? QString("<span style='color:%1'>&#9656;&nbsp;</span>").arg(t.accent.name())
            : QString("&nbsp;&nbsp;");

        html += QString("<div>%1<span style='color:%2'>%3</span>"
                        "<span style='color:%4'>%5</span>"
                        "<span style='color:%6'>%7</span></div>")
            .arg(marker,
                 prefix_color, escape(parts.dim_prefix),
                 method_color, escape(parts.method),
                 t.text_faint.name(), escape(parts.dim_suffix));
    }
    html += "</div>";
    m_frames->setHtml(html);

    m_stats->setText(tr("%1 live objects - %2")
        .arg(QLocale().toString((qulonglong)objects), size_to_string((double)bytes)));

    m_find_references->setEnabled(true);
    m_copy->setEnabled(true);
}

void callstack_inspector::copyToClipboard()
{
    if (m_plain_text.isEmpty())
        return;

    QGuiApplication::clipboard()->setText(m_plain_text);
}
