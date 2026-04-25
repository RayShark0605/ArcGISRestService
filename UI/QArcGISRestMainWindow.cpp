#include "QArcGISRestMainWindow.h"
#include "QMainCanvas.h"
#include "QServiceBrowserPanel.h"
#include "MapRefresher.h"

QArcGISRestMainWindow::QArcGISRestMainWindow(QWidget* parent) : QMainWindow(parent)
{
	InitializeUi();

	MapRefresher* refresher = MapRefresher::GetInstance();
	if (refresher)
	{
		refresher->SetCanvasAndPanel(mainCanvas, serviceBrowserPanel);
	}
}

void QArcGISRestMainWindow::InitializeUi()
{
	setObjectName(QStringLiteral("QArcGISRestMainWindow"));
	setWindowTitle(QStringLiteral("MapWeaver"));
	resize(1280, 800);

	setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);

	mainCanvas = new QMainCanvas(this);
	mainCanvas->setObjectName(QStringLiteral("mainCanvas"));
	setCentralWidget(mainCanvas);

	serviceBrowserPanel = new QServiceBrowserPanel(this);
	serviceBrowserPanel->setObjectName(QStringLiteral("serviceBrowserPanel"));
	serviceBrowserPanel->setMinimumWidth(260);
	serviceBrowserPanel->BindMainCanvas(mainCanvas);
	addDockWidget(QServiceBrowserPanel::GetDefaultDockWidgetArea(), serviceBrowserPanel);
}

QArcGISRestMainWindow::~QArcGISRestMainWindow()
{
}