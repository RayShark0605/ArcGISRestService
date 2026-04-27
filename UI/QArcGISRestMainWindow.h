#pragma once
#include <QMainWindow>
#include <QString>


class QAction;
class QLabel;
class QMenu;
class QMainCanvas;
class QServiceBrowserPanel;
class QLayerManagerPanel;

class QArcGISRestMainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit QArcGISRestMainWindow(QWidget* parent = nullptr);
	virtual ~QArcGISRestMainWindow() override;

	QMainCanvas* GetCanvas() const;

private:
	QMainCanvas* mainCanvas = nullptr;
	QServiceBrowserPanel* serviceBrowserPanel = nullptr;
	QLayerManagerPanel* layerManagerPanel = nullptr;

	void InitializeUi();
};



