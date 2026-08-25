#include "mono_profiler_ui.h"
#include "main_window.h"
#include "theme.h"
#include "icons.h"

#include <qapplication.h>

int main(int argc, char **argv)
{
	// Must be set before the QApplication exists
	QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

	QApplication app(argc, argv);

	QCoreApplication::setOrganizationName("Owlcat Games");
	QCoreApplication::setOrganizationDomain("owlcatgames.com");
	QCoreApplication::setApplicationName("Owlcat Mono Profiler");

	// Style, palette and stylesheet all come from one token table (theme.cpp);
	// QSettings needs the organization/application names above, so this must
	// run after them
	theme::apply(theme::preferred());
	app.setWindowIcon(icons::app());

	main_window wnd;
	wnd.setWindowState(Qt::WindowMaximized);
	wnd.show();

	return app.exec();
}
