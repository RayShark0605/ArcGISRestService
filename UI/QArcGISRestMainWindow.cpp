#include "QArcGISRestMainWindow.h"
#include "QMainCanvas.h"
#include "QServiceBrowserPanel.h"
#include "LayerRefresher.h"
#include "QLayerManagerPanel.h"

QArcGISRestMainWindow::QArcGISRestMainWindow(QWidget* parent) : QMainWindow(parent)
{
	InitializeUi();

	LayerRefresher* refresher = LayerRefresher::GetInstance();
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

	layerManagerPanel = new QLayerManagerPanel(this);
	layerManagerPanel->setObjectName(QStringLiteral("layerManagerPanel"));
	layerManagerPanel->setMinimumWidth(260);
	layerManagerPanel->BindServiceBrowserPanel(serviceBrowserPanel);
	addDockWidget(QLayerManagerPanel::GetDefaultDockWidgetArea(), layerManagerPanel);
}

QArcGISRestMainWindow::~QArcGISRestMainWindow()
{
}