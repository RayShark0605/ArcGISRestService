#include "GeoCrsTransform.h"

#include "GeoCrs.h"
#include "GeoCrsManager.h"

#include "GeoBase/GB_Logger.h"
#include "GeoBase/GB_Utf8String.h"

#include "GeoBase/Geometry/GB_Point2d.h"
#include "GeoBase/Geometry/GB_Rectangle.h"

#include "gdal_version.h"
#include "ogr_spatialref.h"
#include "ogr_srs_api.h"

#include "opencv2/core.hpp"
#include "opencv2/imgproc.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace
{
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
    using OgrSrsPtr = std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter>;

    static bool IsFinite(double value)
    {
        return std::isfinite(value);
    }

    static bool IsFinitePoint(const GB_Point2d& point)
    {
        return IsFinite(point.x) && IsFinite(point.y);
    }

    static bool IsPositiveFinite(double value)
    {
        return IsFinite(value) && value > 0.0;
    }

    static bool IsNearlyEqual(double left, double right, double tolerance)
    {
        return std::fabs(left - right) <= tolerance;
    }

    static bool IsDegreeAngularUnit(const OGRSpatialReference& srs)
    {
        if (srs.IsGeographic() == 0)
        {
            return false;
        }

        const double radiansPerUnit = srs.GetAngularUnits(nullptr);
        constexpr double degreeToRadians = 0.017453292519943295769236907684886;
        return IsPositiveFinite(radiansPerUnit) && IsNearlyEqual(radiansPerUnit, degreeToRadians, 1e-14);
    }

    static double GetRectangleWidth(const GB_Rectangle& rect)
    {
        return rect.maxX - rect.minX;
    }

    static double GetRectangleHeight(const GB_Rectangle& rect)
    {
        return rect.maxY - rect.minY;
    }

    static bool IsNonEmptyRectangle(const GB_Rectangle& rect)
    {
        return rect.IsValid() && GetRectangleWidth(rect) > 0.0 && GetRectangleHeight(rect) > 0.0;
    }

    static bool IsInsideRectangle(const GB_Rectangle& rect, double x, double y)
    {
        return x >= rect.minX && x <= rect.maxX && y >= rect.minY && y <= rect.maxY;
    }

    static void EnsureTraditionalGisAxisOrder(OGRSpatialReference& srs)
    {
        srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }

    static double NormalizeLongitudeDegrees(double longitude)
    {
        if (!IsFinite(longitude))
        {
            return longitude;
        }

        // 归一化到 [-180, 180]
        double normalized = std::fmod(longitude, 360.0);
        if (normalized > 180.0)
        {
            normalized -= 360.0;
        }
        else if (normalized < -180.0)
        {
            normalized += 360.0;
        }

        return normalized;
    }

    static bool TryClampRectangleToValidArea(GB_Rectangle& rect, const GB_Rectangle& validRect)
    {
        if (!IsNonEmptyRectangle(rect))
        {
            return false;
        }

        if (!IsNonEmptyRectangle(validRect))
        {
            return true;
        }

        const GB_Rectangle clippedRect = rect.Intersected(validRect);
        if (!IsNonEmptyRectangle(clippedRect))
        {
            rect = GB_Rectangle::Invalid;
            return false;
        }

        rect = clippedRect;
        return true;
    }

    struct TransformKey
    {
        std::string sourceUid;
        std::string targetUid;

        bool operator==(const TransformKey& other) const
        {
            return sourceUid == other.sourceUid && targetUid == other.targetUid;
        }
    };

    struct TransformKeyHasher
    {
        size_t operator()(const TransformKey& key) const
        {
            const std::hash<std::string> hasher;
            size_t hashValue = hasher(key.sourceUid);
            hashValue ^= hasher(key.targetUid) + 0x9e3779b97f4a7c15ULL + (hashValue << 6) + (hashValue >> 2);
            return hashValue;
        }
    };

    struct TransformItem
    {
        OgrSrsPtr sourceSrs;
        OgrSrsPtr targetSrs;
        CoordinateTransformationPtr transform;

        bool sourceIsGeographic = false;
        bool targetIsGeographic = false;
        bool sourceLongitudeIsDegree = false;
        bool targetLongitudeIsDegree = false;

        // 源 CRS 的自身有效范围（便于 bbox / GeoImage 先求交）
        GB_Rectangle sourceValidRect;
        bool hasSourceValidRect = false;

        // 目标 CRS 的自身有效范围（确保输出范围不会越过目标 CRS 的合法范围）
        GB_Rectangle targetValidRect;
        bool hasTargetValidRect = false;

        std::string canonicalTargetWkt;
    };

    static thread_local std::unordered_map<TransformKey, TransformItem, TransformKeyHasher> g_threadTransformCache;

    static bool TryGetTransformItem(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, TransformItem*& outItem)
    {
        outItem = nullptr;

        const std::string trimmedSourceWkt = GB_Utf8Trim(sourceWktUtf8);
        const std::string trimmedTargetWkt = GB_Utf8Trim(targetWktUtf8);
        if (trimmedSourceWkt.empty() || trimmedTargetWkt.empty())
        {
            return false;
        }

        const std::shared_ptr<const GeoCrs> sourceCrs = GeoCrsManager::GetFromDefinitionCached(trimmedSourceWkt);
        const std::shared_ptr<const GeoCrs> targetCrs = GeoCrsManager::GetFromDefinitionCached(trimmedTargetWkt);
        if (!sourceCrs || !targetCrs || !sourceCrs->IsValid() || !targetCrs->IsValid())
        {
            return false;
        }

        const std::string canonicalSourceWkt = sourceCrs->ExportToWktUtf8(GeoCrs::WktFormat::Wkt2_2018, false);
        const std::string canonicalTargetWkt = targetCrs->ExportToWktUtf8(GeoCrs::WktFormat::Wkt2_2018, false);

        std::string sourceCacheKey = canonicalSourceWkt.empty() ? sourceCrs->GetUidUtf8() : canonicalSourceWkt;
        std::string targetCacheKey = canonicalTargetWkt.empty() ? targetCrs->GetUidUtf8() : canonicalTargetWkt;
        if (sourceCacheKey.empty() || targetCacheKey.empty())
        {
            return false;
        }

        TransformKey key;
        key.sourceUid = std::move(sourceCacheKey);
        key.targetUid = std::move(targetCacheKey);

        auto it = g_threadTransformCache.find(key);
        if (it != g_threadTransformCache.end())
        {
            outItem = &it->second;
            return it->second.transform != nullptr;
        }

        TransformItem item;
        item.sourceIsGeographic = sourceCrs->IsGeographic();
        item.targetIsGeographic = targetCrs->IsGeographic();

        const OGRSpatialReference& sourceRef = sourceCrs->GetConstRef();
        const OGRSpatialReference& targetRef = targetCrs->GetConstRef();
        item.sourceLongitudeIsDegree = IsDegreeAngularUnit(sourceRef);
        item.targetLongitudeIsDegree = IsDegreeAngularUnit(targetRef);

        const std::string validAreaSourceWkt = canonicalSourceWkt.empty() ? trimmedSourceWkt : canonicalSourceWkt;

        // 使用目标 CRS 的规范化 WKT（WKT2_2018），确保输出 GeoBoundingBox 的 wktUtf8 稳定。
        item.canonicalTargetWkt = canonicalTargetWkt;
        if (item.canonicalTargetWkt.empty())
        {
            // 兜底：至少保留用户输入。
            item.canonicalTargetWkt = trimmedTargetWkt;
        }

        // 取源 CRS 的自身有效范围（如果可用）。
        // 注意：TryGetValidAreasCached 以 WKT 为输入，因此这里优先使用规范化 WKT，避免传入 "EPSG:xxxx" 时无法取到有效范围。
        {
            GeoBoundingBox lonLatArea;
            GeoBoundingBox selfArea;
            GeoCrsManager::TryGetValidAreasCached(validAreaSourceWkt, lonLatArea, selfArea);
            if (selfArea.rect.IsValid())
            {
                item.sourceValidRect = selfArea.rect;
                item.hasSourceValidRect = IsNonEmptyRectangle(selfArea.rect);
            }
        }

        // 取目标 CRS 的自身有效范围（如果可用）。
        {
            GeoBoundingBox lonLatArea;
            GeoBoundingBox selfArea;
            GeoCrsManager::TryGetValidAreasCached(item.canonicalTargetWkt, lonLatArea, selfArea);
            if (selfArea.rect.IsValid())
            {
                item.targetValidRect = selfArea.rect;
                item.hasTargetValidRect = IsNonEmptyRectangle(selfArea.rect);
            }
        }

        item.sourceSrs.reset(sourceRef.Clone());
        item.targetSrs.reset(targetRef.Clone());
        if (!item.sourceSrs || !item.targetSrs)
        {
            return false;
        }

        EnsureTraditionalGisAxisOrder(*item.sourceSrs);
        EnsureTraditionalGisAxisOrder(*item.targetSrs);

        item.transform.reset(OGRCreateCoordinateTransformation(item.sourceSrs.get(), item.targetSrs.get()));
        if (item.transform == nullptr)
        {
            return false;
        }

        auto insertResult = g_threadTransformCache.emplace(std::move(key), std::move(item));
        outItem = &insertResult.first->second;
        return outItem->transform != nullptr;
    }

    static bool TryTransformSingleXYInternal(TransformItem& item, double x, double y, double& outX, double& outY)
    {
        outX = x;
        outY = y;

        if (!IsFinite(x) || !IsFinite(y) || item.transform == nullptr)
        {
            return false;
        }

        // 对 Geographic CRS 的经度做适度归一化，减少“超范围但等价”的失败。
        double inputX = x;
        double inputY = y;
        if (item.sourceLongitudeIsDegree)
        {
            inputX = NormalizeLongitudeDegrees(inputX);
        }

        int successFlag = FALSE;
        double transformedX = inputX;
        double transformedY = inputY;
        const int overallOk = item.transform->Transform(1, &transformedX, &transformedY, nullptr, &successFlag);

        if (overallOk == FALSE || successFlag == FALSE || !IsFinite(transformedX) || !IsFinite(transformedY))
        {
            return false;
        }

        // 若目标是 Geographic CRS，也进行经度归一化。跨日期线时，单点归一化是安全的。
        if (item.targetLongitudeIsDegree)
        {
            transformedX = NormalizeLongitudeDegrees(transformedX);
        }

        outX = transformedX;
        outY = transformedY;
        return true;
    }

    static bool TryTransformSingleXYZInternal(TransformItem& item, double x, double y, double z, double& outX, double& outY, double& outZ)
    {
        outX = x;
        outY = y;
        outZ = z;

        if (!IsFinite(x) || !IsFinite(y) || !IsFinite(z) || item.transform == nullptr)
        {
            return false;
        }

        double inputX = x;
        double inputY = y;
        double inputZ = z;
        if (item.sourceLongitudeIsDegree)
        {
            inputX = NormalizeLongitudeDegrees(inputX);
        }

        int successFlag = FALSE;
        const int overallOk = item.transform->Transform(1, &inputX, &inputY, &inputZ, &successFlag);

        if (overallOk == FALSE || successFlag == FALSE || !IsFinite(inputX) || !IsFinite(inputY) || !IsFinite(inputZ))
        {
            return false;
        }

        if (item.targetLongitudeIsDegree)
        {
            inputX = NormalizeLongitudeDegrees(inputX);
        }

        outX = inputX;
        outY = inputY;
        outZ = inputZ;
        return true;
    }

    static bool TryTransformXYArrayInternal(TransformItem& item, std::vector<double>& xValues, std::vector<double>& yValues, std::vector<int>& successFlags)
    {
        const size_t count = xValues.size();
        if (count == 0)
        {
            successFlags.clear();
            return true;
        }

        if (count != yValues.size() || count > static_cast<size_t>(std::numeric_limits<int>::max()) || item.transform == nullptr)
        {
            successFlags.assign(count, FALSE);
            return false;
        }

        if (item.sourceLongitudeIsDegree)
        {
            for (size_t i = 0; i < count; i++)
            {
                xValues[i] = NormalizeLongitudeDegrees(xValues[i]);
            }
        }

        successFlags.assign(count, FALSE);
        const int overallOk = item.transform->Transform(static_cast<int>(count), xValues.data(), yValues.data(), nullptr, successFlags.data());

        for (size_t i = 0; i < count; i++)
        {
            if (successFlags[i] == FALSE || !IsFinite(xValues[i]) || !IsFinite(yValues[i]))
            {
                successFlags[i] = FALSE;
                continue;
            }

            if (item.targetLongitudeIsDegree)
            {
                xValues[i] = NormalizeLongitudeDegrees(xValues[i]);
            }
        }

        return overallOk != FALSE;
    }

    static void AppendRectangleGridSamples(const GB_Rectangle& rect, int sampleGridCount, std::vector<double>& xs, std::vector<double>& ys)
    {
        const int count = std::max(2, sampleGridCount);
        xs.reserve(xs.size() + static_cast<size_t>(count) * static_cast<size_t>(count));
        ys.reserve(ys.size() + static_cast<size_t>(count) * static_cast<size_t>(count));

        for (int yIndex = 0; yIndex < count; yIndex++)
        {
            const double yT = (count <= 1) ? 0.0 : static_cast<double>(yIndex) / static_cast<double>(count - 1);
            const double y = rect.minY + (rect.maxY - rect.minY) * yT;

            for (int xIndex = 0; xIndex < count; xIndex++)
            {
                const double xT = (count <= 1) ? 0.0 : static_cast<double>(xIndex) / static_cast<double>(count - 1);
                const double x = rect.minX + (rect.maxX - rect.minX) * xT;
                xs.push_back(x);
                ys.push_back(y);
            }
        }
    }

    static bool TryFinalizeTargetRectangleInternal(TransformItem& item, bool clampToCrsValidArea, GB_Rectangle& targetRect)
    {
        if (!IsNonEmptyRectangle(targetRect))
        {
            targetRect = GB_Rectangle::Invalid;
            return false;
        }

        if (clampToCrsValidArea && item.hasTargetValidRect && !TryClampRectangleToValidArea(targetRect, item.targetValidRect))
        {
            return false;
        }

        return IsNonEmptyRectangle(targetRect);
    }

    static bool TryTransformRectangleToAabbInternal(TransformItem& item, const GB_Rectangle& sourceRect, int sampleGridCount, bool clampToCrsValidArea, GB_Rectangle& outTargetRect)
    {
        outTargetRect = GB_Rectangle::Invalid;

        if (!IsNonEmptyRectangle(sourceRect) || item.transform == nullptr)
        {
            return false;
        }

        GB_Rectangle workingRect = sourceRect;

        // 与源 CRS 的有效范围求交（更接近“部分交集”的真实语义）。
        if (clampToCrsValidArea && item.hasSourceValidRect && item.sourceValidRect.IsValid())
        {
            workingRect = workingRect.Intersected(item.sourceValidRect);
        }

        // 若求交后退化/无效，则认为无法给出合理转换结果。
        if (!IsNonEmptyRectangle(workingRect))
        {
            return false;
        }

        // 优先使用 TransformBounds（GDAL 3.4+）：内部会沿边界加密采样，通常比手工网格更可靠。
        // 兼容性：若 GDAL < 3.4，则直接走下方“兜底：手工网格采样”。
#if defined(GDAL_VERSION_NUM) && GDAL_VERSION_NUM >= 3040000
        const int densifyPoints = std::max(2, sampleGridCount);
        double boundsMinX = 0.0;
        double boundsMinY = 0.0;
        double boundsMaxX = 0.0;
        double boundsMaxY = 0.0;
        const int boundsOk = item.transform->TransformBounds(workingRect.minX, workingRect.minY, workingRect.maxX, workingRect.maxY, &boundsMinX, &boundsMinY, &boundsMaxX, &boundsMaxY, densifyPoints);

        if (boundsOk != FALSE && IsFinite(boundsMinX) && IsFinite(boundsMinY) && IsFinite(boundsMaxX) && IsFinite(boundsMaxY))
        {
            // 目标为 Geographic CRS 时：TransformBounds 用“xmax < xmin”表示跨越反经线（日期变更线）。
            // 由于 GB_Rectangle 只能表示单段经度，这里保守返回全球经度范围。
            if (item.targetIsGeographic && boundsMaxX < boundsMinX)
            {
                boundsMinX = -180.0;
                boundsMaxX = 180.0;
            }

            outTargetRect.Set(boundsMinX, boundsMinY, boundsMaxX, boundsMaxY);
            return TryFinalizeTargetRectangleInternal(item, clampToCrsValidArea, outTargetRect);
        }
#endif

        // ---- 兜底：手工网格采样 ----
        std::vector<double> xValues;
        std::vector<double> yValues;
        AppendRectangleGridSamples(workingRect, sampleGridCount, xValues, yValues);

        const size_t numPoints = xValues.size();
        if (numPoints == 0 || numPoints > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        std::vector<int> successFlags(numPoints, FALSE);
        TryTransformXYArrayInternal(item, xValues, yValues, successFlags);

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

            const double x = xValues[i];
            const double y = yValues[i];
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
            return false;
        }

        if (item.targetIsGeographic)
        {
            const double lonRange = maxX - minX;
            if (lonRange > 180.0)
            {
                minX = -180.0;
                maxX = 180.0;
            }
        }

        outTargetRect.Set(minX, minY, maxX, maxY);
        return TryFinalizeTargetRectangleInternal(item, clampToCrsValidArea, outTargetRect);
    }

    static void TransformPointsChunkInternal(TransformItem& item, std::vector<GB_Point2d>& points, size_t baseIndex, size_t count, std::atomic_bool& allOk, std::vector<double>& xValues, std::vector<double>& yValues, std::vector<int>& successFlags, std::vector<size_t>& indexMap)
    {
        xValues.clear();
        yValues.clear();
        indexMap.clear();

        xValues.reserve(count);
        yValues.reserve(count);
        indexMap.reserve(count);

        for (size_t i = 0; i < count; i++)
        {
            const size_t pointIndex = baseIndex + i;
            GB_Point2d& point = points[pointIndex];

            if (!IsFinitePoint(point))
            {
                allOk.store(false, std::memory_order_relaxed);
                continue;
            }

            double x = point.x;
            const double y = point.y;
            if (item.sourceLongitudeIsDegree)
            {
                x = NormalizeLongitudeDegrees(x);
            }

            indexMap.push_back(pointIndex);
            xValues.push_back(x);
            yValues.push_back(y);
        }

        const size_t validCount = indexMap.size();
        if (validCount == 0)
        {
            return;
        }

        if (validCount > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            allOk.store(false, std::memory_order_relaxed);
            return;
        }

        successFlags.assign(validCount, FALSE);
        const int overallOk = item.transform->Transform(static_cast<int>(validCount), xValues.data(), yValues.data(), nullptr, successFlags.data());

        if (overallOk == FALSE)
        {
            allOk.store(false, std::memory_order_relaxed);
        }

        for (size_t i = 0; i < validCount; i++)
        {
            if (successFlags[i] == FALSE || !IsFinite(xValues[i]) || !IsFinite(yValues[i]))
            {
                allOk.store(false, std::memory_order_relaxed);
                continue;
            }

            double x = xValues[i];
            const double y = yValues[i];
            if (item.targetLongitudeIsDegree)
            {
                x = NormalizeLongitudeDegrees(x);
            }

            GB_Point2d& point = points[indexMap[i]];
            point.Set(x, y);
            if (!point.IsValid())
            {
                allOk.store(false, std::memory_order_relaxed);
            }
        }
    }

    static int ToCvInterpolation(GB_ImageInterpolation interpolation)
    {
        switch (interpolation)
        {
        case GB_ImageInterpolation::Nearest:
            return cv::INTER_NEAREST;
        case GB_ImageInterpolation::Cubic:
            return cv::INTER_CUBIC;
        case GB_ImageInterpolation::Lanczos4:
            return cv::INTER_LANCZOS4;
        case GB_ImageInterpolation::Area:
            // cv::remap 不适合使用 INTER_AREA，这里回退到线性插值。
            return cv::INTER_LINEAR;
        case GB_ImageInterpolation::Linear:
        default:
            return cv::INTER_LINEAR;
        }
    }

    static bool TryCreateBgraSourceImage(const GB_Image& sourceImage, GB_Image& outImage)
    {
        outImage.Clear();

        if (sourceImage.IsEmpty())
        {
            return false;
        }

        const int channels = sourceImage.GetChannels();
        if (channels == 1)
        {
            outImage = sourceImage.ConvertColor(GB_ImageColorConversion::GrayToBgra);
        }
        else if (channels == 3)
        {
            outImage = sourceImage.ConvertColor(GB_ImageColorConversion::BgrToBgra);
        }
        else if (channels == 4)
        {
            outImage = sourceImage.Clone();
        }
        else
        {
            GBLOG_ERROR(GB_STR("【GeoCrsTransform::ReprojectGeoImage】暂不支持 1/3/4 通道之外的 GeoImage 重投影。"));
            return false;
        }

        return !outImage.IsEmpty() && outImage.GetChannels() == 4;
    }
    static bool ApplySourceWorkingRectMask(cv::Mat& sourceMat, const GB_Rectangle& sourceRect, const GB_Rectangle& sourceWorkingRect)
    {
        if (sourceMat.empty() || !IsNonEmptyRectangle(sourceRect) || !IsNonEmptyRectangle(sourceWorkingRect))
        {
            return false;
        }

        const int rows = sourceMat.rows;
        const int cols = sourceMat.cols;
        if (rows <= 0 || cols <= 0)
        {
            return false;
        }

        const size_t bytesPerPixel = sourceMat.elemSize();
        if (bytesPerPixel == 0)
        {
            return false;
        }

        const double pixelSizeX = GetRectangleWidth(sourceRect) / static_cast<double>(cols);
        const double pixelSizeY = GetRectangleHeight(sourceRect) / static_cast<double>(rows);
        if (!IsPositiveFinite(pixelSizeX) || !IsPositiveFinite(pixelSizeY))
        {
            return false;
        }

        for (int row = 0; row < rows; row++)
        {
            const double y = sourceRect.maxY - (static_cast<double>(row) + 0.5) * pixelSizeY;
            unsigned char* rowData = sourceMat.ptr<unsigned char>(row);

            for (int col = 0; col < cols; col++)
            {
                const double x = sourceRect.minX + (static_cast<double>(col) + 0.5) * pixelSizeX;
                if (!IsInsideRectangle(sourceWorkingRect, x, y))
                {
                    std::memset(rowData + static_cast<size_t>(col) * bytesPerPixel, 0, bytesPerPixel);
                }
            }
        }

        return true;
    }


    static bool TryGetSourceWorkingRect(const GeoBoundingBox& sourceBox, TransformItem& forwardItem, bool clampToCrsValidArea, GB_Rectangle& outWorkingRect)
    {
        outWorkingRect = sourceBox.rect;

        if (!IsNonEmptyRectangle(outWorkingRect))
        {
            outWorkingRect = GB_Rectangle::Invalid;
            return false;
        }

        if (clampToCrsValidArea && forwardItem.hasSourceValidRect)
        {
            outWorkingRect = outWorkingRect.Intersected(forwardItem.sourceValidRect);
        }

        if (!IsNonEmptyRectangle(outWorkingRect))
        {
            outWorkingRect = GB_Rectangle::Invalid;
            return false;
        }

        return true;
    }

    static double MedianPositiveValue(std::vector<double>& values)
    {
        values.erase(std::remove_if(values.begin(), values.end(), [](double value) { return !IsPositiveFinite(value); }), values.end());
        if (values.empty())
        {
            return 0.0;
        }

        const size_t middleIndex = values.size() / 2;
        std::nth_element(values.begin(), values.begin() + middleIndex, values.end());
        return values[middleIndex];
    }

    static bool TryEstimateTargetPixelSize(TransformItem& forwardItem, const GB_Rectangle& sourceSampleRect, const GB_Rectangle& sourcePixelRect, size_t sourceCols, size_t sourceRows, int sampleGridCount, double& outPixelSizeX, double& outPixelSizeY)
    {
        outPixelSizeX = 0.0;
        outPixelSizeY = 0.0;

        if (!IsNonEmptyRectangle(sourceSampleRect) || !IsNonEmptyRectangle(sourcePixelRect) || sourceCols == 0 || sourceRows == 0)
        {
            return false;
        }

        // 像元大小必须来自整幅源影像的真实地理范围，而不是裁剪后的有效参与范围。
        // 否则当 sourceSampleRect 只是 sourcePixelRect 的一部分时，输出分辨率会被错误放大，导致重投影后图像尺寸偏大。
        const double sourcePixelSizeX = GetRectangleWidth(sourcePixelRect) / static_cast<double>(sourceCols);
        const double sourcePixelSizeY = GetRectangleHeight(sourcePixelRect) / static_cast<double>(sourceRows);
        if (!IsPositiveFinite(sourcePixelSizeX) || !IsPositiveFinite(sourcePixelSizeY))
        {
            return false;
        }

        const int count = std::max(2, std::min(sampleGridCount, 9));
        std::vector<double> xPixelSizes;
        std::vector<double> yPixelSizes;
        xPixelSizes.reserve(static_cast<size_t>(count) * static_cast<size_t>(count));
        yPixelSizes.reserve(static_cast<size_t>(count) * static_cast<size_t>(count));

        for (int yIndex = 0; yIndex < count; yIndex++)
        {
            const double yT = static_cast<double>(yIndex) / static_cast<double>(count - 1);
            const double sourceY = sourceSampleRect.minY + GetRectangleHeight(sourceSampleRect) * yT;

            for (int xIndex = 0; xIndex < count; xIndex++)
            {
                const double xT = static_cast<double>(xIndex) / static_cast<double>(count - 1);
                const double sourceX = sourceSampleRect.minX + GetRectangleWidth(sourceSampleRect) * xT;

                double centerX = 0.0;
                double centerY = 0.0;
                if (!TryTransformSingleXYInternal(forwardItem, sourceX, sourceY, centerX, centerY))
                {
                    continue;
                }

                const double xNeighborSourceX = (sourceX + sourcePixelSizeX <= sourceSampleRect.maxX) ? sourceX + sourcePixelSizeX : sourceX - sourcePixelSizeX;
                double xNeighborX = 0.0;
                double xNeighborY = 0.0;
                if (xNeighborSourceX >= sourceSampleRect.minX && xNeighborSourceX <= sourceSampleRect.maxX && TryTransformSingleXYInternal(forwardItem, xNeighborSourceX, sourceY, xNeighborX, xNeighborY))
                {
                    const double distance = std::sqrt((xNeighborX - centerX) * (xNeighborX - centerX) + (xNeighborY - centerY) * (xNeighborY - centerY));
                    if (IsPositiveFinite(distance))
                    {
                        xPixelSizes.push_back(distance);
                    }
                }

                const double yNeighborSourceY = (sourceY + sourcePixelSizeY <= sourceSampleRect.maxY) ? sourceY + sourcePixelSizeY : sourceY - sourcePixelSizeY;
                double yNeighborX = 0.0;
                double yNeighborY = 0.0;
                if (yNeighborSourceY >= sourceSampleRect.minY && yNeighborSourceY <= sourceSampleRect.maxY && TryTransformSingleXYInternal(forwardItem, sourceX, yNeighborSourceY, yNeighborX, yNeighborY))
                {
                    const double distance = std::sqrt((yNeighborX - centerX) * (yNeighborX - centerX) + (yNeighborY - centerY) * (yNeighborY - centerY));
                    if (IsPositiveFinite(distance))
                    {
                        yPixelSizes.push_back(distance);
                    }
                }
            }
        }

        outPixelSizeX = MedianPositiveValue(xPixelSizes);
        outPixelSizeY = MedianPositiveValue(yPixelSizes);
        return IsPositiveFinite(outPixelSizeX) && IsPositiveFinite(outPixelSizeY);
    }

    static bool TryScaleOutputSizeToLimits(size_t& width, size_t& height, const GeoImageReprojectOptions& options)
    {
        if (width == 0 || height == 0)
        {
            return false;
        }

        double scale = 1.0;
        if (options.maxOutputWidth > 0 && width > options.maxOutputWidth)
        {
            scale = std::min(scale, static_cast<double>(options.maxOutputWidth) / static_cast<double>(width));
        }
        if (options.maxOutputHeight > 0 && height > options.maxOutputHeight)
        {
            scale = std::min(scale, static_cast<double>(options.maxOutputHeight) / static_cast<double>(height));
        }
        if (options.maxOutputPixelCount > 0)
        {
            const long double pixelCount = static_cast<long double>(width) * static_cast<long double>(height);
            if (pixelCount > static_cast<long double>(options.maxOutputPixelCount))
            {
                const long double pixelScale = std::sqrt(static_cast<long double>(options.maxOutputPixelCount) / pixelCount);
                scale = std::min(scale, static_cast<double>(pixelScale));
            }
        }

        if (scale < 1.0)
        {
            width = std::max<size_t>(1, static_cast<size_t>(std::floor(static_cast<double>(width) * scale)));
            height = std::max<size_t>(1, static_cast<size_t>(std::floor(static_cast<double>(height) * scale)));
        }

        return width > 0 && height > 0 && width <= static_cast<size_t>(std::numeric_limits<int>::max()) && height <= static_cast<size_t>(std::numeric_limits<int>::max());
    }

    static bool TryCeilPositiveToSizeT(double value, size_t& outValue)
    {
        outValue = 0;
        if (!IsPositiveFinite(value) || value > static_cast<double>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        outValue = static_cast<size_t>(std::ceil(value));
        if (outValue == 0)
        {
            outValue = 1;
        }

        return true;
    }

    static bool TryResolveOutputSize(TransformItem& forwardItem, const GB_Rectangle& sourceWorkingRect, const GB_Rectangle& sourcePixelRect, const GB_Rectangle& targetRect, size_t sourceCols, size_t sourceRows, const GeoImageReprojectOptions& options, size_t& outWidth, size_t& outHeight)
    {
        outWidth = options.outputWidth;
        outHeight = options.outputHeight;

        if (!IsNonEmptyRectangle(sourceWorkingRect) || !IsNonEmptyRectangle(sourcePixelRect) || !IsNonEmptyRectangle(targetRect) || sourceCols == 0 || sourceRows == 0)
        {
            return false;
        }

        if (outWidth > 0 && outHeight > 0)
        {
            return TryScaleOutputSizeToLimits(outWidth, outHeight, options);
        }

        double targetPixelSizeX = options.targetPixelSizeX;
        double targetPixelSizeY = options.targetPixelSizeY;
        if (!IsPositiveFinite(targetPixelSizeX) || !IsPositiveFinite(targetPixelSizeY))
        {
            double estimatedPixelSizeX = 0.0;
            double estimatedPixelSizeY = 0.0;
            if (TryEstimateTargetPixelSize(forwardItem, sourceWorkingRect, sourcePixelRect, sourceCols, sourceRows, options.sampleGridCount, estimatedPixelSizeX, estimatedPixelSizeY))
            {
                if (!IsPositiveFinite(targetPixelSizeX))
                {
                    targetPixelSizeX = estimatedPixelSizeX;
                }
                if (!IsPositiveFinite(targetPixelSizeY))
                {
                    targetPixelSizeY = estimatedPixelSizeY;
                }
            }
        }

        if (!IsPositiveFinite(targetPixelSizeX))
        {
            targetPixelSizeX = GetRectangleWidth(targetRect) / static_cast<double>(sourceCols);
        }
        if (!IsPositiveFinite(targetPixelSizeY))
        {
            targetPixelSizeY = GetRectangleHeight(targetRect) / static_cast<double>(sourceRows);
        }

        if (!IsPositiveFinite(targetPixelSizeX) || !IsPositiveFinite(targetPixelSizeY))
        {
            return false;
        }

        if (outWidth == 0)
        {
            if (!TryCeilPositiveToSizeT(GetRectangleWidth(targetRect) / targetPixelSizeX, outWidth))
            {
                return false;
            }
        }
        if (outHeight == 0)
        {
            if (!TryCeilPositiveToSizeT(GetRectangleHeight(targetRect) / targetPixelSizeY, outHeight))
            {
                return false;
            }
        }

        return TryScaleOutputSizeToLimits(outWidth, outHeight, options);
    }

}

GeoImage::GeoImage() = default;

GeoImage::GeoImage(const GB_Image& sourceImage, const GeoBoundingBox& sourceBoundingBox)
    : image(sourceImage), boundingBox(sourceBoundingBox)
{
}

GeoImage::GeoImage(GB_Image&& sourceImage, GeoBoundingBox&& sourceBoundingBox) noexcept
    : image(std::move(sourceImage)), boundingBox(std::move(sourceBoundingBox))
{
}

bool GeoImage::IsValid() const
{
    return !image.IsEmpty() && image.GetWidth() > 0 && image.GetHeight() > 0 && boundingBox.IsValid() && IsNonEmptyRectangle(boundingBox.rect);
}

void GeoImage::Reset()
{
    image.Clear();
    boundingBox.Reset();
}

void GeoImage::Set(const GB_Image& sourceImage, const GeoBoundingBox& sourceBoundingBox)
{
    image = sourceImage;
    boundingBox = sourceBoundingBox;
}

void GeoImage::Set(GB_Image&& sourceImage, GeoBoundingBox&& sourceBoundingBox) noexcept
{
    image = std::move(sourceImage);
    boundingBox = std::move(sourceBoundingBox);
}

bool GeoCrsTransform::TransformPoint(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, const GB_Point2d& sourcePoint, GB_Point2d& outPoint)
{
    outPoint = sourcePoint;

    if (!IsFinitePoint(sourcePoint))
    {
        return false;
    }

    TransformItem* item = nullptr;
    if (!TryGetTransformItem(sourceWktUtf8, targetWktUtf8, item) || item == nullptr)
    {
        return false;
    }

    double outX = sourcePoint.x;
    double outY = sourcePoint.y;
    if (!TryTransformSingleXYInternal(*item, sourcePoint.x, sourcePoint.y, outX, outY))
    {
        return false;
    }

    outPoint.Set(outX, outY);
    return outPoint.IsValid();
}

bool GeoCrsTransform::TransformPoint(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, GB_Point2d& inOutPoint)
{
    GB_Point2d transformed;
    if (!TransformPoint(sourceWktUtf8, targetWktUtf8, inOutPoint, transformed))
    {
        return false;
    }

    inOutPoint = transformed;
    return true;
}

bool GeoCrsTransform::TransformPoints(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, const std::vector<GB_Point2d>& sourcePoints, std::vector<GB_Point2d>& outPoints, bool enableOpenMP)
{
    outPoints = sourcePoints;
    return TransformPoints(sourceWktUtf8, targetWktUtf8, outPoints, enableOpenMP);
}

bool GeoCrsTransform::TransformPoints(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, std::vector<GB_Point2d>& inOutPoints, bool enableOpenMP)
{
    std::atomic_bool allOk(true);

    const size_t count = inOutPoints.size();
    if (count == 0)
    {
        return true;
    }

    constexpr size_t chunkSize = 4096;
    const size_t chunkCount = (count + chunkSize - 1) / chunkSize;
    const bool useParallel = enableOpenMP && chunkCount <= static_cast<size_t>(std::numeric_limits<int>::max());

    if (useParallel)
    {
#pragma omp parallel
        {
            TransformItem* threadItem = nullptr;
            if (!TryGetTransformItem(sourceWktUtf8, targetWktUtf8, threadItem) || threadItem == nullptr || threadItem->transform == nullptr)
            {
                allOk.store(false, std::memory_order_relaxed);
            }

            std::vector<double> xValues;
            std::vector<double> yValues;
            std::vector<int> successFlags;
            std::vector<size_t> indexMap;

#pragma omp for schedule(static)
            for (int chunkIndex = 0; chunkIndex < static_cast<int>(chunkCount); chunkIndex++)
            {
                if (threadItem == nullptr || threadItem->transform == nullptr)
                {
                    allOk.store(false, std::memory_order_relaxed);
                    continue;
                }

                const size_t baseIndex = static_cast<size_t>(chunkIndex) * chunkSize;
                const size_t remaining = count - baseIndex;
                const size_t thisChunkCount = std::min(chunkSize, remaining);
                TransformPointsChunkInternal(*threadItem, inOutPoints, baseIndex, thisChunkCount, allOk, xValues, yValues, successFlags, indexMap);
            }
        }
    }
    else
    {
        TransformItem* item = nullptr;
        if (!TryGetTransformItem(sourceWktUtf8, targetWktUtf8, item) || item == nullptr || item->transform == nullptr)
        {
            return false;
        }

        std::vector<double> xValues;
        std::vector<double> yValues;
        std::vector<int> successFlags;
        std::vector<size_t> indexMap;

        for (size_t baseIndex = 0; baseIndex < count; baseIndex += chunkSize)
        {
            const size_t remaining = count - baseIndex;
            const size_t thisChunkCount = std::min(chunkSize, remaining);
            TransformPointsChunkInternal(*item, inOutPoints, baseIndex, thisChunkCount, allOk, xValues, yValues, successFlags, indexMap);
        }
    }

    return allOk.load(std::memory_order_relaxed);
}

bool GeoCrsTransform::TransformXY(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, double x, double y, double& outX, double& outY)
{
    outX = x;
    outY = y;

    TransformItem* item = nullptr;
    if (!TryGetTransformItem(sourceWktUtf8, targetWktUtf8, item) || item == nullptr)
    {
        return false;
    }

    return TryTransformSingleXYInternal(*item, x, y, outX, outY);
}

bool GeoCrsTransform::TransformXYZ(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, double x, double y, double z, double& outX, double& outY, double& outZ)
{
    outX = x;
    outY = y;
    outZ = z;

    TransformItem* item = nullptr;
    if (!TryGetTransformItem(sourceWktUtf8, targetWktUtf8, item) || item == nullptr)
    {
        return false;
    }

    return TryTransformSingleXYZInternal(*item, x, y, z, outX, outY, outZ);
}

bool GeoCrsTransform::TransformBoundingBox(const GeoBoundingBox& sourceBox, const std::string& targetWktUtf8, GeoBoundingBox& outBox, int sampleGridCount)
{
    outBox = GeoBoundingBox::Invalid;

    const std::string trimmedSourceWkt = GB_Utf8Trim(sourceBox.wktUtf8);
    const std::string trimmedTargetWkt = GB_Utf8Trim(targetWktUtf8);
    if (trimmedSourceWkt.empty() || trimmedTargetWkt.empty())
    {
        return false;
    }

    if (!sourceBox.IsValid() || !sourceBox.rect.IsValid())
    {
        return false;
    }

    TransformItem* item = nullptr;
    if (!TryGetTransformItem(trimmedSourceWkt, trimmedTargetWkt, item) || item == nullptr)
    {
        return false;
    }

    GB_Rectangle targetRect;
    if (!TryTransformRectangleToAabbInternal(*item, sourceBox.rect, sampleGridCount, true, targetRect))
    {
        return false;
    }

    GeoBoundingBox result;
    result.Set(item->canonicalTargetWkt, targetRect);
    outBox = result;
    return outBox.IsValid();
}

bool GeoCrsTransform::TransformBoundingBox(GeoBoundingBox& inOutBox, const std::string& targetWktUtf8, int sampleGridCount)
{
    GeoBoundingBox transformed;
    if (!TransformBoundingBox(inOutBox, targetWktUtf8, transformed, sampleGridCount))
    {
        return false;
    }

    inOutBox = transformed;
    return true;
}

bool GeoCrsTransform::TransformBoundingBoxes(const std::vector<GeoBoundingBox>& sourceBoxes, const std::string& targetWktUtf8, std::vector<GeoBoundingBox>& outBoxes, bool enableOpenMP, int sampleGridCount)
{
    outBoxes = sourceBoxes;
    return TryTransformBoundingBoxes(outBoxes, targetWktUtf8, enableOpenMP, sampleGridCount);
}

bool GeoCrsTransform::TryTransformBoundingBoxes(std::vector<GeoBoundingBox>& inOutBoxes, const std::string& targetWktUtf8, bool enableOpenMP, int sampleGridCount)
{
    const std::string trimmedTargetWkt = GB_Utf8Trim(targetWktUtf8);
    if (trimmedTargetWkt.empty())
    {
        for (GeoBoundingBox& bbox : inOutBoxes)
        {
            bbox = GeoBoundingBox::Invalid;
        }
        return false;
    }

    std::atomic_bool allOk(true);
    const size_t count = inOutBoxes.size();
    if (count == 0)
    {
        return true;
    }

    auto transformOne = [&](size_t index) {
        GeoBoundingBox& bbox = inOutBoxes[index];

        if (!bbox.IsValid() || !bbox.rect.IsValid())
        {
            bbox = GeoBoundingBox::Invalid;
            allOk.store(false, std::memory_order_relaxed);
            return;
        }

        const std::string trimmedSourceWkt = GB_Utf8Trim(bbox.wktUtf8);
        if (trimmedSourceWkt.empty())
        {
            bbox = GeoBoundingBox::Invalid;
            allOk.store(false, std::memory_order_relaxed);
            return;
        }

        TransformItem* item = nullptr;
        if (!TryGetTransformItem(trimmedSourceWkt, trimmedTargetWkt, item) || item == nullptr)
        {
            bbox = GeoBoundingBox::Invalid;
            allOk.store(false, std::memory_order_relaxed);
            return;
        }

        GB_Rectangle targetRect;
        if (!TryTransformRectangleToAabbInternal(*item, bbox.rect, sampleGridCount, true, targetRect))
        {
            bbox = GeoBoundingBox::Invalid;
            allOk.store(false, std::memory_order_relaxed);
            return;
        }

        bbox.Set(item->canonicalTargetWkt, targetRect);
        };

    if (enableOpenMP)
    {
#pragma omp parallel for
        for (long long i = 0; i < static_cast<long long>(count); i++)
        {
            transformOne(static_cast<size_t>(i));
        }
    }
    else
    {
        for (size_t i = 0; i < count; i++)
        {
            transformOne(i);
        }
    }

    return allOk.load(std::memory_order_relaxed);
}

bool GeoCrsTransform::ReprojectGeoImage(const GeoImage& sourceImage, const std::string& targetWktUtf8, GeoImage& outImage, const GeoImageReprojectOptions& options)
{
    outImage.Reset();

    if (!sourceImage.IsValid())
    {
        return false;
    }

    const std::string trimmedSourceWkt = GB_Utf8Trim(sourceImage.boundingBox.wktUtf8);
    const std::string trimmedTargetWkt = GB_Utf8Trim(targetWktUtf8);
    if (trimmedSourceWkt.empty() || trimmedTargetWkt.empty())
    {
        return false;
    }

    TransformItem* forwardItem = nullptr;
    if (!TryGetTransformItem(trimmedSourceWkt, trimmedTargetWkt, forwardItem) || forwardItem == nullptr)
    {
        return false;
    }

    GB_Rectangle sourceWorkingRect;
    if (!TryGetSourceWorkingRect(sourceImage.boundingBox, *forwardItem, options.clampToCrsValidArea, sourceWorkingRect))
    {
        return false;
    }

    GB_Rectangle targetRect;
    if (!TryTransformRectangleToAabbInternal(*forwardItem, sourceWorkingRect, options.sampleGridCount, options.clampToCrsValidArea, targetRect))
    {
        return false;
    }

    GeoBoundingBox targetBoundingBox;
    targetBoundingBox.Set(forwardItem->canonicalTargetWkt, targetRect);
    if (!targetBoundingBox.IsValid())
    {
        return false;
    }

    const size_t sourceCols = sourceImage.image.GetWidth();
    const size_t sourceRows = sourceImage.image.GetHeight();
    size_t outputWidth = 0;
    size_t outputHeight = 0;
    if (!TryResolveOutputSize(*forwardItem, sourceWorkingRect, sourceImage.boundingBox.rect, targetBoundingBox.rect, sourceCols, sourceRows, options, outputWidth, outputHeight))
    {
        return false;
    }

    GB_Image bgraSourceImage;
    if (!TryCreateBgraSourceImage(sourceImage.image, bgraSourceImage))
    {
        return false;
    }

    GeoImage workingSourceImage(bgraSourceImage, sourceImage.boundingBox);
    cv::Mat sourceMat = bgraSourceImage.ToCvMat(GB_ImageCopyMode::ShallowCopy);
    if (sourceMat.empty() || sourceMat.channels() != 4)
    {
        return false;
    }
    if (!ApplySourceWorkingRectMask(sourceMat, workingSourceImage.boundingBox.rect, sourceWorkingRect))
    {
        return false;
    }


    cv::Mat mapX;
    cv::Mat mapY;

    auto buildMaps = [&](const std::string& inverseSourceWkt, const std::string& inverseTargetWkt) -> bool {
        mapX.release();
        mapY.release();

        if (outputWidth > static_cast<size_t>(std::numeric_limits<int>::max()) || outputHeight > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        try
        {
            mapX = cv::Mat(static_cast<int>(outputHeight), static_cast<int>(outputWidth), CV_32FC1, cv::Scalar(-1.0));
            mapY = cv::Mat(static_cast<int>(outputHeight), static_cast<int>(outputWidth), CV_32FC1, cv::Scalar(-1.0));
        }
        catch (const cv::Exception&)
        {
            mapX.release();
            mapY.release();
            return false;
        }

        const double sourcePixelSizeX = GetRectangleWidth(workingSourceImage.boundingBox.rect) / static_cast<double>(sourceCols);
        const double sourcePixelSizeY = GetRectangleHeight(workingSourceImage.boundingBox.rect) / static_cast<double>(sourceRows);
        const double targetPixelSizeX = GetRectangleWidth(targetBoundingBox.rect) / static_cast<double>(outputWidth);
        const double targetPixelSizeY = GetRectangleHeight(targetBoundingBox.rect) / static_cast<double>(outputHeight);
        if (!IsPositiveFinite(sourcePixelSizeX) || !IsPositiveFinite(sourcePixelSizeY) || !IsPositiveFinite(targetPixelSizeX) || !IsPositiveFinite(targetPixelSizeY))
        {
            return false;
        }

        std::atomic_bool allOk(true);
        std::atomic_bool hasAnyMappedPixel(false);
        const bool useParallel = options.enableOpenMP && outputHeight <= static_cast<size_t>(std::numeric_limits<int>::max());

        auto buildRow = [&](size_t row, TransformItem& inverseItem, std::vector<double>& xValues, std::vector<double>& yValues, std::vector<int>& successFlags) {
            xValues.resize(outputWidth);
            yValues.resize(outputWidth);

            const double targetY = targetBoundingBox.rect.maxY - (static_cast<double>(row) + 0.5) * targetPixelSizeY;
            for (size_t col = 0; col < outputWidth; col++)
            {
                xValues[col] = targetBoundingBox.rect.minX + (static_cast<double>(col) + 0.5) * targetPixelSizeX;
                yValues[col] = targetY;
            }

            TryTransformXYArrayInternal(inverseItem, xValues, yValues, successFlags);

            float* mapXRow = mapX.ptr<float>(static_cast<int>(row));
            float* mapYRow = mapY.ptr<float>(static_cast<int>(row));
            for (size_t col = 0; col < outputWidth; col++)
            {
                if (successFlags[col] == FALSE)
                {
                    continue;
                }

                const double sourceX = xValues[col];
                const double sourceY = yValues[col];
                if (!IsInsideRectangle(sourceWorkingRect, sourceX, sourceY))
                {
                    continue;
                }

                const double sourcePixelX = (sourceX - workingSourceImage.boundingBox.rect.minX) / sourcePixelSizeX - 0.5;
                const double sourcePixelY = (workingSourceImage.boundingBox.rect.maxY - sourceY) / sourcePixelSizeY - 0.5;
                if (!IsFinite(sourcePixelX) || !IsFinite(sourcePixelY))
                {
                    continue;
                }

                mapXRow[col] = static_cast<float>(sourcePixelX);
                hasAnyMappedPixel.store(true, std::memory_order_relaxed);
                mapYRow[col] = static_cast<float>(sourcePixelY);
            }
            };

        if (useParallel)
        {
#pragma omp parallel
            {
                TransformItem* inverseItem = nullptr;
                if (!TryGetTransformItem(inverseSourceWkt, inverseTargetWkt, inverseItem) || inverseItem == nullptr || inverseItem->transform == nullptr)
                {
                    allOk.store(false, std::memory_order_relaxed);
                }

                std::vector<double> xValues;
                std::vector<double> yValues;
                std::vector<int> successFlags;

#pragma omp for schedule(static)
                for (int row = 0; row < static_cast<int>(outputHeight); row++)
                {
                    if (inverseItem == nullptr || inverseItem->transform == nullptr)
                    {
                        allOk.store(false, std::memory_order_relaxed);
                        continue;
                    }

                    buildRow(static_cast<size_t>(row), *inverseItem, xValues, yValues, successFlags);
                }
            }
        }
        else
        {
            TransformItem* inverseItem = nullptr;
            if (!TryGetTransformItem(inverseSourceWkt, inverseTargetWkt, inverseItem) || inverseItem == nullptr || inverseItem->transform == nullptr)
            {
                return false;
            }

            std::vector<double> xValues;
            std::vector<double> yValues;
            std::vector<int> successFlags;

            for (size_t row = 0; row < outputHeight; row++)
            {
                buildRow(row, *inverseItem, xValues, yValues, successFlags);
            }
        }

        return allOk.load(std::memory_order_relaxed) && hasAnyMappedPixel.load(std::memory_order_relaxed);
        };

    if (!buildMaps(targetBoundingBox.wktUtf8, trimmedSourceWkt))
    {
        return false;
    }

    cv::Mat targetMat;
    try
    {
        targetMat = cv::Mat(static_cast<int>(outputHeight), static_cast<int>(outputWidth), sourceMat.type(), cv::Scalar(0.0, 0.0, 0.0, 0.0));
        cv::remap(sourceMat, targetMat, mapX, mapY, ToCvInterpolation(options.interpolation), cv::BORDER_CONSTANT, cv::Scalar(0.0, 0.0, 0.0, 0.0));
    }
    catch (const cv::Exception&)
    {
        return false;
    }

    GB_Image targetImage;
    if (!targetImage.SetFromCvMat(targetMat, GB_ImageCopyMode::DeepCopy))
    {
        return false;
    }

    outImage.Set(std::move(targetImage), std::move(targetBoundingBox));
    return outImage.IsValid();
}

bool GeoCrsTransform::ReprojectGeoImages(const std::vector<GeoImage>& sourceImages, const std::string& targetWktUtf8, std::vector<GeoImage>& outImages, const GeoImageReprojectOptions& options)
{
    outImages.clear();
    outImages.resize(sourceImages.size());

    const size_t count = sourceImages.size();
    if (count == 0)
    {
        return true;
    }

    std::atomic_bool allOk(true);
    if (options.enableOpenMP && count <= static_cast<size_t>(std::numeric_limits<int>::max()))
    {
#pragma omp parallel for schedule(dynamic)
        for (int i = 0; i < static_cast<int>(count); i++)
        {
            GeoImageReprojectOptions itemOptions = options;
            itemOptions.enableOpenMP = false;

            GeoImage projectedImage;
            if (!ReprojectGeoImage(sourceImages[static_cast<size_t>(i)], targetWktUtf8, projectedImage, itemOptions))
            {
                outImages[static_cast<size_t>(i)].Reset();
                allOk.store(false, std::memory_order_relaxed);
                continue;
            }

            outImages[static_cast<size_t>(i)] = std::move(projectedImage);
        }
    }
    else
    {
        for (size_t i = 0; i < count; i++)
        {
            GeoImage projectedImage;
            if (!ReprojectGeoImage(sourceImages[i], targetWktUtf8, projectedImage, options))
            {
                outImages[i].Reset();
                allOk.store(false, std::memory_order_relaxed);
                continue;
            }

            outImages[i] = std::move(projectedImage);
        }
    }

    return allOk.load(std::memory_order_relaxed);
}

bool GeoCrsTransform::TryReprojectGeoImages(std::vector<GeoImage>& inOutImages, const std::string& targetWktUtf8, const GeoImageReprojectOptions& options)
{
    std::vector<GeoImage> transformedImages;
    const bool ok = ReprojectGeoImages(inOutImages, targetWktUtf8, transformedImages, options);
    inOutImages.swap(transformedImages);
    return ok;
}
