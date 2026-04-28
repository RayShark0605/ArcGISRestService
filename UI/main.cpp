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

//#include "QMainCanvas.h"
//static QMainCanvas* g_mainCanvas = nullptr;
//
//void ThreadFunc()
//{
//	std::this_thread::sleep_for(std::chrono::seconds(5));
//	if (g_mainCanvas)
//	{
//		g_mainCanvas->SetCrsWkt(GB_ToWkt("EPSG:3349"));
//		std::this_thread::sleep_for(std::chrono::seconds(3));
//		g_mainCanvas->SetCrsValidAreaVisible(true);
//	}
//}
//
//int main(int argc, char* argv[])
//{
//	QApplication app(argc, argv);
//	QArcGISRestMainWindow mainWindow;
//	mainWindow.show();
//	g_mainCanvas = mainWindow.GetCanvas();
//
//	std::thread workerThread(ThreadFunc);
//	workerThread.detach();
//	return app.exec();
//}