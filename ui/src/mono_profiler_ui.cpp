#include "mono_profiler_ui.h"
#include "main_window.h"

#include <qapplication.h>

int main(int argc, char **argv)
{
	QApplication app(argc, argv);

	QCoreApplication::setOrganizationName("Owlcat Games");
	QCoreApplication::setOrganizationDomain("owlcatgames.com");
	QCoreApplication::setApplicationName("Owlcat Mono Profiler");

	main_window wnd;
	wnd.setWindowState(Qt::WindowMaximized);
	wnd.show();

	return app.exec();
}
