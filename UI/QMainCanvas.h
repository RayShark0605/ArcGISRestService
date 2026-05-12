#pragma once

#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QString>

#include <memory>
#include <string>
#include <vector>

#include "DataDef.h"
#include "GeoBase/CV/GB_ColorRGBA.h"
#include "GeoBase/Geometry/GB_Point2d.h"
#include "GeoBase/Geometry/GB_Rectangle.h"

Q_DECLARE_METATYPE(GB_Rectangle)
Q_DECLARE_METATYPE(GB_Point2d)

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QMouseEvent;
class QResizeEvent;
class QWheelEvent;

enum class QMainCanvasRenderBackend
{
	QPainter = 0,
	OpenGL
};

class QMainCanvas : public QOpenGLWidget, protected QOpenGLFunctions
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

	void SetRenderBackend(QMainCanvasRenderBackend backend);
	QMainCanvasRenderBackend GetRenderBackend() const;
	bool IsOpenGLRenderBackendUsable() const;

signals:
	void ViewStateChanged(const GB_Rectangle& extent, double approximateMetersPerPixel);
	void ViewExtentDisplayChanged(const GB_Rectangle& extent);
	void MousePositionChanged(const GB_Point2d& position, bool hasPosition);
	void CrsDisplayTextChanged(const QString& crsDisplayText);
	void LayerDropRequested(const QString& nodeUid, const QString& url, const QString& text, int nodeType);

protected:
	virtual void initializeGL() override;
	virtual void resizeGL(int width, int height) override;
	virtual void paintGL() override;
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
	class Impl;
	std::unique_ptr<Impl> impl;
};
