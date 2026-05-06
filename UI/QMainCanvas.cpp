#include "QMainCanvas.h"
#include "GeoBoundingBox.h"
#include "GeoCrs.h"
#include "GeoCrsManager.h"
#include "GeoBase/GB_Math.h"

#include <QApplication>
#include <QBrush>
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
#include <cstddef>
#include <cstring>
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
	constexpr int MaxVectorBatchPointCount = 8192;
	constexpr double MinimumVectorPaintMarginPixels = 1.0;
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

	void RemoveClosingDuplicatePoint(std::vector<GB_Point2d>& points)
	{
		if (points.size() > 1 && points.front().IsNearEqual(points.back(), GB_Epsilon))
		{
			points.pop_back();
		}
	}

	void ClipPolygonByBoundary(const std::vector<GB_Point2d>& sourcePolygon, const GB_Rectangle& clipRect, ClipBoundary boundary, std::vector<GB_Point2d>& outPolygon)
	{
		outPolygon.clear();
		if (sourcePolygon.empty())
		{
			return;
		}

		outPolygon.reserve(sourcePolygon.size() + 4);
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
					AppendPointIfUseful(outPolygon, intersectionPoint);
				}
			}

			if (currentInside)
			{
				AppendPointIfUseful(outPolygon, currentPoint);
			}

			previousPoint = currentPoint;
			previousInside = currentInside;
		}

		RemoveClosingDuplicatePoint(outPolygon);
	}

	bool ClipPolygonToRectangle(const std::vector<GB_Point2d>& sourcePolygon, const GB_Rectangle& clipRect, std::vector<GB_Point2d>& outPolygon, std::vector<GB_Point2d>& scratchPolygon)
	{
		outPolygon.clear();
		scratchPolygon.clear();
		outPolygon.reserve(sourcePolygon.size());

		for (const GB_Point2d& point : sourcePolygon)
		{
			if (!IsPointFinite(point))
			{
				outPolygon.clear();
				return false;
			}

			AppendPointIfUseful(outPolygon, point);
		}

		RemoveClosingDuplicatePoint(outPolygon);
		if (outPolygon.size() < 3)
		{
			outPolygon.clear();
			return false;
		}

		ClipPolygonByBoundary(outPolygon, clipRect, ClipBoundary::Left, scratchPolygon);
		outPolygon.swap(scratchPolygon);
		if (outPolygon.size() < 3)
		{
			outPolygon.clear();
			return false;
		}

		ClipPolygonByBoundary(outPolygon, clipRect, ClipBoundary::Right, scratchPolygon);
		outPolygon.swap(scratchPolygon);
		if (outPolygon.size() < 3)
		{
			outPolygon.clear();
			return false;
		}

		ClipPolygonByBoundary(outPolygon, clipRect, ClipBoundary::Bottom, scratchPolygon);
		outPolygon.swap(scratchPolygon);
		if (outPolygon.size() < 3)
		{
			outPolygon.clear();
			return false;
		}

		ClipPolygonByBoundary(outPolygon, clipRect, ClipBoundary::Top, scratchPolygon);
		outPolygon.swap(scratchPolygon);
		RemoveClosingDuplicatePoint(outPolygon);

		if (outPolygon.size() < 3)
		{
			outPolygon.clear();
			return false;
		}

		return true;
	}

	bool IsFinitePositive(double value)
	{
		return std::isfinite(value) && value > 0;
	}

	bool IsFiniteNonNegative(double value)
	{
		return std::isfinite(value) && value >= 0;
	}


	GB_Rectangle MakePointExtent(const GB_Point2d& point)
	{
		if (!IsPointFinite(point))
		{
			return GB_Rectangle::Invalid;
		}

		return GB_Rectangle(point);
	}

	GB_Rectangle CalculatePointsExtent(const std::vector<GB_Point2d>& points)
	{
		GB_Rectangle extent;
		extent.Reset();
		for (const GB_Point2d& point : points)
		{
			if (IsPointFinite(point))
			{
				extent.Expand(point);
			}
		}
		return extent;
	}

	double GetPointDrawableScreenMarginPixels(const PointDrawable& point)
	{
		const double symbolSize = std::max(1, point.symbolSize);
		const double borderWidth = std::max(0, point.borderWidth);
		return symbolSize * 0.5 + borderWidth + MinimumVectorPaintMarginPixels;
	}

	double GetPolylineDrawableScreenMarginPixels(const PolylineDrawable& polyline)
	{
		return std::max(0, polyline.lineWidth) * 0.5 + MinimumVectorPaintMarginPixels;
	}

	double GetPolygonDrawableScreenMarginPixels(const PolygonDrawable& polygon)
	{
		return std::max(0, polygon.borderWidth) * 0.5 + MinimumVectorPaintMarginPixels;
	}

	GB_Rectangle ExpandedWorldQueryExtent(const GB_Rectangle& extent, double marginPixels, double currentPixelSize)
	{
		if (!extent.IsValid())
		{
			return GB_Rectangle::Invalid;
		}

		GB_Rectangle result = extent;
		if (std::isfinite(marginPixels) && marginPixels > 0.0 && std::isfinite(currentPixelSize) && currentPixelSize > 0.0)
		{
			const double marginWorld = marginPixels * currentPixelSize;
			result.Buffer(marginWorld, marginWorld);
		}
		return result;
	}

	int ComputeClipOutCode(const GB_Point2d& point, const GB_Rectangle& clipRect)
	{
		int code = 0;
		if (point.x < clipRect.minX)
		{
			code |= 1;
		}
		else if (point.x > clipRect.maxX)
		{
			code |= 2;
		}

		if (point.y < clipRect.minY)
		{
			code |= 4;
		}
		else if (point.y > clipRect.maxY)
		{
			code |= 8;
		}

		return code;
	}

	bool ClipSegmentToRectangle(const GB_Point2d& sourceStart, const GB_Point2d& sourceEnd, const GB_Rectangle& clipRect, GB_Point2d& outStart, GB_Point2d& outEnd)
	{
		outStart = sourceStart;
		outEnd = sourceEnd;

		if (!IsPointFinite(outStart) || !IsPointFinite(outEnd) || !clipRect.IsValid())
		{
			return false;
		}

		int startCode = ComputeClipOutCode(outStart, clipRect);
		int endCode = ComputeClipOutCode(outEnd, clipRect);

		while (true)
		{
			if ((startCode | endCode) == 0)
			{
				return true;
			}

			if ((startCode & endCode) != 0)
			{
				return false;
			}

			const int outsideCode = startCode != 0 ? startCode : endCode;
			double x = 0.0;
			double y = 0.0;

			const double deltaX = outEnd.x - outStart.x;
			const double deltaY = outEnd.y - outStart.y;

			if ((outsideCode & 8) != 0)
			{
				if (std::abs(deltaY) <= GB_Epsilon)
				{
					return false;
				}
				x = outStart.x + deltaX * (clipRect.maxY - outStart.y) / deltaY;
				y = clipRect.maxY;
			}
			else if ((outsideCode & 4) != 0)
			{
				if (std::abs(deltaY) <= GB_Epsilon)
				{
					return false;
				}
				x = outStart.x + deltaX * (clipRect.minY - outStart.y) / deltaY;
				y = clipRect.minY;
			}
			else if ((outsideCode & 2) != 0)
			{
				if (std::abs(deltaX) <= GB_Epsilon)
				{
					return false;
				}
				y = outStart.y + deltaY * (clipRect.maxX - outStart.x) / deltaX;
				x = clipRect.maxX;
			}
			else if ((outsideCode & 1) != 0)
			{
				if (std::abs(deltaX) <= GB_Epsilon)
				{
					return false;
				}
				y = outStart.y + deltaY * (clipRect.minX - outStart.x) / deltaX;
				x = clipRect.minX;
			}

			if (!std::isfinite(x) || !std::isfinite(y))
			{
				return false;
			}

			if (outsideCode == startCode)
			{
				outStart.Set(x, y);
				startCode = ComputeClipOutCode(outStart, clipRect);
			}
			else
			{
				outEnd.Set(x, y);
				endCode = ComputeClipOutCode(outEnd, clipRect);
			}
		}
	}

	bool AreScreenPointsNearlyEqual(const QPointF& firstPoint, const QPointF& secondPoint)
	{
		return std::abs(firstPoint.x() - secondPoint.x()) <= 0.01 && std::abs(firstPoint.y() - secondPoint.y()) <= 0.01;
	}

	int GetSafeVectorReserveCount(size_t pointCount)
	{
		const size_t safeCount = std::min(pointCount, static_cast<size_t>(MaxVectorBatchPointCount));
		return safeCount > static_cast<size_t>(std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max() : static_cast<int>(safeCount);
	}


	template <typename TContainer, typename TGetExtent>
	void RebuildDrawableSpatialIndex(const TContainer& drawables, std::vector<size_t>& minXOrderCache, std::vector<size_t>& maxXOrderCache, TGetExtent getExtent)
	{
		minXOrderCache.clear();
		maxXOrderCache.clear();
		minXOrderCache.reserve(drawables.size());
		maxXOrderCache.reserve(drawables.size());

		for (size_t i = 0; i < drawables.size(); i++)
		{
			const GB_Rectangle& extent = getExtent(drawables[i]);
			if (extent.IsValid())
			{
				minXOrderCache.push_back(i);
				maxXOrderCache.push_back(i);
			}
		}

		std::sort(minXOrderCache.begin(), minXOrderCache.end(), [&drawables, &getExtent](size_t firstIndex, size_t secondIndex) -> bool
			{
				const double firstMinX = getExtent(drawables[firstIndex]).minX;
				const double secondMinX = getExtent(drawables[secondIndex]).minX;
				if (firstMinX != secondMinX)
				{
					return firstMinX < secondMinX;
				}

				return firstIndex < secondIndex;
			});

		std::sort(maxXOrderCache.begin(), maxXOrderCache.end(), [&drawables, &getExtent](size_t firstIndex, size_t secondIndex) -> bool
			{
				const double firstMaxX = getExtent(drawables[firstIndex]).maxX;
				const double secondMaxX = getExtent(drawables[secondIndex]).maxX;
				if (firstMaxX != secondMaxX)
				{
					return firstMaxX < secondMaxX;
				}

				return firstIndex < secondIndex;
			});
	}

	template <typename TContainer, typename TGetExtent, typename TIsVisible>
	void CollectVisibleDrawableIndicesByExtent(const TContainer& drawables, const std::vector<size_t>& minXOrderCache, const std::vector<size_t>& maxXOrderCache,
		const GB_Rectangle& queryWorldExtent, std::vector<size_t>& outIndices, TGetExtent getExtent, TIsVisible isVisible)
	{
		outIndices.clear();
		if (drawables.empty() || !queryWorldExtent.IsValid() || minXOrderCache.empty() || maxXOrderCache.empty())
		{
			return;
		}

		const double queryMinX = queryWorldExtent.minX - GB_Epsilon;
		const double queryMaxX = queryWorldExtent.maxX + GB_Epsilon;

		const auto minXEndIter = std::upper_bound(minXOrderCache.begin(), minXOrderCache.end(), queryMaxX,
			[&drawables, &getExtent](double value, size_t drawableIndex) -> bool
			{
				return value < getExtent(drawables[drawableIndex]).minX;
			});

		const auto maxXBeginIter = std::lower_bound(maxXOrderCache.begin(), maxXOrderCache.end(), queryMinX,
			[&drawables, &getExtent](size_t drawableIndex, double value) -> bool
			{
				return getExtent(drawables[drawableIndex]).maxX < value;
			});

		const size_t minXCandidateCount = static_cast<size_t>(std::distance(minXOrderCache.begin(), minXEndIter));
		const size_t maxXCandidateCount = static_cast<size_t>(std::distance(maxXBeginIter, maxXOrderCache.end()));
		const size_t reserveCount = std::min(minXCandidateCount, maxXCandidateCount);
		outIndices.reserve(reserveCount);

		const auto TryAppendDrawableIndex = [&drawables, &queryWorldExtent, &outIndices, &getExtent, &isVisible](size_t drawableIndex)
			{
				if (drawableIndex >= drawables.size())
				{
					return;
				}

				const auto& drawable = drawables[drawableIndex];
				const GB_Rectangle& extent = getExtent(drawable);
				if (isVisible(drawable) && extent.IsValid() && extent.IsIntersects(queryWorldExtent))
				{
					outIndices.push_back(drawableIndex);
				}
			};

		if (minXCandidateCount <= maxXCandidateCount)
		{
			for (auto iter = minXOrderCache.begin(); iter != minXEndIter; ++iter)
			{
				TryAppendDrawableIndex(*iter);
			}
		}
		else
		{
			for (auto iter = maxXBeginIter; iter != maxXOrderCache.end(); ++iter)
			{
				TryAppendDrawableIndex(*iter);
			}
		}

		if (outIndices.size() > 1)
		{
			std::sort(outIndices.begin(), outIndices.end());
		}
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
	setAttribute(Qt::WA_OpaquePaintEvent, true);

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
	InvalidateCrsValidAreaRectCache();
	InvalidateCrsMetersPerUnitCache();
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
	if (AreRectanglesNearlyEqual(viewExtent, previousExtent))
	{
		return;
	}

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

	if (TryGetCachedCrsValidAreaPolygonsExtent(outExtent))
	{
		return true;
	}

	return TryGetCrsValidArea(outExtent);
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
	GB_Rectangle allExtent;
	allExtent.Reset();

	bool hasAppliedCrsClipForZoom = false;
	if (clipMapTilesToCrsValidArea)
	{
		GB_Rectangle crsValidAreaPolygonsExtent;
		const bool hasCrsValidAreaPolygons = TryGetCachedCrsValidAreaPolygonsExtent(crsValidAreaPolygonsExtent);
		if (hasCrsValidAreaPolygons)
		{
			hasAppliedCrsClipForZoom = true;
			for (const CachedMapTile& item : mapTiles)
			{
				if (!item.tile.visible || !item.tile.extent.IsValid())
				{
					continue;
				}

				GB_Rectangle clippedTileExtent;
				if (TryIntersectRectangleWithCrsValidAreaPolygons(item.tile.extent, clippedTileExtent))
				{
					allExtent.Expand(clippedTileExtent);
				}
			}

			if (!allExtent.IsValid())
			{
				allExtent = crsValidAreaPolygonsExtent;
			}
		}
		else
		{
			GB_Rectangle crsValidArea;
			const bool hasCrsValidArea = TryGetCrsValidArea(crsValidArea);
			if (hasCrsValidArea)
			{
				hasAppliedCrsClipForZoom = true;
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
		}
	}

	if (hasAppliedCrsClipForZoom)
	{
		const GB_Rectangle vectorDrawableExtent = CalculateVisibleVectorDrawableExtent();
		if (vectorDrawableExtent.IsValid())
		{
			allExtent.Expand(vectorDrawableExtent);
		}
	}

	if (!hasAppliedCrsClipForZoom)
	{
		allExtent = CalculateAllDrawableExtent();
		if (!allExtent.IsValid())
		{
			GB_Rectangle crsValidAreaPolygonsExtent;
			if (TryGetCrsValidAreaPolygonsExtent(crsValidAreaPolygonsExtent))
			{
				allExtent = crsValidAreaPolygonsExtent;
			}
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

void QMainCanvas::AddMapTile(const MapTileDrawable& tile)
{
	AddMapTile(tile, tile.layerNumber);
}

void QMainCanvas::AddMapTile(const MapTileDrawable& tile, double layerNumber)
{
	CachedMapTile cachedTile;
	if (!TryCreateCachedMapTile(tile, layerNumber, cachedTile))
	{
		return;
	}

	const bool shouldZoomToAddedTile = !viewExtent.IsValid();
	const GB_Rectangle addedExtent = cachedTile.tile.extent;

	InsertCachedMapTile(std::move(cachedTile));
	InvalidateMapTileSpatialIndex();
	InvalidateAllDrawableExtentCache();
	InvalidateMapContentCache();

	if (shouldZoomToAddedTile && addedExtent.IsValid())
	{
		ZoomToExtent(addedExtent, 0.05);
	}
	else
	{
		update();
	}
}

void QMainCanvas::AddMapTiles(const std::vector<MapTileDrawable>& tiles)
{
	if (tiles.empty())
	{
		return;
	}

	std::vector<CachedMapTile> cachedTiles;
	cachedTiles.reserve(tiles.size());

	for (const MapTileDrawable& tile : tiles)
	{
		CachedMapTile cachedTile;
		if (TryCreateCachedMapTile(tile, tile.layerNumber, cachedTile))
		{
			cachedTiles.push_back(std::move(cachedTile));
		}
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

	std::sort(cachedTiles.begin(), cachedTiles.end(), IsCachedMapTilePaintOrderLess);

	const size_t oldMapTileCount = mapTiles.size();
	mapTiles.reserve(mapTiles.size() + cachedTiles.size());
	for (CachedMapTile& cachedTile : cachedTiles)
	{
		mapTiles.push_back(std::move(cachedTile));
	}

	if (oldMapTileCount > 0)
	{
		std::inplace_merge(mapTiles.begin(), mapTiles.begin() + static_cast<std::ptrdiff_t>(oldMapTileCount), mapTiles.end(), IsCachedMapTilePaintOrderLess);
	}

	InvalidateMapTileSpatialIndex();
	InvalidateAllDrawableExtentCache();
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

void QMainCanvas::AddPointDrawable(const PointDrawable& point)
{
	AddPointDrawable(point, point.layerNumber);
}

void QMainCanvas::AddPointDrawable(const PointDrawable& point, double layerNumber)
{
	PointDrawable pointCopy = point;
	AddPointDrawable(std::move(pointCopy), layerNumber);
}

void QMainCanvas::AddPointDrawable(PointDrawable&& point)
{
	const double layerNumber = point.layerNumber;
	AddPointDrawable(std::move(point), layerNumber);
}

void QMainCanvas::AddPointDrawable(PointDrawable&& point, double layerNumber)
{
	CachedPointDrawable cachedPoint;
	if (!TryCreateCachedPointDrawable(std::move(point), layerNumber, cachedPoint))
	{
		return;
	}

	const bool shouldZoomToAddedPoint = !viewExtent.IsValid();
	const GB_Rectangle addedExtent = cachedPoint.extent;

	pointDrawables.push_back(std::move(cachedPoint));
	InvalidatePointDrawableSpatialIndex();
	pointDrawableMaxScreenMarginCacheDirty = true;
	InvalidateAllDrawableExtentCache();
	InvalidateMapContentCache();

	if (shouldZoomToAddedPoint && addedExtent.IsValid())
	{
		ZoomToExtent(addedExtent, 0.05);
	}
	else
	{
		update();
	}
}

void QMainCanvas::AddPointDrawables(const std::vector<PointDrawable>& points)
{
	if (points.empty())
	{
		return;
	}

	std::vector<PointDrawable> pointCopies = points;
	AddPointDrawables(std::move(pointCopies));
}

void QMainCanvas::AddPointDrawables(std::vector<PointDrawable>&& points)
{
	if (points.empty())
	{
		return;
	}

	std::vector<CachedPointDrawable> cachedPoints;
	cachedPoints.reserve(points.size());
	GB_Rectangle addedExtent;
	addedExtent.Reset();

	for (PointDrawable& point : points)
	{
		const double layerNumber = point.layerNumber;
		CachedPointDrawable cachedPoint;
		if (TryCreateCachedPointDrawable(std::move(point), layerNumber, cachedPoint))
		{
			addedExtent.Expand(cachedPoint.extent);
			cachedPoints.push_back(std::move(cachedPoint));
		}
	}

	if (cachedPoints.empty())
	{
		return;
	}

	const bool shouldZoomToAddedPoints = !viewExtent.IsValid();
	pointDrawables.reserve(pointDrawables.size() + cachedPoints.size());
	for (CachedPointDrawable& cachedPoint : cachedPoints)
	{
		pointDrawables.push_back(std::move(cachedPoint));
	}

	InvalidatePointDrawableSpatialIndex();
	pointDrawableMaxScreenMarginCacheDirty = true;
	InvalidateAllDrawableExtentCache();
	InvalidateMapContentCache();

	if (shouldZoomToAddedPoints && addedExtent.IsValid())
	{
		ZoomToExtent(addedExtent, 0.05);
	}
	else
	{
		update();
	}
}

void QMainCanvas::AddPolylineDrawable(const PolylineDrawable& polyline)
{
	AddPolylineDrawable(polyline, polyline.layerNumber);
}

void QMainCanvas::AddPolylineDrawable(const PolylineDrawable& polyline, double layerNumber)
{
	PolylineDrawable polylineCopy = polyline;
	AddPolylineDrawable(std::move(polylineCopy), layerNumber);
}

void QMainCanvas::AddPolylineDrawable(PolylineDrawable&& polyline)
{
	const double layerNumber = polyline.layerNumber;
	AddPolylineDrawable(std::move(polyline), layerNumber);
}

void QMainCanvas::AddPolylineDrawable(PolylineDrawable&& polyline, double layerNumber)
{
	CachedPolylineDrawable cachedPolyline;
	if (!TryCreateCachedPolylineDrawable(std::move(polyline), layerNumber, cachedPolyline))
	{
		return;
	}

	const bool shouldZoomToAddedPolyline = !viewExtent.IsValid();
	const GB_Rectangle addedExtent = cachedPolyline.extent;

	polylineDrawables.push_back(std::move(cachedPolyline));
	InvalidatePolylineDrawableSpatialIndex();
	polylineDrawableMaxScreenMarginCacheDirty = true;
	InvalidateAllDrawableExtentCache();
	InvalidateMapContentCache();

	if (shouldZoomToAddedPolyline && addedExtent.IsValid())
	{
		ZoomToExtent(addedExtent, 0.05);
	}
	else
	{
		update();
	}
}

void QMainCanvas::AddPolylineDrawables(const std::vector<PolylineDrawable>& polylines)
{
	if (polylines.empty())
	{
		return;
	}

	std::vector<PolylineDrawable> polylineCopies = polylines;
	AddPolylineDrawables(std::move(polylineCopies));
}

void QMainCanvas::AddPolylineDrawables(std::vector<PolylineDrawable>&& polylines)
{
	if (polylines.empty())
	{
		return;
	}

	std::vector<CachedPolylineDrawable> cachedPolylines;
	cachedPolylines.reserve(polylines.size());
	GB_Rectangle addedExtent;
	addedExtent.Reset();

	for (PolylineDrawable& polyline : polylines)
	{
		const double layerNumber = polyline.layerNumber;
		CachedPolylineDrawable cachedPolyline;
		if (TryCreateCachedPolylineDrawable(std::move(polyline), layerNumber, cachedPolyline))
		{
			addedExtent.Expand(cachedPolyline.extent);
			cachedPolylines.push_back(std::move(cachedPolyline));
		}
	}

	if (cachedPolylines.empty())
	{
		return;
	}

	const bool shouldZoomToAddedPolylines = !viewExtent.IsValid();
	polylineDrawables.reserve(polylineDrawables.size() + cachedPolylines.size());
	for (CachedPolylineDrawable& cachedPolyline : cachedPolylines)
	{
		polylineDrawables.push_back(std::move(cachedPolyline));
	}

	InvalidatePolylineDrawableSpatialIndex();
	polylineDrawableMaxScreenMarginCacheDirty = true;
	InvalidateAllDrawableExtentCache();
	InvalidateMapContentCache();

	if (shouldZoomToAddedPolylines && addedExtent.IsValid())
	{
		ZoomToExtent(addedExtent, 0.05);
	}
	else
	{
		update();
	}
}

void QMainCanvas::AddPolygonDrawable(const PolygonDrawable& polygon)
{
	AddPolygonDrawable(polygon, polygon.layerNumber);
}

void QMainCanvas::AddPolygonDrawable(const PolygonDrawable& polygon, double layerNumber)
{
	PolygonDrawable polygonCopy = polygon;
	AddPolygonDrawable(std::move(polygonCopy), layerNumber);
}

void QMainCanvas::AddPolygonDrawable(PolygonDrawable&& polygon)
{
	const double layerNumber = polygon.layerNumber;
	AddPolygonDrawable(std::move(polygon), layerNumber);
}

void QMainCanvas::AddPolygonDrawable(PolygonDrawable&& polygon, double layerNumber)
{
	CachedPolygonDrawable cachedPolygon;
	if (!TryCreateCachedPolygonDrawable(std::move(polygon), layerNumber, cachedPolygon))
	{
		return;
	}

	const bool shouldZoomToAddedPolygon = !viewExtent.IsValid();
	const GB_Rectangle addedExtent = cachedPolygon.extent;

	polygonDrawables.push_back(std::move(cachedPolygon));
	InvalidatePolygonDrawableSpatialIndex();
	polygonDrawableMaxScreenMarginCacheDirty = true;
	InvalidateAllDrawableExtentCache();
	InvalidateMapContentCache();

	if (shouldZoomToAddedPolygon && addedExtent.IsValid())
	{
		ZoomToExtent(addedExtent, 0.05);
	}
	else
	{
		update();
	}
}

void QMainCanvas::AddPolygonDrawables(const std::vector<PolygonDrawable>& polygons)
{
	if (polygons.empty())
	{
		return;
	}

	std::vector<PolygonDrawable> polygonCopies = polygons;
	AddPolygonDrawables(std::move(polygonCopies));
}

void QMainCanvas::AddPolygonDrawables(std::vector<PolygonDrawable>&& polygons)
{
	if (polygons.empty())
	{
		return;
	}

	std::vector<CachedPolygonDrawable> cachedPolygons;
	cachedPolygons.reserve(polygons.size());
	GB_Rectangle addedExtent;
	addedExtent.Reset();

	for (PolygonDrawable& polygon : polygons)
	{
		const double layerNumber = polygon.layerNumber;
		CachedPolygonDrawable cachedPolygon;
		if (TryCreateCachedPolygonDrawable(std::move(polygon), layerNumber, cachedPolygon))
		{
			addedExtent.Expand(cachedPolygon.extent);
			cachedPolygons.push_back(std::move(cachedPolygon));
		}
	}

	if (cachedPolygons.empty())
	{
		return;
	}

	const bool shouldZoomToAddedPolygons = !viewExtent.IsValid();
	polygonDrawables.reserve(polygonDrawables.size() + cachedPolygons.size());
	for (CachedPolygonDrawable& cachedPolygon : cachedPolygons)
	{
		polygonDrawables.push_back(std::move(cachedPolygon));
	}

	InvalidatePolygonDrawableSpatialIndex();
	polygonDrawableMaxScreenMarginCacheDirty = true;
	InvalidateAllDrawableExtentCache();
	InvalidateMapContentCache();

	if (shouldZoomToAddedPolygons && addedExtent.IsValid())
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
	return !mapTiles.empty() || !pointDrawables.empty() || !polylineDrawables.empty() || !polygonDrawables.empty();
}

void QMainCanvas::ClearDrawables()
{
	const bool hadDrawables = HasDrawables();
	mapTiles.clear();
	pointDrawables.clear();
	polylineDrawables.clear();
	polygonDrawables.clear();
	InvalidateMapTileSpatialIndex();
	InvalidateVectorDrawableSpatialIndexes();
	InvalidateVectorDrawableMaxScreenMarginCaches();
	InvalidateAllDrawableExtentCache();
	InvalidateMapContentCache();
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
	bool mapTilesChanged = false;
	bool pointsChanged = false;
	bool polylinesChanged = false;
	bool polygonsChanged = false;

	const size_t oldMapTileCount = mapTiles.size();
	mapTiles.erase(std::remove_if(mapTiles.begin(), mapTiles.end(), [&uidSet](const CachedMapTile& item) {
		return !item.tile.uid.empty() && uidSet.find(item.tile.uid) != uidSet.end();
		}), mapTiles.end());
	mapTilesChanged = oldMapTileCount != mapTiles.size();
	hasChanged = hasChanged || mapTilesChanged;

	const size_t oldPointCount = pointDrawables.size();
	pointDrawables.erase(std::remove_if(pointDrawables.begin(), pointDrawables.end(), [&uidSet](const CachedPointDrawable& item) {
		return !item.point.uid.empty() && uidSet.find(item.point.uid) != uidSet.end();
		}), pointDrawables.end());
	pointsChanged = oldPointCount != pointDrawables.size();
	hasChanged = hasChanged || pointsChanged;

	const size_t oldPolylineCount = polylineDrawables.size();
	polylineDrawables.erase(std::remove_if(polylineDrawables.begin(), polylineDrawables.end(), [&uidSet](const CachedPolylineDrawable& item) {
		return !item.polyline.uid.empty() && uidSet.find(item.polyline.uid) != uidSet.end();
		}), polylineDrawables.end());
	polylinesChanged = oldPolylineCount != polylineDrawables.size();
	hasChanged = hasChanged || polylinesChanged;

	const size_t oldPolygonCount = polygonDrawables.size();
	polygonDrawables.erase(std::remove_if(polygonDrawables.begin(), polygonDrawables.end(), [&uidSet](const CachedPolygonDrawable& item) {
		return !item.polygon.uid.empty() && uidSet.find(item.polygon.uid) != uidSet.end();
		}), polygonDrawables.end());
	polygonsChanged = oldPolygonCount != polygonDrawables.size();
	hasChanged = hasChanged || polygonsChanged;

	if (mapTilesChanged)
	{
		InvalidateMapTileSpatialIndex();
	}
	if (pointsChanged)
	{
		InvalidatePointDrawableSpatialIndex();
		pointDrawableMaxScreenMarginCacheDirty = true;
	}
	if (polylinesChanged)
	{
		InvalidatePolylineDrawableSpatialIndex();
		polylineDrawableMaxScreenMarginCacheDirty = true;
	}
	if (polygonsChanged)
	{
		InvalidatePolygonDrawableSpatialIndex();
		polygonDrawableMaxScreenMarginCacheDirty = true;
	}

	if (hasChanged)
	{
		InvalidateAllDrawableExtentCache();
		InvalidateMapContentCache();
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
	bool pointVisibilityChanged = false;
	bool polylineVisibilityChanged = false;
	bool polygonVisibilityChanged = false;

	for (CachedMapTile& item : mapTiles)
	{
		if (!item.tile.uid.empty() && uidSet.find(item.tile.uid) != uidSet.end() && item.tile.visible != visible)
		{
			item.tile.visible = visible;
			hasChanged = true;
		}
	}

	for (CachedPointDrawable& item : pointDrawables)
	{
		if (!item.point.uid.empty() && uidSet.find(item.point.uid) != uidSet.end() && item.point.visible != visible)
		{
			item.point.visible = visible;
			hasChanged = true;
			pointVisibilityChanged = true;
		}
	}

	for (CachedPolylineDrawable& item : polylineDrawables)
	{
		if (!item.polyline.uid.empty() && uidSet.find(item.polyline.uid) != uidSet.end() && item.polyline.visible != visible)
		{
			item.polyline.visible = visible;
			hasChanged = true;
			polylineVisibilityChanged = true;
		}
	}

	for (CachedPolygonDrawable& item : polygonDrawables)
	{
		if (!item.polygon.uid.empty() && uidSet.find(item.polygon.uid) != uidSet.end() && item.polygon.visible != visible)
		{
			item.polygon.visible = visible;
			hasChanged = true;
			polygonVisibilityChanged = true;
		}
	}

	if (pointVisibilityChanged)
	{
		pointDrawableMaxScreenMarginCacheDirty = true;
	}
	if (polylineVisibilityChanged)
	{
		polylineDrawableMaxScreenMarginCacheDirty = true;
	}
	if (polygonVisibilityChanged)
	{
		polygonDrawableMaxScreenMarginCacheDirty = true;
	}

	if (hasChanged)
	{
		InvalidateAllDrawableExtentCache();
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
	bool mapTileLayerChanged = false;

	for (CachedMapTile& item : mapTiles)
	{
		if (!item.tile.uid.empty() && uidSet.find(item.tile.uid) != uidSet.end() && item.tile.layerNumber != normalizedLayerNumber)
		{
			item.tile.layerNumber = normalizedLayerNumber;
			hasChanged = true;
			mapTileLayerChanged = true;
		}
	}

	for (CachedPointDrawable& item : pointDrawables)
	{
		if (!item.point.uid.empty() && uidSet.find(item.point.uid) != uidSet.end() && item.point.layerNumber != normalizedLayerNumber)
		{
			item.point.layerNumber = normalizedLayerNumber;
			hasChanged = true;
		}
	}

	for (CachedPolylineDrawable& item : polylineDrawables)
	{
		if (!item.polyline.uid.empty() && uidSet.find(item.polyline.uid) != uidSet.end() && item.polyline.layerNumber != normalizedLayerNumber)
		{
			item.polyline.layerNumber = normalizedLayerNumber;
			hasChanged = true;
		}
	}

	for (CachedPolygonDrawable& item : polygonDrawables)
	{
		if (!item.polygon.uid.empty() && uidSet.find(item.polygon.uid) != uidSet.end() && item.polygon.layerNumber != normalizedLayerNumber)
		{
			item.polygon.layerNumber = normalizedLayerNumber;
			hasChanged = true;
		}
	}

	if (hasChanged)
	{
		if (mapTileLayerChanged)
		{
			std::sort(mapTiles.begin(), mapTiles.end(), IsCachedMapTilePaintOrderLess);
			InvalidateMapTileSpatialIndex();
		}
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
		if (!hasPanPreview)
		{
			InvalidateMapContentCache();
		}
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

	const QByteArray jsonBytes = event->mimeData()->data(GetServiceNodeMimeType());
	const QJsonDocument document = QJsonDocument::fromJson(jsonBytes);
	const QJsonObject object = document.isObject() ? document.object() : QJsonObject();

	const auto ReadStringValue = [&object](const char* key) -> QString
		{
			const QJsonValue value = object.value(QString::fromLatin1(key));
			return value.isString() ? value.toString() : QString();
		};

	const auto ReadIntValue = [&object](const char* key, int defaultValue) -> int
		{
			const QJsonValue value = object.value(QString::fromLatin1(key));
			return value.isDouble() ? value.toInt(defaultValue) : defaultValue;
		};

	const QString nodeUid = ReadStringValue("uid");
	const QString url = ReadStringValue("url");
	const QString text = ReadStringValue("text");
	const int nodeType = ReadIntValue("nodeType", 0);

	emit LayerDropRequested(nodeUid, url, text, nodeType);
	event->acceptProposedAction();
}

QImage QMainCanvas::CreateQImageFromGBImage(const GB_Image& image) const
{
	if (image.IsEmpty() || image.GetWidth() == 0 || image.GetHeight() == 0)
	{
		return QImage();
	}

	const int channels = image.GetChannels();
	if (image.GetDepth() != GB_ImageDepth::UInt8 || (channels != 1 && channels != 3 && channels != 4))
	{
		return QImage();
	}

	if (image.GetWidth() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
		image.GetHeight() > static_cast<size_t>(std::numeric_limits<int>::max()))
	{
		return QImage();
	}

	const int imageWidth = static_cast<int>(image.GetWidth());
	const int imageHeight = static_cast<int>(image.GetHeight());

	if (channels == 1)
	{
		QImage result(imageWidth, imageHeight, QImage::Format_Grayscale8);
		if (result.isNull())
		{
			return QImage();
		}

		const size_t sourceRowStrideBytes = image.GetRowStrideBytes();
		if (sourceRowStrideBytes < static_cast<size_t>(imageWidth))
		{
			return QImage();
		}

		for (int row = 0; row < imageHeight; row++)
		{
			const unsigned char* sourceRow = image.GetRowData(static_cast<size_t>(row));
			unsigned char* targetRow = result.scanLine(row);
			if (sourceRow == nullptr || targetRow == nullptr)
			{
				return QImage();
			}

			std::memcpy(targetRow, sourceRow, static_cast<size_t>(imageWidth));
		}

		return result;
	}

	QImage result(imageWidth, imageHeight, QImage::Format_ARGB32);
	if (result.isNull())
	{
		return QImage();
	}

	for (int row = 0; row < imageHeight; row++)
	{
		QRgb* scanLine = reinterpret_cast<QRgb*>(result.scanLine(row));
		if (scanLine == nullptr)
		{
			return QImage();
		}

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

bool QMainCanvas::IsCachedDrawablePaintOrderLess(double firstLayerNumber, std::uint64_t firstInsertionSequence, double secondLayerNumber, std::uint64_t secondInsertionSequence)
{
	const int firstGroup = GetLayerPaintOrderGroup(firstLayerNumber);
	const int secondGroup = GetLayerPaintOrderGroup(secondLayerNumber);
	if (firstGroup != secondGroup)
	{
		return firstGroup < secondGroup;
	}

	if (firstGroup == 1 && firstLayerNumber != secondLayerNumber)
	{
		// QPainter 后绘制的内容会覆盖先绘制的内容。
		// 普通层号越大越靠底层，因此需要先绘制较大的层号。
		return firstLayerNumber > secondLayerNumber;
	}

	return firstInsertionSequence < secondInsertionSequence;
}

bool QMainCanvas::IsCachedMapTilePaintOrderLess(const CachedMapTile& firstTile, const CachedMapTile& secondTile)
{
	return IsCachedDrawablePaintOrderLess(firstTile.tile.layerNumber, firstTile.insertionSequence, secondTile.tile.layerNumber, secondTile.insertionSequence);
}

bool QMainCanvas::IsVisibleDrawableReferencePaintOrderLess(const VisibleDrawableReference& firstReference, const VisibleDrawableReference& secondReference) const
{
	return IsCachedDrawablePaintOrderLess(GetVisibleDrawableReferenceLayerNumber(firstReference), GetVisibleDrawableReferenceInsertionSequence(firstReference),
		GetVisibleDrawableReferenceLayerNumber(secondReference), GetVisibleDrawableReferenceInsertionSequence(secondReference));
}

double QMainCanvas::GetVisibleDrawableReferenceLayerNumber(const VisibleDrawableReference& reference) const
{
	switch (reference.kind)
	{
	case CachedDrawableKind::MapTile:
		return reference.index < mapTiles.size() ? mapTiles[reference.index].tile.layerNumber : GetDefaultLayerNumber();
	case CachedDrawableKind::Point:
		return reference.index < pointDrawables.size() ? pointDrawables[reference.index].point.layerNumber : GetDefaultLayerNumber();
	case CachedDrawableKind::Polyline:
		return reference.index < polylineDrawables.size() ? polylineDrawables[reference.index].polyline.layerNumber : GetDefaultLayerNumber();
	case CachedDrawableKind::Polygon:
		return reference.index < polygonDrawables.size() ? polygonDrawables[reference.index].polygon.layerNumber : GetDefaultLayerNumber();
	default:
		return GetDefaultLayerNumber();
	}
}

std::uint64_t QMainCanvas::GetVisibleDrawableReferenceInsertionSequence(const VisibleDrawableReference& reference) const
{
	switch (reference.kind)
	{
	case CachedDrawableKind::MapTile:
		return reference.index < mapTiles.size() ? mapTiles[reference.index].insertionSequence : 0;
	case CachedDrawableKind::Point:
		return reference.index < pointDrawables.size() ? pointDrawables[reference.index].insertionSequence : 0;
	case CachedDrawableKind::Polyline:
		return reference.index < polylineDrawables.size() ? polylineDrawables[reference.index].insertionSequence : 0;
	case CachedDrawableKind::Polygon:
		return reference.index < polygonDrawables.size() ? polygonDrawables[reference.index].insertionSequence : 0;
	default:
		return 0;
	}
}

bool QMainCanvas::TryCreateCachedMapTile(const MapTileDrawable& tile, double layerNumber, CachedMapTile& outCachedTile)
{
	outCachedTile = CachedMapTile();

	if (!tile.extent.IsValid() || tile.image.IsEmpty())
	{
		return false;
	}

	CachedMapTile cachedTile;
	cachedTile.tile = tile;
	cachedTile.tile.layerNumber = NormalizeLayerNumber(layerNumber);
	if (cachedTile.tile.uid.empty())
	{
		cachedTile.tile.uid = cachedTile.tile.CalculateUid();
	}

	cachedTile.tileExtentWidth = cachedTile.tile.extent.Width();
	cachedTile.tileExtentHeight = cachedTile.tile.extent.Height();
	if (!IsFinitePositive(cachedTile.tileExtentWidth) || !IsFinitePositive(cachedTile.tileExtentHeight))
	{
		return false;
	}

	cachedTile.pixmap = CreateQPixmapFromGBImage(cachedTile.tile.image);
	if (cachedTile.pixmap.isNull())
	{
		return false;
	}

	cachedTile.pixmapWidth = static_cast<double>(cachedTile.pixmap.width());
	cachedTile.pixmapHeight = static_cast<double>(cachedTile.pixmap.height());
	if (!IsFinitePositive(cachedTile.pixmapWidth) || !IsFinitePositive(cachedTile.pixmapHeight))
	{
		return false;
	}

	// 绘制阶段只依赖已经上传到 GUI 侧的 QPixmap。
	// 清理 CachedMapTile 中的 GB_Image 句柄，避免每个瓦片同时长期持有 CPU 侧原始图像与 QPixmap。
	// 对大瓦片/多瓦片场景，这可以显著降低内存压力。
	cachedTile.tile.image.Clear();

	cachedTile.inverseTileExtentWidth = 1.0 / cachedTile.tileExtentWidth;
	cachedTile.inverseTileExtentHeight = 1.0 / cachedTile.tileExtentHeight;
	cachedTile.insertionSequence = nextDrawableInsertionSequence;
	nextDrawableInsertionSequence++;

	outCachedTile = std::move(cachedTile);
	return true;
}

bool QMainCanvas::TryCreateCachedPointDrawable(PointDrawable point, double layerNumber, CachedPointDrawable& outCachedPoint)
{
	outCachedPoint = CachedPointDrawable();
	if (!IsPointFinite(point.position))
	{
		return false;
	}

	if (point.symbolSize <= 0 || (point.fillColor.IsTransparent() && (point.borderWidth <= 0 || point.borderColor.IsTransparent())))
	{
		return false;
	}

	CachedPointDrawable cachedPoint;
	cachedPoint.point = std::move(point);
	cachedPoint.point.layerNumber = NormalizeLayerNumber(layerNumber);
	if (cachedPoint.point.uid.empty())
	{
		cachedPoint.point.uid = cachedPoint.point.CalculateUid();
	}

	cachedPoint.extent = MakePointExtent(cachedPoint.point.position);
	if (!cachedPoint.extent.IsValid())
	{
		return false;
	}

	cachedPoint.screenMarginPixels = GetPointDrawableScreenMarginPixels(cachedPoint.point);
	cachedPoint.insertionSequence = nextDrawableInsertionSequence;
	nextDrawableInsertionSequence++;
	outCachedPoint = std::move(cachedPoint);
	return true;
}

bool QMainCanvas::TryCreateCachedPolylineDrawable(PolylineDrawable polyline, double layerNumber, CachedPolylineDrawable& outCachedPolyline)
{
	outCachedPolyline = CachedPolylineDrawable();
	if (polyline.vertices.size() < 2 || polyline.lineWidth <= 0 || polyline.lineColor.IsTransparent())
	{
		return false;
	}

	GB_Rectangle extent = CalculatePointsExtent(polyline.vertices);
	if (!extent.IsValid())
	{
		return false;
	}

	CachedPolylineDrawable cachedPolyline;
	cachedPolyline.polyline = std::move(polyline);
	cachedPolyline.polyline.layerNumber = NormalizeLayerNumber(layerNumber);
	if (cachedPolyline.polyline.uid.empty())
	{
		cachedPolyline.polyline.uid = cachedPolyline.polyline.CalculateUid();
	}

	cachedPolyline.extent = extent;
	cachedPolyline.screenMarginPixels = GetPolylineDrawableScreenMarginPixels(cachedPolyline.polyline);
	cachedPolyline.insertionSequence = nextDrawableInsertionSequence;
	nextDrawableInsertionSequence++;
	outCachedPolyline = std::move(cachedPolyline);
	return true;
}

bool QMainCanvas::TryCreateCachedPolygonDrawable(PolygonDrawable polygon, double layerNumber, CachedPolygonDrawable& outCachedPolygon)
{
	outCachedPolygon = CachedPolygonDrawable();
	if (polygon.vertices.size() < 3)
	{
		return false;
	}

	const bool canFill = !polygon.fillColor.IsTransparent();
	const bool canDrawBorder = polygon.borderWidth > 0 && !polygon.borderColor.IsTransparent();
	if (!canFill && !canDrawBorder)
	{
		return false;
	}

	GB_Rectangle extent = CalculatePointsExtent(polygon.vertices);
	if (!extent.IsValid())
	{
		return false;
	}

	CachedPolygonDrawable cachedPolygon;
	cachedPolygon.polygon = std::move(polygon);
	cachedPolygon.polygon.layerNumber = NormalizeLayerNumber(layerNumber);
	if (cachedPolygon.polygon.uid.empty())
	{
		cachedPolygon.polygon.uid = cachedPolygon.polygon.CalculateUid();
	}

	cachedPolygon.extent = extent;
	cachedPolygon.screenMarginPixels = GetPolygonDrawableScreenMarginPixels(cachedPolygon.polygon);
	cachedPolygon.insertionSequence = nextDrawableInsertionSequence;
	nextDrawableInsertionSequence++;
	outCachedPolygon = std::move(cachedPolygon);
	return true;
}

void QMainCanvas::InsertCachedMapTile(CachedMapTile&& cachedTile)
{
	const auto insertIter = std::lower_bound(mapTiles.begin(), mapTiles.end(), cachedTile, IsCachedMapTilePaintOrderLess);
	mapTiles.insert(insertIter, std::move(cachedTile));
}

void QMainCanvas::EnsureMapTileSpatialIndex() const
{
	if (!mapTileSpatialIndexDirty)
	{
		return;
	}

	mapTileMinXOrderCache.clear();
	mapTileMaxXOrderCache.clear();
	mapTileMinXOrderCache.reserve(mapTiles.size());
	mapTileMaxXOrderCache.reserve(mapTiles.size());

	for (size_t i = 0; i < mapTiles.size(); i++)
	{
		const CachedMapTile& cachedTile = mapTiles[i];
		if (cachedTile.tile.extent.IsValid())
		{
			mapTileMinXOrderCache.push_back(i);
			mapTileMaxXOrderCache.push_back(i);
		}
	}

	std::sort(mapTileMinXOrderCache.begin(), mapTileMinXOrderCache.end(), [this](size_t firstIndex, size_t secondIndex) -> bool
		{
			const double firstMinX = mapTiles[firstIndex].tile.extent.minX;
			const double secondMinX = mapTiles[secondIndex].tile.extent.minX;
			if (firstMinX != secondMinX)
			{
				return firstMinX < secondMinX;
			}

			return firstIndex < secondIndex;
		});

	std::sort(mapTileMaxXOrderCache.begin(), mapTileMaxXOrderCache.end(), [this](size_t firstIndex, size_t secondIndex) -> bool
		{
			const double firstMaxX = mapTiles[firstIndex].tile.extent.maxX;
			const double secondMaxX = mapTiles[secondIndex].tile.extent.maxX;
			if (firstMaxX != secondMaxX)
			{
				return firstMaxX < secondMaxX;
			}

			return firstIndex < secondIndex;
		});

	mapTileSpatialIndexDirty = false;
}

void QMainCanvas::InvalidateMapTileSpatialIndex() const
{
	mapTileSpatialIndexDirty = true;
	mapTileMinXOrderCache.clear();
	mapTileMaxXOrderCache.clear();
	mapTileVisibleIndexScratch.clear();
}

void QMainCanvas::CollectVisibleMapTileIndices(const GB_Rectangle& queryWorldExtent, std::vector<size_t>& outIndices) const
{
	outIndices.clear();

	if (mapTiles.empty() || !queryWorldExtent.IsValid())
	{
		return;
	}

	EnsureMapTileSpatialIndex();
	if (mapTileMinXOrderCache.empty() || mapTileMaxXOrderCache.empty())
	{
		return;
	}

	const double queryMinX = queryWorldExtent.minX - GB_Epsilon;
	const double queryMaxX = queryWorldExtent.maxX + GB_Epsilon;

	const auto minXEndIter = std::upper_bound(mapTileMinXOrderCache.begin(), mapTileMinXOrderCache.end(), queryMaxX,
		[this](double value, size_t tileIndex) -> bool
		{
			return value < mapTiles[tileIndex].tile.extent.minX;
		});

	const auto maxXBeginIter = std::lower_bound(mapTileMaxXOrderCache.begin(), mapTileMaxXOrderCache.end(), queryMinX,
		[this](size_t tileIndex, double value) -> bool
		{
			return mapTiles[tileIndex].tile.extent.maxX < value;
		});

	const size_t minXCandidateCount = static_cast<size_t>(std::distance(mapTileMinXOrderCache.begin(), minXEndIter));
	const size_t maxXCandidateCount = static_cast<size_t>(std::distance(maxXBeginIter, mapTileMaxXOrderCache.end()));
	const size_t reserveCount = std::min(minXCandidateCount, maxXCandidateCount);
	outIndices.reserve(reserveCount);

	const auto TryAppendTileIndex = [this, &queryWorldExtent, &outIndices](size_t tileIndex)
		{
			if (tileIndex >= mapTiles.size())
			{
				return;
			}

			const CachedMapTile& cachedTile = mapTiles[tileIndex];
			if (!cachedTile.tile.visible || cachedTile.pixmap.isNull() || !cachedTile.tile.extent.IsValid())
			{
				return;
			}

			if (cachedTile.tile.extent.IsIntersects(queryWorldExtent))
			{
				outIndices.push_back(tileIndex);
			}
		};

	if (minXCandidateCount <= maxXCandidateCount)
	{
		for (auto iter = mapTileMinXOrderCache.begin(); iter != minXEndIter; ++iter)
		{
			TryAppendTileIndex(*iter);
		}
	}
	else
	{
		for (auto iter = maxXBeginIter; iter != mapTileMaxXOrderCache.end(); ++iter)
		{
			TryAppendTileIndex(*iter);
		}
	}

	if (outIndices.size() > 1)
	{
		std::sort(outIndices.begin(), outIndices.end());
	}
}

void QMainCanvas::EnsurePointDrawableSpatialIndex() const
{
	if (!pointDrawableSpatialIndexDirty)
	{
		return;
	}

	RebuildDrawableSpatialIndex(pointDrawables, pointDrawableMinXOrderCache, pointDrawableMaxXOrderCache, [](const CachedPointDrawable& item) -> const GB_Rectangle&
		{
			return item.extent;
		});
	pointDrawableSpatialIndexDirty = false;
}

void QMainCanvas::InvalidatePointDrawableSpatialIndex() const
{
	pointDrawableSpatialIndexDirty = true;
	pointDrawableMinXOrderCache.clear();
	pointDrawableMaxXOrderCache.clear();
	pointDrawableVisibleIndexScratch.clear();
}

void QMainCanvas::CollectVisiblePointDrawableIndices(const GB_Rectangle& queryWorldExtent, std::vector<size_t>& outIndices) const
{
	EnsurePointDrawableSpatialIndex();
	CollectVisibleDrawableIndicesByExtent(pointDrawables, pointDrawableMinXOrderCache, pointDrawableMaxXOrderCache, queryWorldExtent, outIndices,
		[](const CachedPointDrawable& item) -> const GB_Rectangle&
		{
			return item.extent;
		},
		[](const CachedPointDrawable& item) -> bool
		{
			return item.point.visible;
		});
}

void QMainCanvas::EnsurePolylineDrawableSpatialIndex() const
{
	if (!polylineDrawableSpatialIndexDirty)
	{
		return;
	}

	RebuildDrawableSpatialIndex(polylineDrawables, polylineDrawableMinXOrderCache, polylineDrawableMaxXOrderCache, [](const CachedPolylineDrawable& item) -> const GB_Rectangle&
		{
			return item.extent;
		});
	polylineDrawableSpatialIndexDirty = false;
}

void QMainCanvas::InvalidatePolylineDrawableSpatialIndex() const
{
	polylineDrawableSpatialIndexDirty = true;
	polylineDrawableMinXOrderCache.clear();
	polylineDrawableMaxXOrderCache.clear();
	polylineDrawableVisibleIndexScratch.clear();
}

void QMainCanvas::CollectVisiblePolylineDrawableIndices(const GB_Rectangle& queryWorldExtent, std::vector<size_t>& outIndices) const
{
	EnsurePolylineDrawableSpatialIndex();
	CollectVisibleDrawableIndicesByExtent(polylineDrawables, polylineDrawableMinXOrderCache, polylineDrawableMaxXOrderCache, queryWorldExtent, outIndices,
		[](const CachedPolylineDrawable& item) -> const GB_Rectangle&
		{
			return item.extent;
		},
		[](const CachedPolylineDrawable& item) -> bool
		{
			return item.polyline.visible;
		});
}

void QMainCanvas::EnsurePolygonDrawableSpatialIndex() const
{
	if (!polygonDrawableSpatialIndexDirty)
	{
		return;
	}

	RebuildDrawableSpatialIndex(polygonDrawables, polygonDrawableMinXOrderCache, polygonDrawableMaxXOrderCache, [](const CachedPolygonDrawable& item) -> const GB_Rectangle&
		{
			return item.extent;
		});
	polygonDrawableSpatialIndexDirty = false;
}

void QMainCanvas::InvalidatePolygonDrawableSpatialIndex() const
{
	polygonDrawableSpatialIndexDirty = true;
	polygonDrawableMinXOrderCache.clear();
	polygonDrawableMaxXOrderCache.clear();
	polygonDrawableVisibleIndexScratch.clear();
}

void QMainCanvas::CollectVisiblePolygonDrawableIndices(const GB_Rectangle& queryWorldExtent, std::vector<size_t>& outIndices) const
{
	EnsurePolygonDrawableSpatialIndex();
	CollectVisibleDrawableIndicesByExtent(polygonDrawables, polygonDrawableMinXOrderCache, polygonDrawableMaxXOrderCache, queryWorldExtent, outIndices,
		[](const CachedPolygonDrawable& item) -> const GB_Rectangle&
		{
			return item.extent;
		},
		[](const CachedPolygonDrawable& item) -> bool
		{
			return item.polygon.visible;
		});
}

void QMainCanvas::InvalidateVectorDrawableSpatialIndexes() const
{
	InvalidatePointDrawableSpatialIndex();
	InvalidatePolylineDrawableSpatialIndex();
	InvalidatePolygonDrawableSpatialIndex();
}

void QMainCanvas::InvalidateVectorDrawableMaxScreenMarginCaches() const
{
	pointDrawableMaxScreenMarginCacheDirty = true;
	pointDrawableMaxScreenMarginCache = 0.0;
	polylineDrawableMaxScreenMarginCacheDirty = true;
	polylineDrawableMaxScreenMarginCache = 0.0;
	polygonDrawableMaxScreenMarginCacheDirty = true;
	polygonDrawableMaxScreenMarginCache = 0.0;
}

double QMainCanvas::GetMaxPointDrawableScreenMarginPixels() const
{
	if (!pointDrawableMaxScreenMarginCacheDirty)
	{
		return pointDrawableMaxScreenMarginCache;
	}

	double maxMarginPixels = 0.0;
	for (const CachedPointDrawable& item : pointDrawables)
	{
		if (item.point.visible && std::isfinite(item.screenMarginPixels))
		{
			maxMarginPixels = std::max(maxMarginPixels, item.screenMarginPixels);
		}
	}

	pointDrawableMaxScreenMarginCache = maxMarginPixels;
	pointDrawableMaxScreenMarginCacheDirty = false;
	return pointDrawableMaxScreenMarginCache;
}

double QMainCanvas::GetMaxPolylineDrawableScreenMarginPixels() const
{
	if (!polylineDrawableMaxScreenMarginCacheDirty)
	{
		return polylineDrawableMaxScreenMarginCache;
	}

	double maxMarginPixels = 0.0;
	for (const CachedPolylineDrawable& item : polylineDrawables)
	{
		if (item.polyline.visible && std::isfinite(item.screenMarginPixels))
		{
			maxMarginPixels = std::max(maxMarginPixels, item.screenMarginPixels);
		}
	}

	polylineDrawableMaxScreenMarginCache = maxMarginPixels;
	polylineDrawableMaxScreenMarginCacheDirty = false;
	return polylineDrawableMaxScreenMarginCache;
}

double QMainCanvas::GetMaxPolygonDrawableScreenMarginPixels() const
{
	if (!polygonDrawableMaxScreenMarginCacheDirty)
	{
		return polygonDrawableMaxScreenMarginCache;
	}

	double maxMarginPixels = 0.0;
	for (const CachedPolygonDrawable& item : polygonDrawables)
	{
		if (item.polygon.visible && std::isfinite(item.screenMarginPixels))
		{
			maxMarginPixels = std::max(maxMarginPixels, item.screenMarginPixels);
		}
	}

	polygonDrawableMaxScreenMarginCache = maxMarginPixels;
	polygonDrawableMaxScreenMarginCacheDirty = false;
	return polygonDrawableMaxScreenMarginCache;
}

void QMainCanvas::DrawBackground(QPainter& painter) const
{
	painter.fillRect(rect(), Qt::white);
}

bool QMainCanvas::TryGetCrsValidArea(GB_Rectangle& outValidArea) const
{
	outValidArea.Reset();

	if (!crsValidAreaRectCacheDirty && crsValidAreaRectCacheCrsWkt == crsWkt)
	{
		if (!crsValidAreaRectCache.IsValid())
		{
			return false;
		}

		outValidArea = crsValidAreaRectCache;
		return true;
	}

	crsValidAreaRectCache.Reset();
	crsValidAreaRectCacheCrsWkt = crsWkt;
	crsValidAreaRectCacheDirty = false;

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

	crsValidAreaRectCache = validArea.rect;
	crsValidAreaRectCache.Normalize();
	if (!crsValidAreaRectCache.IsValid())
	{
		crsValidAreaRectCache.Reset();
		return false;
	}

	outValidArea = crsValidAreaRectCache;
	return true;
}

bool QMainCanvas::TryGetCachedCrsValidAreaPolygonsExtent(GB_Rectangle& outExtent) const
{
	outExtent.Reset();

	if (!EnsureCrsValidAreaPolygonsCache() || crsValidAreaPolygonsCache.empty())
	{
		return false;
	}

	const bool canUseCachedExtents = crsValidAreaPolygonExtentsCache.size() == crsValidAreaPolygonsCache.size();
	if (canUseCachedExtents)
	{
		for (const GB_Rectangle& polygonExtent : crsValidAreaPolygonExtentsCache)
		{
			if (polygonExtent.IsValid())
			{
				outExtent.Expand(polygonExtent);
			}
		}
	}
	else
	{
		for (const std::vector<GB_Point2d>& polygon : crsValidAreaPolygonsCache)
		{
			for (const GB_Point2d& point : polygon)
			{
				if (IsPointFinite(point))
				{
					outExtent.Expand(point);
				}
			}
		}
	}

	if (!outExtent.IsValid())
	{
		return false;
	}

	outExtent.Normalize();
	return outExtent.IsValid();
}

bool QMainCanvas::TryBuildCrsValidAreaScreenPath(QPainterPath& outPath, GB_Rectangle& outWorldExtent) const
{
	outPath = QPainterPath();
	outPath.setFillRule(Qt::WindingFill);
	outWorldExtent.Reset();

	const QPainterPath* cachedPath = nullptr;
	if (!TryGetCrsValidAreaScreenPathCache(cachedPath, outWorldExtent) || cachedPath == nullptr)
	{
		return false;
	}

	outPath = *cachedPath;
	return true;
}

bool QMainCanvas::TryGetCrsValidAreaScreenPathCache(const QPainterPath*& outPath, GB_Rectangle& outWorldExtent) const
{
	outPath = nullptr;
	outWorldExtent.Reset();

	if (!crsValidAreaScreenPathCacheDirty &&
		!crsValidAreaPolygonsCacheDirty &&
		crsValidAreaScreenPathCacheCrsWkt == crsWkt &&
		AreRectanglesNearlyEqual(crsValidAreaScreenPathCacheViewExtent, viewExtent) &&
		AreNearlyEqual(crsValidAreaScreenPathCachePixelSize, pixelSize))
	{
		if (crsValidAreaScreenPathCache.isEmpty() || !crsValidAreaScreenPathWorldExtentCache.IsValid())
		{
			return false;
		}

		outPath = &crsValidAreaScreenPathCache;
		outWorldExtent = crsValidAreaScreenPathWorldExtentCache;
		return true;
	}

	crsValidAreaScreenPathCache = QPainterPath();
	crsValidAreaScreenPathCache.setFillRule(Qt::WindingFill);
	crsValidAreaScreenPathWorldExtentCache.Reset();
	crsValidAreaScreenPathCacheViewExtent = viewExtent;
	crsValidAreaScreenPathCachePixelSize = pixelSize;
	crsValidAreaScreenPathCacheCrsWkt = crsWkt;
	crsValidAreaScreenPathCacheDirty = false;

	if (!viewExtent.IsValid() || !IsFinitePositive(pixelSize))
	{
		return false;
	}

	GB_Rectangle crsPolygonsExtent;
	if (!TryGetCachedCrsValidAreaPolygonsExtent(crsPolygonsExtent) || !crsPolygonsExtent.IsIntersects(viewExtent))
	{
		return false;
	}

	// EnsureCrsValidAreaPolygonsCache() 在重建多边形缓存时可能会顺带清空屏幕路径缓存，
	// 因此屏幕路径缓存的键值必须在多边形缓存准备完成之后再写入。
	crsValidAreaScreenPathCache = QPainterPath();
	crsValidAreaScreenPathCache.setFillRule(Qt::WindingFill);
	crsValidAreaScreenPathWorldExtentCache.Reset();
	crsValidAreaScreenPathCacheViewExtent = viewExtent;
	crsValidAreaScreenPathCachePixelSize = pixelSize;
	crsValidAreaScreenPathCacheCrsWkt = crsWkt;
	crsValidAreaScreenPathCacheDirty = false;

	const bool canUseCachedExtents = crsValidAreaPolygonExtentsCache.size() == crsValidAreaPolygonsCache.size();
	const double inversePixelSize = 1.0 / pixelSize;
	const double viewMinX = viewExtent.minX;
	const double viewMaxY = viewExtent.maxY;

	std::vector<GB_Point2d> clippedPolygon;
	std::vector<GB_Point2d> clippedPolygonScratch;

	for (size_t polygonIndex = 0; polygonIndex < crsValidAreaPolygonsCache.size(); polygonIndex++)
	{
		const std::vector<GB_Point2d>& polygon = crsValidAreaPolygonsCache[polygonIndex];
		if (polygon.size() < 3)
		{
			continue;
		}

		if (canUseCachedExtents)
		{
			const GB_Rectangle& polygonExtent = crsValidAreaPolygonExtentsCache[polygonIndex];
			if (!polygonExtent.IsValid() || !polygonExtent.IsIntersects(viewExtent))
			{
				continue;
			}
		}

		if (!ClipPolygonToRectangle(polygon, viewExtent, clippedPolygon, clippedPolygonScratch))
		{
			continue;
		}

		QPolygonF screenPolygon;
		screenPolygon.reserve(GetSafeVectorReserveCount(clippedPolygon.size()));
		GB_Rectangle clippedPolygonExtent;
		clippedPolygonExtent.Reset();

		for (const GB_Point2d& point : clippedPolygon)
		{
			const double screenX = (point.x - viewMinX) * inversePixelSize;
			const double screenY = (viewMaxY - point.y) * inversePixelSize;
			if (!std::isfinite(screenX) || !std::isfinite(screenY))
			{
				screenPolygon.clear();
				clippedPolygonExtent.Reset();
				break;
			}

			screenPolygon << QPointF(screenX, screenY);
			clippedPolygonExtent.Expand(point);
		}

		if (screenPolygon.size() >= 3 && clippedPolygonExtent.IsValid())
		{
			crsValidAreaScreenPathCache.addPolygon(screenPolygon);
			crsValidAreaScreenPathCache.closeSubpath();
			crsValidAreaScreenPathWorldExtentCache.Expand(clippedPolygonExtent);
		}
	}

	if (crsValidAreaScreenPathCache.isEmpty() || !crsValidAreaScreenPathWorldExtentCache.IsValid())
	{
		crsValidAreaScreenPathCache = QPainterPath();
		crsValidAreaScreenPathCache.setFillRule(Qt::WindingFill);
		crsValidAreaScreenPathWorldExtentCache.Reset();
		return false;
	}

	outPath = &crsValidAreaScreenPathCache;
	outWorldExtent = crsValidAreaScreenPathWorldExtentCache;
	return true;
}

bool QMainCanvas::TryIntersectRectangleWithCrsValidAreaPolygons(const GB_Rectangle& rect, GB_Rectangle& outIntersectionExtent) const
{
	outIntersectionExtent.Reset();

	if (!rect.IsValid() || rect.Width() <= 0.0 || rect.Height() <= 0.0)
	{
		return false;
	}

	if (!EnsureCrsValidAreaPolygonsCache() || crsValidAreaPolygonsCache.empty())
	{
		return false;
	}

	const bool canUseCachedExtents = crsValidAreaPolygonExtentsCache.size() == crsValidAreaPolygonsCache.size();
	std::vector<GB_Point2d> clippedPolygon;
	std::vector<GB_Point2d> clippedPolygonScratch;

	for (size_t polygonIndex = 0; polygonIndex < crsValidAreaPolygonsCache.size(); polygonIndex++)
	{
		if (canUseCachedExtents)
		{
			const GB_Rectangle& polygonExtent = crsValidAreaPolygonExtentsCache[polygonIndex];
			if (!polygonExtent.IsValid() || !polygonExtent.IsIntersects(rect))
			{
				continue;
			}
		}

		if (!ClipPolygonToRectangle(crsValidAreaPolygonsCache[polygonIndex], rect, clippedPolygon, clippedPolygonScratch))
		{
			continue;
		}

		for (const GB_Point2d& point : clippedPolygon)
		{
			if (IsPointFinite(point))
			{
				outIntersectionExtent.Expand(point);
			}
		}
	}

	if (!outIntersectionExtent.IsValid())
	{
		return false;
	}

	outIntersectionExtent.Normalize();
	return outIntersectionExtent.IsValid();
}

bool QMainCanvas::IsRectangleIntersectsCachedCrsValidAreaPolygonExtent(const GB_Rectangle& rect) const
{
	if (!rect.IsValid())
	{
		return false;
	}

	if (!EnsureCrsValidAreaPolygonsCache() || crsValidAreaPolygonsCache.empty())
	{
		return false;
	}

	const bool canUseCachedExtents = crsValidAreaPolygonExtentsCache.size() == crsValidAreaPolygonsCache.size();
	if (canUseCachedExtents)
	{
		for (const GB_Rectangle& polygonExtent : crsValidAreaPolygonExtentsCache)
		{
			if (polygonExtent.IsValid() && polygonExtent.IsIntersects(rect))
			{
				return true;
			}
		}

		return false;
	}

	for (const std::vector<GB_Point2d>& polygon : crsValidAreaPolygonsCache)
	{
		GB_Rectangle polygonExtent;
		polygonExtent.Reset();
		for (const GB_Point2d& point : polygon)
		{
			if (IsPointFinite(point))
			{
				polygonExtent.Expand(point);
			}
		}

		if (polygonExtent.IsValid() && polygonExtent.IsIntersects(rect))
		{
			return true;
		}
	}

	return false;
}

void QMainCanvas::DrawDrawables(QPainter& painter, const QRectF& exposedRect) const
{
	if (!viewExtent.IsValid() || !IsFinitePositive(pixelSize))
	{
		return;
	}

	const QRectF widgetRect = QRectF(rect());
	QRectF safeExposedRect = exposedRect.isValid() ? exposedRect.intersected(widgetRect) : widgetRect;
	if (!safeExposedRect.isValid() || safeExposedRect.isEmpty())
	{
		return;
	}

	const double viewMinX = viewExtent.minX;
	const double viewMaxY = viewExtent.maxY;
	GB_Rectangle baseQueryWorldExtent;
	baseQueryWorldExtent.Set(viewMinX + safeExposedRect.left() * pixelSize,
		viewMaxY - safeExposedRect.bottom() * pixelSize,
		viewMinX + safeExposedRect.right() * pixelSize,
		viewMaxY - safeExposedRect.top() * pixelSize);
	baseQueryWorldExtent = baseQueryWorldExtent.Intersected(viewExtent);
	if (!baseQueryWorldExtent.IsValid() || baseQueryWorldExtent.Width() <= 0.0 || baseQueryWorldExtent.Height() <= 0.0)
	{
		return;
	}

	GB_Rectangle mapTileQueryWorldExtent = baseQueryWorldExtent;
	GB_Rectangle crsClipWorldExtent;
	const QPainterPath* crsClipPath = nullptr;
	bool hasPreciseCrsClip = false;
	bool hasRectangularCrsClip = false;

	if (clipMapTilesToCrsValidArea && !mapTiles.empty())
	{
		if (EnsureCrsValidAreaPolygonsCache() && !crsValidAreaPolygonsCache.empty())
		{
			hasPreciseCrsClip = TryGetCrsValidAreaScreenPathCache(crsClipPath, crsClipWorldExtent);
			if (hasPreciseCrsClip)
			{
				mapTileQueryWorldExtent = mapTileQueryWorldExtent.Intersected(crsClipWorldExtent);
			}
			else
			{
				mapTileQueryWorldExtent.Reset();
			}
		}
		else if (TryGetCrsValidArea(crsClipWorldExtent))
		{
			crsClipWorldExtent = crsClipWorldExtent.Intersected(viewExtent);
			hasRectangularCrsClip = crsClipWorldExtent.IsValid() && crsClipWorldExtent.Width() > 0.0 && crsClipWorldExtent.Height() > 0.0;
			if (hasRectangularCrsClip)
			{
				mapTileQueryWorldExtent = mapTileQueryWorldExtent.Intersected(crsClipWorldExtent);
			}
			else
			{
				mapTileQueryWorldExtent.Reset();
			}
		}
	}

	const bool canUseCrsPolygonExtents = hasPreciseCrsClip && crsValidAreaPolygonExtentsCache.size() == crsValidAreaPolygonsCache.size();

	visibleDrawableReferenceScratch.clear();

	if (!mapTiles.empty() && mapTileQueryWorldExtent.IsValid() && mapTileQueryWorldExtent.Width() > 0.0 && mapTileQueryWorldExtent.Height() > 0.0)
	{
		CollectVisibleMapTileIndices(mapTileQueryWorldExtent, mapTileVisibleIndexScratch);
		visibleDrawableReferenceScratch.reserve(visibleDrawableReferenceScratch.size() + mapTileVisibleIndexScratch.size());
		for (size_t mapTileIndex : mapTileVisibleIndexScratch)
		{
			visibleDrawableReferenceScratch.push_back(VisibleDrawableReference{ CachedDrawableKind::MapTile, mapTileIndex });
		}
	}

	if (!pointDrawables.empty())
	{
		const GB_Rectangle pointQueryWorldExtent = ExpandedWorldQueryExtent(baseQueryWorldExtent, GetMaxPointDrawableScreenMarginPixels(), pixelSize);
		CollectVisiblePointDrawableIndices(pointQueryWorldExtent, pointDrawableVisibleIndexScratch);
		visibleDrawableReferenceScratch.reserve(visibleDrawableReferenceScratch.size() + pointDrawableVisibleIndexScratch.size());
		for (size_t pointIndex : pointDrawableVisibleIndexScratch)
		{
			visibleDrawableReferenceScratch.push_back(VisibleDrawableReference{ CachedDrawableKind::Point, pointIndex });
		}
	}

	if (!polylineDrawables.empty())
	{
		const GB_Rectangle polylineQueryWorldExtent = ExpandedWorldQueryExtent(baseQueryWorldExtent, GetMaxPolylineDrawableScreenMarginPixels(), pixelSize);
		CollectVisiblePolylineDrawableIndices(polylineQueryWorldExtent, polylineDrawableVisibleIndexScratch);
		visibleDrawableReferenceScratch.reserve(visibleDrawableReferenceScratch.size() + polylineDrawableVisibleIndexScratch.size());
		for (size_t polylineIndex : polylineDrawableVisibleIndexScratch)
		{
			visibleDrawableReferenceScratch.push_back(VisibleDrawableReference{ CachedDrawableKind::Polyline, polylineIndex });
		}
	}

	if (!polygonDrawables.empty())
	{
		const GB_Rectangle polygonQueryWorldExtent = ExpandedWorldQueryExtent(baseQueryWorldExtent, GetMaxPolygonDrawableScreenMarginPixels(), pixelSize);
		CollectVisiblePolygonDrawableIndices(polygonQueryWorldExtent, polygonDrawableVisibleIndexScratch);
		visibleDrawableReferenceScratch.reserve(visibleDrawableReferenceScratch.size() + polygonDrawableVisibleIndexScratch.size());
		for (size_t polygonIndex : polygonDrawableVisibleIndexScratch)
		{
			visibleDrawableReferenceScratch.push_back(VisibleDrawableReference{ CachedDrawableKind::Polygon, polygonIndex });
		}
	}

	if (visibleDrawableReferenceScratch.empty())
	{
		return;
	}

	std::sort(visibleDrawableReferenceScratch.begin(), visibleDrawableReferenceScratch.end(), [this](const VisibleDrawableReference& firstReference, const VisibleDrawableReference& secondReference) -> bool
		{
			return IsVisibleDrawableReferencePaintOrderLess(firstReference, secondReference);
		});

	painter.setRenderHint(QPainter::Antialiasing, false);
	painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

	const GB_Rectangle pointDrawQueryWorldExtent = ExpandedWorldQueryExtent(baseQueryWorldExtent, GetMaxPointDrawableScreenMarginPixels(), pixelSize);
	const GB_Rectangle polylineDrawQueryWorldExtent = ExpandedWorldQueryExtent(baseQueryWorldExtent, GetMaxPolylineDrawableScreenMarginPixels(), pixelSize);
	const GB_Rectangle polygonDrawQueryWorldExtent = ExpandedWorldQueryExtent(baseQueryWorldExtent, GetMaxPolygonDrawableScreenMarginPixels(), pixelSize);

	for (const VisibleDrawableReference& reference : visibleDrawableReferenceScratch)
	{
		switch (reference.kind)
		{
		case CachedDrawableKind::MapTile:
			if (reference.index < mapTiles.size())
			{
				DrawCachedMapTile(painter, mapTiles[reference.index], mapTileQueryWorldExtent, safeExposedRect, crsClipPath, hasPreciseCrsClip, canUseCrsPolygonExtents);
			}
			break;
		case CachedDrawableKind::Point:
			if (reference.index < pointDrawables.size())
			{
				DrawCachedPointDrawable(painter, pointDrawables[reference.index], pointDrawQueryWorldExtent, safeExposedRect);
			}
			break;
		case CachedDrawableKind::Polyline:
			if (reference.index < polylineDrawables.size())
			{
				DrawCachedPolylineDrawable(painter, polylineDrawables[reference.index], polylineDrawQueryWorldExtent, safeExposedRect);
			}
			break;
		case CachedDrawableKind::Polygon:
			if (reference.index < polygonDrawables.size())
			{
				DrawCachedPolygonDrawable(painter, polygonDrawables[reference.index], polygonDrawQueryWorldExtent, safeExposedRect);
			}
			break;
		default:
			break;
		}
	}

	painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
	painter.setRenderHint(QPainter::Antialiasing, false);
}

void QMainCanvas::DrawCachedMapTile(QPainter& painter, const CachedMapTile& cachedTile, const GB_Rectangle& queryWorldExtent, const QRectF& exposedRect,
	const QPainterPath* crsClipPath, bool hasPreciseCrsClip, bool canUseCrsPolygonExtents) const
{
	if (!cachedTile.tile.visible || cachedTile.pixmap.isNull() || !queryWorldExtent.IsValid() || !viewExtent.IsValid() || !IsFinitePositive(pixelSize))
	{
		return;
	}

	if (!IsFinitePositive(cachedTile.tileExtentWidth) || !IsFinitePositive(cachedTile.tileExtentHeight) ||
		!IsFinitePositive(cachedTile.pixmapWidth) || !IsFinitePositive(cachedTile.pixmapHeight))
	{
		return;
	}

	GB_Rectangle clippedWorldRect = cachedTile.tile.extent.Intersected(queryWorldExtent);
	if (!clippedWorldRect.IsValid() || clippedWorldRect.Width() <= 0.0 || clippedWorldRect.Height() <= 0.0)
	{
		return;
	}

	if (canUseCrsPolygonExtents)
	{
		bool mayIntersectCrsPolygon = false;
		for (const GB_Rectangle& polygonExtent : crsValidAreaPolygonExtentsCache)
		{
			if (polygonExtent.IsValid() && polygonExtent.IsIntersects(clippedWorldRect))
			{
				mayIntersectCrsPolygon = true;
				break;
			}
		}

		if (!mayIntersectCrsPolygon)
		{
			return;
		}
	}

	const double inversePixelSize = 1.0 / pixelSize;
	const double viewMinX = viewExtent.minX;
	const double viewMaxY = viewExtent.maxY;
	const QRectF targetRect(
		(clippedWorldRect.minX - viewMinX) * inversePixelSize,
		(viewMaxY - clippedWorldRect.maxY) * inversePixelSize,
		clippedWorldRect.Width() * inversePixelSize,
		clippedWorldRect.Height() * inversePixelSize);

	if (!targetRect.isValid() || targetRect.isEmpty() || !targetRect.intersects(exposedRect))
	{
		return;
	}

	const QRectF sourceRect(
		(clippedWorldRect.minX - cachedTile.tile.extent.minX) * cachedTile.inverseTileExtentWidth * cachedTile.pixmapWidth,
		(cachedTile.tile.extent.maxY - clippedWorldRect.maxY) * cachedTile.inverseTileExtentHeight * cachedTile.pixmapHeight,
		clippedWorldRect.Width() * cachedTile.inverseTileExtentWidth * cachedTile.pixmapWidth,
		clippedWorldRect.Height() * cachedTile.inverseTileExtentHeight * cachedTile.pixmapHeight);

	if (!sourceRect.isValid() || sourceRect.isEmpty())
	{
		return;
	}

	const bool needsSmoothTransform = std::abs(targetRect.width() - sourceRect.width()) > 0.25 || std::abs(targetRect.height() - sourceRect.height()) > 0.25;
	painter.setRenderHint(QPainter::SmoothPixmapTransform, needsSmoothTransform);

	if (hasPreciseCrsClip && crsClipPath != nullptr)
	{
		painter.save();
		painter.setClipPath(*crsClipPath, Qt::IntersectClip);
		painter.drawPixmap(targetRect, cachedTile.pixmap, sourceRect);
		painter.restore();
	}
	else
	{
		painter.drawPixmap(targetRect, cachedTile.pixmap, sourceRect);
	}
}

void QMainCanvas::DrawCachedPointDrawable(QPainter& painter, const CachedPointDrawable& cachedPoint, const GB_Rectangle& queryWorldExtent, const QRectF& exposedRect) const
{
	if (!cachedPoint.point.visible || !cachedPoint.extent.IsValid() || !cachedPoint.extent.IsIntersects(queryWorldExtent))
	{
		return;
	}

	const GB_Point2d screenPoint = WorldToScreen(cachedPoint.point.position);
	if (!screenPoint.IsValid())
	{
		return;
	}

	const double symbolSize = static_cast<double>(std::max(1, cachedPoint.point.symbolSize));
	const double halfSize = symbolSize * 0.5;
	const QRectF symbolRect(screenPoint.x - halfSize, screenPoint.y - halfSize, symbolSize, symbolSize);
	if (!symbolRect.adjusted(-cachedPoint.screenMarginPixels, -cachedPoint.screenMarginPixels, cachedPoint.screenMarginPixels, cachedPoint.screenMarginPixels).intersects(exposedRect))
	{
		return;
	}

	const bool canFill = cachedPoint.point.symbolFilled && !cachedPoint.point.fillColor.IsTransparent();
	const bool canDrawBorder = cachedPoint.point.borderWidth > 0 && !cachedPoint.point.borderColor.IsTransparent();
	if (!canFill && !canDrawBorder)
	{
		return;
	}

	QPen pen(Qt::NoPen);
	if (canDrawBorder)
	{
		pen = QPen(ToQColor(cachedPoint.point.borderColor), cachedPoint.point.borderWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
	}

	const QBrush brush = canFill ? QBrush(ToQColor(cachedPoint.point.fillColor)) : QBrush(Qt::NoBrush);
	painter.setPen(pen);
	painter.setBrush(brush);

	const QPointF center(screenPoint.x, screenPoint.y);
	switch (cachedPoint.point.symbolShape)
	{
	case PointDrawable::SymbolShape::Circle:
		painter.drawEllipse(symbolRect);
		break;
	case PointDrawable::SymbolShape::Square:
		painter.drawRect(symbolRect);
		break;
	case PointDrawable::SymbolShape::Triangle:
	{
		QPolygonF polygon;
		polygon.reserve(3);
		polygon << QPointF(center.x(), center.y() - halfSize) << QPointF(center.x() + halfSize, center.y() + halfSize) << QPointF(center.x() - halfSize, center.y() + halfSize);
		painter.drawPolygon(polygon);
		break;
	}
	case PointDrawable::SymbolShape::Cross:
	{
		painter.setBrush(Qt::NoBrush);
		QPen crossPen = canDrawBorder ? pen : QPen(ToQColor(cachedPoint.point.fillColor), std::max(1, cachedPoint.point.borderWidth));
		painter.setPen(crossPen);
		painter.drawLine(QPointF(center.x() - halfSize, center.y()), QPointF(center.x() + halfSize, center.y()));
		painter.drawLine(QPointF(center.x(), center.y() - halfSize), QPointF(center.x(), center.y() + halfSize));
		break;
	}
	case PointDrawable::SymbolShape::X:
	{
		painter.setBrush(Qt::NoBrush);
		QPen xPen = canDrawBorder ? pen : QPen(ToQColor(cachedPoint.point.fillColor), std::max(1, cachedPoint.point.borderWidth));
		painter.setPen(xPen);
		painter.drawLine(QPointF(center.x() - halfSize, center.y() - halfSize), QPointF(center.x() + halfSize, center.y() + halfSize));
		painter.drawLine(QPointF(center.x() - halfSize, center.y() + halfSize), QPointF(center.x() + halfSize, center.y() - halfSize));
		break;
	}
	case PointDrawable::SymbolShape::Star:
	{
		painter.setBrush(Qt::NoBrush);
		QPen starPen = canDrawBorder ? pen : QPen(ToQColor(cachedPoint.point.fillColor), std::max(1, cachedPoint.point.borderWidth));
		painter.setPen(starPen);
		painter.drawLine(QPointF(center.x() - halfSize, center.y()), QPointF(center.x() + halfSize, center.y()));
		painter.drawLine(QPointF(center.x(), center.y() - halfSize), QPointF(center.x(), center.y() + halfSize));
		painter.drawLine(QPointF(center.x() - halfSize, center.y() - halfSize), QPointF(center.x() + halfSize, center.y() + halfSize));
		painter.drawLine(QPointF(center.x() - halfSize, center.y() + halfSize), QPointF(center.x() + halfSize, center.y() - halfSize));
		break;
	}
	case PointDrawable::SymbolShape::FivePointStar:
	{
		QPolygonF polygon;
		polygon.reserve(10);
		const double innerRadius = halfSize * 0.45;
		for (int i = 0; i < 10; i++)
		{
			const double radius = (i % 2 == 0) ? halfSize : innerRadius;
			const double angle = -GB_HalfPi + static_cast<double>(i) * GB_Pi / 5.0;
			polygon << QPointF(center.x() + std::cos(angle) * radius, center.y() + std::sin(angle) * radius);
		}
		painter.drawPolygon(polygon);
		break;
	}
	case PointDrawable::SymbolShape::Diamond:
	{
		QPolygonF polygon;
		polygon.reserve(4);
		polygon << QPointF(center.x(), center.y() - halfSize) << QPointF(center.x() + halfSize, center.y()) << QPointF(center.x(), center.y() + halfSize) << QPointF(center.x() - halfSize, center.y());
		painter.drawPolygon(polygon);
		break;
	}
	default:
		painter.drawEllipse(symbolRect);
		break;
	}
}

void QMainCanvas::DrawCachedPolylineDrawable(QPainter& painter, const CachedPolylineDrawable& cachedPolyline, const GB_Rectangle& queryWorldExtent, const QRectF& exposedRect) const
{
	Q_UNUSED(exposedRect);
	if (!cachedPolyline.polyline.visible || cachedPolyline.polyline.vertices.size() < 2 || cachedPolyline.polyline.lineWidth <= 0 || cachedPolyline.polyline.lineColor.IsTransparent())
	{
		return;
	}

	if (!cachedPolyline.extent.IsValid() || !cachedPolyline.extent.IsIntersects(queryWorldExtent))
	{
		return;
	}

	const double inversePixelSize = 1.0 / pixelSize;
	const double viewMinX = viewExtent.minX;
	const double viewMaxY = viewExtent.maxY;
	const auto ToScreenPoint = [inversePixelSize, viewMinX, viewMaxY](const GB_Point2d& point) -> QPointF
		{
			return QPointF((point.x - viewMinX) * inversePixelSize, (viewMaxY - point.y) * inversePixelSize);
		};

	QPen pen(ToQColor(cachedPolyline.polyline.lineColor), std::max(1, cachedPolyline.polyline.lineWidth), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	painter.setPen(pen);
	painter.setBrush(Qt::NoBrush);

	QPolygonF currentPart;
	currentPart.reserve(GetSafeVectorReserveCount(cachedPolyline.polyline.vertices.size()));

	const auto FlushCurrentPart = [&painter, &currentPart]()
		{
			if (currentPart.size() >= 2)
			{
				painter.drawPolyline(currentPart);
			}
			currentPart.clear();
		};

	for (size_t vertexIndex = 1; vertexIndex < cachedPolyline.polyline.vertices.size(); vertexIndex++)
	{
		GB_Point2d clippedStart;
		GB_Point2d clippedEnd;
		if (!ClipSegmentToRectangle(cachedPolyline.polyline.vertices[vertexIndex - 1], cachedPolyline.polyline.vertices[vertexIndex], queryWorldExtent, clippedStart, clippedEnd))
		{
			FlushCurrentPart();
			continue;
		}

		const QPointF screenStart = ToScreenPoint(clippedStart);
		const QPointF screenEnd = ToScreenPoint(clippedEnd);
		if (!std::isfinite(screenStart.x()) || !std::isfinite(screenStart.y()) || !std::isfinite(screenEnd.x()) || !std::isfinite(screenEnd.y()))
		{
			FlushCurrentPart();
			continue;
		}

		if (currentPart.isEmpty())
		{
			currentPart << screenStart << screenEnd;
		}
		else if (AreScreenPointsNearlyEqual(currentPart.back(), screenStart))
		{
			if (!AreScreenPointsNearlyEqual(currentPart.back(), screenEnd))
			{
				currentPart << screenEnd;
			}
		}
		else
		{
			FlushCurrentPart();
			currentPart << screenStart << screenEnd;
		}

		if (currentPart.size() >= MaxVectorBatchPointCount)
		{
			const QPointF lastPoint = currentPart.back();
			painter.drawPolyline(currentPart);
			currentPart.clear();
			currentPart << lastPoint;
		}
	}

	FlushCurrentPart();
}

void QMainCanvas::DrawCachedPolygonDrawable(QPainter& painter, const CachedPolygonDrawable& cachedPolygon, const GB_Rectangle& queryWorldExtent, const QRectF& exposedRect) const
{
	Q_UNUSED(exposedRect);
	if (!cachedPolygon.polygon.visible || cachedPolygon.polygon.vertices.size() < 3 || !cachedPolygon.extent.IsValid() || !cachedPolygon.extent.IsIntersects(queryWorldExtent))
	{
		return;
	}

	const bool canFill = !cachedPolygon.polygon.fillColor.IsTransparent();
	const bool canDrawBorder = cachedPolygon.polygon.borderWidth > 0 && !cachedPolygon.polygon.borderColor.IsTransparent();
	if (!canFill && !canDrawBorder)
	{
		return;
	}

	const double inversePixelSize = 1.0 / pixelSize;
	const double viewMinX = viewExtent.minX;
	const double viewMaxY = viewExtent.maxY;
	const auto ToScreenPoint = [inversePixelSize, viewMinX, viewMaxY](const GB_Point2d& point) -> QPointF
		{
			return QPointF((point.x - viewMinX) * inversePixelSize, (viewMaxY - point.y) * inversePixelSize);
		};

	if (canFill)
	{
		std::vector<GB_Point2d> clippedPolygon;
		std::vector<GB_Point2d> clippedPolygonScratch;
		if (ClipPolygonToRectangle(cachedPolygon.polygon.vertices, queryWorldExtent, clippedPolygon, clippedPolygonScratch))
		{
			QPolygonF screenPolygon;
			screenPolygon.reserve(GetSafeVectorReserveCount(clippedPolygon.size()));
			for (const GB_Point2d& point : clippedPolygon)
			{
				const QPointF screenPoint = ToScreenPoint(point);
				if (std::isfinite(screenPoint.x()) && std::isfinite(screenPoint.y()))
				{
					screenPolygon << screenPoint;
				}
			}

			if (screenPolygon.size() >= 3)
			{
				painter.setPen(Qt::NoPen);
				painter.setBrush(ToQColor(cachedPolygon.polygon.fillColor));
				painter.drawPolygon(screenPolygon, Qt::WindingFill);
			}
		}
	}

	if (canDrawBorder)
	{
		QPen pen(ToQColor(cachedPolygon.polygon.borderColor), std::max(1, cachedPolygon.polygon.borderWidth), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
		painter.setPen(pen);
		painter.setBrush(Qt::NoBrush);

		QPolygonF currentPart;
		currentPart.reserve(GetSafeVectorReserveCount(cachedPolygon.polygon.vertices.size()));

		const auto FlushCurrentPart = [&painter, &currentPart]()
			{
				if (currentPart.size() >= 2)
				{
					painter.drawPolyline(currentPart);
				}
				currentPart.clear();
			};

		const size_t vertexCount = cachedPolygon.polygon.vertices.size();
		for (size_t vertexIndex = 0; vertexIndex < vertexCount; vertexIndex++)
		{
			const GB_Point2d& sourceStart = cachedPolygon.polygon.vertices[vertexIndex];
			const GB_Point2d& sourceEnd = cachedPolygon.polygon.vertices[(vertexIndex + 1) % vertexCount];

			GB_Point2d clippedStart;
			GB_Point2d clippedEnd;
			if (!ClipSegmentToRectangle(sourceStart, sourceEnd, queryWorldExtent, clippedStart, clippedEnd))
			{
				FlushCurrentPart();
				continue;
			}

			const QPointF screenStart = ToScreenPoint(clippedStart);
			const QPointF screenEnd = ToScreenPoint(clippedEnd);
			if (!std::isfinite(screenStart.x()) || !std::isfinite(screenStart.y()) || !std::isfinite(screenEnd.x()) || !std::isfinite(screenEnd.y()))
			{
				FlushCurrentPart();
				continue;
			}

			if (currentPart.isEmpty())
			{
				currentPart << screenStart << screenEnd;
			}
			else if (AreScreenPointsNearlyEqual(currentPart.back(), screenStart))
			{
				if (!AreScreenPointsNearlyEqual(currentPart.back(), screenEnd))
				{
					currentPart << screenEnd;
				}
			}
			else
			{
				FlushCurrentPart();
				currentPart << screenStart << screenEnd;
			}

			if (currentPart.size() >= MaxVectorBatchPointCount)
			{
				const QPointF lastPoint = currentPart.back();
				painter.drawPolyline(currentPart);
				currentPart.clear();
				currentPart << lastPoint;
			}
		}

		FlushCurrentPart();
	}
}

void QMainCanvas::DrawMapContent(QPainter& painter, const QRectF& exposedRect) const
{
	DrawBackground(painter);
	DrawDrawables(painter, exposedRect);
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

	const QPainterPath* fillPath = nullptr;
	GB_Rectangle fillWorldExtent;
	if (!TryGetCrsValidAreaScreenPathCache(fillPath, fillWorldExtent) || fillPath == nullptr)
	{
		return;
	}

	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
	painter.setPen(Qt::NoPen);
	painter.setBrush(ToQColor(crsValidAreaColor));
	painter.drawPath(*fillPath);
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
	crsValidAreaPolygonExtentsCache.clear();
	InvalidateCrsValidAreaScreenPathCache();
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
	crsValidAreaPolygonExtentsCache.reserve(polygons.size());
	for (const std::vector<GB_Point2d>& polygon : polygons)
	{
		if (polygon.size() < 3)
		{
			continue;
		}

		std::vector<GB_Point2d> filteredPolygon;
		filteredPolygon.reserve(polygon.size());
		GB_Rectangle filteredPolygonExtent;
		filteredPolygonExtent.Reset();
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
			for (const GB_Point2d& point : filteredPolygon)
			{
				filteredPolygonExtent.Expand(point);
			}

			if (filteredPolygonExtent.IsValid())
			{
				crsValidAreaPolygonExtentsCache.push_back(filteredPolygonExtent);
				crsValidAreaPolygonsCache.push_back(std::move(filteredPolygon));
			}
		}
	}

	return !crsValidAreaPolygonsCache.empty();
}

void QMainCanvas::InvalidateCrsValidAreaPolygonsCache() const
{
	crsValidAreaPolygonsCacheDirty = true;
	crsValidAreaPolygonsCacheCrsWkt.clear();
	crsValidAreaPolygonsCache.clear();
	crsValidAreaPolygonExtentsCache.clear();
	InvalidateCrsValidAreaScreenPathCache();
}

void QMainCanvas::InvalidateCrsValidAreaRectCache() const
{
	crsValidAreaRectCacheDirty = true;
	crsValidAreaRectCacheCrsWkt.clear();
	crsValidAreaRectCache.Reset();
}

void QMainCanvas::InvalidateCrsValidAreaScreenPathCache() const
{
	crsValidAreaScreenPathCacheDirty = true;
	crsValidAreaScreenPathCacheCrsWkt.clear();
	crsValidAreaScreenPathCacheViewExtent.Reset();
	crsValidAreaScreenPathCachePixelSize = 0.0;
	crsValidAreaScreenPathCache = QPainterPath();
	crsValidAreaScreenPathCache.setFillRule(Qt::WindingFill);
	crsValidAreaScreenPathWorldExtentCache.Reset();
}

double QMainCanvas::GetCrsMetersPerUnitCached() const
{
	if (!crsMetersPerUnitCacheDirty && crsMetersPerUnitCacheCrsWkt == crsWkt)
	{
		return crsMetersPerUnitCache;
	}

	crsMetersPerUnitCacheDirty = false;
	crsMetersPerUnitCacheCrsWkt = crsWkt;
	crsMetersPerUnitCache = 0.0;

	if (crsWkt.empty())
	{
		return 0.0;
	}

	std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromWktCached(crsWkt);
	if (!crs || !crs->IsValid())
	{
		return 0.0;
	}

	const double metersPerUnit = crs->GetMetersPerUnit();
	if (IsFinitePositive(metersPerUnit))
	{
		crsMetersPerUnitCache = metersPerUnit;
	}

	return crsMetersPerUnitCache;
}

void QMainCanvas::InvalidateCrsMetersPerUnitCache() const
{
	crsMetersPerUnitCacheDirty = true;
	crsMetersPerUnitCacheCrsWkt.clear();
	crsMetersPerUnitCache = 0.0;
}

bool QMainCanvas::IsDrawableUidInSet(const std::vector<std::string>& drawablesUids, const std::string& uid) const
{
	if (uid.empty())
	{
		return false;
	}

	return std::find(drawablesUids.begin(), drawablesUids.end(), uid) != drawablesUids.end();
}

bool QMainCanvas::HasVisibleMapTileIntersectingExtent(const GB_Rectangle& extent) const
{
	if (!extent.IsValid())
	{
		return false;
	}

	for (const CachedMapTile& item : mapTiles)
	{
		if (item.tile.visible && item.tile.extent.IsValid() && item.tile.extent.IsIntersects(extent))
		{
			return true;
		}
	}

	return false;
}

GB_Rectangle QMainCanvas::CalculateVisibleVectorDrawableExtent() const
{
	GB_Rectangle result;
	result.Reset();

	for (const CachedPointDrawable& item : pointDrawables)
	{
		if (item.point.visible && item.extent.IsValid())
		{
			result.Expand(item.extent);
		}
	}

	for (const CachedPolylineDrawable& item : polylineDrawables)
	{
		if (item.polyline.visible && item.extent.IsValid())
		{
			result.Expand(item.extent);
		}
	}

	for (const CachedPolygonDrawable& item : polygonDrawables)
	{
		if (item.polygon.visible && item.extent.IsValid())
		{
			result.Expand(item.extent);
		}
	}

	return result;
}

GB_Rectangle QMainCanvas::CalculateAllDrawableExtent() const
{
	if (!allDrawableExtentCacheDirty)
	{
		return allDrawableExtentCache;
	}

	GB_Rectangle result;
	result.Reset();

	for (const CachedMapTile& item : mapTiles)
	{
		if (item.tile.visible && item.tile.extent.IsValid())
		{
			result.Expand(item.tile.extent);
		}
	}

	const GB_Rectangle vectorDrawableExtent = CalculateVisibleVectorDrawableExtent();
	if (vectorDrawableExtent.IsValid())
	{
		result.Expand(vectorDrawableExtent);
	}

	allDrawableExtentCache = result;
	allDrawableExtentCacheDirty = false;
	return allDrawableExtentCache;
}

void QMainCanvas::InvalidateAllDrawableExtentCache() const
{
	allDrawableExtentCacheDirty = true;
	allDrawableExtentCache.Reset();
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
	if (AreRectanglesNearlyEqual(normalizedExtent, previousExtent))
	{
		return;
	}

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

	const double metersPerUnit = GetCrsMetersPerUnitCached();
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
