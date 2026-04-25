#include "QArcGISRestMainWindow.h"
#include "QMainCanvas.h"
#include "QServiceBrowserPanel.h"

QArcGISRestMainWindow::QArcGISRestMainWindow(QWidget* parent) : QMainWindow(parent)
{
	InitializeUi();
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
	addDockWidget(QServiceBrowserPanel::GetDefaultDockWidgetArea(), serviceBrowserPanel);
}

QArcGISRestMainWindow::~QArcGISRestMainWindow()
{
}