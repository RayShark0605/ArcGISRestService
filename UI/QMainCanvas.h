#pragma once

#include <QImage>
#include <QPixmap>
#include <QPainterPath>
#include <QPoint>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "DataDef.h"
#include "GeoBase/Geometry/GB_Point2d.h"
#include "GeoBase/Geometry/GB_Rectangle.h"
#include "GeoBase/CV/GB_ColorRGBA.h"

Q_DECLARE_METATYPE(GB_Rectangle)
Q_DECLARE_METATYPE(GB_Point2d)
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QPainterPath;
class QResizeEvent;
class QWheelEvent;


struct QMainCanvasSpatialIndexNode
{
	GB_Rectangle extent;
	std::vector<size_t> drawableIndices;
	int firstChildIndex = -1;
};

struct QMainCanvasSpatialIndex
{
	GB_Rectangle extent;
	std::vector<QMainCanvasSpatialIndexNode> nodes;
	bool isValid = false;
};

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
	static const std::string& GetCrsValidAreaDrawableUid();

	void SetCrsWkt(const std::string& wktUtf8);
	const std::string& GetCrsWkt() const;
	QString GetCrsDisplayText() const;

	void SetClipMapTilesToCrsValidArea(bool enabled);
	bool IsClipMapTilesToCrsValidAreaEnabled() const;

	void SetCrsValidAreaVisible(bool visible, const GB_ColorRGBA& color = GB_ColorRGBA(255, 0, 0, 80));
	void HideCrsValidArea();
	bool GetCrsValidAreaVisible(GB_ColorRGBA& outColor) const;

	void SetViewCenter(double centerX, double centerY);
	void SetViewCenter(const GB_Point2d& center);
	GB_Point2d GetViewCenter() const;

	GB_Rectangle GetCurrentViewExtent() const;
	bool TryGetCurrentViewExtent(GB_Rectangle& outExtent) const;

	double GetPixelSize() const;

	void SetViewExtent(const GB_Rectangle& extent);
	void ZoomToExtent(const GB_Rectangle& extent, double marginRatio = 0.05);
	bool TryGetCrsValidAreaPolygonsExtent(GB_Rectangle& outExtent) const;
	bool ZoomToCrsValidArea(double marginRatio = 0.05);
	void ZoomFull();

	GB_Point2d WorldToScreen(const GB_Point2d& point) const;
	GB_Point2d ScreenToWorld(const GB_Point2d& point) const;
	bool TryGetCurrentMouseWorldPosition(GB_Point2d& outPosition) const;

	void AddMapTile(const MapTileDrawable& tile);
	void AddMapTile(const MapTileDrawable& tile, double layerNumber);
	void AddMapTiles(const std::vector<MapTileDrawable>& tiles);

	void AddPointDrawable(const PointDrawable& point);
	void AddPointDrawable(const PointDrawable& point, double layerNumber);
	void AddPointDrawable(PointDrawable&& point);
	void AddPointDrawable(PointDrawable&& point, double layerNumber);
	void AddPointDrawables(const std::vector<PointDrawable>& points);
	void AddPointDrawables(std::vector<PointDrawable>&& points);

	void AddPolylineDrawable(const PolylineDrawable& polyline);
	void AddPolylineDrawable(const PolylineDrawable& polyline, double layerNumber);
	void AddPolylineDrawable(PolylineDrawable&& polyline);
	void AddPolylineDrawable(PolylineDrawable&& polyline, double layerNumber);
	void AddPolylineDrawables(const std::vector<PolylineDrawable>& polylines);
	void AddPolylineDrawables(std::vector<PolylineDrawable>&& polylines);

	void AddPolygonDrawable(const PolygonDrawable& polygon);
	void AddPolygonDrawable(const PolygonDrawable& polygon, double layerNumber);
	void AddPolygonDrawable(PolygonDrawable&& polygon);
	void AddPolygonDrawable(PolygonDrawable&& polygon, double layerNumber);
	void AddPolygonDrawables(const std::vector<PolygonDrawable>& polygons);
	void AddPolygonDrawables(std::vector<PolygonDrawable>&& polygons);

	bool HasDrawables() const;

	void ClearDrawables();
	void RemoveDrawables(const std::vector<std::string>& drawablesUids);
	void SetDrawablesVisible(const std::vector<std::string>& drawablesUids, bool visible);
	void SetDrawablesLayerNumber(const std::vector<std::string>& drawablesUids, double layerNumber);

signals:
	void ViewStateChanged(const GB_Rectangle& extent, double approximateMetersPerPixel);
	void ViewExtentDisplayChanged(const GB_Rectangle& extent);
	void MousePositionChanged(const GB_Point2d& position, bool hasPosition);
	void CrsDisplayTextChanged(const QString& crsDisplayText);
	void LayerDropRequested(const QString& nodeUid, const QString& url, const QString& text, int nodeType);

protected:
	virtual void paintEvent(QPaintEvent* event) override;
	virtual void resizeEvent(QResizeEvent* event) override;
	virtual void mousePressEvent(QMouseEvent* event) override;
	virtual void mouseMoveEvent(QMouseEvent* event) override;
	virtual void mouseReleaseEvent(QMouseEvent* event) override;
	virtual void wheelEvent(QWheelEvent* event) override;
	virtual void leaveEvent(QEvent* event) override;
	virtual void dragEnterEvent(QDragEnterEvent* event) override;
	virtual void dragMoveEvent(QDragMoveEvent* event) override;
	virtual void dropEvent(QDropEvent* event) override;

private:
	struct CachedMapTile
	{
		MapTileDrawable tile;
		QPixmap pixmap;
		double tileExtentWidth = 0.0;
		double tileExtentHeight = 0.0;
		double inverseTileExtentWidth = 0.0;
		double inverseTileExtentHeight = 0.0;
		double pixmapWidth = 0.0;
		double pixmapHeight = 0.0;
		std::uint64_t insertionSequence = 0;
	};

	struct CachedPointDrawable
	{
		PointDrawable point;
		GB_Rectangle extent;
		double screenMarginPixels = 0.0;
		std::uint64_t insertionSequence = 0;
	};

	struct CachedPolylineDrawable
	{
		PolylineDrawable polyline;
		GB_Rectangle extent;
		double screenMarginPixels = 0.0;
		std::uint64_t insertionSequence = 0;
	};

	struct CachedPolygonDrawable
	{
		PolygonDrawable polygon;
		GB_Rectangle extent;
		double screenMarginPixels = 0.0;
		std::uint64_t insertionSequence = 0;
	};

	enum class CachedDrawableKind
	{
		MapTile = 0,
		Point,
		Polyline,
		Polygon
	};

	struct VisibleDrawableReference
	{
		CachedDrawableKind kind = CachedDrawableKind::MapTile;
		size_t index = 0;
	};

	std::string crsWkt = "";
	QString crsDisplayText;
	GB_Rectangle viewExtent;
	double pixelSize = 1;
	bool clipMapTilesToCrsValidArea = false;
	bool crsValidAreaVisible = false;
	GB_ColorRGBA crsValidAreaColor = GB_ColorRGBA(255, 0, 0, 80);

	mutable std::vector<std::vector<GB_Point2d>> crsValidAreaPolygonsCache;
	mutable std::vector<GB_Rectangle> crsValidAreaPolygonExtentsCache;
	mutable bool crsValidAreaPolygonsCacheDirty = true;
	mutable std::string crsValidAreaPolygonsCacheCrsWkt;

	mutable GB_Rectangle crsValidAreaRectCache;
	mutable bool crsValidAreaRectCacheDirty = true;
	mutable std::string crsValidAreaRectCacheCrsWkt;

	mutable QPainterPath crsValidAreaScreenPathCache;
	mutable GB_Rectangle crsValidAreaScreenPathWorldExtentCache;
	mutable bool crsValidAreaScreenPathCacheDirty = true;
	mutable GB_Rectangle crsValidAreaScreenPathCacheViewExtent;
	mutable double crsValidAreaScreenPathCachePixelSize = 0.0;
	mutable std::string crsValidAreaScreenPathCacheCrsWkt;

	mutable bool crsMetersPerUnitCacheDirty = true;
	mutable std::string crsMetersPerUnitCacheCrsWkt;
	mutable double crsMetersPerUnitCache = 0.0;

	std::vector<CachedMapTile> mapTiles;
	std::vector<CachedPointDrawable> pointDrawables;
	std::vector<CachedPolylineDrawable> polylineDrawables;
	std::vector<CachedPolygonDrawable> polygonDrawables;
	std::uint64_t nextDrawableInsertionSequence = 0;

	mutable bool mapTileSpatialIndexDirty = true;
	mutable std::vector<size_t> mapTileMinXOrderCache;
	mutable std::vector<size_t> mapTileMaxXOrderCache;
	mutable std::vector<size_t> mapTileVisibleIndexScratch;

	mutable bool pointDrawableSpatialIndexDirty = true;
	mutable QMainCanvasSpatialIndex pointDrawableSpatialIndexCache;
	mutable std::vector<size_t> pointDrawableVisibleIndexScratch;

	mutable bool polylineDrawableSpatialIndexDirty = true;
	mutable QMainCanvasSpatialIndex polylineDrawableSpatialIndexCache;
	mutable std::vector<size_t> polylineDrawableVisibleIndexScratch;

	mutable bool polygonDrawableSpatialIndexDirty = true;
	mutable QMainCanvasSpatialIndex polygonDrawableSpatialIndexCache;
	mutable std::vector<size_t> polygonDrawableVisibleIndexScratch;

	mutable std::vector<VisibleDrawableReference> visibleDrawableReferenceScratch;

	mutable bool pointDrawableMaxScreenMarginCacheDirty = true;
	mutable double pointDrawableMaxScreenMarginCache = 0.0;
	mutable bool polylineDrawableMaxScreenMarginCacheDirty = true;
	mutable double polylineDrawableMaxScreenMarginCache = 0.0;
	mutable bool polygonDrawableMaxScreenMarginCacheDirty = true;
	mutable double polygonDrawableMaxScreenMarginCache = 0.0;

	mutable bool allDrawableExtentCacheDirty = true;
	mutable GB_Rectangle allDrawableExtentCache;
	bool isPanning = false;
	QPoint lastPanPosition;
	bool hasMousePosition = false;
	QPoint lastMousePosition;

	QTimer viewStateChangedDebounceTimer;

	mutable QPixmap mapContentCache;
	mutable bool isMapContentCacheDirty = true;
	mutable GB_Rectangle mapContentCacheViewExtent;
	mutable double mapContentCachePixelSize = 0.0;
	mutable bool mapContentCacheClipMapTilesToCrsValidArea = false;
	mutable std::string mapContentCacheCrsWkt;
	mutable bool mapContentCacheCrsValidAreaVisible = false;
	mutable GB_ColorRGBA mapContentCacheCrsValidAreaColor = GB_ColorRGBA(255, 0, 0, 80);

	QPixmap panPreviewPixmap;
	bool hasPanPreview = false;
	GB_Rectangle panPreviewViewExtent;
	double panPreviewPixelSize = 0.0;
	bool hasPendingViewStateChanged = false;
	bool hasEmittedViewState = false;
	GB_Rectangle lastEmittedViewExtent;
	double lastEmittedApproximateMetersPerPixel = 0.0;

	QImage CreateQImageFromGBImage(const GB_Image& image) const;
	QPixmap CreateQPixmapFromGBImage(const GB_Image& image) const;
	static double NormalizeLayerNumber(double layerNumber);
	static bool IsTopLayerNumber(double layerNumber);
	static bool IsBottomLayerNumber(double layerNumber);
	static int GetLayerPaintOrderGroup(double layerNumber);
	static bool IsCachedDrawablePaintOrderLess(double firstLayerNumber, std::uint64_t firstInsertionSequence, double secondLayerNumber, std::uint64_t secondInsertionSequence);
	static bool IsCachedMapTilePaintOrderLess(const CachedMapTile& firstTile, const CachedMapTile& secondTile);
	bool IsVisibleDrawableReferencePaintOrderLess(const VisibleDrawableReference& firstReference, const VisibleDrawableReference& secondReference) const;
	double GetVisibleDrawableReferenceLayerNumber(const VisibleDrawableReference& reference) const;
	std::uint64_t GetVisibleDrawableReferenceInsertionSequence(const VisibleDrawableReference& reference) const;
	bool TryCreateCachedMapTile(const MapTileDrawable& tile, double layerNumber, CachedMapTile& outCachedTile);
	bool TryCreateCachedPointDrawable(PointDrawable point, double layerNumber, CachedPointDrawable& outCachedPoint);
	bool TryCreateCachedPolylineDrawable(PolylineDrawable polyline, double layerNumber, CachedPolylineDrawable& outCachedPolyline);
	bool TryCreateCachedPolygonDrawable(PolygonDrawable polygon, double layerNumber, CachedPolygonDrawable& outCachedPolygon);
	void InsertCachedMapTile(CachedMapTile&& cachedTile);
	void EnsureMapTileSpatialIndex() const;
	void InvalidateMapTileSpatialIndex() const;
	void CollectVisibleMapTileIndices(const GB_Rectangle& queryWorldExtent, std::vector<size_t>& outIndices) const;
	void EnsurePointDrawableSpatialIndex() const;
	void InvalidatePointDrawableSpatialIndex() const;
	void CollectVisiblePointDrawableIndices(const GB_Rectangle& queryWorldExtent, std::vector<size_t>& outIndices) const;
	void EnsurePolylineDrawableSpatialIndex() const;
	void InvalidatePolylineDrawableSpatialIndex() const;
	void CollectVisiblePolylineDrawableIndices(const GB_Rectangle& queryWorldExtent, std::vector<size_t>& outIndices) const;
	void EnsurePolygonDrawableSpatialIndex() const;
	void InvalidatePolygonDrawableSpatialIndex() const;
	void CollectVisiblePolygonDrawableIndices(const GB_Rectangle& queryWorldExtent, std::vector<size_t>& outIndices) const;
	void InvalidateVectorDrawableSpatialIndexes() const;
	void InvalidateVectorDrawableMaxScreenMarginCaches() const;
	double GetMaxPointDrawableScreenMarginPixels() const;
	double GetMaxPolylineDrawableScreenMarginPixels() const;
	double GetMaxPolygonDrawableScreenMarginPixels() const;
	void DrawBackground(QPainter& painter) const;
	void DrawDrawables(QPainter& painter, const QRectF& exposedRect) const;
	void DrawCachedMapTile(QPainter& painter, const CachedMapTile& cachedTile, const GB_Rectangle& queryWorldExtent, const QRectF& exposedRect, const QPainterPath* crsClipPath, bool hasPreciseCrsClip, bool canUseCrsPolygonExtents) const;
	void DrawCachedPointDrawable(QPainter& painter, const CachedPointDrawable& cachedPoint, const GB_Rectangle& queryWorldExtent, const QRectF& exposedRect) const;
	void DrawCachedPolylineDrawable(QPainter& painter, const CachedPolylineDrawable& cachedPolyline, const GB_Rectangle& queryWorldExtent, const QRectF& exposedRect) const;
	void DrawCachedPolygonDrawable(QPainter& painter, const CachedPolygonDrawable& cachedPolygon, const GB_Rectangle& queryWorldExtent, const QRectF& exposedRect) const;
	void DrawVisibleDrawableReferencesExact(QPainter& painter, const QRectF& exposedRect, const GB_Rectangle& mapTileQueryWorldExtent, const GB_Rectangle& pointDrawQueryWorldExtent, const GB_Rectangle& polylineDrawQueryWorldExtent, const GB_Rectangle& polygonDrawQueryWorldExtent, const QPainterPath* crsClipPath, bool hasPreciseCrsClip, bool canUseCrsPolygonExtents) const;
	void DrawVectorDrawablesFastLod(QPainter& painter, const QRectF& exposedRect, const GB_Rectangle& pointDrawQueryWorldExtent, const GB_Rectangle& polylineDrawQueryWorldExtent, const GB_Rectangle& polygonDrawQueryWorldExtent, const std::vector<size_t>& pointIndices, const std::vector<size_t>& polylineIndices, const std::vector<size_t>& polygonIndices) const;
	void DrawCoordinateAxes(QPainter& painter) const;
	void DrawCrsValidArea(QPainter& painter) const;
	void DrawMapContent(QPainter& painter, const QRectF& exposedRect) const;
	void EnsureMapContentCache() const;
	void InvalidateMapContentCache() const;
	bool IsMapContentCacheValid() const;
	//void DrawVectorDrawables(QPainter& painter) const;
	//void DrawExtentMarkers(QPainter& painter) const;
	void UpdateCrsDisplayText();
	bool TryGetCrsValidArea(GB_Rectangle& outValidArea) const;
	bool TryGetCachedCrsValidAreaPolygonsExtent(GB_Rectangle& outExtent) const;
	bool TryBuildCrsValidAreaScreenPath(QPainterPath& outPath, GB_Rectangle& outWorldExtent) const;
	bool TryGetCrsValidAreaScreenPathCache(const QPainterPath*& outPath, GB_Rectangle& outWorldExtent) const;
	bool TryIntersectRectangleWithCrsValidAreaPolygons(const GB_Rectangle& rect, GB_Rectangle& outIntersectionExtent) const;
	bool IsRectangleIntersectsCachedCrsValidAreaPolygonExtent(const GB_Rectangle& rect) const;
	bool EnsureCrsValidAreaPolygonsCache() const;
	void InvalidateCrsValidAreaPolygonsCache() const;
	void InvalidateCrsValidAreaRectCache() const;
	void InvalidateCrsValidAreaScreenPathCache() const;
	double GetCrsMetersPerUnitCached() const;
	void InvalidateCrsMetersPerUnitCache() const;

	bool IsDrawableUidInSet(const std::vector<std::string>& drawablesUids, const std::string& uid) const;
	bool HasVisibleMapTileIntersectingExtent(const GB_Rectangle& extent) const;
	GB_Rectangle CalculateVisibleVectorDrawableExtent() const;
	GB_Rectangle CalculateAllDrawableExtent() const;
	void InvalidateAllDrawableExtentCache() const;

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
	void EmitViewExtentDisplayChangedIfNeeded(const GB_Rectangle& previousExtent);
	bool SetMousePosition(const QPoint& position);
	bool ClearMousePosition();
	void EmitMousePositionChanged();
	void ScheduleViewStateChangedWithDelay(int debounceIntervalMs);
	void ScheduleWheelZoomViewStateChanged();
	void FlushPendingViewStateChanged();
	void EmitViewStateChanged();
	bool ShouldEmitViewStateChanged(double approximateMetersPerPixel) const;
	double CalculateApproximateMetersPerPixel() const;
	QRectF WorldRectangleToScreenRectangle(const GB_Rectangle& rect) const;
};

