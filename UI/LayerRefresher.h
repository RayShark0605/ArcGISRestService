#pragma once

#include <QObject>
#include <QPointer>

#include <string>

class QMainCanvas;
class QServiceBrowserPanel;
struct LayerImportRequestInfo;

class LayerRefresher : public QObject
{
	Q_OBJECT

public:
	static LayerRefresher* GetInstance();
	void SetCanvasAndPanel(QMainCanvas* canvas, QServiceBrowserPanel* panel);

private:
	class ArcGISRestLayerImportThread;
	class ArcGISRestImageDownloadThread;

	QPointer<QMainCanvas> mainCanvas;
	QPointer<QServiceBrowserPanel> serviceBrowserPanel;

	LayerRefresher();
	virtual ~LayerRefresher() override;

	bool SetCanvasCrsIfNeeded(const std::string& wkt) const;
	void OnArcGISRestLayerImportRequested(const LayerImportRequestInfo& request);
};
