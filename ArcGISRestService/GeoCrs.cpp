#include "GeoCrs.h"
#include "GeoBoundingBox.h"
#include "GeoBase/Geometry/GB_Point2d.h"
#include "GeoBase/Geometry/GB_Rectangle.h"
#include "GeoBase/GB_Logger.h"
#include "GeoBase/GB_Utf8String.h"

#include "cpl_conv.h"
#include "cpl_error.h"
#include "gdal_version.h"
#include "ogr_spatialref.h"
#include "ogr_srs_api.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
	struct CplCharDeleter
	{
		void operator()(char* ptr) const noexcept
		{
			if (ptr != nullptr)
			{
				CPLFree(ptr);
			}
		}
	};

	using CplCharPtr = std::unique_ptr<char, CplCharDeleter>;

	struct CoordinateTransformationDeleter
	{
		void operator()(OGRCoordinateTransformation* transform) const noexcept
		{
			if (transform != nullptr)
			{
				OCTDestroyCoordinateTransformation(transform);
			}
		}
	};

	using CoordinateTransformationPtr = std::unique_ptr<OGRCoordinateTransformation, CoordinateTransformationDeleter>;

	static bool EqualsIgnoreCaseAscii(const char* left, const char* right)
	{
		if (left == nullptr || right == nullptr)
		{
			return false;
		}

		while (*left != '\0' && *right != '\0')
		{
			char leftChar = *left;
			char rightChar = *right;

			if (leftChar >= 'A' && leftChar <= 'Z')
			{
				leftChar = static_cast<char>(leftChar - 'A' + 'a');
			}

			if (rightChar >= 'A' && rightChar <= 'Z')
			{
				rightChar = static_cast<char>(rightChar - 'A' + 'a');
			}

			if (leftChar != rightChar)
			{
				return false;
			}

			left++;
			right++;
		}

		return *left == '\0' && *right == '\0';
	}

	static int ParsePositiveInt(const char* text)
	{
		if (text == nullptr || *text == '\0')
		{
			return 0;
		}

		errno = 0;
		char* endPtr = nullptr;
		const long long value = std::strtoll(text, &endPtr, 10);

		if (endPtr == text || endPtr == nullptr || *endPtr != '\0')
		{
			return 0;
		}

		if (errno == ERANGE)
		{
			return 0;
		}

		if (value <= 0 || value > static_cast<long long>(std::numeric_limits<int>::max()))
		{
			return 0;
		}

		return static_cast<int>(value);
	}

	static int ExtractEpsgCodeFromSrs(const OGRSpatialReference& srs)
	{
		// 只检查根节点的 AUTHORITY（pszTargetKey=nullptr）
		// 避免从子节点（例如基础 GEOGCRS）“捡到”EPSG，从而把自定义 PROJCRS 误判为 EPSG:4326。
		const char* authorityName = srs.GetAuthorityName(nullptr);
		const char* authorityCode = srs.GetAuthorityCode(nullptr);
		if (authorityName == nullptr || authorityCode == nullptr)
		{
			return 0;
		}

		if (!EqualsIgnoreCaseAscii(authorityName, "EPSG"))
		{
			return 0;
		}

		return ParsePositiveInt(authorityCode);
	}

	static std::string NormalizeAuthorityNameUtf8(const char* authorityName)
	{
		if (authorityName == nullptr || authorityName[0] == '\0')
		{
			return "";
		}

		std::string result = GB_Utf8Trim(std::string(authorityName));
		for (size_t i = 0; i < result.size(); i++)
		{
			if (result[i] >= 'a' && result[i] <= 'z')
			{
				result[i] = static_cast<char>(result[i] - 'a' + 'A');
			}
		}
		return result;
	}

	static std::string ExtractRootAuthorityCodeFromSrs(const OGRSpatialReference& srs)
	{
		const char* authorityName = srs.GetAuthorityName(nullptr);
		const char* authorityCode = srs.GetAuthorityCode(nullptr);
		if (authorityName == nullptr || authorityCode == nullptr || authorityName[0] == '\0' || authorityCode[0] == '\0')
		{
			return "";
		}

		const std::string normalizedAuthorityName = NormalizeAuthorityNameUtf8(authorityName);
		const std::string trimmedAuthorityCode = GB_Utf8Trim(std::string(authorityCode));
		if (normalizedAuthorityName.empty() || trimmedAuthorityCode.empty())
		{
			return "";
		}

		return normalizedAuthorityName + ":" + trimmedAuthorityCode;
	}

	static bool IsFinite(double value)
	{
		return std::isfinite(value);
	}

	static uint64_t Fnv1a64(const void* data, size_t size)
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		uint64_t hashValue = 14695981039346656037ull;

		for (size_t i = 0; i < size; i++)
		{
			hashValue ^= bytes[i];
			hashValue *= 1099511628211ull;
		}

		return hashValue;
	}

	static std::string ToHex64(uint64_t value)
	{
		static const char* const hexChars = "0123456789abcdef";
		std::string result(16, '0');

		for (int i = 15; i >= 0; i--)
		{
			result[static_cast<size_t>(i)] = hexChars[static_cast<size_t>(value & 0xFULL)];
			value >>= 4;
		}

		return result;
	}

	static bool IsUnknownAreaOfUseValue(double value)
	{
		// GDAL 的 area of use 若不可用，常以 -1000 表示未知。
		// 这里用一个小阈值做容错。
		return value <= -999.5;
	}

	static std::string SafeCString(const char* text)
	{
		return text ? std::string(text) : std::string();
	}

	static bool IsOnlyAsciiWhitespaceOrEnd(const char* text)
	{
		if (text == nullptr)
		{
			return true;
		}

		const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
		while (*cursor != '\0')
		{
			if (!std::isspace(*cursor))
			{
				return false;
			}
			cursor++;
		}

		return true;
	}

	static bool IsValidAreaOfUseValues(double west, double south, double east, double north)
	{
		if (!IsFinite(west) || !IsFinite(south) || !IsFinite(east) || !IsFinite(north))
		{
			return false;
		}

		if (IsUnknownAreaOfUseValue(west) || IsUnknownAreaOfUseValue(south) ||
			IsUnknownAreaOfUseValue(east) || IsUnknownAreaOfUseValue(north))
		{
			return false;
		}

		// area of use 的经纬度可以跨越日期变更线（west > east），但纬度必须形成非退化区间。
		if (!(south < north))
		{
			return false;
		}

		// west == east 通常表示缺失或退化范围；不作为有效二维范围使用。
		if (west == east)
		{
			return false;
		}

		return true;
	}

	static bool TryGetAreaOfUseFromSrs(const OGRSpatialReference& srs, double& west, double& south, double& east, double& north)
	{
		const char* areaName = nullptr;
		if (srs.GetAreaOfUse(&west, &south, &east, &north, &areaName) && IsValidAreaOfUseValues(west, south, east, north))
		{
			return true;
		}

		// 某些 WKT 片段自身不带 AREA / BBOX，但可以通过 EPSG/ESRI 匹配找回 area of use。
		std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> cloned(srs.Clone());
		if (cloned)
		{
			CPLPushErrorHandler(CPLQuietErrorHandler);
			const OGRErr err = cloned->AutoIdentifyEPSG();
			CPLPopErrorHandler();

			if (err == OGRERR_NONE && cloned->GetAreaOfUse(&west, &south, &east, &north, &areaName) && IsValidAreaOfUseValues(west, south, east, north))
			{
				return true;
			}
		}

		static const char* const preferredAuthorities[] = { "EPSG", "ESRI" };
		for (size_t i = 0; i < sizeof(preferredAuthorities) / sizeof(preferredAuthorities[0]); i++)
		{
			CPLPushErrorHandler(CPLQuietErrorHandler);
			OGRSpatialReference* bestMatchRaw = srs.FindBestMatch(70, preferredAuthorities[i], nullptr);
			CPLPopErrorHandler();

			std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> bestMatch(bestMatchRaw);
			if (!bestMatch)
			{
				continue;
			}

			if (bestMatch->GetAreaOfUse(&west, &south, &east, &north, &areaName) && IsValidAreaOfUseValues(west, south, east, north))
			{
				return true;
			}
		}

		return false;
	}

	static double ClampDouble(double value, double minValue, double maxValue)
	{
		return std::max(minValue, std::min(maxValue, value));
	}

	static void AppendLonLatGridSamples(
		double west,
		double south,
		double east,
		double north,
		int gridCount,
		std::vector<double>& longitudes,
		std::vector<double>& latitudes)
	{
		const int count = std::max(2, gridCount);

		for (int yIndex = 0; yIndex < count; yIndex++)
		{
			const double yT = (count <= 1) ? 0.0 : static_cast<double>(yIndex) / static_cast<double>(count - 1);
			const double lat = south + (north - south) * yT;

			for (int xIndex = 0; xIndex < count; xIndex++)
			{
				const double xT = (count <= 1) ? 0.0 : static_cast<double>(xIndex) / static_cast<double>(count - 1);
				const double lon = west + (east - west) * xT;

				longitudes.push_back(lon);
				latitudes.push_back(lat);
			}
		}
	}

	static bool TryExpandRectangle(GB_Rectangle& inOutRect, const GB_Rectangle& rect)
	{
		if (!rect.IsValid())
		{
			return false;
		}

		if (!inOutRect.IsValid())
		{
			inOutRect = rect;
		}
		else
		{
			inOutRect.Expand(rect);
		}

		return true;
	}

	static bool TryTransformLonLatSegmentBounds(
		OGRCoordinateTransformation& transform,
		const GeoCrs::LonLatAreaSegment& segment,
		int densifyPointCount,
		GB_Rectangle& outRect)
	{
		outRect = GB_Rectangle::Invalid;

		if (!IsFinite(segment.west) || !IsFinite(segment.south) ||
			!IsFinite(segment.east) || !IsFinite(segment.north) ||
			!(segment.west < segment.east) || !(segment.south < segment.north))
		{
			return false;
		}

#if defined(GDAL_VERSION_NUM) && GDAL_VERSION_NUM >= 3040000
		double minX = 0.0;
		double minY = 0.0;
		double maxX = 0.0;
		double maxY = 0.0;
		const int normalizedDensifyPointCount = std::max(2, densifyPointCount);
		const int ok = transform.TransformBounds(
			segment.west,
			segment.south,
			segment.east,
			segment.north,
			&minX,
			&minY,
			&maxX,
			&maxY,
			normalizedDensifyPointCount);

		if (ok != FALSE && IsFinite(minX) && IsFinite(minY) && IsFinite(maxX) && IsFinite(maxY))
		{
			outRect.Set(minX, minY, maxX, maxY);
			return outRect.IsValid();
		}
#else
		(void)transform;
		(void)densifyPointCount;
#endif

		return false;
	}

	static int NormalizeValidAreaPolygonEdgeSampleCount(int edgeSampleCount)
	{
		if (edgeSampleCount < 2)
		{
			return 2;
		}

		// 防止外部误传过大的采样密度导致一次性构造过多点。
		const int maxEdgeSampleCount = 2049;
		return std::min(edgeSampleCount, maxEdgeSampleCount);
	}

	static bool IsValidLonLatAreaSegment(const GeoCrs::LonLatAreaSegment& segment)
	{
		if (!IsFinite(segment.west) || !IsFinite(segment.south) || !IsFinite(segment.east) || !IsFinite(segment.north))
		{
			return false;
		}

		return segment.west < segment.east && segment.south < segment.north;
	}

	static void AppendLonLatBoundarySamples(
		double west,
		double south,
		double east,
		double north,
		int edgeSampleCount,
		std::vector<double>& longitudes,
		std::vector<double>& latitudes)
	{
		const int count = NormalizeValidAreaPolygonEdgeSampleCount(edgeSampleCount);
		longitudes.reserve(longitudes.size() + static_cast<size_t>(4 * count + 1));
		latitudes.reserve(latitudes.size() + static_cast<size_t>(4 * count + 1));

		// 按逆时针方向采样：左下 -> 右下 -> 右上 -> 左上 -> 左下。
		for (int xIndex = 0; xIndex < count; xIndex++)
		{
			const double t = (count <= 1) ? 0.0 : static_cast<double>(xIndex) / static_cast<double>(count - 1);
			longitudes.push_back(west + (east - west) * t);
			latitudes.push_back(south);
		}

		for (int yIndex = 1; yIndex < count; yIndex++)
		{
			const double t = (count <= 1) ? 0.0 : static_cast<double>(yIndex) / static_cast<double>(count - 1);
			longitudes.push_back(east);
			latitudes.push_back(south + (north - south) * t);
		}

		for (int xIndex = count - 2; xIndex >= 0; xIndex--)
		{
			const double t = (count <= 1) ? 0.0 : static_cast<double>(xIndex) / static_cast<double>(count - 1);
			longitudes.push_back(west + (east - west) * t);
			latitudes.push_back(north);
		}

		for (int yIndex = count - 2; yIndex >= 1; yIndex--)
		{
			const double t = (count <= 1) ? 0.0 : static_cast<double>(yIndex) / static_cast<double>(count - 1);
			longitudes.push_back(west);
			latitudes.push_back(south + (north - south) * t);
		}

		// 显式闭合，避免调用方误把返回值当成开环折线。
		longitudes.push_back(west);
		latitudes.push_back(south);
	}

	static bool IsNearlySamePoint(const GB_Point2d& left, const GB_Point2d& right, double tolerance = 1e-10)
	{
		if (!left.IsValid() || !right.IsValid())
		{
			return false;
		}

		return std::fabs(left.x - right.x) <= tolerance && std::fabs(left.y - right.y) <= tolerance;
	}

	static void AppendDistinctPoint(std::vector<GB_Point2d>& points, const GB_Point2d& point)
	{
		if (!point.IsValid())
		{
			return;
		}

		if (!points.empty() && IsNearlySamePoint(points.back(), point))
		{
			return;
		}

		points.push_back(point);
	}

	static bool FinalizeClosedPolygon(std::vector<GB_Point2d>& polygon)
	{
		while (polygon.size() > 1 && IsNearlySamePoint(polygon[polygon.size() - 2], polygon.back()))
		{
			polygon.pop_back();
		}

		if (polygon.size() < 3)
		{
			polygon.clear();
			return false;
		}

		if (!IsNearlySamePoint(polygon.front(), polygon.back()))
		{
			polygon.push_back(polygon.front());
		}

		if (polygon.size() < 4)
		{
			polygon.clear();
			return false;
		}

		double minX = std::numeric_limits<double>::infinity();
		double minY = std::numeric_limits<double>::infinity();
		double maxX = -std::numeric_limits<double>::infinity();
		double maxY = -std::numeric_limits<double>::infinity();

		for (const GB_Point2d& point : polygon)
		{
			if (!point.IsValid())
			{
				polygon.clear();
				return false;
			}

			minX = std::min(minX, point.x);
			minY = std::min(minY, point.y);
			maxX = std::max(maxX, point.x);
			maxY = std::max(maxY, point.y);
		}

		if (!(maxX > minX) || !(maxY > minY))
		{
			polygon.clear();
			return false;
		}

		return true;
	}

	static bool TryTransformLonLatBoundaryToSelfPolygon(
		OGRCoordinateTransformation& transform,
		const std::vector<double>& sourceLongitudes,
		const std::vector<double>& sourceLatitudes,
		std::vector<GB_Point2d>& outPolygon)
	{
		outPolygon.clear();

		const size_t numPoints = sourceLongitudes.size();
		if (numPoints == 0 || numPoints != sourceLatitudes.size() || numPoints > static_cast<size_t>(std::numeric_limits<int>::max()))
		{
			return false;
		}

		std::vector<double> xValues = sourceLongitudes;
		std::vector<double> yValues = sourceLatitudes;
		std::vector<int> successFlags(numPoints, FALSE);

		const int overallOk = transform.Transform(static_cast<int>(numPoints), xValues.data(), yValues.data(), nullptr, successFlags.data());
		bool allPointsOk = (overallOk != FALSE);

		outPolygon.reserve(numPoints);
		for (size_t i = 0; i < numPoints; i++)
		{
			if (successFlags[i] == FALSE || !IsFinite(xValues[i]) || !IsFinite(yValues[i]))
			{
				allPointsOk = false;
				continue;
			}

			AppendDistinctPoint(outPolygon, GB_Point2d(xValues[i], yValues[i]));
		}

		if (!FinalizeClosedPolygon(outPolygon))
		{
			return false;
		}

		if (!allPointsOk)
		{
			GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaPolygons】部分边界点转换失败，已使用成功点构造多边形。"));
		}

		return true;
	}

	static inline GeoBoundingBox MakeInvalidGeoBoundingBox()
	{
		return GeoBoundingBox::Invalid;
	}

	static OGRSpatialReference* CreateOgrSpatialReference()
	{
		const OGRSpatialReferenceH handle = OSRNewSpatialReference(nullptr);
		return OGRSpatialReference::FromHandle(handle);
	}

	static std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> CreateOgrSpatialReferencePtr()
	{
		return std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter>(CreateOgrSpatialReference());
	}

	static bool ValidatePreparedSpatialReference(OGRSpatialReference& srs, const std::string& logContextUtf8)
	{
		if (srs.IsEmpty())
		{
			GBLOG_WARNING(logContextUtf8 + GB_STR("坐标系为空。"));
			return false;
		}

		CPLPushErrorHandler(CPLQuietErrorHandler);
		CPLErrorReset();
		const OGRErr err = srs.Validate();
		const std::string lastErrorMessage = SafeCString(CPLGetLastErrorMsg());
		CPLPopErrorHandler();

		if (err != OGRERR_NONE)
		{
			std::string message = logContextUtf8 + GB_STR("坐标系未通过 Validate 校验: err=") + std::to_string(static_cast<int>(err));
			if (!lastErrorMessage.empty())
			{
				message += GB_STR(", message=") + lastErrorMessage;
			}
			GBLOG_WARNING(message);
			return false;
		}

		return true;
	}

	static void ApplyAxisOrderStrategy(OGRSpatialReference& srs, bool useTraditionalGisAxisOrder)
	{
		srs.SetAxisMappingStrategy(useTraditionalGisAxisOrder ? OAMS_TRADITIONAL_GIS_ORDER : OAMS_AUTHORITY_COMPLIANT);
	}

	static void EnsureTraditionalGisAxisOrder(OGRSpatialReference& srs)
	{
		// 强制使用传统 GIS 顺序 (X=经度/Easting, Y=纬度/Northing)。
		srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
	}
	static std::string ExportSrsToWktUtf8(const OGRSpatialReference& srs, const char* const* options)
	{
		char* wktRaw = nullptr;
		const OGRErr err = srs.exportToWkt(&wktRaw, options);
		CplCharPtr wkt(wktRaw);

		if (err != OGRERR_NONE || wkt == nullptr)
		{
			return "";
		}

		return std::string(wkt.get());
	}

	static std::vector<std::string> SplitStringByChar(const std::string& text, char delimiter)
	{
		std::vector<std::string> parts;
		parts.reserve(8);

		size_t beginIndex = 0;
		for (size_t i = 0; i < text.size(); i++)
		{
			if (text[i] == delimiter)
			{
				parts.emplace_back(text.substr(beginIndex, i - beginIndex));
				beginIndex = i + 1;
			}
		}

		parts.emplace_back(text.substr(beginIndex));
		return parts;
	}

	static bool IsAsciiDigitsAndDots(const std::string& text)
	{
		if (text.empty())
		{
			return false;
		}

		for (size_t i = 0; i < text.size(); i++)
		{
			const char ch = text[i];
			if ((ch < '0' || ch > '9') && ch != '.')
			{
				return false;
			}
		}

		return true;
	}

	static bool TryExtractEpsgCodeFromOgcCrsUrn(const std::string& definitionUtf8, int& epsgCode)
	{
		epsgCode = 0;

		const std::vector<std::string> parts = SplitStringByChar(definitionUtf8, ':');
		if (parts.size() < 6)
		{
			return false;
		}

		if (!EqualsIgnoreCaseAscii(parts[0].c_str(), "urn") ||
			!(EqualsIgnoreCaseAscii(parts[1].c_str(), "ogc") || EqualsIgnoreCaseAscii(parts[1].c_str(), "x-ogc")) ||
			!EqualsIgnoreCaseAscii(parts[2].c_str(), "def") ||
			!EqualsIgnoreCaseAscii(parts[3].c_str(), "crs") ||
			!EqualsIgnoreCaseAscii(parts[4].c_str(), "EPSG"))
		{
			return false;
		}

		for (size_t i = 5; i + 1 < parts.size(); i++)
		{
			if (!parts[i].empty() && !IsAsciiDigitsAndDots(parts[i]))
			{
				return false;
			}
		}

		const int parsedEpsgCode = ParsePositiveInt(parts.back().c_str());
		if (parsedEpsgCode <= 0)
		{
			return false;
		}

		epsgCode = parsedEpsgCode;
		return true;
	}

}

void GeoCrsOgrSrsDeleter::operator()(OGRSpatialReference* srs) const noexcept
{
	if (srs == nullptr)
	{
		return;
	}

	// OGRSpatialReference 是引用计数对象。
	srs->Release();
}

GeoCrs::GeoCrs() : spatialReference(nullptr)
{
	spatialReference.reset(CreateOgrSpatialReference());
	if (spatialReference)
	{
		ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
	}

	InvalidateCachesNoLock();
}


GeoCrs::GeoCrs(const GeoCrs& other) : spatialReference(nullptr)
{
	std::lock_guard<std::recursive_mutex> otherLock(other.mutex);
	if (other.spatialReference)
	{
		spatialReference.reset(other.spatialReference->Clone());
	}
	else
	{
		spatialReference.reset(CreateOgrSpatialReference());
	}

	useTraditionalGisAxisOrder = other.useTraditionalGisAxisOrder;

	if (spatialReference)
	{
		ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
	}

	InvalidateCachesNoLock();
}


GeoCrs::GeoCrs(GeoCrs&& other) noexcept : spatialReference(nullptr)
{
	std::lock_guard<std::recursive_mutex> otherLock(other.mutex);

	spatialReference = std::move(other.spatialReference);
	useTraditionalGisAxisOrder = other.useTraditionalGisAxisOrder;

	if (spatialReference)
	{
		ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
	}

	InvalidateCachesNoLock();

	// 保持被移动对象处于可用状态：确保其内部指针非空，且缓存失效。
	other.useTraditionalGisAxisOrder = true;
	if (other.spatialReference == nullptr)
	{
		other.spatialReference.reset(CreateOgrSpatialReference());
		if (other.spatialReference)
		{
			ApplyAxisOrderStrategy(*other.spatialReference, other.useTraditionalGisAxisOrder);
		}
	}
	other.InvalidateCachesNoLock();
}


void GeoCrs::InvalidateCachesNoLock() const
{
	hasCachedIsValid = false;
	cachedIsValid = false;

	hasCachedDefaultEpsgCode = false;
	cachedDefaultEpsgCode = 0;

	cachedUidKind = -2;
	cachedUidWktHash = 0;

	hasCachedMetersPerUnit = false;
	cachedMetersPerUnit = 0.0;
}

void GeoCrs::InvalidateCaches() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	InvalidateCachesNoLock();
}

bool GeoCrs::IsEmptyNoLock() const
{
	if (spatialReference == nullptr)
	{
		return true;
	}

	return spatialReference->IsEmpty();
}

bool GeoCrs::IsValidNoLock() const
{
	if (IsEmptyNoLock())
	{
		hasCachedIsValid = true;
		cachedIsValid = false;
		return false;
	}

	if (hasCachedIsValid)
	{
		return cachedIsValid;
	}

	const bool valid = (spatialReference->Validate() == OGRERR_NONE);
	cachedIsValid = valid;
	hasCachedIsValid = true;
	return valid;
}

bool GeoCrs::IsAxisOrderReversedByAuthorityNoLock() const
{
	if (IsEmptyNoLock())
	{
		return false;
	}

	const OGRSpatialReferenceH srsHandle = reinterpret_cast<OGRSpatialReferenceH>(const_cast<OGRSpatialReference*>(spatialReference.get()));
	if (srsHandle == nullptr)
	{
		return false;
	}

	return OSREPSGTreatsAsLatLong(srsHandle) != 0 || OSREPSGTreatsAsNorthingEasting(srsHandle) != 0;
}

OGRSpatialReference* GeoCrs::EnsureSpatialReferenceNoLock()
{
	const bool needCreate = (spatialReference == nullptr);
	if (needCreate)
	{
		spatialReference.reset(CreateOgrSpatialReference());
		if (spatialReference)
		{
			ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
		}
	}

	return spatialReference.get();
}

bool GeoCrs::ResetNoLock()
{
	OGRSpatialReference* srs = EnsureSpatialReferenceNoLock();
	if (srs == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::Reset】空的 srs。"));
		InvalidateCachesNoLock();
		return false;
	}

	srs->Clear();
	ApplyAxisOrderStrategy(*srs, useTraditionalGisAxisOrder);
	InvalidateCachesNoLock();
	return true;
}

int GeoCrs::TryGetEpsgCodeNoLock(bool tryAutoIdentify, bool tryFindBestMatch, int minMatchConfidence) const
{
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::TryGetEpsgCode】变量为空。"));
		return 0;
	}

	// FindBestMatch 的最小匹配置信度建议在 [0, 100] 区间内；这里做一次夹取，避免异常入参。
	minMatchConfidence = std::max(0, std::min(100, minMatchConfidence));

	const bool isDefaultQuery = (tryAutoIdentify && !tryFindBestMatch && minMatchConfidence == 90);
	if (isDefaultQuery && hasCachedDefaultEpsgCode)
	{
		return cachedDefaultEpsgCode;
	}

	int epsgCode = ExtractEpsgCodeFromSrs(*spatialReference);
	if (epsgCode > 0)
	{
		if (isDefaultQuery)
		{
			cachedDefaultEpsgCode = epsgCode;
			hasCachedDefaultEpsgCode = true;
		}
		return epsgCode;
	}

	if (tryAutoIdentify)
	{
		std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> cloned(spatialReference->Clone());
		if (cloned)
		{
			const OGRErr err = cloned->AutoIdentifyEPSG();
			if (err == OGRERR_NONE)
			{
				epsgCode = ExtractEpsgCodeFromSrs(*cloned);
				if (epsgCode > 0)
				{
					if (isDefaultQuery)
					{
						cachedDefaultEpsgCode = epsgCode;
						hasCachedDefaultEpsgCode = true;
					}
					return epsgCode;
				}
			}
		}
	}

	if (tryFindBestMatch)
	{
		OGRSpatialReference* bestMatch = spatialReference->FindBestMatch(minMatchConfidence, "EPSG", nullptr);
		std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> bestMatchHolder(bestMatch);
		if (bestMatchHolder)
		{
			epsgCode = ExtractEpsgCodeFromSrs(*bestMatchHolder);
			if (epsgCode > 0)
			{
				return epsgCode;
			}
		}
	}

	if (isDefaultQuery)
	{
		cachedDefaultEpsgCode = 0;
		hasCachedDefaultEpsgCode = true;
	}

	return 0;
}

std::string GeoCrs::ToAuthorityStringUtf8NoLock(bool tryAutoIdentify, bool tryFindBestMatch, int minMatchConfidence) const
{
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ToAuthorityStringUtf8】变量为空。"));
		return "";
	}

	minMatchConfidence = std::max(0, std::min(100, minMatchConfidence));

	std::string authorityCode = ExtractRootAuthorityCodeFromSrs(*spatialReference);
	if (!authorityCode.empty())
	{
		return authorityCode;
	}

	if (tryAutoIdentify)
	{
		std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> cloned(spatialReference->Clone());
		if (cloned)
		{
			const OGRErr err = cloned->AutoIdentifyEPSG();
			if (err == OGRERR_NONE)
			{
				authorityCode = ExtractRootAuthorityCodeFromSrs(*cloned);
				if (!authorityCode.empty())
				{
					return authorityCode;
				}
			}
		}
	}

	if (tryFindBestMatch)
	{
		static const char* const preferredAuthorities[] = { "EPSG", "ESRI" };
		for (size_t i = 0; i < sizeof(preferredAuthorities) / sizeof(preferredAuthorities[0]); i++)
		{
			OGRSpatialReference* bestMatch = spatialReference->FindBestMatch(minMatchConfidence, preferredAuthorities[i], nullptr);
			std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> bestMatchHolder(bestMatch);
			if (!bestMatchHolder)
			{
				continue;
			}

			authorityCode = ExtractRootAuthorityCodeFromSrs(*bestMatchHolder);
			if (!authorityCode.empty())
			{
				return authorityCode;
			}
		}
	}

	return "";
}

std::string GeoCrs::ExportToWktUtf8NoLock(WktFormat format, bool multiline) const
{
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToWktUtf8】变量为空。"));
		return "";
	}

	const char* formatOption = nullptr;
	switch (format)
	{
	case WktFormat::Default:
		formatOption = nullptr;
		break;
	case WktFormat::Wkt1Gdal:
		formatOption = "FORMAT=WKT1_GDAL";
		break;
	case WktFormat::Wkt1Esri:
		formatOption = "FORMAT=WKT1_ESRI";
		break;
	case WktFormat::Wkt2_2015:
		formatOption = "FORMAT=WKT2_2015";
		break;
	case WktFormat::Wkt2_2018:
		formatOption = "FORMAT=WKT2_2018";
		break;
	case WktFormat::Wkt2:
		formatOption = "FORMAT=WKT2";
		break;
	default:
		formatOption = "FORMAT=WKT2_2018";
		break;
	}

	const char* const multilineOption = multiline ? "MULTILINE=YES" : "MULTILINE=NO";

	if (formatOption == nullptr)
	{
		const char* const options[] = { multilineOption, nullptr };
		return ExportSrsToWktUtf8(*spatialReference, options);
	}

	const char* const options[] = { formatOption, multilineOption, nullptr };
	return ExportSrsToWktUtf8(*spatialReference, options);
}

std::vector<GeoCrs::LonLatAreaSegment> GeoCrs::GetValidAreaLonLatSegmentsNoLock() const
{
	std::vector<LonLatAreaSegment> segments;

	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaLonLatSegments】对象为空。"));
		return segments;
	}

	double west = 0;
	double south = 0;
	double east = 0;
	double north = 0;

	if (!TryGetAreaOfUseFromSrs(*spatialReference, west, south, east, north))
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaLonLatSegments】未能获取可用的 area of use。"));
		return segments;
	}

	// 容错：约束在经纬度常用范围内
	west = ClampDouble(west, -180.0, 180.0);
	east = ClampDouble(east, -180.0, 180.0);
	south = ClampDouble(south, -90.0, 90.0);
	north = ClampDouble(north, -90.0, 90.0);

	if (south > north)
	{
		std::swap(south, north);
	}

	if (!(south < north) || west == east)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaLonLatSegments】area of use 为退化范围。"));
		return segments;
	}

	// 跨越日期变更线时 west 可能大于 east。此时返回 2 段，以便上层按两段采样/计算。
	if (west < east)
	{
		LonLatAreaSegment segment;
		segment.west = west;
		segment.south = south;
		segment.east = east;
		segment.north = north;
		if (IsValidLonLatAreaSegment(segment))
		{
			segments.push_back(segment);
		}
	}
	else
	{
		LonLatAreaSegment segment1;
		segment1.west = west;
		segment1.south = south;
		segment1.east = 180.0;
		segment1.north = north;

		LonLatAreaSegment segment2;
		segment2.west = -180.0;
		segment2.south = south;
		segment2.east = east;
		segment2.north = north;

		if (IsValidLonLatAreaSegment(segment1))
		{
			segments.push_back(segment1);
		}
		if (IsValidLonLatAreaSegment(segment2))
		{
			segments.push_back(segment2);
		}
	}

	return segments;
}

GeoBoundingBox GeoCrs::GetValidAreaLonLatNoLock() const
{
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaLonLat】对象为空。"));
		return MakeInvalidGeoBoundingBox();
	}

	const std::vector<LonLatAreaSegment> segments = GetValidAreaLonLatSegmentsNoLock();
	if (segments.empty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaLonLat】segments为空。"));
		return MakeInvalidGeoBoundingBox();
	}

	double west = segments[0].west;
	double south = segments[0].south;
	double east = segments[0].east;
	double north = segments[0].north;

	if (segments.size() > 1)
	{
		// GeoBoundingBox 只能表达一个矩形。跨日期变更线时，取保守的全球经度范围。
		west = -180.0;
		east = 180.0;

		for (const LonLatAreaSegment& seg : segments)
		{
			south = std::min(south, seg.south);
			north = std::max(north, seg.north);
		}
	}

	OGRSpatialReference epsg4326;
	if (epsg4326.importFromEPSG(4326) != OGRERR_NONE)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaLonLat】WGS 84 坐标系导入失败。"));
		return MakeInvalidGeoBoundingBox();
	}
	EnsureTraditionalGisAxisOrder(epsg4326);

	const char* const options[] = { "FORMAT=WKT2_2018", "MULTILINE=NO", nullptr };

	GeoBoundingBox result;
	result.wktUtf8 = ExportSrsToWktUtf8(epsg4326, options);
	result.rect = GB_Rectangle(west, south, east, north);
	return result;
}

GeoBoundingBox GeoCrs::GetValidAreaNoLock() const
{
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】对象为空。"));
		return MakeInvalidGeoBoundingBox();
	}

	// ---- Geographic CRS：直接返回经纬度范围（注意：跨日期线时 GetValidAreaLonLat() 会返回保守的全球经度范围）----
	if (spatialReference->IsGeographic() != 0)
	{
		GeoBoundingBox lonLatArea = GetValidAreaLonLatNoLock();
		const std::string selfWkt = ExportToWktUtf8NoLock(WktFormat::Wkt2_2018, false);

		if (!lonLatArea.IsValid())
		{
			GeoBoundingBox fallback;
			fallback.wktUtf8 = selfWkt;
			fallback.rect = useTraditionalGisAxisOrder
				? GB_Rectangle(-180.0, -90.0, 180.0, 90.0)   // X=经度, Y=纬度
				: GB_Rectangle(-90.0, -180.0, 90.0, 180.0);  // X=纬度, Y=经度（权威机构顺序）
			GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】GetValidAreaLonLat无效，返回全球范围。"));
			return fallback;
		}

		GeoBoundingBox result = lonLatArea;
		result.wktUtf8 = selfWkt;

		if (!useTraditionalGisAxisOrder)
		{
			// useTraditionalGisAxisOrder=false 时，数据轴顺序为“纬度/经度”，因此将 (lon,lat) 转为 (lat,lon)。
			result.rect = GB_Rectangle(lonLatArea.rect.minY, lonLatArea.rect.minX, lonLatArea.rect.maxY, lonLatArea.rect.maxX);
		}

		return result;
	}

	// ---- Projected/Local CRS：以经纬度 area of use 采样，再投影到目标 CRS 以估算其有效范围 ----
	const std::vector<LonLatAreaSegment> lonLatSegments = GetValidAreaLonLatSegmentsNoLock();
	if (lonLatSegments.empty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】lonLatSegments为空。"));
		return MakeInvalidGeoBoundingBox();
	}

	OGRSpatialReference sourceSrs;
	if (sourceSrs.importFromEPSG(4326) != OGRERR_NONE)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】WGS 84 坐标系导入失败。"));
		return MakeInvalidGeoBoundingBox();
	}
	EnsureTraditionalGisAxisOrder(sourceSrs);

	std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> targetSrs(spatialReference->Clone());
	if (!targetSrs)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】坐标系克隆失败。"));
		return MakeInvalidGeoBoundingBox();
	}

	// 计算 BoundingBox 时，为避免“权威轴序”导致 X/Y 含义混淆，这里统一用传统 GIS 顺序进行转换。
	// 然后再根据 useTraditionalGisAxisOrder 的配置，决定是否需要把结果交换为 northing/easting 顺序。
	EnsureTraditionalGisAxisOrder(*targetSrs);

	CoordinateTransformationPtr transform(OGRCreateCoordinateTransformation(&sourceSrs, targetSrs.get()));
	if (transform == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】OGRCreateCoordinateTransformation 失败。"));
		return MakeInvalidGeoBoundingBox();
	}

	// GDAL >= 3.4 时优先使用 TransformBounds：它会沿边界加密采样，
	// 对投影边界弯曲、极区与跨日期线分段范围更稳健；失败后再退回手工网格采样。
	const int gridCount = 21;
	GB_Rectangle transformBoundsRect = GB_Rectangle::Invalid;
	bool hasTransformBoundsRect = false;
	for (const LonLatAreaSegment& seg : lonLatSegments)
	{
		GB_Rectangle segmentRect;
		if (TryTransformLonLatSegmentBounds(*transform, seg, gridCount, segmentRect))
		{
			hasTransformBoundsRect = TryExpandRectangle(transformBoundsRect, segmentRect) || hasTransformBoundsRect;
		}
	}

	if (hasTransformBoundsRect && transformBoundsRect.IsValid())
	{
		GeoBoundingBox result;
		result.wktUtf8 = ExportToWktUtf8NoLock(WktFormat::Wkt2_2018, false);
		result.rect = transformBoundsRect;

		if (!useTraditionalGisAxisOrder && spatialReference->EPSGTreatsAsNorthingEasting() != 0)
		{
			result.rect = GB_Rectangle(result.rect.minY, result.rect.minX, result.rect.maxY, result.rect.maxX);
		}

		return result;
	}

	// 兜底采样密度：21x21。相较 3x3 更能覆盖非线性投影边界的弯曲情况。
	std::vector<double> longitudes;
	std::vector<double> latitudes;
	longitudes.reserve(static_cast<size_t>(gridCount) * static_cast<size_t>(gridCount) * lonLatSegments.size());
	latitudes.reserve(static_cast<size_t>(gridCount) * static_cast<size_t>(gridCount) * lonLatSegments.size());

	for (const LonLatAreaSegment& seg : lonLatSegments)
	{
		if (!IsFinite(seg.west) || !IsFinite(seg.south) || !IsFinite(seg.east) || !IsFinite(seg.north))
		{
			continue;
		}

		if (seg.south > seg.north || seg.west > seg.east)
		{
			continue;
		}

		AppendLonLatGridSamples(seg.west, seg.south, seg.east, seg.north, gridCount, longitudes, latitudes);
	}

	const size_t numPoints = longitudes.size();
	if (numPoints == 0 || numPoints > static_cast<size_t>(std::numeric_limits<int>::max()))
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】longitudes 为空。"));
		return MakeInvalidGeoBoundingBox();
	}

	const int numPointsInt = static_cast<int>(numPoints);
	std::vector<int> successFlags(numPoints, FALSE);

	// 注意：Transform 返回值在历史版本 GDAL 中可能不足以判断“部分点失败”，应以 pabSuccess 为准。
	transform->Transform(numPointsInt, longitudes.data(), latitudes.data(), nullptr, successFlags.data());

	double minX = std::numeric_limits<double>::infinity();
	double minY = std::numeric_limits<double>::infinity();
	double maxX = -std::numeric_limits<double>::infinity();
	double maxY = -std::numeric_limits<double>::infinity();
	bool hasAnyPoint = false;

	for (size_t i = 0; i < numPoints; i++)
	{
		if (successFlags[i] == FALSE)
		{
			continue;
		}

		const double x = longitudes[i];
		const double y = latitudes[i];

		if (!IsFinite(x) || !IsFinite(y))
		{
			continue;
		}

		hasAnyPoint = true;
		minX = std::min(minX, x);
		minY = std::min(minY, y);
		maxX = std::max(maxX, x);
		maxY = std::max(maxY, y);
	}

	if (!hasAnyPoint)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidArea】hasAnyPoint == false。"));
		return MakeInvalidGeoBoundingBox();
	}

	GeoBoundingBox result;
	result.wktUtf8 = ExportToWktUtf8NoLock(WktFormat::Wkt2_2018, false);
	result.rect = GB_Rectangle(minX, minY, maxX, maxY);

	// 若采用权威轴序，且该投影 CRS 定义为 northing/easting，则交换 X/Y。
	if (!useTraditionalGisAxisOrder && spatialReference->EPSGTreatsAsNorthingEasting() != 0)
	{
		result.rect = GB_Rectangle(result.rect.minY, result.rect.minX, result.rect.maxY, result.rect.maxX);
	}

	return result;
}

std::vector<std::vector<GB_Point2d>> GeoCrs::GetValidAreaPolygonsNoLock(int edgeSampleCount) const
{
	std::vector<std::vector<GB_Point2d>> result;

	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaPolygons】对象为空。"));
		return result;
	}

	const std::vector<LonLatAreaSegment> lonLatSegments = GetValidAreaLonLatSegmentsNoLock();
	if (lonLatSegments.empty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaPolygons】lonLatSegments为空。"));
		return result;
	}

	OGRSpatialReference sourceSrs;
	if (sourceSrs.importFromEPSG(4326) != OGRERR_NONE)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaPolygons】WGS 84 坐标系导入失败。"));
		return result;
	}
	EnsureTraditionalGisAxisOrder(sourceSrs);

	std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> targetSrs(spatialReference->Clone());
	if (!targetSrs)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaPolygons】坐标系克隆失败。"));
		return result;
	}

	// 输出坐标应与当前 GeoCrs 对象的轴顺序一致：默认传统 GIS 顺序；关闭后遵循权威轴序。
	ApplyAxisOrderStrategy(*targetSrs, useTraditionalGisAxisOrder);

	CoordinateTransformationPtr transform(OGRCreateCoordinateTransformation(&sourceSrs, targetSrs.get()));
	if (transform == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaPolygons】OGRCreateCoordinateTransformation 失败。"));
		return result;
	}

	const int normalizedEdgeSampleCount = NormalizeValidAreaPolygonEdgeSampleCount(edgeSampleCount);
	result.reserve(lonLatSegments.size());

	for (const LonLatAreaSegment& segment : lonLatSegments)
	{
		if (!IsValidLonLatAreaSegment(segment))
		{
			GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaPolygons】跳过无效经纬度分段。"));
			continue;
		}

		std::vector<double> longitudes;
		std::vector<double> latitudes;
		AppendLonLatBoundarySamples(segment.west, segment.south, segment.east, segment.north, normalizedEdgeSampleCount, longitudes, latitudes);

		std::vector<GB_Point2d> polygon;
		if (!TryTransformLonLatBoundaryToSelfPolygon(*transform, longitudes, latitudes, polygon))
		{
			GBLOG_WARNING(GB_STR("【GeoCrs::GetValidAreaPolygons】经纬度边界转换失败或无法形成有效多边形。"));
			continue;
		}

		result.push_back(std::move(polygon));
	}

	return result;
}


GeoCrs::~GeoCrs() = default;

GeoCrs& GeoCrs::operator=(const GeoCrs& other)
{
	if (this == &other)
	{
		return *this;
	}

	// 先在 other 的锁保护下克隆数据，尽量缩短双对象同时被锁住的时间，规避死锁风险。
	std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> clonedSrs(nullptr);
	bool otherUseTraditionalGisAxisOrder = true;
	bool otherHasCachedIsValid = false;
	bool otherCachedIsValid = false;
	bool otherHasCachedDefaultEpsgCode = false;
	int otherCachedDefaultEpsgCode = 0;
	int otherCachedUidKind = -2;
	std::uint64_t otherCachedUidWktHash = 0;
	bool otherHasCachedMetersPerUnit = false;
	double otherCachedMetersPerUnit = 0.0;


	{
		std::lock_guard<std::recursive_mutex> otherLock(other.mutex);

		otherUseTraditionalGisAxisOrder = other.useTraditionalGisAxisOrder;
		otherHasCachedIsValid = other.hasCachedIsValid;
		otherCachedIsValid = other.cachedIsValid;
		otherHasCachedDefaultEpsgCode = other.hasCachedDefaultEpsgCode;
		otherCachedDefaultEpsgCode = other.cachedDefaultEpsgCode;
		otherCachedUidKind = other.cachedUidKind;
		otherCachedUidWktHash = other.cachedUidWktHash;
		otherHasCachedMetersPerUnit = other.hasCachedMetersPerUnit;
		otherCachedMetersPerUnit = other.cachedMetersPerUnit;

		if (other.spatialReference)
		{
			clonedSrs.reset(other.spatialReference->Clone());
		}
		else
		{
			clonedSrs.reset(CreateOgrSpatialReference());
		}
	}

	{
		std::lock_guard<std::recursive_mutex> lock(mutex);

		spatialReference.swap(clonedSrs);
		if (spatialReference == nullptr)
		{
			useTraditionalGisAxisOrder = true;
			InvalidateCachesNoLock();
			return *this;
		}
		useTraditionalGisAxisOrder = otherUseTraditionalGisAxisOrder;

		if (spatialReference)
		{
			ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
		}

		hasCachedIsValid = otherHasCachedIsValid;
		cachedIsValid = otherCachedIsValid;
		hasCachedDefaultEpsgCode = otherHasCachedDefaultEpsgCode;
		cachedDefaultEpsgCode = otherCachedDefaultEpsgCode;
		cachedUidKind = otherCachedUidKind;
		cachedUidWktHash = otherCachedUidWktHash;
		hasCachedMetersPerUnit = otherHasCachedMetersPerUnit;
		cachedMetersPerUnit = otherCachedMetersPerUnit;
	}

	return *this;
}


GeoCrs& GeoCrs::operator=(GeoCrs&& other) noexcept
{
	if (this == &other)
	{
		return *this;
	}

	// 使用 std::lock 同时获取两把锁，规避死锁。
	std::unique_lock<std::recursive_mutex> lock(mutex, std::defer_lock);
	std::unique_lock<std::recursive_mutex> otherLock(other.mutex, std::defer_lock);
	std::lock(lock, otherLock);

	spatialReference = std::move(other.spatialReference);
	useTraditionalGisAxisOrder = other.useTraditionalGisAxisOrder;

	hasCachedIsValid = other.hasCachedIsValid;
	cachedIsValid = other.cachedIsValid;
	hasCachedDefaultEpsgCode = other.hasCachedDefaultEpsgCode;
	cachedDefaultEpsgCode = other.cachedDefaultEpsgCode;
	cachedUidKind = other.cachedUidKind;
	cachedUidWktHash = other.cachedUidWktHash;
	hasCachedMetersPerUnit = other.hasCachedMetersPerUnit;
	cachedMetersPerUnit = other.cachedMetersPerUnit;

	if (spatialReference)
	{
		ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
	}
	else
	{
		useTraditionalGisAxisOrder = true;
		hasCachedIsValid = false;
		cachedIsValid = false;
		hasCachedDefaultEpsgCode = false;
		cachedDefaultEpsgCode = 0;
		cachedUidKind = -2;
		cachedUidWktHash = 0;
		hasCachedMetersPerUnit = false;
		cachedMetersPerUnit = 0.0;
	}

	// 保持被移动对象可用（置为空 CRS）
	other.useTraditionalGisAxisOrder = true;
	other.hasCachedIsValid = false;
	other.cachedIsValid = false;
	other.hasCachedDefaultEpsgCode = false;
	other.cachedDefaultEpsgCode = 0;
	other.cachedUidKind = -2;
	other.cachedUidWktHash = 0;
	other.hasCachedMetersPerUnit = false;
	other.cachedMetersPerUnit = 0.0;

	if (other.spatialReference == nullptr)
	{
		other.spatialReference.reset(CreateOgrSpatialReference());
		if (other.spatialReference)
		{
			ApplyAxisOrderStrategy(*other.spatialReference, other.useTraditionalGisAxisOrder);
		}
	}

	return *this;
}


GeoCrs GeoCrs::CreateFromWkt(const std::string& wktUtf8)
{
	GeoCrs crs;
	crs.SetFromWkt(wktUtf8);
	return crs;
}

GeoCrs GeoCrs::CreateFromEpsgCode(int epsgCode)
{
	GeoCrs crs;
	crs.SetFromEpsgCode(epsgCode);
	return crs;
}

GeoCrs GeoCrs::CreateFromUserInput(const std::string& definitionUtf8, bool allowNetworkAccess, bool allowFileAccess)
{
	GeoCrs crs;
	crs.SetFromUserInput(definitionUtf8, allowNetworkAccess, allowFileAccess);
	return crs;
}

bool GeoCrs::Reset()
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return ResetNoLock();
}


bool GeoCrs::SetFromWkt(const std::string& wktUtf8)
{
	const std::string trimmed = GB_Utf8Trim(wktUtf8);
	if (trimmed.empty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromWkt】wkt为空。"));
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(mutex);

	std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> newSrs = CreateOgrSpatialReferencePtr();
	if (!newSrs)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromWkt】创建 srs 失败。"));
		return false;
	}

	const char* wkt = trimmed.c_str();
	CPLPushErrorHandler(CPLQuietErrorHandler);
	CPLErrorReset();
	const OGRErr err = newSrs->importFromWkt(&wkt);
	const std::string lastErrorMessage = SafeCString(CPLGetLastErrorMsg());
	CPLPopErrorHandler();
	if (err != OGRERR_NONE)
	{
		std::string message = GB_STR("【GeoCrs::SetFromWkt】importFromWkt失败: err=") + std::to_string(static_cast<int>(err));
		if (!lastErrorMessage.empty())
		{
			message += GB_STR(", message=") + lastErrorMessage;
		}
		GBLOG_WARNING(message);
		return false;
	}

	if (!IsOnlyAsciiWhitespaceOrEnd(wkt))
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromWkt】WKT 后存在无法解析的多余字符。"));
		return false;
	}

	ApplyAxisOrderStrategy(*newSrs, useTraditionalGisAxisOrder);
	if (!ValidatePreparedSpatialReference(*newSrs, GB_STR("【GeoCrs::SetFromWkt】")))
	{
		return false;
	}

	spatialReference.swap(newSrs);
	InvalidateCachesNoLock();
	return true;
}


bool GeoCrs::SetFromEpsgCode(int epsgCode)
{
	if (epsgCode <= 0)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromEpsgCode】epsgCode无效: ") + std::to_string(epsgCode));
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(mutex);

	std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> newSrs = CreateOgrSpatialReferencePtr();
	if (!newSrs)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromEpsgCode】创建 srs 失败。"));
		return false;
	}

	CPLPushErrorHandler(CPLQuietErrorHandler);
	CPLErrorReset();
	const OGRErr err = newSrs->importFromEPSG(epsgCode);
	const std::string lastErrorMessage = SafeCString(CPLGetLastErrorMsg());
	CPLPopErrorHandler();
	if (err != OGRERR_NONE)
	{
		std::string message = GB_STR("【GeoCrs::SetFromEpsgCode】importFromEPSG失败: err=") + std::to_string(static_cast<int>(err));
		if (!lastErrorMessage.empty())
		{
			message += GB_STR(", message=") + lastErrorMessage;
		}
		GBLOG_WARNING(message);
		return false;
	}

	ApplyAxisOrderStrategy(*newSrs, useTraditionalGisAxisOrder);
	if (!ValidatePreparedSpatialReference(*newSrs, GB_STR("【GeoCrs::SetFromEpsgCode】")))
	{
		return false;
	}

	spatialReference.swap(newSrs);
	InvalidateCachesNoLock();
	return true;
}


bool GeoCrs::SetFromUserInput(const std::string& definitionUtf8, bool allowNetworkAccess, bool allowFileAccess)
{
	const std::string trimmed = GB_Utf8Trim(definitionUtf8);
	if (trimmed.empty())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromUserInput】definition为空。"));
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(mutex);

	std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> newSrs = CreateOgrSpatialReferencePtr();
	if (!newSrs)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::SetFromUserInput】创建 srs 失败。"));
		return false;
	}

	// GDAL 3.x 起，为了安全性，SetFromUserInput() 默认会带有访问限制（不允许网络/文件读取）。
	// 若确需放开，需要使用带 options 的重载，显式传入 ALLOW_NETWORK_ACCESS / ALLOW_FILE_ACCESS。
	const char* const networkOption = allowNetworkAccess ? "ALLOW_NETWORK_ACCESS=YES" : "ALLOW_NETWORK_ACCESS=NO";
	const char* const fileOption = allowFileAccess ? "ALLOW_FILE_ACCESS=YES" : "ALLOW_FILE_ACCESS=NO";
	const char* const options[] = { networkOption, fileOption, nullptr };

	CPLPushErrorHandler(CPLQuietErrorHandler);
	CPLErrorReset();
	const OGRErr err = newSrs->SetFromUserInput(trimmed.c_str(), options);
	const std::string lastErrorMessage = SafeCString(CPLGetLastErrorMsg());
	CPLPopErrorHandler();

	if (err != OGRERR_NONE)
	{
		// 一些 WMTS/WMS 能力文档里会出现历史遗留的 EPSG URN 写法，例如：
		//   urn:ogc:def:crs:EPSG:6.18:3:3857
		// 其中 "6.18:3" 实际上是把版本号 "6.18.3" 错误拆成了两个 URN 字段。
		int epsgCode = 0;
		if (TryExtractEpsgCodeFromOgcCrsUrn(trimmed, epsgCode))
		{
			return SetFromEpsgCode(epsgCode);
		}

		std::string message = GB_STR("【GeoCrs::SetFromUserInput】SetFromUserInput失败: err=") + std::to_string(static_cast<int>(err)) + GB_STR(", definition=") + trimmed;
		if (!lastErrorMessage.empty())
		{
			message += GB_STR(", message=") + lastErrorMessage;
		}
		GBLOG_WARNING(message);
		return false;
	}

	ApplyAxisOrderStrategy(*newSrs, useTraditionalGisAxisOrder);
	if (!ValidatePreparedSpatialReference(*newSrs, GB_STR("【GeoCrs::SetFromUserInput】")))
	{
		return false;
	}

	spatialReference.swap(newSrs);
	InvalidateCachesNoLock();
	return true;
}


bool GeoCrs::IsEmpty() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return IsEmptyNoLock();
}


bool GeoCrs::IsValid() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return IsValidNoLock();
}


std::string GeoCrs::GetNameUtf8() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetNameUtf8】变量为空。"));
		return "";
	}

	const char* name = spatialReference->GetName();
	return name ? std::string(name) : std::string();
}


std::string GeoCrs::GetReferenceEllipsoidNameUtf8() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetReferenceEllipsoidNameUtf8】变量为空。"));
		return "";
	}

	const char* ellipsoidName = spatialReference->GetAttrValue("ELLIPSOID", 0);
	if (ellipsoidName == nullptr || ellipsoidName[0] == '\0')
	{
		ellipsoidName = spatialReference->GetAttrValue("SPHEROID", 0);
	}

	return ellipsoidName ? std::string(ellipsoidName) : std::string();
}


std::string GeoCrs::GetUidUtf8() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);

	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetUidUtf8】变量为空。"));
		return "";
	}

	// 命中缓存
	if (cachedUidKind != -2)
	{
		if (cachedUidKind < 0)
		{
			return "";
		}
		if (cachedUidKind > 0)
		{
			return "EPSG:" + std::to_string(cachedUidKind);
		}

		return "WKT2_2018_HASH:" + ToHex64(cachedUidWktHash);
	}

	const int epsgCode = TryGetEpsgCodeNoLock(false, false, 0);
	if (epsgCode > 0)
	{
		cachedUidKind = epsgCode;
		cachedUidWktHash = 0;
		return "EPSG:" + std::to_string(epsgCode);
	}

	const std::string wkt = ExportToWktUtf8NoLock(WktFormat::Wkt2_2018, false);
	if (wkt.empty())
	{
		cachedUidKind = -1;
		cachedUidWktHash = 0;
		return "";
	}

	const std::uint64_t wktHash = Fnv1a64(wkt.data(), wkt.size());
	cachedUidKind = 0;
	cachedUidWktHash = wktHash;
	return "WKT2_2018_HASH:" + ToHex64(wktHash);
}


bool GeoCrs::operator==(const GeoCrs& other) const
{
	if (this == &other)
	{
		return true;
	}

	const GeoCrs* first = this;
	const GeoCrs* second = &other;

	// 规避死锁：按地址顺序加锁
	if (std::less<const GeoCrs*>()(second, first))
	{
		std::swap(first, second);
	}

	std::lock_guard<std::recursive_mutex> firstLock(first->mutex);
	std::lock_guard<std::recursive_mutex> secondLock(second->mutex);

	const bool thisEmpty = this->IsEmptyNoLock();
	const bool otherEmpty = other.IsEmptyNoLock();
	if (thisEmpty && otherEmpty)
	{
		return true;
	}
	if (thisEmpty != otherEmpty)
	{
		return false;
	}

	return spatialReference->IsSame(other.spatialReference.get());
}


bool GeoCrs::operator!=(const GeoCrs& other) const
{
	return !(*this == other);
}

bool GeoCrs::IsGeographic() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::IsGeographic】变量为空。"));
		return false;
	}

	return spatialReference->IsGeographic() != 0;
}


bool GeoCrs::IsProjected() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::IsProjected】变量为空。"));
		return false;
	}

	return spatialReference->IsProjected() != 0;
}


bool GeoCrs::IsLocal() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::IsLocal】变量为空。"));
		return false;
	}

	return spatialReference->IsLocal() != 0;
}

bool GeoCrs::IsTraditionalGisAxisOrderEnabled() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return useTraditionalGisAxisOrder;
}


void GeoCrs::SetTraditionalGisAxisOrder(bool enable)
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	useTraditionalGisAxisOrder = enable;

	if (spatialReference)
	{
		ApplyAxisOrderStrategy(*spatialReference, useTraditionalGisAxisOrder);
	}

	InvalidateCachesNoLock();
}


std::string GeoCrs::ExportToWktUtf8(WktFormat format, bool multiline) const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return ExportToWktUtf8NoLock(format, multiline);
}


std::string GeoCrs::ExportToPrettyWktUtf8(bool simplify) const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToPrettyWktUtf8】变量为空。"));
		return "";
	}

	char* wktRaw = nullptr;
	const OGRErr err = spatialReference->exportToPrettyWkt(&wktRaw, simplify ? TRUE : FALSE);
	CplCharPtr wkt(wktRaw);

	if (err != OGRERR_NONE || wkt == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToPrettyWktUtf8】exportToPrettyWkt 失败: err=") +
			std::to_string(static_cast<int>(err)) +
			GB_STR(", wkt=") +
			(wktRaw ? std::string(wktRaw) : ""));
		return "";
	}

	return std::string(wkt.get());
}

std::string GeoCrs::ExportToProj4Utf8() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToProj4Utf8】变量为空。"));
		return "";
	}

	char* proj4Raw = nullptr;
	const OGRErr err = spatialReference->exportToProj4(&proj4Raw);
	CplCharPtr proj4(proj4Raw);

	if (err != OGRERR_NONE || proj4 == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToProj4Utf8】exportToProj4 失败: err=") +
			std::to_string(static_cast<int>(err)) +
			GB_STR(", proj4=") +
			(proj4Raw ? std::string(proj4Raw) : ""));
		return "";
	}

	return std::string(proj4.get());
}

std::string GeoCrs::ExportToProjJsonUtf8() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToProjJsonUtf8】变量为空。"));
		return "";
	}

	char* projJsonRaw = nullptr;
	const OGRErr err = spatialReference->exportToPROJJSON(&projJsonRaw, nullptr);
	CplCharPtr projJson(projJsonRaw);

	if (err != OGRERR_NONE || projJson == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ExportToProjJsonUtf8】exportToPROJJSON 失败: err=") +
			std::to_string(static_cast<int>(err)) +
			GB_STR(", projJson=") +
			(projJsonRaw ? std::string(projJsonRaw) : ""));
		return "";
	}

	return std::string(projJson.get());
}

int GeoCrs::TryGetEpsgCode(bool tryAutoIdentify, bool tryFindBestMatch, int minMatchConfidence) const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return TryGetEpsgCodeNoLock(tryAutoIdentify, tryFindBestMatch, minMatchConfidence);
}

std::string GeoCrs::ToEpsgStringUtf8() const
{
	const int epsgCode = TryGetEpsgCode(true);
	if (epsgCode <= 0)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ToEpsgStringUtf8】未能获取 EPSG code。"));
		return "";
	}
	return "EPSG:" + std::to_string(epsgCode);
}

std::string GeoCrs::ToAuthorityStringUtf8(bool tryAutoIdentify, bool tryFindBestMatch, int minMatchConfidence) const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return ToAuthorityStringUtf8NoLock(tryAutoIdentify, tryFindBestMatch, minMatchConfidence);
}

std::string GeoCrs::ToOgcUrnStringUtf8() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ToOgcUrnStringUtf8】对象为空。"));
		return "";
	}

	char* urnRaw = spatialReference->GetOGCURN();
	CplCharPtr urn(urnRaw);
	if (urn == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::ToOgcUrnStringUtf8】空的 urn。"));
		return "";
	}

	return std::string(urn.get());
}

GeoCrs::UnitsInfo GeoCrs::GetLinearUnits() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	UnitsInfo info;
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetLinearUnits】对象为空。"));
		return info;
	}

	const char* unitName = nullptr;
	const double toMeters = spatialReference->GetLinearUnits(&unitName);
	info.toSI = toMeters;
	info.nameUtf8 = unitName ? std::string(unitName) : std::string();
	return info;
}

GeoCrs::UnitsInfo GeoCrs::GetAngularUnits() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	UnitsInfo info;
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetAngularUnits】对象为空。"));
		return info;
	}

	const char* unitName = nullptr;
	const double toRadians = spatialReference->GetAngularUnits(&unitName);
	info.toSI = toRadians;
	info.nameUtf8 = unitName ? std::string(unitName) : std::string();
	return info;
}

double GeoCrs::GetMetersPerUnit() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (IsEmptyNoLock())
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetMetersPerUnit】对象为空。"));
		return 0.0;
	}

	if (hasCachedMetersPerUnit)
	{
		return cachedMetersPerUnit;
	}

	double metersPerUnit = 0.0;

	// Geographic CRS 的坐标单位为角度/弧度等角单位。这里返回“每 1 个角单位对应的弧长（米）”，
	// 以椭球长半轴作为半径，近似等于赤道处的米/度。
	if (spatialReference->IsGeographic() != 0)
	{
		const char* angularUnitName = nullptr;
		const double angularUnitToRadians = spatialReference->GetAngularUnits(&angularUnitName);
		const double semiMajorAxisMeters = spatialReference->GetSemiMajor(nullptr);

		if (IsFinite(angularUnitToRadians) && angularUnitToRadians > 0.0 &&
			IsFinite(semiMajorAxisMeters) && semiMajorAxisMeters > 0.0)
		{
			metersPerUnit = angularUnitToRadians * semiMajorAxisMeters;
		}
	}
	else
	{
		const char* linearUnitName = nullptr;
		const double linearUnitToMeters = spatialReference->GetLinearUnits(&linearUnitName);
		if (IsFinite(linearUnitToMeters) && linearUnitToMeters > 0.0)
		{
			metersPerUnit = linearUnitToMeters;
		}
	}

	if (!IsFinite(metersPerUnit) || metersPerUnit <= 0.0)
	{
		metersPerUnit = 0.0;
	}

	cachedMetersPerUnit = metersPerUnit;
	hasCachedMetersPerUnit = true;
	return metersPerUnit;
}

std::vector<GeoCrs::LonLatAreaSegment> GeoCrs::GetValidAreaLonLatSegments() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return GetValidAreaLonLatSegmentsNoLock();
}

GeoBoundingBox GeoCrs::GetValidArea() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return GetValidAreaNoLock();
}

std::vector<std::vector<GB_Point2d>> GeoCrs::GetValidAreaPolygons(int edgeSampleCount) const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return GetValidAreaPolygonsNoLock(edgeSampleCount);
}


GeoBoundingBox GeoCrs::GetValidAreaLonLat() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return GetValidAreaLonLatNoLock();
}

const OGRSpatialReference* GeoCrs::GetConst() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	return spatialReference.get();
}

const OGRSpatialReference& GeoCrs::GetConstRef() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	// 约定上 spatialReference 在构造/Reset 后都应存在；此处仍做兜底，避免空指针解引用。
	if (spatialReference == nullptr)
	{
		static const OGRSpatialReference emptySrs;
		return emptySrs;
	}

	return *spatialReference;
}

std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> GeoCrs::CloneOgrSpatialReference() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex);
	if (spatialReference == nullptr)
	{
		return std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter>(nullptr);
	}

	std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> cloned(spatialReference->Clone());
	if (cloned)
	{
		ApplyAxisOrderStrategy(*cloned, useTraditionalGisAxisOrder);
	}
	return cloned;
}

OGRSpatialReference& GeoCrs::GetRef()
{
	std::lock_guard<std::recursive_mutex> lock(mutex);

	OGRSpatialReference* srs = EnsureSpatialReferenceNoLock();
	if (srs == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::GetRef】空的 srs。"));

		static OGRSpatialReference empty;
		return empty;
	}

	InvalidateCachesNoLock();
	return *srs;
}

OGRSpatialReference* GeoCrs::Get()
{
	std::lock_guard<std::recursive_mutex> lock(mutex);

	OGRSpatialReference* srs = EnsureSpatialReferenceNoLock();
	if (srs == nullptr)
	{
		GBLOG_WARNING(GB_STR("【GeoCrs::Get】空的 srs。"));
		return nullptr;
	}

	InvalidateCachesNoLock();
	return srs;
}
