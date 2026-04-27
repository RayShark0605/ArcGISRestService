#include <QApplication>
#include "QArcGISRestMainWindow.h"
#include "GeoCrsManager.h"
#include <thread>

int main(int argc, char* argv[])
{
	QApplication app(argc, argv);
	QArcGISRestMainWindow mainWindow;
	mainWindow.show();
	return app.exec();
}





