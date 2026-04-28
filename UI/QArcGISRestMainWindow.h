#pragma once
#include <QMainWindow>
#include <QString>

class QAction;
class QCheckBox;
class QLabel;
class QMenu;
class QPushButton;
class QToolButton;
class QWidget;
class QMainCanvas;
class QServiceBrowserPanel;
class QLayerManagerPanel;
class GB_Point2d;
class GB_Rectangle;

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
	QCheckBox* longitudeLatitudeStatusCheckBox = nullptr;
	QLabel* mousePositionStatusLabel = nullptr;
	QLabel* viewExtentStatusLabel = nullptr;
	QPushButton* crsStatusButton = nullptr;
	QToolButton* crsValidAreaVisibleButton = nullptr;
	QWidget* leftStatusWidget = nullptr;
	QWidget* rightStatusWidget = nullptr;

	void InitializeUi();
	void InitializeStatusBar();
	void UpdateMousePositionStatus(const GB_Point2d& position, bool hasPosition);
	void UpdateViewExtentStatus(const GB_Rectangle& extent);
	void UpdateCrsStatus(const QString& crsDisplayText);
	void UpdateCrsValidAreaVisibleButtonTooltip();
	void BalanceStatusSideWidgetWidths();

	void OnLongitudeLatitudeCheckBoxStateChanged(int checkState);
	void OnCrsStatusButtonClicked();
	void OnCrsValidAreaVisibleButtonToggled(bool checked);
};



