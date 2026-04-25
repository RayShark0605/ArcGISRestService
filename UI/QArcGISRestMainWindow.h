#pragma once
#include <QMainWindow>
#include <QString>


class QAction;
class QLabel;
class QMenu;
class QMainCanvas;
class QServiceBrowserPanel;

class QArcGISRestMainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit QArcGISRestMainWindow(QWidget* parent = nullptr);
	virtual ~QArcGISRestMainWindow() override;


private:
	QMainCanvas* mainCanvas = nullptr;
	QServiceBrowserPanel* serviceBrowserPanel = nullptr;

	void InitializeUi();
};



