#pragma once

#include <QImage>
#include <QPoint>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <string>
#include <vector>

#include "DataDef.h"
#include "GeoBase/Geometry/GB_Point2d.h"
#include "GeoBase/Geometry/GB_Rectangle.h"

Q_DECLARE_METATYPE(GB_Rectangle)
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QResizeEvent;
class QWheelEvent;

class MapTile;
class QMainCanvas : public QWidget
{
	Q_OBJECT

public:
	explicit QMainCanvas(QWidget* parent = nullptr);
	virtual ~QMainCanvas() override;

	static QString GetServiceNodeMimeType();

	static double GetDefaultLayerNumber();
	static double GetTopLayerNumber();
	static double GetBottomLayerNumber();

	void SetCrsWkt(const std::string& wktUtf8);
	const std::string& GetCrsWkt() const;

	void SetClipMapTilesToCrsValidArea(bool enabled);
	bool IsClipMapTilesToCrsValidAreaEnabled() const;

	void SetViewCenter(double centerX, double centerY);
	void SetViewCenter(const GB_Point2d& center);
	GB_Point2d GetViewCenter() const;

	GB_Rectangle GetCurrentViewExtent() const;
	bool TryGetCurrentViewExtent(GB_Rectangle& outExtent) const;

	double GetPixelSize() const;

	void SetViewExtent(const GB_Rectangle& extent);
	void ZoomToExtent(const GB_Rectangle& extent, double marginRatio = 0.05);
	void ZoomFull();

	GB_Point2d WorldToScreen(const GB_Point2d& point) const;
	GB_Point2d ScreenToWorld(const GB_Point2d& point) const;

	void AddMapTile(const MapTile& tile);
	void AddMapTile(const MapTile& tile, double layerNumber);
	bool HasDrawables() const;

	void ClearDrawables();
	void RemoveDrawables(const std::vector<std::string>& drawablesUids);
	void SetDrawablesVisible(const std::vector<std::string>& drawablesUids, bool visible);
	void SetDrawablesLayerNumber(const std::vector<std::string>& drawablesUids, double layerNumber);

signals:
	void ViewStateChanged(const GB_Rectangle& extent, double approximateMetersPerPixel);
	void LayerDropRequested(const QString& nodeUid, const QString& url, const QString& text, int nodeType);

protected:
	virtual void paintEvent(QPaintEvent* event) override;
	virtual void resizeEvent(QResizeEvent* event) override;
	virtual void mousePressEvent(QMouseEvent* event) override;
	virtual void mouseMoveEvent(QMouseEvent* event) override;
	virtual void mouseReleaseEvent(QMouseEvent* event) override;
	virtual void mouseDoubleClickEvent(QMouseEvent* event) override;
	virtual void wheelEvent(QWheelEvent* event) override;
	virtual void leaveEvent(QEvent* event) override;
	virtual void dragEnterEvent(QDragEnterEvent* event) override;
	virtual void dragMoveEvent(QDragMoveEvent* event) override;
	virtual void dropEvent(QDropEvent* event) override;

private:
	struct CachedMapTile
	{
		MapTile tile;
		QImage image;
		std::uint64_t insertionSequence = 0;
	};

	std::string crsWkt = "";
	QString crsDisplayText;
	GB_Rectangle viewExtent;
	double pixelSize = 1;
	bool clipMapTilesToCrsValidArea = false;

	std::vector<CachedMapTile> mapTiles;
	std::uint64_t nextDrawableInsertionSequence = 0;
	bool isPanning = false;
	QPoint lastPanPosition;
	bool hasMousePosition = false;
	QPoint lastMousePosition;

	QTimer viewStateChangedDebounceTimer;
	bool hasPendingViewStateChanged = false;
	bool hasEmittedViewState = false;
	GB_Rectangle lastEmittedViewExtent;
	double lastEmittedApproximateMetersPerPixel = 0.0;

	QImage CreateQImageFromGBImage(const GB_Image& image) const;
	static double NormalizeLayerNumber(double layerNumber);
	static bool IsTopLayerNumber(double layerNumber);
	static bool IsBottomLayerNumber(double layerNumber);
	static int GetLayerPaintOrderGroup(double layerNumber);
	static bool IsCachedMapTilePaintOrderLess(const CachedMapTile& firstTile, const CachedMapTile& secondTile);
	void InsertCachedMapTile(CachedMapTile&& cachedTile);
	void DrawBackground(QPainter& painter) const;
	void DrawMapTiles(QPainter& painter) const;
	void DrawCoordinateAxes(QPainter& painter) const;
	//void DrawVectorDrawables(QPainter& painter) const;
	//void DrawExtentMarkers(QPainter& painter) const;
	void DrawOverlay(QPainter& painter) const;
	void UpdateCrsDisplayText();
	bool TryGetCrsValidArea(GB_Rectangle& outValidArea) const;

	bool IsDrawableUidInSet(const std::vector<std::string>& drawablesUids, const std::string& uid) const;
	GB_Rectangle CalculateAllDrawableExtent() const;

	void SetViewExtentInternal(const GB_Rectangle& extent, bool emitSignal);
	double GetMinimumPixelSizeForCurrentWidget() const;
	double GetMaximumPixelSizeForCurrentWidget() const;
	double ClampPixelSizeForCurrentWidget(double targetPixelSize) const;
	GB_Point2d ClampViewCenterForPixelSize(const GB_Point2d& center, double targetPixelSize) const;
	void UpdatePixelSizeFromViewExtent();
	void UpdateViewExtentFromCenterAndPixelSize(const GB_Point2d& center);
	GB_Rectangle MakeExtentByCenterAndPixelSize(const GB_Point2d& center, double targetPixelSize) const;
	GB_Rectangle NormalizeExtentForCurrentWidget(const GB_Rectangle& extent) const;
	GB_Rectangle EnsureUsableExtent(const GB_Rectangle& extent) const;
	void ScheduleViewStateChanged();
	void ScheduleViewStateChangedWithDelay(int debounceIntervalMs);
	void ScheduleWheelZoomViewStateChanged();
	void FlushPendingViewStateChanged();
	void EmitViewStateChanged();
	bool ShouldEmitViewStateChanged(double approximateMetersPerPixel) const;
	double CalculateApproximateMetersPerPixel() const;
	QRectF WorldRectangleToScreenRectangle(const GB_Rectangle& rect) const;
};

