#include "theme.h"

#include <qapplication.h>
#include <qstylefactory.h>
#include <qpalette.h>
#include <qsettings.h>
#include <qfontdatabase.h>

namespace
{
    theme::tokens_t make_dark()
    {
        theme::tokens_t t;
        t.dark             = true;
        t.window           = QColor("#14161a");
        t.panel            = QColor("#1b1e23");
        t.panel_alt        = QColor("#181b20");
        t.panel_hi         = QColor("#21252b");
        t.border           = QColor("#2c3138");
        t.border_strong    = QColor("#3a4149");
        t.text             = QColor("#e4e7ec");
        t.text_bright      = QColor("#f2f4f7");
        t.text_dim         = QColor("#8d95a1");
        t.text_faint       = QColor("#6d7580");
        t.accent           = QColor("#e0a33c");
        t.accent_bg        = QColor("#3a2f16");
        t.accent_border    = QColor("#6d5520");
        t.accent_text      = QColor("#f0c877");
        t.ok               = QColor("#6fbf85");
        t.warn             = QColor("#e0a33c");
        t.danger           = QColor("#c9645c");
        t.grid             = QColor("#262b32");
        t.plot_bg          = QColor("#16191d");
        t.series_tracked   = QColor("#e0a33c");
        t.series_committed = QColor("#c9645c");
        t.series_native    = QColor("#7ea5c4");
        t.series_allocs    = QColor("#8494e8");
        t.series_gc        = QColor("#6fbf85");
        t.selection_band   = QColor("#e0a33c");
        return t;
    }

    theme::tokens_t make_light()
    {
        theme::tokens_t t;
        t.dark             = false;
        t.window           = QColor("#fbfaf7");
        t.panel            = QColor("#ffffff");
        t.panel_alt        = QColor("#f6f4ef");
        t.panel_hi         = QColor("#eeebe4");
        t.border           = QColor("#e7e4dd");
        t.border_strong    = QColor("#ddd8cf");
        t.text             = QColor("#1b1a18");
        t.text_bright      = QColor("#000000");
        t.text_dim         = QColor("#6f6b64");
        t.text_faint       = QColor("#a39c92");
        t.accent           = QColor("#4d6bb3");
        t.accent_bg        = QColor("#eef1f8");
        t.accent_border    = QColor("#cfd8ea");
        t.accent_text      = QColor("#33518f");
        t.ok               = QColor("#2f8464");
        t.warn             = QColor("#a56b52");
        t.danger           = QColor("#a53a2a");
        t.grid             = QColor("#efece5");
        t.plot_bg          = QColor("#ffffff");
        t.series_tracked   = QColor("#4d6bb3");
        t.series_committed = QColor("#a56b52");
        t.series_native    = QColor("#5f8f7a");
        t.series_allocs    = QColor("#8d9fc9");
        t.series_gc        = QColor("#2f8464");
        t.selection_band   = QColor("#4d6bb3");
        return t;
    }

    theme::tokens_t  g_tokens = make_dark();
    theme::mode      g_mode   = theme::mode::dark;

    QPalette make_palette(const theme::tokens_t& t)
    {
        QPalette p;
        p.setColor(QPalette::Window,          t.window);
        p.setColor(QPalette::WindowText,      t.text);
        p.setColor(QPalette::Base,            t.panel);
        p.setColor(QPalette::AlternateBase,   t.panel_alt);
        p.setColor(QPalette::Text,            t.text);
        p.setColor(QPalette::BrightText,      t.text_bright);
        p.setColor(QPalette::Button,          t.panel_hi);
        p.setColor(QPalette::ButtonText,      t.text);
        p.setColor(QPalette::ToolTipBase,     t.panel_hi);
        p.setColor(QPalette::ToolTipText,     t.text);
        p.setColor(QPalette::Highlight,       t.accent);
        p.setColor(QPalette::HighlightedText, t.dark ? QColor("#14161a") : QColor("#ffffff"));
        p.setColor(QPalette::Link,            t.accent);
        p.setColor(QPalette::LinkVisited,     t.accent);
        p.setColor(QPalette::Mid,             t.border);
        p.setColor(QPalette::Dark,            t.border_strong);
        p.setColor(QPalette::Shadow,          t.dark ? QColor("#0b0c0e") : QColor("#cfcbc2"));

        p.setColor(QPalette::Disabled, QPalette::Text,       t.text_faint);
        p.setColor(QPalette::Disabled, QPalette::WindowText, t.text_faint);
        p.setColor(QPalette::Disabled, QPalette::ButtonText, t.text_faint);
        p.setColor(QPalette::Disabled, QPalette::Base,       t.panel);
        p.setColor(QPalette::Disabled, QPalette::Highlight,  t.border);
        return p;
    }

    // One stylesheet, tokens substituted in. Nothing else in the app may set a
    // stylesheet on a widget - keep every colour here.
    QString make_qss(const theme::tokens_t& t)
    {
        static const char* tpl = R"QSS(
QWidget                 { font-size: 13px; }
QMainWindow, QDialog    { background: @window@; }
QToolTip                { background: @panel_hi@; color: @text@; border: 1px solid @border_strong@; padding: 3px 6px; }

QMenuBar                { background: @panel_alt@; color: @text@; border-bottom: 1px solid @border@; padding: 1px 4px; }
QMenuBar::item          { padding: 4px 9px; border-radius: 4px; background: transparent; }
QMenuBar::item:selected { background: @panel_hi@; }
QMenu                   { background: @panel@; color: @text@; border: 1px solid @border_strong@; padding: 5px; }
QMenu::item             { padding: 6px 22px 6px 12px; border-radius: 4px; }
QMenu::item:selected    { background: @panel_hi@; }
QMenu::item:disabled    { color: @text_faint@; }
QMenu::separator        { height: 1px; background: @border@; margin: 4px 6px; }
QMenu::indicator        { width: 14px; left: 6px; }

QToolBar                { background: @panel_alt@; border: none; border-bottom: 1px solid @border@;
                          spacing: 4px; padding: 4px 8px; }
QToolBar::separator     { width: 1px; background: @border@; margin: 3px 6px; }
QToolButton             { color: @text@; background: @panel_hi@; border: 1px solid @border_strong@;
                          border-radius: 4px; padding: 4px 9px; min-height: 18px; }
QToolButton:hover       { background: @hover@; border-color: @accent_border@; }
QToolButton:pressed     { background: @accent_bg@; }
QToolButton:checked     { background: @accent_bg@; border-color: @accent_border@; color: @accent_text@; }
QToolButton:disabled    { color: @text_faint@; background: transparent; border-color: @border@; }
QToolButton#primaryAction         { background: @accent_bg@; border-color: @accent_border@; color: @accent_text@; }
QToolButton#primaryAction:hover   { background: @accent_bg2@; }
QToolButton::menu-indicator       { image: none; width: 0px; }

QStatusBar              { background: @panel_alt@; color: @text_dim@; border-top: 1px solid @border@; }
QStatusBar::item        { border: none; }
QStatusBar QLabel       { color: @text_dim@; padding: 0 2px; }

QLineEdit               { background: @plot_bg@; color: @text@; border: 1px solid @border_strong@;
                          border-radius: 4px; padding: 4px 8px; selection-background-color: @accent@; }
QLineEdit:focus         { border-color: @accent@; }
QLineEdit:disabled      { color: @text_faint@; }

QPushButton             { background: @panel_hi@; color: @text@; border: 1px solid @border_strong@;
                          border-radius: 4px; padding: 5px 14px; min-height: 18px; }
QPushButton:hover       { background: @hover@; }
QPushButton:default     { background: @accent_bg@; border-color: @accent_border@; color: @accent_text@; }
QPushButton:disabled    { color: @text_faint@; background: transparent; }

QCheckBox               { color: @text@; spacing: 7px; }
QCheckBox::indicator    { width: 14px; height: 14px; border-radius: 3px;
                          border: 1px solid @border_strong@; background: @plot_bg@; }
QCheckBox::indicator:checked { background: @accent@; border-color: @accent@; }

QTabWidget::pane        { border: 1px solid @border@; border-radius: 5px; top: -1px; background: @panel@; }
QTabBar                 { background: transparent; }
QTabBar::tab            { background: transparent; color: @text_dim@; border: 1px solid transparent;
                          border-top-left-radius: 4px; border-top-right-radius: 4px; padding: 5px 14px; margin-right: 2px; }
QTabBar::tab:hover      { color: @text@; }
QTabBar::tab:selected   { background: @panel@; color: @text_bright@; border-color: @border@; border-bottom-color: @panel@; }

QTableView, QTreeView, QListWidget, QTableWidget
                        { background: @panel@; alternate-background-color: @panel@; color: @text@;
                          border: 1px solid @border@; border-radius: 5px;
                          gridline-color: transparent; outline: none;
                          selection-background-color: @sel_bg@; selection-color: @text_bright@; }
QTableView::item, QTreeView::item, QListWidget::item
                        { border-bottom: 1px solid @row_line@; padding: 2px 4px; }
QTableView::item:hover, QTreeView::item:hover, QListWidget::item:hover
                        { background: @hover@; }
QTableView::item:selected, QTreeView::item:selected, QListWidget::item:selected
                        { background: @sel_bg@; color: @text_bright@; }
QHeaderView             { background: transparent; }
QHeaderView::section    { background: @panel_alt@; color: @text_dim@; border: none;
                          border-bottom: 1px solid @border@; border-right: 1px solid @row_line@;
                          padding: 5px 8px; font-size: 11px; }
QHeaderView::section:hover { color: @text@; }
QHeaderView::down-arrow, QHeaderView::up-arrow { width: 9px; height: 9px; subcontrol-position: center right; right: 6px; }

QScrollBar:vertical     { background: transparent; width: 12px; margin: 0; }
QScrollBar:horizontal   { background: transparent; height: 12px; margin: 0; }
QScrollBar::handle:vertical   { background: @scroll@; min-height: 28px; border-radius: 5px; margin: 2px; }
QScrollBar::handle:horizontal { background: @scroll@; min-width: 28px; border-radius: 5px; margin: 2px; }
QScrollBar::handle:hover      { background: @scroll_hi@; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* The graph navigator is not a scrollbar you glance past - it is how you move
   through the capture, so it gets a visible track and an accent-tinted grip */
QScrollBar:horizontal#horizontalScrollBar
                        { background: @plot_bg@; border: 1px solid @border@; border-radius: 5px;
                          height: 16px; margin: 2px 0; }
QScrollBar::handle:horizontal#horizontalScrollBar
                        { background: @nav_grip@; border: 1px solid @nav_grip_border@;
                          border-radius: 4px; min-width: 44px; margin: 1px; }
QScrollBar::handle:horizontal#horizontalScrollBar:hover
                        { background: @nav_grip_hover@; border-color: @accent@; }

QSplitter::handle       { background: @window@; }
QSplitter::handle:horizontal { width: 7px; }
QSplitter::handle:vertical   { height: 7px; }
QSplitter::handle:hover { background: @accent_border@; }

QProgressBar            { background: @plot_bg@; border: 1px solid @border@; border-radius: 4px;
                          text-align: center; color: @text@; height: 16px; }
QProgressBar::chunk     { background: @accent@; border-radius: 3px; }

QFrame[frameShape="4"], QFrame[frameShape="5"] { color: @border@; }
)QSS";

        const auto hex = [](const QColor& c) { return c.name(QColor::HexRgb); };
        QColor hover      = t.dark ? t.panel_hi.lighter(125) : t.panel_hi.darker(103);
        QColor accent_bg2 = t.dark ? t.accent_bg.lighter(130) : t.accent_bg.darker(104);
        QColor sel_bg     = t.dark ? QColor("#2b3138") : QColor("#e6ebf6");
        QColor row_line   = t.dark ? QColor("#22262c") : QColor("#f2efe9");
        QColor scroll     = t.dark ? QColor("#3d444e") : QColor("#c9c3b8");
        QColor scroll_hi  = t.dark ? QColor("#525a66") : QColor("#b0a99c");

        // Accent blended towards the panel: clearly the active control, without
        // shouting louder than the curves it sits under
        const auto blend = [](const QColor& a, const QColor& b, double k)
        {
            return QColor((int)(a.red()   * (1 - k) + b.red()   * k),
                          (int)(a.green() * (1 - k) + b.green() * k),
                          (int)(a.blue()  * (1 - k) + b.blue()  * k));
        };
        QColor nav_grip        = blend(t.panel_hi, t.accent, t.dark ? 0.45 : 0.30);
        QColor nav_grip_border = blend(t.panel_hi, t.accent, t.dark ? 0.75 : 0.55);
        QColor nav_grip_hover  = blend(t.panel_hi, t.accent, t.dark ? 0.70 : 0.45);

        QString qss = QString::fromUtf8(tpl);
        qss.replace("@window@",           hex(t.window));
        qss.replace("@panel_alt@",        hex(t.panel_alt));
        qss.replace("@panel_hi@",         hex(t.panel_hi));
        qss.replace("@panel@",            hex(t.panel));
        qss.replace("@border_strong@",    hex(t.border_strong));
        qss.replace("@border@",           hex(t.border));
        qss.replace("@text_bright@",      hex(t.text_bright));
        qss.replace("@text_dim@",         hex(t.text_dim));
        qss.replace("@text_faint@",       hex(t.text_faint));
        qss.replace("@text@",             hex(t.text));
        qss.replace("@accent_bg2@",       hex(accent_bg2));
        qss.replace("@accent_bg@",        hex(t.accent_bg));
        qss.replace("@accent_border@",    hex(t.accent_border));
        qss.replace("@accent_text@",      hex(t.accent_text));
        qss.replace("@accent@",           hex(t.accent));
        qss.replace("@plot_bg@",          hex(t.plot_bg));
        qss.replace("@hover@",            hex(hover));
        qss.replace("@sel_bg@",           hex(sel_bg));
        qss.replace("@row_line@",         hex(row_line));
        qss.replace("@scroll_hi@",        hex(scroll_hi));
        qss.replace("@scroll@",           hex(scroll));
        qss.replace("@nav_grip_border@",  hex(nav_grip_border));
        qss.replace("@nav_grip_hover@",   hex(nav_grip_hover));
        qss.replace("@nav_grip@",         hex(nav_grip));
        return qss;
    }
}

namespace theme
{
    const tokens_t& tokens()
    {
        return g_tokens;
    }

    void apply(mode m)
    {
        g_mode = m;
        g_tokens = (m == mode::dark) ? make_dark() : make_light();

        if (qApp == nullptr)
            return;

        // Fusion is the only Qt 5 style that honours a custom palette everywhere,
        // including the frame Qwt draws around its canvas
        qApp->setStyle(QStyleFactory::create("Fusion"));
        qApp->setPalette(make_palette(g_tokens));
        qApp->setStyleSheet(make_qss(g_tokens));
    }

    mode current()
    {
        return g_mode;
    }

    QString preference()
    {
        return QSettings().value("ui/theme", "system").toString();
    }

    mode preferred()
    {
        const QString pref = preference();
        if (pref == "dark")
            return mode::dark;
        if (pref == "light")
            return mode::light;

#ifdef Q_OS_WIN
        // 0 = dark, 1 = light; absent on older Windows, where light is the safe guess
        QSettings personalize("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                              QSettings::NativeFormat);
        const QVariant apps_light = personalize.value("AppsUseLightTheme");
        if (apps_light.isValid())
            return apps_light.toInt() == 0 ? mode::dark : mode::light;
#endif
        return mode::dark;
    }

    void set_preference(const QString& pref)
    {
        QSettings().setValue("ui/theme", pref);
        apply(preferred());
    }

    QString mono_family()
    {
        static QString family;
        if (!family.isEmpty())
            return family;

        const QStringList wanted = { "Cascadia Mono", "Consolas", "JetBrains Mono", "IBM Plex Mono", "Courier New" };
        QFontDatabase db;
        for (const auto& candidate : wanted)
        {
            if (db.families().contains(candidate))
            {
                family = candidate;
                return family;
            }
        }
        family = "monospace";
        return family;
    }

    QFont mono_font(int pixel_size_delta)
    {
        QFont f(mono_family());
        f.setStyleHint(QFont::Monospace);
        // Tabular figures, so columns of sizes and counts line up
        f.setStyleStrategy(QFont::PreferQuality);
        f.setPixelSize(qMax(9, 13 + pixel_size_delta));
        return f;
    }
}
