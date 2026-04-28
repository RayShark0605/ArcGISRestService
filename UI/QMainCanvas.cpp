#include "QMainCanvas.h"
#include "GeoBoundingBox.h"
#include "GeoCrs.h"
#include "GeoCrsManager.h"
#include "GeoBase/GB_Math.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPixmap>
#include <QPen>
#include <QPolygonF>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>

namespace
{
	constexpr double DefaultPixelSize = 1.0;
	constexpr double FallbackMinimumPixelSize = 1e-12;
	constexpr double FallbackMaximumPixelSize = 1e+18;
	constexpr double DefaultHalfExtentSize = 100.0;
	constexpr double MinimumCoordinateRange = 1e-8;
	constexpr double MaximumCoordinateAbsValue = 1e10;
	constexpr double FixedAxisPixelLength = 50.0;
	constexpr double DefaultLayerNumberValue = 0.0;
	constexpr double TopLayerNumberValue = -1.0;
	constexpr double BottomLayerNumberValue = static_cast<double>(GB_IntMax / 2);
	constexpr int ViewStateChangedDebounceIntervalMs = 180;
	constexpr int WheelZoomEndDetectionIntervalMs = 300;
	constexpr int CrsValidAreaPolygonEdgeSampleCount = 129;
	const std::string CrsValidAreaDrawableUid = "__QMainCanvas_CrsValidArea__";

	QColor ToQColor(const GB_ColorRGBA& color)
	{
		return QColor(static_cast<int>(color.r), static_cast<int>(color.g), static_cast<int>(color.b), static_cast<int>(color.a));
	}

	bool IsPointFinite(const GB_Point2d& point)
	{
		return std::isfinite(point.x) && std::isfinite(point.y);
	}

	enum class ClipBoundary
	{
		Left,
		Right,
		Bottom,
		Top
	};

	bool IsPointInsideClipBoundary(const GB_Point2d& point, const GB_Rectangle& clipRect, ClipBoundary boundary)
	{
		switch (boundary)
		{
		case ClipBoundary::Left:
			return point.x >= clipRect.minX - GB_Epsilon;
		case ClipBoundary::Right:
			return point.x <= clipRect.maxX + GB_Epsilon;
		case ClipBoundary::Bottom:
			return point.y >= clipRect.minY - GB_Epsilon;
		case ClipBoundary::Top:
			return point.y <= clipRect.maxY + GB_Epsilon;
		default:
			return false;
		}
	}

	bool TryGetClipBoundaryIntersection(const GB_Point2d& startPoint, const GB_Point2d& endPoint, const GB_Rectangle& clipRect, ClipBoundary boundary, GB_Point2d& outPoint)
	{
		outPoint = GB_Point2d(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN());

		const double deltaX = endPoint.x - startPoint.x;
		const double deltaY = endPoint.y - startPoint.y;
		double t = std::numeric_limits<double>::quiet_NaN();

		switch (boundary)
		{
		case ClipBoundary::Left:
			if (std::abs(deltaX) <= GB_Epsilon)
			{
				return false;
			}
			t = (clipRect.minX - startPoint.x) / deltaX;
			outPoint.Set(clipRect.minX, startPoint.y + deltaY * t);
			break;
		case ClipBoundary::Right:
			if (std::abs(deltaX) <= GB_Epsilon)
			{
				return false;
			}
			t = (clipRect.maxX - startPoint.x) / deltaX;
			outPoint.Set(clipRect.maxX, startPoint.y + deltaY * t);
			break;
		case ClipBoundary::Bottom:
			if (std::abs(deltaY) <= GB_Epsilon)
			{
				return false;
			}
			t = (clipRect.minY - startPoint.y) / deltaY;
			outPoint.Set(startPoint.x + deltaX * t, clipRect.minY);
			break;
		case ClipBoundary::Top:
			if (std::abs(deltaY) <= GB_Epsilon)
			{
				return false;
			}
			t = (clipRect.maxY - startPoint.y) / deltaY;
			outPoint.Set(startPoint.x + deltaX * t, clipRect.maxY);
			break;
		default:
			return false;
		}

		return t >= -GB_Epsilon && t <= 1.0 + GB_Epsilon && IsPointFinite(outPoint);
	}

	void AppendPointIfUseful(std::vector<GB_Point2d>& points, const GB_Point2d& point)
	{
		if (!IsPointFinite(point))
		{
			return;
		}

		if (!points.empty() && points.back().IsNearEqual(point, GB_Epsilon))
		{
			return;
		}

		points.push_back(point);
	}

	std::vector<GB_Point2d> ClipPolygonByBoundary(const std::vector<GB_Point2d>& sourcePolygon, const GB_Rectangle& clipRect, ClipBoundary boundary)
	{
		std::vector<GB_Point2d> result;
		if (sourcePolygon.empty())
		{
			return result;
		}

		result.reserve(sourcePolygon.size() + 4);
		GB_Point2d previousPoint = sourcePolygon.back();
		bool previousInside = IsPointInsideClipBoundary(previousPoint, clipRect, boundary);

		for (const GB_Point2d& currentPoint : sourcePolygon)
		{
			const bool currentInside = IsPointInsideClipBoundary(currentPoint, clipRect, boundary);
			if (currentInside != previousInside)
			{
				GB_Point2d intersectionPoint;
				if (TryGetClipBoundaryIntersection(previousPoint, currentPoint, clipRect, boundary, intersectionPoint))
				{
					AppendPointIfUseful(result, intersectionPoint);
				}
			}

			if (currentInside)
			{
				AppendPointIfUseful(result, currentPoint);
			}

			previousPoint = currentPoint;
			previousInside = currentInside;
		}

		if (result.size() > 1 && result.front().IsNearEqual(result.back(), GB_Epsilon))
		{
			result.pop_back();
		}

		return result;
	}

	std::vector<GB_Point2d> ClipPolygonToRectangle(const std::vector<GB_Point2d>& sourcePolygon, const GB_Rectangle& clipRect)
	{
		std::vector<GB_Point2d> workingPolygon;
		workingPolygon.reserve(sourcePolygon.size());
		for (const GB_Point2d& point : sourcePolygon)
		{
			if (!IsPointFinite(point))
			{
				return std::vector<GB_Point2d>();
			}

			AppendPointIfUseful(workingPolygon, point);
		}

		if (workingPolygon.size() > 1 && workingPolygon.front().IsNearEqual(workingPolygon.back(), GB_Epsilon))
		{
			workingPolygon.pop_back();
		}

		if (workingPolygon.size() < 3)
		{
			return std::vector<GB_Point2d>();
		}

		workingPolygon = ClipPolygonByBoundary(workingPolygon, clipRect, ClipBoundary::Left);
		workingPolygon = ClipPolygonByBoundary(workingPolygon, clipRect, ClipBoundary::Right);
		workingPolygon = ClipPolygonByBoundary(workingPolygon, clipRect, ClipBoundary::Bottom);
		workingPolygon = ClipPolygonByBoundary(workingPolygon, clipRect, ClipBoundary::Top);

		if (workingPolygon.size() > 1 && workingPolygon.front().IsNearEqual(workingPolygon.back(), GB_Epsilon))
		{
			workingPolygon.pop_back();
		}

		if (workingPolygon.size() < 3)
		{
			return std::vector<GB_Point2d>();
		}

		return workingPolygon;
	}

	bool IsFinitePositive(double value)
	{
		return std::isfinite(value) && value > 0;
	}

	bool IsFiniteNonNegative(double value)
	{
		return std::isfinite(value) && value >= 0;
	}

	bool AreNearlyEqual(double firstValue, double secondValue)
	{
		if (std::isnan(firstValue) || std::isnan(secondValue))
		{
			return std::isnan(firstValue) && std::isnan(secondValue);
		}

		if (!std::isfinite(firstValue) || !std::isfinite(secondValue))
		{
			return firstValue == secondValue;
		}

		const double scale = std::max(1.0, std::max(std::abs(firstValue), std::abs(secondValue)));
		return std::abs(firstValue - secondValue) <= scale * 1e-12;
	}

	bool AreRectanglesNearlyEqual(const GB_Rectangle& firstRect, const GB_Rectangle& secondRect)
	{
		if (!firstRect.IsValid() || !secondRect.IsValid())
		{
			return !firstRect.IsValid() && !secondRect.IsValid();
		}

		return AreNearlyEqual(firstRect.minX, secondRect.minX) &&
			AreNearlyEqual(firstRect.minY, secondRect.minY) &&
			AreNearlyEqual(firstRect.maxX, secondRect.maxX) &&
			AreNearlyEqual(firstRect.maxY, secondRect.maxY);
	}


	int SafeWidgetWidth(const QWidget* widget)
	{
		if (!widget)
		{
			return 1;
		}
		return std::max(1, widget->width());
	}

	int SafeWidgetHeight(const QWidget* widget)
	{
		if (!widget)
		{
			return 1;
		}
		return std::max(1, widget->height());
	}

	QByteArray ReadJsonText(const QMimeData* mimeData, const char* key)
	{
		if (!mimeData || !mimeData->hasFormat(QMainCanvas::GetServiceNodeMimeType()))
		{
			return QByteArray();
		}

		const QByteArray jsonBytes = mimeData->data(QMainCanvas::GetServiceNodeMimeType());
		const QJsonDocument document = QJsonDocument::fromJson(jsonBytes);
		if (!document.isObject())
		{
			return QByteArray();
		}

		const QJsonObject object = document.object();
		const QJsonValue value = object.value(QString::fromLatin1(key));
		if (!value.isString())
		{
			return QByteArray();
		}

		return value.toString().toUtf8();
	}

	int ReadJsonInt(const QMimeData* mimeData, const char* key, int defaultValue)
	{
		if (!mimeData || !mimeData->hasFormat(QMainCanvas::GetServiceNodeMimeType()))
		{
			return defaultValue;
		}

		const QByteArray jsonBytes = mimeData->data(QMainCanvas::GetServiceNodeMimeType());
		const QJsonDocument document = QJsonDocument::fromJson(jsonBytes);
		if (!document.isObject())
		{
			return defaultValue;
		}

		const QJsonValue value = document.object().value(QString::fromLatin1(key));
		if (!value.isDouble())
		{
			return defaultValue;
		}
		return value.toInt(defaultValue);
	}
}

QMainCanvas::QMainCanvas(QWidget* parent) : QWidget(parent)
{
	qRegisterMetaType<GB_Rectangle>("GB_Rectangle");
	qRegisterMetaType<GB_Point2d>("GB_Point2d");

	setMinimumSize(400, 300);
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	setAcceptDrops(true);
	setAutoFillBackground(false);

	viewStateChangedDebounceTimer.setSingleShot(true);
	viewStateChangedDebounceTimer.setTimerType(Qt::PreciseTimer);
	viewStateChangedDebounceTimer.setInterval(ViewStateChangedDebounceIntervalMs);
	connect(&viewStateChangedDebounceTimer, &QTimer::timeout, this, [this]()
		{
			FlushPendingViewStateChanged();
		});

	UpdateCrsDisplayText();

	viewExtent.Set(-DefaultHalfExtentSize, -DefaultHalfExtentSize, DefaultHalfExtentSize, DefaultHalfExtentSize);
	UpdatePixelSizeFromViewExtent();
}

QMainCanvas::~QMainCanvas()
{
}

QString QMainCanvas::GetServiceNodeMimeType()
{
	return QStringLiteral("application/x-arcgis-rest-service-node");
}

double QMainCanvas::GetDefaultLayerNumber()
{
	return DefaultLayerNumberValue;
}

double QMainCanvas::GetTopLayerNumber()
{
	return TopLayerNumberValue;
}

double QMainCanvas::GetBottomLayerNumber()
{
	return BottomLayerNumberValue;
}

const std::string& QMainCanvas::GetCrsValidAreaDrawableUid()
{
	return CrsValidAreaDrawableUid;
}

void QMainCanvas::SetCrsWkt(const std::string& wktUtf8)
{
	if (crsWkt == wktUtf8)
	{
		return;
	}

	crsWkt = wktUtf8;
	UpdateCrsDisplayText();
	InvalidateCrsValidAreaPolygonsCache();
	InvalidateMapContentCache();
	hasPanPreview = false;
	panPreviewPixmap = QPixmap();
	EmitMousePositionChanged();
	EmitViewStateChanged();
	update();
}

const std::string& QMainCanvas::GetCrsWkt() const
{
	return crsWkt;
}

QString QMainCanvas::GetCrsDisplayText() const
{
	return crsDisplayText;
}

void QMainCanvas::SetClipMapTilesToCrsValidArea(bool enabled)
{
	if (clipMapTilesToCrsValidArea == enabled)
	{
		return;
	}

	clipMapTilesToCrsValidArea = enabled;
	InvalidateMapContentCache();
	update();
}

bool QMainCanvas::IsClipMapTilesToCrsValidAreaEnabled() const
{
	return clipMapTilesToCrsValidArea;
}

void QMainCanvas::SetCrsValidAreaVisible(bool visible, const GB_ColorRGBA& color)
{
	GB_ColorRGBA targetColor = crsValidAreaColor;
	if (visible)
	{
		targetColor = color;
	}

	if (crsValidAreaVisible == visible && crsValidAreaColor == targetColor)
	{
		return;
	}

	crsValidAreaVisible = visible;
	crsValidAreaColor = targetColor;
	hasPanPreview = false;
	panPreviewPixmap = QPixmap();
	InvalidateMapContentCache();
	update();
}

void QMainCanvas::HideCrsValidArea()
{
	SetCrsValidAreaVisible(false, crsValidAreaColor);
}

bool QMainCanvas::GetCrsValidAreaVisible(GB_ColorRGBA& outColor) const
{
	outColor = crsValidAreaColor;
	return crsValidAreaVisible;
}

void QMainCanvas::UpdateCrsDisplayText()
{
	const QString previousCrsDisplayText = crsDisplayText;

	if (crsWkt.empty())
	{
		crsDisplayText = QStringLiteral("未设置");
	}
	else
	{
		const std::string epsgStringUtf8 = GeoCrsManager::WktToEpsgCodeUtf8(crsWkt);
		if (!epsgStringUtf8.empty())
		{
			crsDisplayText = QString::fromUtf8(epsgStringUtf8.c_str());
		}
		else
		{
			crsDisplayText = QStringLiteral("自定义坐标系");
		}
	}

	if (crsDisplayText != previousCrsDisplayText)
	{
		emit CrsDisplayTextChanged(crsDisplayText);
	}
}

void QMainCanvas::SetViewCenter(double centerX, double centerY)
{
	SetViewCenter(GB_Point2d(centerX, centerY));
}

void QMainCanvas::SetViewCenter(const GB_Point2d& center)
{
	if (!center.IsValid())
	{
		return;
	}

	const GB_Rectangle previousExtent = viewExtent;
	UpdateViewExtentFromCenterAndPixelSize(center);
	InvalidateMapContentCache();
	EmitViewExtentDisplayChangedIfNeeded(previousExtent);
	EmitMousePositionChanged();
	EmitViewStateChanged();
	update();
}

GB_Point2d QMainCanvas::GetViewCenter() const
{
	if (!viewExtent.IsValid())
	{
		return GB_Point2d(0, 0);
	}
	return viewExtent.Center();
}

GB_Rectangle QMainCanvas::GetCurrentViewExtent() const
{
	return viewExtent;
}

bool QMainCanvas::TryGetCurrentViewExtent(GB_Rectangle& outExtent) const
{
	if (!viewExtent.IsValid())
	{
		return false;
	}

	outExtent = viewExtent;
	return true;
}

double QMainCanvas::GetPixelSize() const
{
	return pixelSize;
}

void QMainCanvas::SetViewExtent(const GB_Rectangle& extent)
{
	SetViewExtentInternal(extent, true);
}

void QMainCanvas::ZoomToExtent(const GB_Rectangle& extent, double marginRatio)
{
	GB_Rectangle usableExtent = EnsureUsableExtent(extent);
	if (!usableExtent.IsValid())
	{
		return;
	}

	const double safeMarginRatio = std::max(0.0, marginRatio);
	const double deltaX = usableExtent.Width() * safeMarginRatio;
	const double deltaY = usableExtent.Height() * safeMarginRatio;
	usableExtent.Buffer(deltaX, deltaY);
	SetViewExtentInternal(usableExtent, true);
}

bool QMainCanvas::TryGetCrsValidAreaPolygonsExtent(GB_Rectangle& outExtent) const
{
	outExtent.Reset();

	if (!EnsureCrsValidAreaPolygonsCache() || crsValidAreaPolygonsCache.empty())
	{
		return TryGetCrsValidArea(outExtent);
	}

	for (const std::vector<GB_Point2d>& polygon : crsValidAreaPolygonsCache)
	{
		if (polygon.size() < 3)
		{
			continue;
		}

		for (const GB_Point2d& point : polygon)
		{
			if (IsPointFinite(point))
			{
				outExtent.Expand(point);
			}
		}
	}

	if (!outExtent.IsValid())
	{
		return TryGetCrsValidArea(outExtent);
	}

	outExtent.Normalize();
	return outExtent.IsValid();
}

bool QMainCanvas::ZoomToCrsValidArea(double marginRatio)
{
	GB_Rectangle crsValidAreaExtent;
	if (!TryGetCrsValidAreaPolygonsExtent(crsValidAreaExtent))
	{
		return false;
	}

	ZoomToExtent(crsValidAreaExtent, marginRatio);
	return true;
}

void QMainCanvas::ZoomFull()
{
	GB_Rectangle crsValidArea;
	const bool shouldClipToCrsValidArea = clipMapTilesToCrsValidArea && TryGetCrsValidArea(crsValidArea);

	GB_Rectangle allExtent;
	allExtent.Reset();
	if (shouldClipToCrsValidArea)
	{
		for (const CachedMapTile& item : mapTiles)
		{
			if (!item.tile.visible || !item.tile.extent.IsValid())
			{
				continue;
			}

			const GB_Rectangle clippedTileExtent = item.tile.extent.Intersected(crsValidArea);
			if (clippedTileExtent.IsValid())
			{
				allExtent.Expand(clippedTileExtent);
			}
		}

		if (!allExtent.IsValid())
		{
			allExtent = crsValidArea;
		}
	}
	else
	{
		allExtent = CalculateAllDrawableExtent();
		if (!allExtent.IsValid() && TryGetCrsValidArea(crsValidArea))
		{
			allExtent = crsValidArea;
		}
	}

	if (!allExtent.IsValid())
	{
		allExtent.Set(-180, -90, 180, 90);
	}

	ZoomToExtent(allExtent, 0.05);
}

GB_Point2d QMainCanvas::WorldToScreen(const GB_Point2d& point) const
{
	if (!point.IsValid() || !viewExtent.IsValid() || !IsFinitePositive(pixelSize))
	{
		return GB_Point2d(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN());
	}

	const double screenX = (point.x - viewExtent.minX) / pixelSize;
	const double screenY = (viewExtent.maxY - point.y) / pixelSize;
	return GB_Point2d(screenX, screenY);
}

GB_Point2d QMainCanvas::ScreenToWorld(const GB_Point2d& point) const
{
	if (!point.IsValid() || !viewExtent.IsValid() || !IsFinitePositive(pixelSize))
	{
		return GB_Point2d(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN());
	}

	const double worldX = viewExtent.minX + point.x * pixelSize;
	const double worldY = viewExtent.maxY - point.y * pixelSize;
	return GB_Point2d(worldX, worldY);
}

bool QMainCanvas::TryGetCurrentMouseWorldPosition(GB_Point2d& outPosition) const
{
	outPosition = GB_Point2d(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN());
	if (!hasMousePosition)
	{
		return false;
	}

	outPosition = ScreenToWorld(GB_Point2d(static_cast<double>(lastMousePosition.x()), static_cast<double>(lastMousePosition.y())));
	return outPosition.IsValid();
}

void QMainCanvas::AddMapTile(const MapTile& tile)
{
	AddMapTile(tile, tile.layerNumber);
}

void QMainCanvas::AddMapTile(const MapTile& tile, double layerNumber)
{
	MapTile normalizedTile = tile;
	normalizedTile.layerNumber = NormalizeLayerNumber(layerNumber);

	std::vector<MapTile> tiles;
	tiles.push_back(std::move(normalizedTile));
	AddMapTiles(tiles);
}

void QMainCanvas::AddMapTiles(const std::vector<MapTile>& tiles)
{
	if (tiles.empty())
	{
		return;
	}

	std::vector<CachedMapTile> cachedTiles;
	cachedTiles.reserve(tiles.size());

	for (const MapTile& tile : tiles)
	{
		if (!tile.extent.IsValid() || tile.image.IsEmpty())
		{
			continue;
		}

		CachedMapTile cachedTile;
		cachedTile.tile = tile;
		cachedTile.tile.layerNumber = NormalizeLayerNumber(tile.layerNumber);
		if (cachedTile.tile.uid.empty())
		{
			cachedTile.tile.uid = cachedTile.tile.CalculateUid();
		}

		cachedTile.pixmap = CreateQPixmapFromGBImage(cachedTile.tile.image);
		if (cachedTile.pixmap.isNull())
		{
			continue;
		}

		cachedTile.insertionSequence = nextDrawableInsertionSequence;
		nextDrawableInsertionSequence++;
		cachedTiles.push_back(std::move(cachedTile));
	}

	if (cachedTiles.empty())
	{
		return;
	}

	const bool shouldZoomToAddedTiles = !viewExtent.IsValid();
	GB_Rectangle addedExtent;
	addedExtent.Reset();
	for (const CachedMapTile& cachedTile : cachedTiles)
	{
		addedExtent.Expand(cachedTile.tile.extent);
	}

	mapTiles.reserve(mapTiles.size() + cachedTiles.size());
	for (CachedMapTile& cachedTile : cachedTiles)
	{
		mapTiles.push_back(std::move(cachedTile));
	}

	std::sort(mapTiles.begin(), mapTiles.end(), IsCachedMapTilePaintOrderLess);
	InvalidateMapContentCache();

	if (shouldZoomToAddedTiles && addedExtent.IsValid())
	{
		ZoomToExtent(addedExtent, 0.05);
	}
	else
	{
		update();
	}
}

bool QMainCanvas::HasDrawables() const
{
	if (!mapTiles.empty())
	{
		return true;
	}

	// 后续如果恢复 vectorDrawables / extentMarkerDrawables，
	// 这里也应把它们纳入 Canvas 是否已有内容 的判断。
	return false;
}

void QMainCanvas::ClearDrawables()
{
	const bool hadDrawables = HasDrawables();
	mapTiles.clear();
	InvalidateMapContentCache();
	//vectorDrawables.clear();
	//extentMarkerDrawables.clear();
	if (hadDrawables)
	{
		update();
	}
}

void QMainCanvas::RemoveDrawables(const std::vector<std::string>& drawablesUids)
{
	if (drawablesUids.empty())
	{
		return;
	}

	const std::unordered_set<std::string> uidSet(drawablesUids.begin(), drawablesUids.end());

	bool hasChanged = false;
	const size_t oldMapTileCount = mapTiles.size();
	mapTiles.erase(std::remove_if(mapTiles.begin(), mapTiles.end(), [&uidSet](const CachedMapTile& item) {
		return !item.tile.uid.empty() && uidSet.find(item.tile.uid) != uidSet.end();
		}), mapTiles.end());
	hasChanged = hasChanged || oldMapTileCount != mapTiles.size();
	if (hasChanged)
	{
		InvalidateMapContentCache();
	}

	//vectorDrawables.erase(std::remove_if(vectorDrawables.begin(), vectorDrawables.end(), [this, &drawablesUids](const VectorDrawable& item) {
	//	return IsDrawableUidInSet(drawablesUids, item.uid);
	//	}), vectorDrawables.end());

	//extentMarkerDrawables.erase(std::remove_if(extentMarkerDrawables.begin(), extentMarkerDrawables.end(), [this, &drawablesUids](const ExtentMarkerDrawable& item) {
	//	return IsDrawableUidInSet(drawablesUids, item.uid);
	//	}), extentMarkerDrawables.end());

	if (hasChanged)
	{
		update();
	}
}

void QMainCanvas::SetDrawablesVisible(const std::vector<std::string>& drawablesUids, bool visible)
{
	if (drawablesUids.empty())
	{
		return;
	}

	const std::unordered_set<std::string> uidSet(drawablesUids.begin(), drawablesUids.end());

	if (uidSet.find(GetCrsValidAreaDrawableUid()) != uidSet.end())
	{
		SetCrsValidAreaVisible(visible, crsValidAreaColor);
	}

	bool hasChanged = false;
	for (CachedMapTile& item : mapTiles)
	{
		if (!item.tile.uid.empty() && uidSet.find(item.tile.uid) != uidSet.end() && item.tile.visible != visible)
		{
			item.tile.visible = visible;
			hasChanged = true;
		}
	}

	//for (VectorDrawable& item : vectorDrawables)
	//{
	//	if (IsDrawableUidInSet(drawablesUids, item.uid))
	//	{
	//		item.visible = visible;
	//	}
	//}

	//for (ExtentMarkerDrawable& item : extentMarkerDrawables)
	//{
	//	if (IsDrawableUidInSet(drawablesUids, item.uid))
	//	{
	//		item.visible = visible;
	//	}
	//}

	if (hasChanged)
	{
		InvalidateMapContentCache();
		update();
	}
}

void QMainCanvas::SetDrawablesLayerNumber(const std::vector<std::string>& drawablesUids, double layerNumber)
{
	if (drawablesUids.empty())
	{
		return;
	}

	const std::unordered_set<std::string> uidSet(drawablesUids.begin(), drawablesUids.end());

	const double normalizedLayerNumber = NormalizeLayerNumber(layerNumber);
	bool hasChanged = false;
	for (CachedMapTile& item : mapTiles)
	{
		if (!item.tile.uid.empty() && uidSet.find(item.tile.uid) != uidSet.end() && item.tile.layerNumber != normalizedLayerNumber)
		{
			item.tile.layerNumber = normalizedLayerNumber;
			hasChanged = true;
		}
	}

	// 后续如果恢复 vectorDrawables / extentMarkerDrawables，
	// 这里也应同步调整它们的 layerNumber 并参与重排。

	if (hasChanged)
	{
		std::sort(mapTiles.begin(), mapTiles.end(), IsCachedMapTilePaintOrderLess);
		InvalidateMapContentCache();
		update();
	}
}

void QMainCanvas::paintEvent(QPaintEvent* event)
{
	const QRectF exposedRect = event ? QRectF(event->rect()) : QRectF(rect());

	QPainter painter(this);
	painter.setClipRect(exposedRect);

	if (isPanning && hasPanPreview && !panPreviewPixmap.isNull() && viewExtent.IsValid() && panPreviewViewExtent.IsValid() && AreNearlyEqual(pixelSize, panPreviewPixelSize))
	{
		DrawBackground(painter);

		const double offsetX = (panPreviewViewExtent.minX - viewExtent.minX) / pixelSize;
		const double offsetY = (viewExtent.maxY - panPreviewViewExtent.maxY) / pixelSize;
		if (std::isfinite(offsetX) && std::isfinite(offsetY))
		{
			painter.drawPixmap(QPointF(offsetX, offsetY), panPreviewPixmap);
		}
	}
	else
	{
		EnsureMapContentCache();
		if (!mapContentCache.isNull())
		{
			painter.drawPixmap(QPoint(0, 0), mapContentCache);
		}
		else
		{
			DrawMapContent(painter, exposedRect);
		}
	}

}

void QMainCanvas::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);
	InvalidateMapContentCache();
	hasPanPreview = false;
	panPreviewPixmap = QPixmap();

	if (viewExtent.IsValid())
	{
		const GB_Rectangle previousExtent = viewExtent;
		const GB_Point2d center = viewExtent.Center();
		UpdateViewExtentFromCenterAndPixelSize(center);
		if (!AreRectanglesNearlyEqual(viewExtent, previousExtent))
		{
			EmitViewExtentDisplayChangedIfNeeded(previousExtent);
			EmitMousePositionChanged();
			ScheduleViewStateChanged();
		}
	}
}

void QMainCanvas::mousePressEvent(QMouseEvent* event)
{
	if (!event)
	{
		return;
	}

	SetMousePosition(event->pos());

	if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)
	{
		EnsureMapContentCache();
		panPreviewPixmap = mapContentCache;
		hasPanPreview = !panPreviewPixmap.isNull() && viewExtent.IsValid() && IsFinitePositive(pixelSize);
		panPreviewViewExtent = viewExtent;
		panPreviewPixelSize = pixelSize;

		isPanning = true;
		lastPanPosition = event->pos();
		setCursor(Qt::ClosedHandCursor);
		event->accept();
		return;
	}

	QWidget::mousePressEvent(event);
}

void QMainCanvas::mouseMoveEvent(QMouseEvent* event)
{
	if (!event)
	{
		return;
	}

	const QPoint currentMousePosition = event->pos();
	const bool mousePositionChanged = SetMousePosition(currentMousePosition);

	if (isPanning && viewExtent.IsValid() && IsFinitePositive(pixelSize))
	{
		const QPoint delta = event->pos() - lastPanPosition;
		lastPanPosition = event->pos();
		if (delta.isNull())
		{
			event->accept();
			return;
		}

		const double safePixelSize = ClampPixelSizeForCurrentWidget(pixelSize);
		const double offsetX = -static_cast<double>(delta.x()) * safePixelSize;
		const double offsetY = static_cast<double>(delta.y()) * safePixelSize;
		const GB_Point2d currentCenter = viewExtent.Center();
		const GB_Point2d targetCenter(currentCenter.x + offsetX, currentCenter.y + offsetY);
		const GB_Rectangle newExtent = MakeExtentByCenterAndPixelSize(targetCenter, safePixelSize);
		if (!newExtent.IsValid())
		{
			event->accept();
			return;
		}

		if (AreRectanglesNearlyEqual(newExtent, viewExtent))
		{
			event->accept();
			return;
		}

		const GB_Rectangle previousExtent = viewExtent;
		viewExtent = newExtent;
		pixelSize = safePixelSize;
		InvalidateMapContentCache();
		EmitViewExtentDisplayChangedIfNeeded(previousExtent);
		EmitMousePositionChanged();

		ScheduleViewStateChanged();
		update();
		event->accept();
		return;
	}

	Q_UNUSED(mousePositionChanged);

	QWidget::mouseMoveEvent(event);
}

void QMainCanvas::mouseReleaseEvent(QMouseEvent* event)
{
	if (!event)
	{
		return;
	}

	SetMousePosition(event->pos());

	if ((event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) && isPanning)
	{
		isPanning = false;
		hasPanPreview = false;
		panPreviewPixmap = QPixmap();
		InvalidateMapContentCache();
		unsetCursor();
		FlushPendingViewStateChanged();
		update();
		event->accept();
		return;
	}

	QWidget::mouseReleaseEvent(event);
}


void QMainCanvas::wheelEvent(QWheelEvent* event)
{
	if (!event || !viewExtent.IsValid() || !IsFinitePositive(pixelSize))
	{
		return;
	}

	SetMousePosition(event->pos());

	const bool isWheelBeginEvent = event->phase() == Qt::ScrollBegin;
	const bool isWheelEndEvent = event->phase() == Qt::ScrollEnd;

	const QPoint angleDelta = event->angleDelta();
	if (angleDelta.y() == 0)
	{
		if (isWheelEndEvent)
		{
			FlushPendingViewStateChanged();
			event->accept();
			return;
		}

		if (isWheelBeginEvent)
		{
			event->accept();
			return;
		}

		QWidget::wheelEvent(event);
		return;
	}

	const QPointF position(event->pos());
	const GB_Point2d worldBefore = ScreenToWorld(GB_Point2d(position.x(), position.y()));
	if (!worldBefore.IsValid())
	{
		event->accept();
		return;
	}

	const double zoomSteps = static_cast<double>(angleDelta.y()) / 120.0;
	const double zoomFactor = std::pow(1.2, zoomSteps);
	if (!IsFinitePositive(zoomFactor))
	{
		event->accept();
		return;
	}

	const double newPixelSize = ClampPixelSizeForCurrentWidget(pixelSize / zoomFactor);
	const int canvasWidth = SafeWidgetWidth(this);
	const int canvasHeight = SafeWidgetHeight(this);

	const double minX = worldBefore.x - position.x() * newPixelSize;
	const double maxY = worldBefore.y + position.y() * newPixelSize;
	const GB_Rectangle requestedExtent(minX, maxY - static_cast<double>(canvasHeight) * newPixelSize,
		minX + static_cast<double>(canvasWidth) * newPixelSize, maxY);
	const GB_Rectangle newExtent = NormalizeExtentForCurrentWidget(requestedExtent);
	if (!newExtent.IsValid())
	{
		event->accept();
		return;
	}

	if (AreRectanglesNearlyEqual(newExtent, viewExtent))
	{
		if (isWheelEndEvent)
		{
			FlushPendingViewStateChanged();
		}

		event->accept();
		return;
	}

	const GB_Rectangle previousExtent = viewExtent;
	viewExtent = newExtent;
	UpdatePixelSizeFromViewExtent();
	InvalidateMapContentCache();
	EmitViewExtentDisplayChangedIfNeeded(previousExtent);
	EmitMousePositionChanged();
	ScheduleWheelZoomViewStateChanged();
	update();

	if (isWheelEndEvent)
	{
		FlushPendingViewStateChanged();
	}

	event->accept();
}

void QMainCanvas::leaveEvent(QEvent* event)
{
	ClearMousePosition();
	QWidget::leaveEvent(event);
}

void QMainCanvas::dragEnterEvent(QDragEnterEvent* event)
{
	if (event && event->mimeData() && event->mimeData()->hasFormat(GetServiceNodeMimeType()))
	{
		event->acceptProposedAction();
		return;
	}

	QWidget::dragEnterEvent(event);
}

void QMainCanvas::dragMoveEvent(QDragMoveEvent* event)
{
	if (event && event->mimeData() && event->mimeData()->hasFormat(GetServiceNodeMimeType()))
	{
		event->acceptProposedAction();
		return;
	}

	QWidget::dragMoveEvent(event);
}

void QMainCanvas::dropEvent(QDropEvent* event)
{
	if (!event || !event->mimeData() || !event->mimeData()->hasFormat(GetServiceNodeMimeType()))
	{
		QWidget::dropEvent(event);
		return;
	}

	const QString nodeUid = QString::fromUtf8(ReadJsonText(event->mimeData(), "uid"));
	const QString url = QString::fromUtf8(ReadJsonText(event->mimeData(), "url"));
	const QString text = QString::fromUtf8(ReadJsonText(event->mimeData(), "text"));
	const int nodeType = ReadJsonInt(event->mimeData(), "nodeType", 0);

	emit LayerDropRequested(nodeUid, url, text, nodeType);
	event->acceptProposedAction();
}

QImage QMainCanvas::CreateQImageFromGBImage(const GB_Image& image) const
{
	if (image.IsEmpty() || image.GetWidth() == 0 || image.GetHeight() == 0)
	{
		return QImage();
	}

	if (image.GetDepth() != GB_ImageDepth::UInt8 || (image.GetChannels() != 1 && image.GetChannels() != 3 && image.GetChannels() != 4))
	{
		return QImage();
	}

	const int imageWidth = static_cast<int>(image.GetWidth());
	const int imageHeight = static_cast<int>(image.GetHeight());
	QImage result(imageWidth, imageHeight, QImage::Format_ARGB32);
	if (result.isNull())
	{
		return QImage();
	}

	for (int row = 0; row < imageHeight; row++)
	{
		QRgb* scanLine = reinterpret_cast<QRgb*>(result.scanLine(row));
		for (int col = 0; col < imageWidth; col++)
		{
			GB_ColorRGBA color;
			if (image.GetPixelColor(static_cast<size_t>(row), static_cast<size_t>(col), color))
			{
				scanLine[col] = qRgba(color.r, color.g, color.b, color.a);
			}
			else
			{
				scanLine[col] = qRgba(0, 0, 0, 0);
			}
		}
	}

	return result;
}


QPixmap QMainCanvas::CreateQPixmapFromGBImage(const GB_Image& image) const
{
	const QImage qImage = CreateQImageFromGBImage(image);
	if (qImage.isNull())
	{
		return QPixmap();
	}

	return QPixmap::fromImage(qImage);
}

double QMainCanvas::NormalizeLayerNumber(double layerNumber)
{
	if (!std::isfinite(layerNumber))
	{
		return GetDefaultLayerNumber();
	}

	return layerNumber;
}

bool QMainCanvas::IsTopLayerNumber(double layerNumber)
{
	return layerNumber == GetTopLayerNumber();
}

bool QMainCanvas::IsBottomLayerNumber(double layerNumber)
{
	return layerNumber == GetBottomLayerNumber();
}

int QMainCanvas::GetLayerPaintOrderGroup(double layerNumber)
{
	if (IsBottomLayerNumber(layerNumber))
	{
		return 0;
	}

	if (IsTopLayerNumber(layerNumber))
	{
		return 2;
	}

	return 1;
}

bool QMainCanvas::IsCachedMapTilePaintOrderLess(const CachedMapTile& firstTile, const CachedMapTile& secondTile)
{
	const int firstGroup = GetLayerPaintOrderGroup(firstTile.tile.layerNumber);
	const int secondGroup = GetLayerPaintOrderGroup(secondTile.tile.layerNumber);
	if (firstGroup != secondGroup)
	{
		return firstGroup < secondGroup;
	}

	if (firstGroup == 1 && firstTile.tile.layerNumber != secondTile.tile.layerNumber)
	{
		// QPainter 后绘制的内容会覆盖先绘制的内容。
		// 普通层号越大越靠底层，因此需要先绘制较大的层号。
		return firstTile.tile.layerNumber > secondTile.tile.layerNumber;
	}

	return firstTile.insertionSequence < secondTile.insertionSequence;
}

void QMainCanvas::InsertCachedMapTile(CachedMapTile&& cachedTile)
{
	const auto insertIter = std::lower_bound(mapTiles.begin(), mapTiles.end(), cachedTile, IsCachedMapTilePaintOrderLess);
	mapTiles.insert(insertIter, std::move(cachedTile));
}

void QMainCanvas::DrawBackground(QPainter& painter) const
{
	painter.fillRect(rect(), Qt::white);
}

bool QMainCanvas::TryGetCrsValidArea(GB_Rectangle& outValidArea) const
{
	outValidArea.Reset();
	if (crsWkt.empty())
	{
		return false;
	}

	std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromWktCached(crsWkt);
	if (!crs || !crs->IsValid())
	{
		return false;
	}

	const GeoBoundingBox validArea = crs->GetValidArea();
	if (!validArea.IsValid() || !validArea.rect.IsValid())
	{
		return false;
	}

	outValidArea = validArea.rect;
	outValidArea.Normalize();
	return outValidArea.IsValid();
}

void QMainCanvas::DrawMapTiles(QPainter& painter, const QRectF& exposedRect) const
{
	if (!viewExtent.IsValid())
	{
		return;
	}

	GB_Rectangle crsValidArea;
	const bool shouldClipToCrsValidArea = clipMapTilesToCrsValidArea && TryGetCrsValidArea(crsValidArea);
	const QRectF safeExposedRect = exposedRect.isValid() ? exposedRect : QRectF(rect());

	bool lastSmoothPixmapTransform = false;
	painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

	for (const CachedMapTile& cachedTile : mapTiles)
	{
		if (!cachedTile.tile.visible || !cachedTile.tile.extent.IsValid() || cachedTile.pixmap.isNull())
		{
			continue;
		}

		GB_Rectangle clippedWorldRect = cachedTile.tile.extent.Intersected(viewExtent);
		if (!clippedWorldRect.IsValid() || clippedWorldRect.Width() <= 0.0 || clippedWorldRect.Height() <= 0.0)
		{
			continue;
		}

		if (shouldClipToCrsValidArea)
		{
			clippedWorldRect = clippedWorldRect.Intersected(crsValidArea);
			if (!clippedWorldRect.IsValid() || clippedWorldRect.Width() <= 0.0 || clippedWorldRect.Height() <= 0.0)
			{
				continue;
			}
		}

		const QRectF targetRect = WorldRectangleToScreenRectangle(clippedWorldRect);
		if (!targetRect.isValid() || !targetRect.intersects(safeExposedRect))
		{
			continue;
		}

		const double tileWidth = cachedTile.tile.extent.Width();
		const double tileHeight = cachedTile.tile.extent.Height();
		if (!IsFinitePositive(tileWidth) || !IsFinitePositive(tileHeight))
		{
			continue;
		}

		const double imageWidth = static_cast<double>(cachedTile.pixmap.width());
		const double imageHeight = static_cast<double>(cachedTile.pixmap.height());
		if (!IsFinitePositive(imageWidth) || !IsFinitePositive(imageHeight))
		{
			continue;
		}

		const QRectF sourceRect(
			(clippedWorldRect.minX - cachedTile.tile.extent.minX) / tileWidth * imageWidth,
			(cachedTile.tile.extent.maxY - clippedWorldRect.maxY) / tileHeight * imageHeight,
			clippedWorldRect.Width() / tileWidth * imageWidth,
			clippedWorldRect.Height() / tileHeight * imageHeight);
		if (!sourceRect.isValid())
		{
			continue;
		}

		const bool needsSmoothTransform = std::abs(targetRect.width() - sourceRect.width()) > 0.25 ||
			std::abs(targetRect.height() - sourceRect.height()) > 0.25;
		if (needsSmoothTransform != lastSmoothPixmapTransform)
		{
			painter.setRenderHint(QPainter::SmoothPixmapTransform, needsSmoothTransform);
			lastSmoothPixmapTransform = needsSmoothTransform;
		}

		painter.drawPixmap(targetRect, cachedTile.pixmap, sourceRect);
	}

	if (lastSmoothPixmapTransform)
	{
		painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
	}
}

void QMainCanvas::DrawMapContent(QPainter& painter, const QRectF& exposedRect) const
{
	DrawBackground(painter);
	DrawMapTiles(painter, exposedRect);
	DrawCoordinateAxes(painter);
	DrawCrsValidArea(painter);
}

bool QMainCanvas::IsMapContentCacheValid() const
{
	if (isMapContentCacheDirty || mapContentCache.isNull())
	{
		return false;
	}

	if (mapContentCache.size() != size())
	{
		return false;
	}

	if (!AreRectanglesNearlyEqual(mapContentCacheViewExtent, viewExtent))
	{
		return false;
	}

	if (!AreNearlyEqual(mapContentCachePixelSize, pixelSize))
	{
		return false;
	}

	if (mapContentCacheClipMapTilesToCrsValidArea != clipMapTilesToCrsValidArea)
	{
		return false;
	}

	if (mapContentCacheCrsValidAreaVisible != crsValidAreaVisible)
	{
		return false;
	}

	if (mapContentCacheCrsValidAreaColor != crsValidAreaColor)
	{
		return false;
	}

	return mapContentCacheCrsWkt == crsWkt;
}

void QMainCanvas::EnsureMapContentCache() const
{
	if (IsMapContentCacheValid())
	{
		return;
	}

	if (width() <= 0 || height() <= 0)
	{
		mapContentCache = QPixmap();
		isMapContentCacheDirty = false;
		return;
	}

	QPixmap newCache(size());
	newCache.fill(Qt::transparent);

	QPainter cachePainter(&newCache);
	cachePainter.setClipRect(QRectF(QPointF(0.0, 0.0), QSizeF(newCache.size())));
	DrawMapContent(cachePainter, QRectF(QPointF(0.0, 0.0), QSizeF(newCache.size())));
	cachePainter.end();

	mapContentCache = newCache;
	mapContentCacheViewExtent = viewExtent;
	mapContentCachePixelSize = pixelSize;
	mapContentCacheClipMapTilesToCrsValidArea = clipMapTilesToCrsValidArea;
	mapContentCacheCrsWkt = crsWkt;
	mapContentCacheCrsValidAreaVisible = crsValidAreaVisible;
	mapContentCacheCrsValidAreaColor = crsValidAreaColor;
	isMapContentCacheDirty = false;
}

void QMainCanvas::InvalidateMapContentCache() const
{
	isMapContentCacheDirty = true;
}

void QMainCanvas::DrawCrsValidArea(QPainter& painter) const
{
	if (!crsValidAreaVisible || crsValidAreaColor.IsTransparent() || !viewExtent.IsValid() || !IsFinitePositive(pixelSize))
	{
		return;
	}

	if (!EnsureCrsValidAreaPolygonsCache() || crsValidAreaPolygonsCache.empty())
	{
		return;
	}

	QPainterPath fillPath;
	for (const std::vector<GB_Point2d>& polygon : crsValidAreaPolygonsCache)
	{
		if (polygon.size() < 3)
		{
			continue;
		}

		GB_Rectangle polygonExtent;
		polygonExtent.Reset();
		bool hasInvalidPoint = false;
		for (const GB_Point2d& point : polygon)
		{
			if (!IsPointFinite(point))
			{
				hasInvalidPoint = true;
				break;
			}

			polygonExtent.Expand(point);
		}

		if (hasInvalidPoint || !polygonExtent.IsValid() || !polygonExtent.IsIntersects(viewExtent))
		{
			continue;
		}

		const std::vector<GB_Point2d> clippedPolygon = ClipPolygonToRectangle(polygon, viewExtent);
		if (clippedPolygon.size() < 3)
		{
			continue;
		}

		QPolygonF screenPolygon;
		screenPolygon.reserve(static_cast<int>(clippedPolygon.size()));
		for (const GB_Point2d& point : clippedPolygon)
		{
			const GB_Point2d screenPoint = WorldToScreen(point);
			if (!IsPointFinite(screenPoint))
			{
				screenPolygon.clear();
				break;
			}

			screenPolygon << QPointF(screenPoint.x, screenPoint.y);
		}

		if (screenPolygon.size() >= 3)
		{
			fillPath.addPolygon(screenPolygon);
			fillPath.closeSubpath();
		}
	}

	if (fillPath.isEmpty())
	{
		return;
	}

	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
	painter.setPen(Qt::NoPen);
	painter.setBrush(ToQColor(crsValidAreaColor));
	painter.drawPath(fillPath);
	painter.restore();
}

void QMainCanvas::DrawCoordinateAxes(QPainter& painter) const
{
	const int canvasWidth = width();
	const int canvasHeight = height();
	if (canvasWidth <= 0 || canvasHeight <= 0 || !viewExtent.IsValid() || !IsFinitePositive(pixelSize))
	{
		return;
	}

	const QColor xAxisColor(210, 65, 65);
	const QColor yAxisColor(45, 135, 75);
	const QColor originColor(60, 60, 60);
	const double axisPenWidth = 2.0;
	const double arrowLength = 9.0;
	const double arrowHalfWidth = 4.0;
	const double originRadius = 3.0;
	const double textMargin = 6.0;
	const double screenLeft = 0.0;
	const double screenTop = 0.0;
	const double screenRight = static_cast<double>(canvasWidth - 1);
	const double screenBottom = static_cast<double>(canvasHeight - 1);

	const GB_Point2d worldOriginScreenPoint = WorldToScreen(GB_Point2d(0.0, 0.0));
	if (!worldOriginScreenPoint.IsValid())
	{
		return;
	}

	const bool originVisible = worldOriginScreenPoint.x >= screenLeft && worldOriginScreenPoint.x <= screenRight &&
		worldOriginScreenPoint.y >= screenTop && worldOriginScreenPoint.y <= screenBottom;
	if (!originVisible)
	{
		return;
	}

	const auto ClampScreenX = [screenLeft, screenRight](double screenX) -> double
		{
			return GB_Clamp(screenX, screenLeft, screenRight);
		};

	const auto ClampScreenY = [screenTop, screenBottom](double screenY) -> double
		{
			return GB_Clamp(screenY, screenTop, screenBottom);
		};

	const auto MakeHorizontalAxisSegment = [](double originX, double screenLeftValue, double screenRightValue, double requestedLength, double& outStartX, double& outEndX) -> bool
		{
			if (!std::isfinite(originX) || screenRightValue < screenLeftValue || requestedLength <= 0.0)
			{
				return false;
			}

			const double availableLength = screenRightValue - screenLeftValue;
			const double segmentLength = std::min(requestedLength, availableLength);
			outStartX = originX;
			outEndX = originX + segmentLength;

			if (outEndX > screenRightValue)
			{
				outEndX = screenRightValue;
				outStartX = outEndX - segmentLength;
			}

			if (outStartX < screenLeftValue)
			{
				outStartX = screenLeftValue;
				outEndX = outStartX + segmentLength;
			}

			return std::isfinite(outStartX) && std::isfinite(outEndX) && outEndX > outStartX;
		};

	const auto MakeVerticalAxisSegment = [](double originY, double screenTopValue, double screenBottomValue, double requestedLength, double& outStartY, double& outEndY) -> bool
		{
			if (!std::isfinite(originY) || screenBottomValue < screenTopValue || requestedLength <= 0.0)
			{
				return false;
			}

			const double availableLength = screenBottomValue - screenTopValue;
			const double segmentLength = std::min(requestedLength, availableLength);
			outStartY = originY;
			outEndY = originY - segmentLength;

			if (outEndY < screenTopValue)
			{
				outEndY = screenTopValue;
				outStartY = outEndY + segmentLength;
			}

			if (outStartY > screenBottomValue)
			{
				outStartY = screenBottomValue;
				outEndY = outStartY - segmentLength;
			}

			return std::isfinite(outStartY) && std::isfinite(outEndY) && outStartY > outEndY;
		};

	const auto DrawArrowHead = [&painter, arrowLength, arrowHalfWidth](const QPointF& tip, const QPointF& direction, const QColor& color)
		{
			const double length = std::sqrt(direction.x() * direction.x() + direction.y() * direction.y());
			if (!std::isfinite(length) || length <= 0.0)
			{
				return;
			}

			const QPointF unit(direction.x() / length, direction.y() / length);
			const QPointF normal(-unit.y(), unit.x());
			const QPointF backCenter(tip.x() - unit.x() * arrowLength, tip.y() - unit.y() * arrowLength);
			const QPointF left(backCenter.x() + normal.x() * arrowHalfWidth, backCenter.y() + normal.y() * arrowHalfWidth);
			const QPointF right(backCenter.x() - normal.x() * arrowHalfWidth, backCenter.y() - normal.y() * arrowHalfWidth);

			QPolygonF arrow;
			arrow << tip << left << right;

			painter.save();
			painter.setPen(Qt::NoPen);
			painter.setBrush(color);
			painter.drawPolygon(arrow);
			painter.restore();
		};

	double xAxisStartX = 0.0;
	double xAxisEndX = 0.0;
	double yAxisStartY = 0.0;
	double yAxisEndY = 0.0;
	const bool canDrawXAxis = MakeHorizontalAxisSegment(worldOriginScreenPoint.x, screenLeft, screenRight, FixedAxisPixelLength, xAxisStartX, xAxisEndX);
	const bool canDrawYAxis = MakeVerticalAxisSegment(worldOriginScreenPoint.y, screenTop, screenBottom, FixedAxisPixelLength, yAxisStartY, yAxisEndY);
	if (!canDrawXAxis && !canDrawYAxis)
	{
		return;
	}

	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setClipRect(rect());

	if (canDrawXAxis)
	{
		const double xAxisScreenY = ClampScreenY(worldOriginScreenPoint.y);
		const QPointF xAxisStart(xAxisStartX, xAxisScreenY);
		const QPointF xAxisEnd(xAxisEndX, xAxisScreenY);

		QPen xPen(xAxisColor, axisPenWidth);
		xPen.setCapStyle(Qt::RoundCap);
		painter.setPen(xPen);
		painter.setBrush(Qt::NoBrush);
		painter.drawLine(xAxisStart, xAxisEnd);
		DrawArrowHead(xAxisEnd, QPointF(1.0, 0.0), xAxisColor);

		const double xTextX = xAxisEnd.x() >= screenRight - 14.0 ? xAxisEnd.x() - 14.0 : xAxisEnd.x() + textMargin;
		const double xTextY = xAxisScreenY >= screenBottom - 14.0 ? xAxisScreenY - textMargin : xAxisScreenY + 14.0;
		painter.setPen(xAxisColor);
		painter.drawText(QPointF(ClampScreenX(xTextX), ClampScreenY(xTextY)), QStringLiteral("X"));
	}

	if (canDrawYAxis)
	{
		const double yAxisScreenX = ClampScreenX(worldOriginScreenPoint.x);
		const QPointF yAxisStart(yAxisScreenX, yAxisStartY);
		const QPointF yAxisEnd(yAxisScreenX, yAxisEndY);

		QPen yPen(yAxisColor, axisPenWidth);
		yPen.setCapStyle(Qt::RoundCap);
		painter.setPen(yPen);
		painter.setBrush(Qt::NoBrush);
		painter.drawLine(yAxisStart, yAxisEnd);
		DrawArrowHead(yAxisEnd, QPointF(0.0, -1.0), yAxisColor);

		const double yTextX = yAxisScreenX >= screenRight - 14.0 ? yAxisScreenX - 14.0 : yAxisScreenX + textMargin;
		const double yTextY = yAxisEnd.y() <= screenTop + 14.0 ? yAxisEnd.y() + 14.0 : yAxisEnd.y() - textMargin;
		painter.setPen(yAxisColor);
		painter.drawText(QPointF(ClampScreenX(yTextX), ClampScreenY(yTextY)), QStringLiteral("Y"));
	}

	const QPointF origin(ClampScreenX(worldOriginScreenPoint.x), ClampScreenY(worldOriginScreenPoint.y));
	painter.setPen(QPen(originColor, 1.5));
	painter.setBrush(Qt::white);
	painter.drawEllipse(origin, originRadius, originRadius);

	const double originTextX = origin.x() >= screenRight - 16.0 ? origin.x() - 16.0 : origin.x() + textMargin;
	const double originTextY = origin.y() <= screenTop + 12.0 ? origin.y() + 16.0 : origin.y() - textMargin;
	painter.setPen(originColor);
	painter.drawText(QPointF(ClampScreenX(originTextX), ClampScreenY(originTextY)), QStringLiteral("O"));

	painter.restore();
}

bool QMainCanvas::EnsureCrsValidAreaPolygonsCache() const
{
	if (!crsValidAreaPolygonsCacheDirty && crsValidAreaPolygonsCacheCrsWkt == crsWkt)
	{
		return !crsValidAreaPolygonsCache.empty();
	}

	crsValidAreaPolygonsCache.clear();
	crsValidAreaPolygonsCacheCrsWkt = crsWkt;
	crsValidAreaPolygonsCacheDirty = false;

	if (crsWkt.empty())
	{
		return false;
	}

	std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromWktCached(crsWkt);
	if (!crs || !crs->IsValid())
	{
		return false;
	}

	const std::vector<std::vector<GB_Point2d>> polygons = crs->GetValidAreaPolygons(CrsValidAreaPolygonEdgeSampleCount);
	crsValidAreaPolygonsCache.reserve(polygons.size());
	for (const std::vector<GB_Point2d>& polygon : polygons)
	{
		if (polygon.size() < 3)
		{
			continue;
		}

		std::vector<GB_Point2d> filteredPolygon;
		filteredPolygon.reserve(polygon.size());
		bool hasInvalidPoint = false;
		for (const GB_Point2d& point : polygon)
		{
			if (!IsPointFinite(point))
			{
				hasInvalidPoint = true;
				break;
			}

			AppendPointIfUseful(filteredPolygon, point);
		}

		if (hasInvalidPoint)
		{
			continue;
		}

		if (filteredPolygon.size() > 1 && filteredPolygon.front().IsNearEqual(filteredPolygon.back(), GB_Epsilon))
		{
			filteredPolygon.pop_back();
		}

		if (filteredPolygon.size() >= 3)
		{
			crsValidAreaPolygonsCache.push_back(std::move(filteredPolygon));
		}
	}

	return !crsValidAreaPolygonsCache.empty();
}

void QMainCanvas::InvalidateCrsValidAreaPolygonsCache() const
{
	crsValidAreaPolygonsCacheDirty = true;
	crsValidAreaPolygonsCacheCrsWkt.clear();
	crsValidAreaPolygonsCache.clear();
}

bool QMainCanvas::IsDrawableUidInSet(const std::vector<std::string>& drawablesUids, const std::string& uid) const
{
	if (uid.empty())
	{
		return false;
	}

	return std::find(drawablesUids.begin(), drawablesUids.end(), uid) != drawablesUids.end();
}

GB_Rectangle QMainCanvas::CalculateAllDrawableExtent() const
{
	GB_Rectangle result;
	result.Reset();

	for (const CachedMapTile& item : mapTiles)
	{
		if (item.tile.visible && item.tile.extent.IsValid())
		{
			result.Expand(item.tile.extent);
		}
	}

	//for (const VectorDrawable& item : vectorDrawables)
	//{
	//	if (item.visible)
	//	{
	//		const GB_Rectangle extent = item.CalculateExtent();
	//		if (extent.IsValid())
	//		{
	//			result.Expand(extent);
	//		}
	//	}
	//}

	//for (const ExtentMarkerDrawable& item : extentMarkerDrawables)
	//{
	//	if (item.visible && item.extent.IsValid())
	//	{
	//		result.Expand(item.extent);
	//	}
	//}

	return result;
}

void QMainCanvas::SetViewExtentInternal(const GB_Rectangle& extent, bool emitSignal)
{
	const GB_Rectangle usableExtent = EnsureUsableExtent(extent);
	if (!usableExtent.IsValid())
	{
		return;
	}

	const GB_Rectangle normalizedExtent = NormalizeExtentForCurrentWidget(usableExtent);
	if (!normalizedExtent.IsValid())
	{
		return;
	}

	const GB_Rectangle previousExtent = viewExtent;
	viewExtent = normalizedExtent;
	UpdatePixelSizeFromViewExtent();
	InvalidateMapContentCache();
	EmitViewExtentDisplayChangedIfNeeded(previousExtent);
	EmitMousePositionChanged();

	if (emitSignal)
	{
		EmitViewStateChanged();
	}
	update();
}

double QMainCanvas::GetMinimumPixelSizeForCurrentWidget() const
{
	const int canvasWidth = SafeWidgetWidth(this);
	const int canvasHeight = SafeWidgetHeight(this);
	const int minimumCanvasSize = std::max(1, std::min(canvasWidth, canvasHeight));
	const double minimumPixelSize = MinimumCoordinateRange / static_cast<double>(minimumCanvasSize);
	return IsFinitePositive(minimumPixelSize) ? minimumPixelSize : FallbackMinimumPixelSize;
}

double QMainCanvas::GetMaximumPixelSizeForCurrentWidget() const
{
	const int canvasWidth = SafeWidgetWidth(this);
	const int canvasHeight = SafeWidgetHeight(this);
	const int maximumCanvasSize = std::max(1, std::max(canvasWidth, canvasHeight));
	const double maximumCoordinateRange = MaximumCoordinateAbsValue * 2.0;
	const double maximumPixelSize = maximumCoordinateRange / static_cast<double>(maximumCanvasSize);
	return IsFinitePositive(maximumPixelSize) ? maximumPixelSize : FallbackMaximumPixelSize;
}

double QMainCanvas::ClampPixelSizeForCurrentWidget(double targetPixelSize) const
{
	const double minimumPixelSize = GetMinimumPixelSizeForCurrentWidget();
	const double maximumPixelSize = GetMaximumPixelSizeForCurrentWidget();
	if (minimumPixelSize > maximumPixelSize)
	{
		return IsFinitePositive(targetPixelSize) ? GB_Clamp(targetPixelSize, FallbackMinimumPixelSize, FallbackMaximumPixelSize) : DefaultPixelSize;
	}

	if (std::isnan(targetPixelSize))
	{
		return DefaultPixelSize;
	}

	if (!std::isfinite(targetPixelSize))
	{
		return targetPixelSize > 0.0 ? maximumPixelSize : minimumPixelSize;
	}

	if (targetPixelSize <= 0.0)
	{
		return minimumPixelSize;
	}

	return GB_Clamp(targetPixelSize, minimumPixelSize, maximumPixelSize);
}

GB_Point2d QMainCanvas::ClampViewCenterForPixelSize(const GB_Point2d& center, double targetPixelSize) const
{
	if (!center.IsValid() || !IsFinitePositive(targetPixelSize))
	{
		return GB_Point2d(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN());
	}

	const double safePixelSize = ClampPixelSizeForCurrentWidget(targetPixelSize);
	const int canvasWidth = SafeWidgetWidth(this);
	const int canvasHeight = SafeWidgetHeight(this);
	const double halfWidth = static_cast<double>(canvasWidth) * safePixelSize * 0.5;
	const double halfHeight = static_cast<double>(canvasHeight) * safePixelSize * 0.5;

	const auto ClampCenterCoordinate = [](double coordinate, double halfRange) -> double
		{
			if (!std::isfinite(coordinate) || !std::isfinite(halfRange) || halfRange < 0.0)
			{
				return std::numeric_limits<double>::quiet_NaN();
			}

			const double minCenter = -MaximumCoordinateAbsValue + halfRange;
			const double maxCenter = MaximumCoordinateAbsValue - halfRange;
			if (minCenter > maxCenter)
			{
				return 0.0;
			}

			return GB_Clamp(coordinate, minCenter, maxCenter);
		};

	return GB_Point2d(ClampCenterCoordinate(center.x, halfWidth), ClampCenterCoordinate(center.y, halfHeight));
}

void QMainCanvas::UpdatePixelSizeFromViewExtent()
{
	if (!viewExtent.IsValid())
	{
		pixelSize = DefaultPixelSize;
		return;
	}

	const int canvasWidth = SafeWidgetWidth(this);
	const int canvasHeight = SafeWidgetHeight(this);
	const double pixelSizeX = viewExtent.Width() / static_cast<double>(canvasWidth);
	const double pixelSizeY = viewExtent.Height() / static_cast<double>(canvasHeight);
	const double newPixelSize = std::max(pixelSizeX, pixelSizeY);
	if (IsFinitePositive(newPixelSize))
	{
		pixelSize = ClampPixelSizeForCurrentWidget(newPixelSize);
	}
	else
	{
		pixelSize = DefaultPixelSize;
	}
}

void QMainCanvas::UpdateViewExtentFromCenterAndPixelSize(const GB_Point2d& center)
{
	const double safePixelSize = ClampPixelSizeForCurrentWidget(IsFinitePositive(pixelSize) ? pixelSize : DefaultPixelSize);
	viewExtent = MakeExtentByCenterAndPixelSize(center, safePixelSize);
	pixelSize = safePixelSize;
}

GB_Rectangle QMainCanvas::MakeExtentByCenterAndPixelSize(const GB_Point2d& center, double targetPixelSize) const
{
	if (!center.IsValid() || !IsFinitePositive(targetPixelSize))
	{
		return GB_Rectangle::Invalid;
	}

	const double safePixelSize = ClampPixelSizeForCurrentWidget(targetPixelSize);
	const GB_Point2d safeCenter = ClampViewCenterForPixelSize(center, safePixelSize);
	if (!safeCenter.IsValid())
	{
		return GB_Rectangle::Invalid;
	}

	const int canvasWidth = SafeWidgetWidth(this);
	const int canvasHeight = SafeWidgetHeight(this);
	const double halfWidth = static_cast<double>(canvasWidth) * safePixelSize * 0.5;
	const double halfHeight = static_cast<double>(canvasHeight) * safePixelSize * 0.5;
	return GB_Rectangle(safeCenter.x - halfWidth, safeCenter.y - halfHeight, safeCenter.x + halfWidth, safeCenter.y + halfHeight);
}

GB_Rectangle QMainCanvas::NormalizeExtentForCurrentWidget(const GB_Rectangle& extent) const
{
	GB_Rectangle usableExtent = EnsureUsableExtent(extent);
	if (!usableExtent.IsValid())
	{
		return GB_Rectangle::Invalid;
	}

	const int canvasWidth = SafeWidgetWidth(this);
	const int canvasHeight = SafeWidgetHeight(this);
	const double pixelSizeX = usableExtent.Width() / static_cast<double>(canvasWidth);
	const double pixelSizeY = usableExtent.Height() / static_cast<double>(canvasHeight);
	const double targetPixelSize = std::max(pixelSizeX, pixelSizeY);
	const double safePixelSize = ClampPixelSizeForCurrentWidget(IsFinitePositive(targetPixelSize) ? targetPixelSize : DefaultPixelSize);

	return MakeExtentByCenterAndPixelSize(usableExtent.Center(), safePixelSize);
}

GB_Rectangle QMainCanvas::EnsureUsableExtent(const GB_Rectangle& extent) const
{
	if (!extent.IsValid())
	{
		return GB_Rectangle::Invalid;
	}

	GB_Rectangle result = extent;
	result.Normalize();

	const double currentPixelSize = IsFinitePositive(pixelSize) ? pixelSize : DefaultPixelSize;
	const double minimumMapSize = std::max(MinimumCoordinateRange, currentPixelSize * 10.0);

	if (!IsFiniteNonNegative(result.Width()) || !IsFiniteNonNegative(result.Height()))
	{
		return GB_Rectangle::Invalid;
	}

	if (result.Width() <= 0.0)
	{
		result.minX -= minimumMapSize * 0.5;
		result.maxX += minimumMapSize * 0.5;
	}

	if (result.Height() <= 0.0)
	{
		result.minY -= minimumMapSize * 0.5;
		result.maxY += minimumMapSize * 0.5;
	}

	return result;
}

void QMainCanvas::EmitViewExtentDisplayChangedIfNeeded(const GB_Rectangle& previousExtent)
{
	if (!AreRectanglesNearlyEqual(viewExtent, previousExtent))
	{
		emit ViewExtentDisplayChanged(viewExtent);
	}
}

bool QMainCanvas::SetMousePosition(const QPoint& position)
{
	const bool changed = !hasMousePosition || lastMousePosition != position;
	hasMousePosition = true;
	lastMousePosition = position;
	if (changed)
	{
		EmitMousePositionChanged();
	}

	return changed;
}

bool QMainCanvas::ClearMousePosition()
{
	if (!hasMousePosition)
	{
		return false;
	}

	hasMousePosition = false;
	lastMousePosition = QPoint();
	EmitMousePositionChanged();
	return true;
}

void QMainCanvas::EmitMousePositionChanged()
{
	GB_Point2d mouseWorldPosition;
	const bool hasPosition = TryGetCurrentMouseWorldPosition(mouseWorldPosition);
	emit MousePositionChanged(mouseWorldPosition, hasPosition);
}

void QMainCanvas::ScheduleViewStateChanged()
{
	ScheduleViewStateChangedWithDelay(ViewStateChangedDebounceIntervalMs);
}

void QMainCanvas::ScheduleViewStateChangedWithDelay(int debounceIntervalMs)
{
	if (!viewExtent.IsValid())
	{
		return;
	}

	const int safeDebounceIntervalMs = std::max(1, debounceIntervalMs);
	hasPendingViewStateChanged = true;
	viewStateChangedDebounceTimer.start(safeDebounceIntervalMs);
}

void QMainCanvas::ScheduleWheelZoomViewStateChanged()
{
	// Qt::ScrollBegin / Qt::ScrollEnd 并非所有平台都稳定提供。
	// 对没有明确结束事件的平台，用一个更长的单次防抖定时器判定“一次连续缩放”结束。
	ScheduleViewStateChangedWithDelay(WheelZoomEndDetectionIntervalMs);
}

void QMainCanvas::FlushPendingViewStateChanged()
{
	if (!hasPendingViewStateChanged)
	{
		viewStateChangedDebounceTimer.stop();
		return;
	}

	EmitViewStateChanged();
}

void QMainCanvas::EmitViewStateChanged()
{
	hasPendingViewStateChanged = false;
	viewStateChangedDebounceTimer.stop();

	const double approximateMetersPerPixel = CalculateApproximateMetersPerPixel();
	if (!ShouldEmitViewStateChanged(approximateMetersPerPixel))
	{
		return;
	}

	lastEmittedViewExtent = viewExtent;
	lastEmittedApproximateMetersPerPixel = approximateMetersPerPixel;
	hasEmittedViewState = true;

	emit ViewStateChanged(viewExtent, approximateMetersPerPixel);
}

bool QMainCanvas::ShouldEmitViewStateChanged(double approximateMetersPerPixel) const
{
	if (!hasEmittedViewState)
	{
		return true;
	}

	return !AreRectanglesNearlyEqual(viewExtent, lastEmittedViewExtent) ||
		!AreNearlyEqual(approximateMetersPerPixel, lastEmittedApproximateMetersPerPixel);
}

double QMainCanvas::CalculateApproximateMetersPerPixel() const
{
	if (!IsFinitePositive(pixelSize))
	{
		return 0.0;
	}

	if (crsWkt.empty())
	{
		return pixelSize;
	}

	std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromWktCached(crsWkt);
	if (!crs || !crs->IsValid())
	{
		return pixelSize;
	}

	const double metersPerUnit = crs->GetMetersPerUnit();
	if (!IsFinitePositive(metersPerUnit))
	{
		return pixelSize;
	}

	return pixelSize * metersPerUnit;
}

QRectF QMainCanvas::WorldRectangleToScreenRectangle(const GB_Rectangle& rect) const
{
	if (!rect.IsValid())
	{
		return QRectF();
	}

	const GB_Point2d topLeft = WorldToScreen(GB_Point2d(rect.minX, rect.maxY));
	const GB_Point2d bottomRight = WorldToScreen(GB_Point2d(rect.maxX, rect.minY));
	if (!topLeft.IsValid() || !bottomRight.IsValid())
	{
		return QRectF();
	}

	return QRectF(QPointF(topLeft.x, topLeft.y), QPointF(bottomRight.x, bottomRight.y)).normalized();
}
