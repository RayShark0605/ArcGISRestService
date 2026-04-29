#include "LayerRefresher.h"

#include "QMainCanvas.h"
#include "QServiceBrowserPanel.h"
#include "QLayerManagerPanel.h"
#include "GeoBoundingBox.h"
#include "GeoCrsTransform.h"
#include "GeoCrsManager.h"
#include "TileImageCache.h"
#include "GeoBase/GB_Crypto.h"
#include "GeoBase/GB_Network.h"
#include "GeoBase/GB_Utf8String.h"
#include "GeoBase/GB_Timer.h"
#include "GeoBase/CV/GB_Image.h"
#include "GeoBase/GB_FileSystem.h"

#include <QByteArray>
#include <QMetaObject>
#include <QDebug>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	constexpr size_t FallbackLogicalCpuCoreCount = 4;
	constexpr size_t MinParallelTileWorkerCount = 4;
	constexpr unsigned int DefaultTileConnectTimeoutMs = 5000;
	constexpr unsigned int DefaultTileTotalTimeoutMs = 60000;
	constexpr double InitialZoomMarginRatio = 0.05;
	constexpr int ReprojectSampleGridCount = 21;
	constexpr size_t MaxReprojectOutputSideLength = 4096;
	constexpr size_t MaxReprojectOutputPixelCount = 16ULL * 1024ULL * 1024ULL;
	constexpr int DynamicExportVirtualTileSize = 512;
	constexpr int DynamicExportMaxVirtualLevel = 30;
	constexpr long long MaxCalculatedRequestItemCount = 200000;

	std::string TrimmedStdString(const std::string& text)
	{
		size_t beginIndex = 0;
		while (beginIndex < text.size() && std::isspace(static_cast<unsigned char>(text[beginIndex])))
		{
			beginIndex++;
		}

		size_t endIndex = text.size();
		while (endIndex > beginIndex && std::isspace(static_cast<unsigned char>(text[endIndex - 1])))
		{
			endIndex--;
		}

		return text.substr(beginIndex, endIndex - beginIndex);
	}

	std::string ToLowerAscii(std::string text)
	{
		for (char& ch : text)
		{
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		}
		return text;
	}

	bool StartsWithAsciiNoCase(const std::string& text, const std::string& prefix)
	{
		if (text.size() < prefix.size())
		{
			return false;
		}

		for (size_t charIndex = 0; charIndex < prefix.size(); charIndex++)
		{
			const char left = static_cast<char>(std::tolower(static_cast<unsigned char>(text[charIndex])));
			const char right = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[charIndex])));
			if (left != right)
			{
				return false;
			}
		}
		return true;
	}

	std::string ToStdString(const QString& text)
	{
		const QByteArray bytes = text.toUtf8();
		return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
	}

	GB_NetworkRequestOptions CreateNetworkOptionsFromConnectionSettings(const ArcGISRestConnectionSettings& settings)
	{
		GB_NetworkRequestOptions networkOptions;
		networkOptions.connectTimeoutMs = DefaultTileConnectTimeoutMs;
		networkOptions.totalTimeoutMs = DefaultTileTotalTimeoutMs;
		networkOptions.refererUtf8 = TrimmedStdString(settings.httpReferer);

		for (const std::pair<std::string, std::string>& header : settings.httpCustomHeaders)
		{
			const std::string headerName = TrimmedStdString(header.first);
			if (headerName.empty())
			{
				continue;
			}

			networkOptions.headersUtf8.push_back(headerName + ": " + header.second);
		}
		return networkOptions;
	}

	bool IsArcGISRestServiceNodeType(ArcGISRestServiceTreeNode::NodeType nodeType)
	{
		switch (nodeType)
		{
		case ArcGISRestServiceTreeNode::NodeType::MapService:
		case ArcGISRestServiceTreeNode::NodeType::ImageService:
		case ArcGISRestServiceTreeNode::NodeType::FeatureService:
			return true;
		case ArcGISRestServiceTreeNode::NodeType::Unknown:
		case ArcGISRestServiceTreeNode::NodeType::Root:
		case ArcGISRestServiceTreeNode::NodeType::Folder:
		case ArcGISRestServiceTreeNode::NodeType::AllLayers:
		case ArcGISRestServiceTreeNode::NodeType::UnknownVectorLayer:
		case ArcGISRestServiceTreeNode::NodeType::PointVectorLayer:
		case ArcGISRestServiceTreeNode::NodeType::LineVectorLayer:
		case ArcGISRestServiceTreeNode::NodeType::PolygonVectorLayer:
		case ArcGISRestServiceTreeNode::NodeType::RasterLayer:
		case ArcGISRestServiceTreeNode::NodeType::Table:
		default:
			return false;
		}
	}

	bool CanImportNodeAsImage(const LayerImportRequestInfo& request)
	{
		return request.serviceNodeType == ArcGISRestServiceTreeNode::NodeType::MapService ||
			request.serviceNodeType == ArcGISRestServiceTreeNode::NodeType::ImageService;
	}

	std::string GetServiceFallbackWkt(const ArcGISRestServiceInfo& serviceInfo)
	{
		if (serviceInfo.hasSpatialReference && !serviceInfo.spatialReference.wkt.empty())
		{
			return serviceInfo.spatialReference.wkt;
		}

		if (serviceInfo.hasFullExtent && !serviceInfo.fullExtent.spatialReference.wkt.empty())
		{
			return serviceInfo.fullExtent.spatialReference.wkt;
		}

		if (serviceInfo.hasInitialExtent && !serviceInfo.initialExtent.spatialReference.wkt.empty())
		{
			return serviceInfo.initialExtent.spatialReference.wkt;
		}

		return std::string();
	}

	std::string GetImageRequestWkt(const ArcGISRestServiceInfo& serviceInfo, bool isTiled)
	{
		if (isTiled && !serviceInfo.tileInfo.spatialReference.wkt.empty())
		{
			return serviceInfo.tileInfo.spatialReference.wkt;
		}

		return GetServiceFallbackWkt(serviceInfo);
	}

	bool IsCrsDefinitionValid(const std::string& wktUtf8)
	{
		return !GB_Utf8Trim(wktUtf8).empty() && GeoCrsManager::IsDefinitionValidCached(wktUtf8);
	}

	bool AreCrsEquivalent(const std::string& firstWktUtf8, const std::string& secondWktUtf8)
	{
		const std::string firstTrimmed = GB_Utf8Trim(firstWktUtf8);
		const std::string secondTrimmed = GB_Utf8Trim(secondWktUtf8);
		if (firstTrimmed.empty() || secondTrimmed.empty())
		{
			return false;
		}

		if (firstTrimmed == secondTrimmed)
		{
			return true;
		}

		const std::shared_ptr<const GeoCrs> firstCrs = GeoCrsManager::GetFromDefinitionCached(firstTrimmed);
		const std::shared_ptr<const GeoCrs> secondCrs = GeoCrsManager::GetFromDefinitionCached(secondTrimmed);
		if (!firstCrs || !secondCrs || !firstCrs->IsValid() || !secondCrs->IsValid())
		{
			return false;
		}

		return *firstCrs == *secondCrs;
	}

	bool TryTransformRectangleBetweenCrs(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, const GB_Rectangle& sourceRect, GB_Rectangle& outRect)
	{
		outRect = GB_Rectangle::Invalid;
		if (!sourceRect.IsValid() || !IsCrsDefinitionValid(sourceWktUtf8) || !IsCrsDefinitionValid(targetWktUtf8))
		{
			return false;
		}

		if (AreCrsEquivalent(sourceWktUtf8, targetWktUtf8))
		{
			outRect = sourceRect;
			return outRect.IsValid();
		}

		GeoBoundingBox transformedBox;
		const GeoBoundingBox sourceBox(sourceWktUtf8, sourceRect);
		if (!GeoCrsTransform::TransformBoundingBox(sourceBox, targetWktUtf8, transformedBox, ReprojectSampleGridCount))
		{
			return false;
		}

		outRect = transformedBox.rect;
		return outRect.IsValid();
	}


	bool IsPositiveFiniteDouble(double value)
	{
		return std::isfinite(value) && value > 0.0;
	}

	GB_Rectangle AdjustExtentToImageSizeAspectRatio(const GB_Rectangle& extent, int imageWidthInPixels, int imageHeightInPixels)
	{
		if (!extent.IsValid() || imageWidthInPixels <= 0 || imageHeightInPixels <= 0)
		{
			return extent;
		}

		const double extentWidth = extent.Width();
		const double extentHeight = extent.Height();
		if (!IsPositiveFiniteDouble(extentWidth) || !IsPositiveFiniteDouble(extentHeight))
		{
			return extent;
		}

		const double imageAspectRatio = static_cast<double>(imageWidthInPixels) / static_cast<double>(imageHeightInPixels);
		const double extentAspectRatio = extentWidth / extentHeight;
		if (!IsPositiveFiniteDouble(imageAspectRatio) || !IsPositiveFiniteDouble(extentAspectRatio))
		{
			return extent;
		}

		const double ratioDifference = std::fabs(extentAspectRatio - imageAspectRatio) / imageAspectRatio;
		if (ratioDifference <= 1e-12)
		{
			return extent;
		}

		const GB_Point2d center = extent.Center();
		if (!center.IsValid())
		{
			return extent;
		}

		GB_Rectangle adjustedExtent = extent;
		if (extentAspectRatio < imageAspectRatio)
		{
			const double adjustedWidth = extentHeight * imageAspectRatio;
			if (!IsPositiveFiniteDouble(adjustedWidth))
			{
				return extent;
			}

			const double halfWidth = adjustedWidth * 0.5;
			adjustedExtent.Set(center.x - halfWidth, extent.minY, center.x + halfWidth, extent.maxY);
		}
		else
		{
			const double adjustedHeight = extentWidth / imageAspectRatio;
			if (!IsPositiveFiniteDouble(adjustedHeight))
			{
				return extent;
			}

			const double halfHeight = adjustedHeight * 0.5;
			adjustedExtent.Set(extent.minX, center.y - halfHeight, extent.maxX, center.y + halfHeight);
		}

		return adjustedExtent.IsValid() ? adjustedExtent : extent;
	}

	bool TryGetTargetPixelSize(const GB_Rectangle& targetExtent, int targetWidthInPixels, int targetHeightInPixels, double& outPixelSizeX, double& outPixelSizeY)
	{
		outPixelSizeX = 0.0;
		outPixelSizeY = 0.0;

		if (!targetExtent.IsValid() || targetWidthInPixels <= 0 || targetHeightInPixels <= 0)
		{
			return false;
		}

		outPixelSizeX = targetExtent.Width() / static_cast<double>(targetWidthInPixels);
		outPixelSizeY = targetExtent.Height() / static_cast<double>(targetHeightInPixels);
		return std::isfinite(outPixelSizeX) && outPixelSizeX > 0.0 && std::isfinite(outPixelSizeY) && outPixelSizeY > 0.0;
	}

	bool TryCreateBoundingBoxFromEnvelope(const ArcGISRestEnvelope& envelope, const std::string& fallbackWkt, GeoBoundingBox& outBBox)
	{
		std::string wkt = envelope.spatialReference.wkt;
		if (wkt.empty())
		{
			wkt = fallbackWkt;
		}

		if (wkt.empty())
		{
			return false;
		}

		const GB_Rectangle rect(envelope.xmin, envelope.ymin, envelope.xmax, envelope.ymax);
		const GeoBoundingBox bbox(wkt, rect);
		if (!bbox.IsValid())
		{
			return false;
		}

		outBBox = bbox;
		return true;
	}

	bool TryGetLayerBoundingBox(const LayerImportRequestInfo& request, const ArcGISRestServiceInfo& serviceInfo, GeoBoundingBox& outBBox)
	{
		const std::string fallbackWkt = GetServiceFallbackWkt(serviceInfo);

		if (serviceInfo.layerOrTable.hasExtent && TryCreateBoundingBoxFromEnvelope(serviceInfo.layerOrTable.extent, fallbackWkt, outBBox))
		{
			return true;
		}

		if (serviceInfo.hasFullExtent && TryCreateBoundingBoxFromEnvelope(serviceInfo.fullExtent, fallbackWkt, outBBox))
		{
			return true;
		}

		if (serviceInfo.hasInitialExtent && TryCreateBoundingBoxFromEnvelope(serviceInfo.initialExtent, fallbackWkt, outBBox))
		{
			return true;
		}

		(void)request;
		return false;
	}

	std::string NormalizeImageFormatForRequest(const std::string& imageFormat)
	{
		const std::string format = ToLowerAscii(TrimmedStdString(imageFormat));
		if (format.empty())
		{
			return std::string();
		}

		if (StartsWithAsciiNoCase(format, "png32"))
		{
			return "png32";
		}
		if (StartsWithAsciiNoCase(format, "png24"))
		{
			return "png24";
		}
		if (StartsWithAsciiNoCase(format, "png8"))
		{
			return "png8";
		}
		if (StartsWithAsciiNoCase(format, "png"))
		{
			return "png";
		}
		if (StartsWithAsciiNoCase(format, "jpg") || StartsWithAsciiNoCase(format, "jpeg"))
		{
			return "jpg";
		}
		if (StartsWithAsciiNoCase(format, "gif"))
		{
			return "gif";
		}
		if (StartsWithAsciiNoCase(format, "bmp"))
		{
			return "bmp";
		}
		if (StartsWithAsciiNoCase(format, "tif") || StartsWithAsciiNoCase(format, "tiff"))
		{
			return "tiff";
		}
		if (StartsWithAsciiNoCase(format, "webp"))
		{
			return "webp";
		}

		return format;
	}

	bool TryChooseFormatFromCandidates(const std::vector<std::string>& formats, const std::vector<std::string>& candidates, std::string& outFormat)
	{
		for (const std::string& candidate : candidates)
		{
			for (const std::string& format : formats)
			{
				const std::string normalizedFormat = NormalizeImageFormatForRequest(format);
				if (normalizedFormat == candidate)
				{
					outFormat = normalizedFormat;
					return true;
				}
			}
		}
		return false;
	}

	std::string ChooseImageFormat(const ArcGISRestServiceInfo& serviceInfo, bool isTiled)
	{
		if (isTiled)
		{
			const std::string tileFormat = NormalizeImageFormatForRequest(serviceInfo.tileInfo.format);
			return tileFormat.empty() ? "png" : tileFormat;
		}

		static const std::vector<std::string> preferredFormats = { "png32", "png24", "png", "png8", "gif", "jpg", "bmp", "tiff" };
		std::string imageFormat = "";
		if (TryChooseFormatFromCandidates(serviceInfo.supportedImageFormatTypes, preferredFormats, imageFormat))
		{
			return imageFormat;
		}

		return "png32";
	}

	bool IsTiledServiceForImport(const ArcGISRestServiceInfo& serviceInfo)
	{
		return serviceInfo.singleFusedMapCache && serviceInfo.hasTileInfo && !serviceInfo.tileInfo.lods.empty() &&
			serviceInfo.tileInfo.rows > 0 && serviceInfo.tileInfo.cols > 0;
	}

	bool IsImageServerForImport(const LayerImportRequestInfo& request)
	{
		return request.serviceNodeType == ArcGISRestServiceTreeNode::NodeType::ImageService;
	}

	int ChooseDpi(const ArcGISRestServiceInfo& serviceInfo)
	{
		if (serviceInfo.hasTileInfo && serviceInfo.tileInfo.dpi > 0)
		{
			return serviceInfo.tileInfo.dpi;
		}
		return 96;
	}

	size_t ChooseWorkerThreadCount()
	{
		//const unsigned int hardwareThreadCount = 2 * std::thread::hardware_concurrency();
		const unsigned int hardwareThreadCount = 40;
		const size_t logicalCpuCoreCount = (hardwareThreadCount > 0) ? static_cast<size_t>(hardwareThreadCount) : FallbackLogicalCpuCoreCount;
		return std::max<size_t>(MinParallelTileWorkerCount, logicalCpuCoreCount);
	}

	std::string PrefixRequestUrlIfNeeded(const std::string& requestUrl, const ArcGISRestConnectionSettings& connectionSettings)
	{
		const std::string urlPrefix = TrimmedStdString(connectionSettings.urlPrefix);
		if (urlPrefix.empty())
		{
			return requestUrl;
		}

		return urlPrefix + requestUrl;
	}

	std::string NormalizeFileExtForLayerRefresher(const std::string& fileExtUtf8)
	{
		std::string ext = ToLowerAscii(TrimmedStdString(fileExtUtf8));
		if (ext.empty())
		{
			return std::string();
		}

		if (ext[0] != '.')
		{
			ext.insert(ext.begin(), '.');
		}

		if (ext == ".jpeg")
		{
			return ".jpg";
		}
		if (ext == ".tif")
		{
			return ".tiff";
		}
		if (ext == ".png8" || ext == ".png24" || ext == ".png32")
		{
			return ".png";
		}

		bool allSafe = ext.size() >= 2 && ext.size() <= 16;
		for (size_t charIndex = 1; charIndex < ext.size(); charIndex++)
		{
			const unsigned char ch = static_cast<unsigned char>(ext[charIndex]);
			if (!std::isalnum(ch))
			{
				allSafe = false;
				break;
			}
		}

		return allSafe ? ext : std::string();
	}

	std::string GuessFileExtFromContentType(const std::string& contentTypeUtf8)
	{
		const std::string contentType = ToLowerAscii(TrimmedStdString(contentTypeUtf8));
		if (contentType.empty())
		{
			return std::string();
		}

		if (contentType.find("image/png") != std::string::npos)
		{
			return ".png";
		}
		if (contentType.find("image/jpeg") != std::string::npos || contentType.find("image/jpg") != std::string::npos)
		{
			return ".jpg";
		}
		if (contentType.find("image/tiff") != std::string::npos || contentType.find("image/tif") != std::string::npos)
		{
			return ".tiff";
		}
		if (contentType.find("image/gif") != std::string::npos)
		{
			return ".gif";
		}
		if (contentType.find("image/bmp") != std::string::npos || contentType.find("image/x-ms-bmp") != std::string::npos)
		{
			return ".bmp";
		}
		if (contentType.find("image/webp") != std::string::npos)
		{
			return ".webp";
		}

		return std::string();
	}

	std::string GuessFileExtFromFileName(const std::string& fileNameUtf8)
	{
		const std::string fileName = TrimmedStdString(fileNameUtf8);
		const size_t dotPos = fileName.find_last_of('.');
		if (dotPos == std::string::npos || dotPos + 1 >= fileName.size())
		{
			return std::string();
		}

		return NormalizeFileExtForLayerRefresher(fileName.substr(dotPos));
	}

	std::string GuessFileExtFromImageFormatForLayerRefresher(const std::string& imageFormatUtf8)
	{
		const std::string imageFormat = ToLowerAscii(TrimmedStdString(imageFormatUtf8));
		if (imageFormat.empty())
		{
			return std::string();
		}

		if (StartsWithAsciiNoCase(imageFormat, "png"))
		{
			return ".png";
		}
		if (StartsWithAsciiNoCase(imageFormat, "jpg") || StartsWithAsciiNoCase(imageFormat, "jpeg"))
		{
			return ".jpg";
		}
		if (StartsWithAsciiNoCase(imageFormat, "tif") || StartsWithAsciiNoCase(imageFormat, "tiff"))
		{
			return ".tiff";
		}
		if (StartsWithAsciiNoCase(imageFormat, "gif"))
		{
			return ".gif";
		}
		if (StartsWithAsciiNoCase(imageFormat, "bmp"))
		{
			return ".bmp";
		}
		if (StartsWithAsciiNoCase(imageFormat, "webp"))
		{
			return ".webp";
		}

		return NormalizeFileExtForLayerRefresher(imageFormat);
	}

	std::string TrimTrailingSlash(std::string url)
	{
		while (!url.empty() && url.back() == '/')
		{
			url.pop_back();
		}
		return url;
	}

	std::string JoinArcGISLayerIds(const ArcGISRestServiceInfo& serviceInfo)
	{
		std::string result;
		for (const ArcGISMapServiceLayerEntry& layer : serviceInfo.layers)
		{
			if (layer.id.empty())
			{
				continue;
			}

			if (!result.empty())
			{
				result += ",";
			}
			result += layer.id;
		}
		return result;
	}

	std::string GetEffectiveLayerId(const LayerImportRequestInfo& request, const ArcGISRestServiceInfo& serviceInfo, bool isImageServer)
	{
		if (!request.layerId.empty())
		{
			return request.layerId;
		}

		if (!isImageServer)
		{
			return JoinArcGISLayerIds(serviceInfo);
		}

		// ImageServer 的 exportImage 通常不依赖 layers 参数，但内部统一任务结构需要一个非空逻辑 ID。
		return "0";
	}

	bool TryGetLodResolution(const ArcGISRestServiceInfo& serviceInfo, int level, double& outResolution)
	{
		for (const ArcGISMapServiceTileLod& lod : serviceInfo.tileInfo.lods)
		{
			if (lod.level == level && std::isfinite(lod.resolution) && lod.resolution > 0.0)
			{
				outResolution = lod.resolution;
				return true;
			}
		}
		return false;
	}

	std::string BuildTileRequestUrl(const std::string& serviceUrl, int level, int row, int col)
	{
		return TrimTrailingSlash(serviceUrl) + GB_Utf8Format("/tile/%d/%d/%d", level, row, col);
	}

	GB_Rectangle CalculateTileExtent(const ArcGISRestServiceInfo& serviceInfo, int level, int row, int col)
	{
		double resolution = 0.0;
		if (!TryGetLodResolution(serviceInfo, level, resolution))
		{
			return GB_Rectangle::Invalid;
		}

		const int tileWidthInPixels = serviceInfo.tileInfo.cols;
		const int tileHeightInPixels = serviceInfo.tileInfo.rows;
		if (tileWidthInPixels <= 0 || tileHeightInPixels <= 0)
		{
			return GB_Rectangle::Invalid;
		}

		const double tileWidth = static_cast<double>(tileWidthInPixels) * resolution;
		const double tileHeight = static_cast<double>(tileHeightInPixels) * resolution;
		const double minX = serviceInfo.tileInfo.origin.x + static_cast<double>(col) * tileWidth;
		const double maxX = serviceInfo.tileInfo.origin.x + static_cast<double>(col + 1) * tileWidth;
		const double maxY = serviceInfo.tileInfo.origin.y - static_cast<double>(row) * tileHeight;
		const double minY = serviceInfo.tileInfo.origin.y - static_cast<double>(row + 1) * tileHeight;
		return GB_Rectangle(minX, minY, maxX, maxY);
	}

	std::vector<ImageRequestItem> CalculateTiledImageRequestItems(const CalculateImageRequestItemsInput& input)
	{
		const ArcGISRestServiceInfo* serviceInfo = input.serviceInfo;
		if (!serviceInfo || !serviceInfo->hasTileInfo || serviceInfo->tileInfo.lods.empty())
		{
			return std::vector<ImageRequestItem>();
		}

		const int tileWidthInPixels = serviceInfo->tileInfo.cols;
		const int tileHeightInPixels = serviceInfo->tileInfo.rows;
		if (tileWidthInPixels <= 0 || tileHeightInPixels <= 0)
		{
			return std::vector<ImageRequestItem>();
		}

		const double targetResolution = input.viewExtent.Width() / static_cast<double>(input.viewExtentWidthInPixels);
		if (!std::isfinite(targetResolution) || targetResolution <= 0.0)
		{
			return std::vector<ImageRequestItem>();
		}

		std::vector<ArcGISMapServiceTileLod> lods = serviceInfo->tileInfo.lods;
		std::sort(lods.begin(), lods.end(), [](const ArcGISMapServiceTileLod& firstLod, const ArcGISMapServiceTileLod& secondLod) {
			return firstLod.resolution > secondLod.resolution;
			});

		int level = lods.back().level;
		for (const ArcGISMapServiceTileLod& lod : lods)
		{
			if (std::isfinite(lod.resolution) && lod.resolution > 0.0 && lod.resolution <= 1.5 * targetResolution)
			{
				level = lod.level;
				break;
			}
		}

		double resolution = 0.0;
		if (!TryGetLodResolution(*serviceInfo, level, resolution))
		{
			return std::vector<ImageRequestItem>();
		}

		const double tileWidth = static_cast<double>(tileWidthInPixels) * resolution;
		const double tileHeight = static_cast<double>(tileHeightInPixels) * resolution;
		if (!std::isfinite(tileWidth) || !std::isfinite(tileHeight) || tileWidth <= 0.0 || tileHeight <= 0.0)
		{
			return std::vector<ImageRequestItem>();
		}

		const GB_Point2d& origin = serviceInfo->tileInfo.origin;
		const int minCol = static_cast<int>(std::floor((input.viewExtent.minX - origin.x) / tileWidth));
		const int maxCol = static_cast<int>(std::ceil((input.viewExtent.maxX - origin.x) / tileWidth)) - 1;
		const int minRow = static_cast<int>(std::floor((origin.y - input.viewExtent.maxY) / tileHeight));
		const int maxRow = static_cast<int>(std::ceil((origin.y - input.viewExtent.minY) / tileHeight)) - 1;
		if (maxCol < minCol || maxRow < minRow)
		{
			return std::vector<ImageRequestItem>();
		}

		std::vector<ImageRequestItem> requestItems;
		const long long tileCount = static_cast<long long>(maxCol - minCol + 1) * static_cast<long long>(maxRow - minRow + 1);
		if (tileCount > 0 && tileCount < 200000)
		{
			requestItems.reserve(static_cast<size_t>(tileCount));
		}

		for (int row = minRow; row <= maxRow; row++)
		{
			for (int col = minCol; col <= maxCol; col++)
			{
				ImageRequestItem requestItem;
				requestItem.serviceUrl = input.serviceUrl;
				requestItem.layerId = input.layerId;
				requestItem.imageFormat = input.imageFormat;
				requestItem.requestUrl = BuildTileRequestUrl(input.serviceUrl, level, row, col);
				requestItem.imageExtent = CalculateTileExtent(*serviceInfo, level, row, col);
				requestItem.uid = GB_Md5Hash(requestItem.requestUrl);

				if (!requestItem.requestUrl.empty() && requestItem.imageExtent.IsValid())
				{
					requestItems.push_back(std::move(requestItem));
				}
			}
		}
		return requestItems;
	}

	struct DynamicExportVirtualTileGrid
	{
		bool isValid = false;
		int tileSize = DynamicExportVirtualTileSize;
		int level = 0;
		double initialResolution = 0.0;
		double resolution = 0.0;
		double tileSpan = 0.0;
		double originX = 0.0;
		double originY = 0.0;
		bool hasRequestLimitExtent = false;
		GB_Rectangle requestLimitExtent;
		std::string bboxSrParam = "";
		std::string imageSrParam = "";
	};

	int ChooseDynamicExportVirtualTileSize(const ArcGISRestServiceInfo& serviceInfo)
	{
		int tileSize = DynamicExportVirtualTileSize;
		if (serviceInfo.maxImageWidth > 0)
		{
			tileSize = std::min(tileSize, serviceInfo.maxImageWidth);
		}
		if (serviceInfo.maxImageHeight > 0)
		{
			tileSize = std::min(tileSize, serviceInfo.maxImageHeight);
		}
		return std::max(1, tileSize);
	}

	int GetPreferredSpatialReferenceCode(const ArcGISRestSpatialReference& spatialReference)
	{
		if (spatialReference.latestWkid > 0)
		{
			return spatialReference.latestWkid;
		}
		if (spatialReference.wkid > 0)
		{
			return spatialReference.wkid;
		}
		return 0;
	}

	int GetPreferredServiceSpatialReferenceCode(const ArcGISRestServiceInfo& serviceInfo)
	{
		if (serviceInfo.hasSpatialReference)
		{
			const int srCode = GetPreferredSpatialReferenceCode(serviceInfo.spatialReference);
			if (srCode > 0)
			{
				return srCode;
			}
		}

		if (serviceInfo.hasFullExtent)
		{
			const int srCode = GetPreferredSpatialReferenceCode(serviceInfo.fullExtent.spatialReference);
			if (srCode > 0)
			{
				return srCode;
			}
		}

		if (serviceInfo.hasInitialExtent)
		{
			const int srCode = GetPreferredSpatialReferenceCode(serviceInfo.initialExtent.spatialReference);
			if (srCode > 0)
			{
				return srCode;
			}
		}

		if (serviceInfo.layerOrTable.hasExtent)
		{
			const int srCode = GetPreferredSpatialReferenceCode(serviceInfo.layerOrTable.extent.spatialReference);
			if (srCode > 0)
			{
				return srCode;
			}
		}

		return 0;
	}

	bool IsWgs84LikeSpatialReferenceCode(int srCode)
	{
		return srCode == 4326;
	}

	bool IsWebMercatorLikeSpatialReferenceCode(int srCode)
	{
		return srCode == 3857 || srCode == 102100 || srCode == 102113 || srCode == 900913;
	}

	bool TryGetEnvelopeRectInTargetCrs(const ArcGISRestEnvelope& envelope, const std::string& targetWktUtf8, const std::string& fallbackWktUtf8, GB_Rectangle& outRect)
	{
		outRect = GB_Rectangle::Invalid;

		GeoBoundingBox sourceBox;
		if (!TryCreateBoundingBoxFromEnvelope(envelope, fallbackWktUtf8, sourceBox))
		{
			return false;
		}

		if (!IsCrsDefinitionValid(targetWktUtf8) || !IsCrsDefinitionValid(sourceBox.wktUtf8) || AreCrsEquivalent(sourceBox.wktUtf8, targetWktUtf8))
		{
			outRect = sourceBox.rect;
			return outRect.IsValid();
		}

		return TryTransformRectangleBetweenCrs(sourceBox.wktUtf8, targetWktUtf8, sourceBox.rect, outRect);
	}

	bool TryGetServiceReferenceExtentInRequestCrs(const ArcGISRestServiceInfo& serviceInfo, const std::string& requestWktUtf8, GB_Rectangle& outExtent)
	{
		outExtent = GB_Rectangle::Invalid;
		const std::string fallbackWkt = requestWktUtf8.empty() ? GetServiceFallbackWkt(serviceInfo) : requestWktUtf8;

		if (serviceInfo.hasFullExtent && TryGetEnvelopeRectInTargetCrs(serviceInfo.fullExtent, fallbackWkt, fallbackWkt, outExtent) && outExtent.IsValid())
		{
			return true;
		}

		if (serviceInfo.hasInitialExtent && TryGetEnvelopeRectInTargetCrs(serviceInfo.initialExtent, fallbackWkt, fallbackWkt, outExtent) && outExtent.IsValid())
		{
			return true;
		}

		if (serviceInfo.layerOrTable.hasExtent && TryGetEnvelopeRectInTargetCrs(serviceInfo.layerOrTable.extent, fallbackWkt, fallbackWkt, outExtent) && outExtent.IsValid())
		{
			return true;
		}

		return false;
	}

	bool TryFloorToLongLong(double value, long long& outValue)
	{
		outValue = 0;
		if (!std::isfinite(value))
		{
			return false;
		}

		const double flooredValue = std::floor(value);
		if (flooredValue < static_cast<double>(std::numeric_limits<long long>::min()) || flooredValue > static_cast<double>(std::numeric_limits<long long>::max()))
		{
			return false;
		}

		outValue = static_cast<long long>(flooredValue);
		return true;
	}

	int ChooseClosestVirtualTileLevel(double initialResolution, double targetResolution)
	{
		if (!IsPositiveFiniteDouble(initialResolution) || !IsPositiveFiniteDouble(targetResolution))
		{
			return 0;
		}

		const double levelValue = std::log(initialResolution / targetResolution) / std::log(2.0);
		if (!std::isfinite(levelValue))
		{
			return 0;
		}

		const int roundedLevel = static_cast<int>(std::floor(levelValue + 0.5));
		return GB_Clamp(roundedLevel, 0, DynamicExportMaxVirtualLevel);
	}

	void IntersectRequestLimitExtent(DynamicExportVirtualTileGrid& grid, const GB_Rectangle& extent)
	{
		if (!extent.IsValid())
		{
			return;
		}

		if (!grid.hasRequestLimitExtent)
		{
			grid.requestLimitExtent = extent;
			grid.hasRequestLimitExtent = true;
			return;
		}

		const GB_Rectangle intersectedExtent = grid.requestLimitExtent.Intersected(extent);
		grid.requestLimitExtent = intersectedExtent.IsValid() ? intersectedExtent : GB_Rectangle::Invalid;
	}

	bool TryBuildDynamicExportVirtualTileGrid(const CalculateImageRequestItemsInput& input, DynamicExportVirtualTileGrid& outGrid)
	{
		outGrid = DynamicExportVirtualTileGrid();

		const ArcGISRestServiceInfo* serviceInfo = input.serviceInfo;
		if (!serviceInfo || input.isImageServer || !input.viewExtent.IsValid() || input.viewExtentWidthInPixels <= 0)
		{
			return false;
		}

		const double targetResolution = input.viewExtent.Width() / static_cast<double>(input.viewExtentWidthInPixels);
		if (!IsPositiveFiniteDouble(targetResolution))
		{
			return false;
		}

		outGrid.tileSize = ChooseDynamicExportVirtualTileSize(*serviceInfo);
		const int srCode = GetPreferredServiceSpatialReferenceCode(*serviceInfo);
		if (srCode > 0)
		{
			outGrid.bboxSrParam = std::to_string(srCode);
			outGrid.imageSrParam = outGrid.bboxSrParam;
		}

		constexpr double webMercatorHalfWorld = 20037508.342789244;
		if (IsWgs84LikeSpatialReferenceCode(srCode))
		{
			outGrid.originX = -180.0;
			outGrid.originY = 90.0;
			outGrid.initialResolution = 360.0 / static_cast<double>(outGrid.tileSize);
			outGrid.requestLimitExtent.Set(-180.0, -90.0, 180.0, 90.0);
			outGrid.hasRequestLimitExtent = true;
		}
		else if (IsWebMercatorLikeSpatialReferenceCode(srCode))
		{
			outGrid.originX = -webMercatorHalfWorld;
			outGrid.originY = webMercatorHalfWorld;
			outGrid.initialResolution = (webMercatorHalfWorld * 2.0) / static_cast<double>(outGrid.tileSize);
			outGrid.requestLimitExtent.Set(-webMercatorHalfWorld, -webMercatorHalfWorld, webMercatorHalfWorld, webMercatorHalfWorld);
			outGrid.hasRequestLimitExtent = true;
		}
		else
		{
			const std::string requestWkt = GetServiceFallbackWkt(*serviceInfo);
			GB_Rectangle serviceExtent;
			if (!TryGetServiceReferenceExtentInRequestCrs(*serviceInfo, requestWkt, serviceExtent) || !serviceExtent.IsValid())
			{
				return false;
			}

			const double serviceWidth = serviceExtent.Width();
			const double serviceHeight = serviceExtent.Height();
			const double serviceSpan = std::max(serviceWidth, serviceHeight);
			if (!IsPositiveFiniteDouble(serviceSpan))
			{
				return false;
			}

			outGrid.originX = serviceExtent.minX;
			outGrid.originY = serviceExtent.maxY;
			outGrid.initialResolution = serviceSpan / static_cast<double>(outGrid.tileSize);
			outGrid.requestLimitExtent = serviceExtent;
			outGrid.hasRequestLimitExtent = true;
		}

		const std::string requestWkt = GetServiceFallbackWkt(*serviceInfo);
		GB_Rectangle serviceReferenceExtent;
		if (TryGetServiceReferenceExtentInRequestCrs(*serviceInfo, requestWkt, serviceReferenceExtent) && serviceReferenceExtent.IsValid())
		{
			IntersectRequestLimitExtent(outGrid, serviceReferenceExtent);
		}

		outGrid.level = ChooseClosestVirtualTileLevel(outGrid.initialResolution, targetResolution);
		outGrid.resolution = outGrid.initialResolution / std::pow(2.0, static_cast<double>(outGrid.level));
		outGrid.tileSpan = static_cast<double>(outGrid.tileSize) * outGrid.resolution;
		outGrid.isValid = IsPositiveFiniteDouble(outGrid.resolution) && IsPositiveFiniteDouble(outGrid.tileSpan) && std::isfinite(outGrid.originX) && std::isfinite(outGrid.originY);
		return outGrid.isValid;
	}

	std::string BuildDynamicExportVirtualTileUid(const CalculateImageRequestItemsInput& input, const DynamicExportVirtualTileGrid& grid, long long row, long long col)
	{
		std::string keyText;
		keyText.reserve(512);
		keyText += "ArcGISDynamicExportVirtualTileKeyV1\n";
		keyText += "serviceUrl=";
		keyText += TrimTrailingSlash(input.serviceUrl);
		keyText += "\nlayerSet=show:";
		keyText += input.layerId;
		keyText += "\nimageSR=";
		keyText += grid.imageSrParam;
		keyText += "\nbboxSR=";
		keyText += grid.bboxSrParam;
		keyText += "\nlevel=";
		keyText += std::to_string(grid.level);
		keyText += "\nrow=";
		keyText += std::to_string(row);
		keyText += "\ncol=";
		keyText += std::to_string(col);
		keyText += "\ntileSize=";
		keyText += std::to_string(grid.tileSize);
		keyText += "\noriginX=";
		keyText += GB_Utf8Format("%.17g", grid.originX);
		keyText += "\noriginY=";
		keyText += GB_Utf8Format("%.17g", grid.originY);
		keyText += "\ninitialResolution=";
		keyText += GB_Utf8Format("%.17g", grid.initialResolution);
		keyText += "\nformat=";
		keyText += input.imageFormat;
		keyText += "\ndpi=";
		keyText += std::to_string(input.dpi);
		keyText += "\ntransparent=true\n";
		return GB_Md5Hash(keyText);
	}

	std::string BuildDynamicExportRequestUrl(const CalculateImageRequestItemsInput& input, const DynamicExportVirtualTileGrid& grid, const GB_Rectangle& tileExtent)
	{
		std::string imageUrl = TrimTrailingSlash(input.serviceUrl) + "/export";
		const std::string imageSizeInfo = GB_Utf8Format("%d,%d", grid.tileSize, grid.tileSize);
		const std::string imageBBoxInfo = GB_Utf8Format("%.17g,%.17g,%.17g,%.17g", tileExtent.minX, tileExtent.minY, tileExtent.maxX, tileExtent.maxY);

		imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "bbox", imageBBoxInfo);
		imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "size", imageSizeInfo);
		imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "format", input.imageFormat);
		if (!input.layerId.empty())
		{
			const std::string layerInfo = GB_Utf8Format("show:%s", input.layerId.c_str());
			imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "layers", layerInfo);
		}
		imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "transparent", "true");
		imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "f", "image");
		if (input.dpi > 0)
		{
			imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "dpi", std::to_string(input.dpi));
		}
		if (!grid.bboxSrParam.empty())
		{
			imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "bboxSR", grid.bboxSrParam);
		}
		if (!grid.imageSrParam.empty())
		{
			imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "imageSR", grid.imageSrParam);
		}

		return imageUrl;
	}

	std::vector<ImageRequestItem> CalculateDynamicMapServerVirtualTileRequestItems(const CalculateImageRequestItemsInput& input, bool& outUsedVirtualTileMode)
	{
		outUsedVirtualTileMode = false;

		DynamicExportVirtualTileGrid grid;
		if (!TryBuildDynamicExportVirtualTileGrid(input, grid))
		{
			return std::vector<ImageRequestItem>();
		}
		outUsedVirtualTileMode = true;

		GB_Rectangle requestExtent = input.viewExtent;
		if (grid.hasRequestLimitExtent)
		{
			requestExtent = requestExtent.Intersected(grid.requestLimitExtent);
		}
		if (!requestExtent.IsValid() || !IsPositiveFiniteDouble(requestExtent.Width()) || !IsPositiveFiniteDouble(requestExtent.Height()))
		{
			return std::vector<ImageRequestItem>();
		}

		const double adjustedMaxX = std::nextafter(requestExtent.maxX, requestExtent.minX);
		const double adjustedMinY = std::nextafter(requestExtent.minY, requestExtent.maxY);

		long long minCol = 0;
		long long maxCol = 0;
		long long minRow = 0;
		long long maxRow = 0;
		if (!TryFloorToLongLong((requestExtent.minX - grid.originX) / grid.tileSpan, minCol) ||
			!TryFloorToLongLong((adjustedMaxX - grid.originX) / grid.tileSpan, maxCol) ||
			!TryFloorToLongLong((grid.originY - requestExtent.maxY) / grid.tileSpan, minRow) ||
			!TryFloorToLongLong((grid.originY - adjustedMinY) / grid.tileSpan, maxRow))
		{
			return std::vector<ImageRequestItem>();
		}

		if (maxCol < minCol || maxRow < minRow)
		{
			return std::vector<ImageRequestItem>();
		}

		const long long numCols = maxCol - minCol + 1;
		const long long numRows = maxRow - minRow + 1;
		if (numCols <= 0 || numRows <= 0 || numCols > MaxCalculatedRequestItemCount || numRows > MaxCalculatedRequestItemCount)
		{
			return std::vector<ImageRequestItem>();
		}

		const long long tileCount = numCols * numRows;
		if (tileCount <= 0 || tileCount > MaxCalculatedRequestItemCount)
		{
			return std::vector<ImageRequestItem>();
		}

		std::vector<ImageRequestItem> requestItems;
		requestItems.reserve(static_cast<size_t>(tileCount));

		for (long long row = minRow; row <= maxRow; row++)
		{
			for (long long col = minCol; col <= maxCol; col++)
			{
				const double tileMinX = grid.originX + static_cast<double>(col) * grid.tileSpan;
				const double tileMaxX = tileMinX + grid.tileSpan;
				const double tileMaxY = grid.originY - static_cast<double>(row) * grid.tileSpan;
				const double tileMinY = tileMaxY - grid.tileSpan;

				GB_Rectangle tileExtent(tileMinX, tileMinY, tileMaxX, tileMaxY);
				if (!tileExtent.IsValid())
				{
					continue;
				}

				ImageRequestItem requestItem;
				requestItem.serviceUrl = input.serviceUrl;
				requestItem.layerId = input.layerId;
				requestItem.imageFormat = input.imageFormat;
				requestItem.requestUrl = BuildDynamicExportRequestUrl(input, grid, tileExtent);
				requestItem.imageExtent = tileExtent;
				requestItem.uid = BuildDynamicExportVirtualTileUid(input, grid, row, col);
				if (!requestItem.requestUrl.empty())
				{
					requestItems.push_back(std::move(requestItem));
				}
			}
		}

		return requestItems;
	}

	std::vector<ImageRequestItem> CalculateDynamicImageRequestItems(const CalculateImageRequestItemsInput& input)
	{
		const ArcGISRestServiceInfo* serviceInfo = input.serviceInfo;
		if (!input.isImageServer)
		{
			bool usedVirtualTileMode = false;
			const std::vector<ImageRequestItem> virtualTileItems = CalculateDynamicMapServerVirtualTileRequestItems(input, usedVirtualTileMode);
			if (usedVirtualTileMode)
			{
				return virtualTileItems;
			}
		}

		std::string baseUrl = TrimTrailingSlash(input.serviceUrl);
		baseUrl += (input.isImageServer ? "/exportImage" : "/export");

		const int maxImageWidth = (serviceInfo && serviceInfo->maxImageWidth > 0) ? serviceInfo->maxImageWidth : input.viewExtentWidthInPixels;
		const int maxImageHeight = (serviceInfo && serviceInfo->maxImageHeight > 0) ? serviceInfo->maxImageHeight : input.viewExtentHeightInPixels;
		if (maxImageWidth <= 0 || maxImageHeight <= 0)
		{
			return std::vector<ImageRequestItem>();
		}

		const int numStepsInWidth = static_cast<int>(std::ceil(static_cast<double>(input.viewExtentWidthInPixels) / static_cast<double>(maxImageWidth)));
		const int numStepsInHeight = static_cast<int>(std::ceil(static_cast<double>(input.viewExtentHeightInPixels) / static_cast<double>(maxImageHeight)));
		if (numStepsInWidth <= 0 || numStepsInHeight <= 0)
		{
			return std::vector<ImageRequestItem>();
		}

		const GB_Rectangle exportViewExtent = AdjustExtentToImageSizeAspectRatio(input.viewExtent, input.viewExtentWidthInPixels, input.viewExtentHeightInPixels);
		if (!exportViewExtent.IsValid())
		{
			return std::vector<ImageRequestItem>();
		}

		std::vector<ImageRequestItem> requestItems;
		requestItems.reserve(static_cast<size_t>(numStepsInWidth * numStepsInHeight));

		for (int stepX = 0; stepX < numStepsInWidth; stepX++)
		{
			const int pixelStartX = stepX * maxImageWidth;
			const int pixelEndX = std::min(input.viewExtentWidthInPixels, pixelStartX + maxImageWidth);
			const int imageWidth = pixelEndX - pixelStartX;
			if (imageWidth <= 0)
			{
				continue;
			}

			for (int stepY = 0; stepY < numStepsInHeight; stepY++)
			{
				const int pixelStartY = stepY * maxImageHeight;
				const int pixelEndY = std::min(input.viewExtentHeightInPixels, pixelStartY + maxImageHeight);
				const int imageHeight = pixelEndY - pixelStartY;
				if (imageHeight <= 0)
				{
					continue;
				}

				const double imageMinX = exportViewExtent.minX + exportViewExtent.Width() * static_cast<double>(pixelStartX) / static_cast<double>(input.viewExtentWidthInPixels);
				const double imageMaxX = exportViewExtent.minX + exportViewExtent.Width() * static_cast<double>(pixelEndX) / static_cast<double>(input.viewExtentWidthInPixels);
				const double imageMinY = exportViewExtent.minY + exportViewExtent.Height() * static_cast<double>(pixelStartY) / static_cast<double>(input.viewExtentHeightInPixels);
				const double imageMaxY = exportViewExtent.minY + exportViewExtent.Height() * static_cast<double>(pixelEndY) / static_cast<double>(input.viewExtentHeightInPixels);

				const std::string imageSizeInfo = GB_Utf8Format("%d,%d", imageWidth, imageHeight);
				const std::string imageBBoxInfo = GB_Utf8Format("%.17g,%.17g,%.17g,%.17g", imageMinX, imageMinY, imageMaxX, imageMaxY);

				std::string imageUrl = baseUrl;
				imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "bbox", imageBBoxInfo);
				imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "size", imageSizeInfo);
				imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "format", input.imageFormat);
				if (!input.isImageServer && !input.layerId.empty())
				{
					const std::string layerInfo = GB_Utf8Format("show:%s", input.layerId.c_str());
					imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "layers", layerInfo);
				}
				imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "transparent", "true");
				if (input.isImageServer)
				{
					imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "adjustAspectRatio", "false");
				}
				imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "f", "image");
				if (input.dpi > 0)
				{
					imageUrl = GB_UrlOperator::SetUrlQueryValue(imageUrl, "dpi", std::to_string(input.dpi));
				}

				ImageRequestItem requestItem;
				requestItem.serviceUrl = input.serviceUrl;
				requestItem.layerId = input.layerId;
				requestItem.imageFormat = input.imageFormat;
				requestItem.requestUrl = std::move(imageUrl);
				requestItem.imageExtent.Set(imageMinX, imageMinY, imageMaxX, imageMaxY);
				requestItem.uid = GB_Md5Hash(requestItem.requestUrl);
				requestItems.push_back(std::move(requestItem));
			}
		}
		return requestItems;
	}

	std::vector<ImageRequestItem> CalculateLayerImageRequestItems(const CalculateImageRequestItemsInput& input)
	{
		if (!input.viewExtent.IsValid() || input.viewExtentWidthInPixels <= 0 || input.viewExtentHeightInPixels <= 0 ||
			input.serviceUrl.empty() || input.imageFormat.empty())
		{
			return std::vector<ImageRequestItem>();
		}

		if (input.isTiled)
		{
			return CalculateTiledImageRequestItems(input);
		}
		return CalculateDynamicImageRequestItems(input);
	}

	std::string BuildLayerTileUid(const std::string& layerUid, const ImageRequestItem& requestItem, const std::string& displayWktUtf8)
	{
		// 同一个服务请求在不同 Canvas CRS 下会生成不同的显示影像与显示范围，
		// 因此显示目标 CRS 必须纳入 UID，避免切换坐标系后错误复用旧瓦片。
		return GB_Md5Hash(layerUid + "|" + requestItem.uid + "|" + requestItem.requestUrl + "|" + displayWktUtf8);
	}

	std::string BuildFallbackLayerUid(const LayerImportRequestInfo& request)
	{
		std::string baseUid = request.nodeUid;
		if (baseUid.empty())
		{
			baseUid = request.serviceUrl + "|" + request.layerId;
		}
		if (baseUid.empty())
		{
			baseUid = "layer";
		}
		return baseUid;
	}
}

class LayerRefresher::Impl
{
public:
	struct LayerSnapshot
	{
		std::string layerUid = "";
		std::string displayName = "";
		int orderIndex = 0;
		bool visible = true;
		LayerImportRequestInfo importRequest;
	};

	struct TargetTileRecord
	{
		std::string tileUid = "";
		std::string layerUid = "";
		std::string requestUrl = "";
		GB_Rectangle extent;
		double layerNumber = 0.0;
		int layerOrderIndex = 0;
	};

	struct DisplayedTileRecord
	{
		MapTile tile;
		std::string layerUid = "";
		double layerNumber = 0.0;
	};

	struct TileTask
	{
		std::uint64_t generation = 0;
		std::uint64_t sequence = 0;
		int layerOrderIndex = 0;
		double layerNumber = 0.0;
		std::string layerUid = "";
		std::string tileUid = "";
		ImageRequestItem requestItem;
		std::string sourceWktUtf8 = "";
		std::string targetWktUtf8 = "";
		bool needsReprojection = false;
		double targetPixelSizeX = 0.0;
		double targetPixelSizeY = 0.0;
		ArcGISRestConnectionSettings connectionSettings;
		GB_NetworkRequestOptions networkOptions;
	};

	struct TileTaskPriorityGreater
	{
		bool operator()(const TileTask& firstTask, const TileTask& secondTask) const
		{
			if (firstTask.layerOrderIndex != secondTask.layerOrderIndex)
			{
				return firstTask.layerOrderIndex > secondTask.layerOrderIndex;
			}
			return firstTask.sequence > secondTask.sequence;
		}
	};

	struct TileResult
	{
		std::uint64_t generation = 0;
		std::string tileUid = "";
		std::string layerUid = "";
		double layerNumber = 0.0;
		MapTile tile;
	};

	explicit Impl(LayerRefresher* owner)
		: ownerPointer(owner), currentGeneration(0), nextTaskSequence(0), isStopping(false)
	{
		StartWorkers();
	}

	~Impl()
	{
		StopWorkers();
	}

	void SetCanvasAndPanels(QMainCanvas* canvas, QServiceBrowserPanel* servicePanel, QLayerManagerPanel* managerPanel)
	{
		DisconnectCurrentObjects();

		mainCanvas = canvas;
		serviceBrowserPanel = servicePanel;
		layerManagerPanel = managerPanel;
		hasViewExtent = false;
		currentViewExtent.Reset();

		LayerRefresher* owner = ownerPointer.data();
		if (!owner)
		{
			return;
		}

		if (mainCanvas)
		{
			QObject::connect(mainCanvas.data(), &QMainCanvas::ViewStateChanged, owner, &LayerRefresher::OnViewStateChanged, Qt::UniqueConnection);

			GB_Rectangle canvasExtent;
			if (mainCanvas->TryGetCurrentViewExtent(canvasExtent))
			{
				currentViewExtent = canvasExtent;
				hasViewExtent = true;
			}
		}

		if (layerManagerPanel)
		{
			QObject::connect(layerManagerPanel.data(), &QLayerManagerPanel::LayersChanged, owner, &LayerRefresher::OnLayersChanged, Qt::UniqueConnection);
			SetLayersFromLayerInfos(layerManagerPanel->GetLayers());
			PrepareCanvasForCurrentLayers(false, std::vector<std::string>());
			StartRefresh();
			return;
		}

		// 兼容模式：没有图层管理面板时，直接导入信号会生成一个单图层快照。
		if (serviceBrowserPanel)
		{
			QObject::connect(serviceBrowserPanel.data(), &QServiceBrowserPanel::LayerImportRequested, owner, &LayerRefresher::OnArcGISRestLayerImportRequested, Qt::UniqueConnection);
		}
	}

	void DisconnectCurrentObjects()
	{
		LayerRefresher* owner = ownerPointer.data();
		if (!owner)
		{
			return;
		}

		if (mainCanvas)
		{
			QObject::disconnect(mainCanvas.data(), &QMainCanvas::ViewStateChanged, owner, &LayerRefresher::OnViewStateChanged);
		}
		if (layerManagerPanel)
		{
			QObject::disconnect(layerManagerPanel.data(), &QLayerManagerPanel::LayersChanged, owner, &LayerRefresher::OnLayersChanged);
		}
		if (serviceBrowserPanel)
		{
			QObject::disconnect(serviceBrowserPanel.data(), &QServiceBrowserPanel::LayerImportRequested, owner, &LayerRefresher::OnArcGISRestLayerImportRequested);
		}

		StopRefresh();
		RemoveAllDisplayedTiles();
		RemoveAllTransitionTiles();
		layers.clear();
		currentTargetsByUid.clear();
	}

	void StopRefresh()
	{
		currentGeneration.fetch_add(1, std::memory_order_acq_rel);
		pendingTileResults.clear();
		isTileResultFlushScheduled = false;

		{
			std::lock_guard<std::mutex> lock(taskQueueMutex);
			ClearPendingTasksNoLock();
		}
		taskQueueCondition.notify_all();
	}

	void OnViewStateChanged(const GB_Rectangle& extent, double approximateMetersPerPixel)
	{
		if (!extent.IsValid())
		{
			return;
		}

		currentViewExtent = extent;
		currentApproximateMetersPerPixel = approximateMetersPerPixel;
		hasViewExtent = true;
		StartRefresh();
	}

	void OnLayersChanged(const LayerManagerChangeInfo& changeInfo)
	{
		const bool allowAutoZoomToImportedLayer = (changeInfo.actionType == LayerManagerActionType::LayerImported);
		const std::vector<std::string> autoZoomLayerUids = BuildLayerUidList(changeInfo.affectedLayers);

		SetLayersFromLayerInfos(changeInfo.allLayers);
		PrepareCanvasForCurrentLayers(allowAutoZoomToImportedLayer, autoZoomLayerUids);
		StartRefresh();
	}

	void OnDirectLayerImportRequested(const LayerImportRequestInfo& request)
	{
		if (!request.IsValid())
		{
			return;
		}

		std::vector<LayerSnapshot> newLayers;
		LayerSnapshot layer;
		layer.layerUid = BuildFallbackLayerUid(request);
		layer.displayName = request.nodeText;
		layer.orderIndex = 0;
		layer.visible = true;
		layer.importRequest = request;
		std::vector<std::string> autoZoomLayerUids;
		autoZoomLayerUids.push_back(layer.layerUid);

		newLayers.push_back(std::move(layer));
		layers = std::move(newLayers);

		PrepareCanvasForCurrentLayers(true, autoZoomLayerUids);
		StartRefresh();
	}

private:
	std::vector<std::string> BuildLayerUidList(const std::vector<LayerManagerLayerInfo>& layerInfos) const
	{
		std::vector<std::string> layerUids;
		layerUids.reserve(layerInfos.size());
		for (const LayerManagerLayerInfo& layerInfo : layerInfos)
		{
			std::string layerUid = ToStdString(layerInfo.layerUid);
			if (layerUid.empty())
			{
				layerUid = BuildFallbackLayerUid(layerInfo.importRequest);
			}

			if (!layerUid.empty())
			{
				layerUids.push_back(std::move(layerUid));
			}
		}
		return layerUids;
	}

	bool IsLayerUidInList(const std::vector<std::string>& layerUids, const std::string& layerUid) const
	{
		if (layerUid.empty())
		{
			return false;
		}

		return std::find(layerUids.begin(), layerUids.end(), layerUid) != layerUids.end();
	}

	void SetLayersFromLayerInfos(const std::vector<LayerManagerLayerInfo>& layerInfos)
	{
		std::vector<LayerSnapshot> newLayers;
		newLayers.reserve(layerInfos.size());

		for (size_t layerIndex = 0; layerIndex < layerInfos.size(); layerIndex++)
		{
			const LayerManagerLayerInfo& layerInfo = layerInfos[layerIndex];
			LayerSnapshot layer;
			layer.layerUid = ToStdString(layerInfo.layerUid);
			if (layer.layerUid.empty())
			{
				layer.layerUid = BuildFallbackLayerUid(layerInfo.importRequest);
			}
			layer.displayName = ToStdString(layerInfo.displayName);
			layer.orderIndex = static_cast<int>(layerIndex);
			layer.visible = layerInfo.visible;
			layer.importRequest = layerInfo.importRequest;
			newLayers.push_back(std::move(layer));
		}

		layers = std::move(newLayers);
	}

	void PrepareCanvasForCurrentLayers(bool allowAutoZoomToImportedLayer, const std::vector<std::string>& autoZoomLayerUids)
	{
		if (!mainCanvas)
		{
			return;
		}

		GB_Rectangle canvasExtent;
		const bool hasCanvasViewExtent = mainCanvas->TryGetCurrentViewExtent(canvasExtent);

		// QMainCanvas 默认就有一个可用 viewExtent。不能仅凭 TryGetCurrentViewExtent() 成功就跳过首次导入缩放。
		// 自动缩放至图层范围只允许发生在“本次变化确实新导入图层”这一前提下；
		// 显示/隐藏、排序、重命名、移除、初始化绑定等变化只同步当前视口，不改变用户已经浏览到的位置。
		const bool shouldTryAutoZoom = allowAutoZoomToImportedLayer && !autoZoomLayerUids.empty() && !mainCanvas->HasDrawables();
		if (!shouldTryAutoZoom)
		{
			if (hasCanvasViewExtent)
			{
				currentViewExtent = canvasExtent;
				hasViewExtent = true;
			}
			return;
		}

		for (const LayerSnapshot& layer : layers)
		{
			if (!IsLayerUidInList(autoZoomLayerUids, layer.layerUid) || !layer.visible || !layer.importRequest.IsValid() || !CanImportNodeAsImage(layer.importRequest))
			{
				continue;
			}

			const ArcGISRestServiceInfo* serviceInfo = GetServiceInfo(layer.importRequest);
			if (!serviceInfo)
			{
				continue;
			}

			const std::string wkt = GetServiceFallbackWkt(*serviceInfo);
			SetCanvasCrsIfNeeded(wkt);

			GeoBoundingBox bbox;
			if (TryGetLayerBoundingBox(layer.importRequest, *serviceInfo, bbox))
			{
				SetCanvasCrsIfNeeded(bbox.wktUtf8);
				GB_Rectangle zoomExtent = bbox.rect;
				const std::string canvasWkt = mainCanvas->GetCrsWkt();
				if (IsCrsDefinitionValid(canvasWkt) && IsCrsDefinitionValid(bbox.wktUtf8) && !AreCrsEquivalent(bbox.wktUtf8, canvasWkt))
				{
					if (!TryTransformRectangleBetweenCrs(bbox.wktUtf8, canvasWkt, bbox.rect, zoomExtent))
					{
						continue;
					}
				}

				mainCanvas->ZoomToExtent(zoomExtent, InitialZoomMarginRatio);
				if (mainCanvas->TryGetCurrentViewExtent(canvasExtent))
				{
					currentViewExtent = canvasExtent;
					hasViewExtent = true;
				}
				return;
			}
		}

		if (hasCanvasViewExtent)
		{
			currentViewExtent = canvasExtent;
			hasViewExtent = true;
		}
	}

	const ArcGISRestServiceInfo* GetServiceInfo(const LayerImportRequestInfo& request) const
	{
		if (request.serviceInfo)
		{
			return request.serviceInfo;
		}
		return request.serviceInfoHolder.get();
	}

	bool SetCanvasCrsIfNeeded(const std::string& wkt) const
	{
		if (!mainCanvas)
		{
			return false;
		}

		const std::string& canvasCrsWkt = mainCanvas->GetCrsWkt();
		if (GeoCrsManager::IsDefinitionValidCached(canvasCrsWkt))
		{
			return true;
		}

		if (!GeoCrsManager::IsDefinitionValidCached(wkt))
		{
			return false;
		}
		mainCanvas->SetCrsWkt(wkt);
		return true;
	}

	void StartRefresh()
	{
		if (isStartingRefresh)
		{
			needsRestartAfterCurrentRefresh = true;
			return;
		}

		if (!mainCanvas)
		{
			return;
		}

		isStartingRefresh = true;
		needsRestartAfterCurrentRefresh = false;

		const std::uint64_t generation = currentGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
		{
			std::lock_guard<std::mutex> lock(taskQueueMutex);
			ClearPendingTasksNoLock();
		}

		GB_Rectangle canvasExtent;
		if (mainCanvas->TryGetCurrentViewExtent(canvasExtent))
		{
			currentViewExtent = canvasExtent;
			hasViewExtent = true;
		}

		std::unordered_map<std::string, TargetTileRecord> newTargetsByUid;
		std::vector<TileTask> newTasks;

		if (hasViewExtent && currentViewExtent.IsValid() && !layers.empty())
		{
			BuildRefreshTasks(generation, newTargetsByUid, newTasks);
		}

		ReconcileDisplayedTiles(newTargetsByUid, newTasks);
		currentTargetsByUid.swap(newTargetsByUid);
		BeginRefreshCompletionTracking(generation, newTasks.size());
		EnqueueTasks(newTasks);

		isStartingRefresh = false;
		if (needsRestartAfterCurrentRefresh)
		{
			needsRestartAfterCurrentRefresh = false;
			StartRefresh();
		}
	}

	void BuildRefreshTasks(std::uint64_t generation, std::unordered_map<std::string, TargetTileRecord>& outTargetsByUid, std::vector<TileTask>& outTasks)
	{
		if (!mainCanvas || !currentViewExtent.IsValid())
		{
			return;
		}

		const int viewWidth = std::max(1, mainCanvas->width());
		const int viewHeight = std::max(1, mainCanvas->height());
		for (const LayerSnapshot& layer : layers)
		{
			if (!layer.visible || !layer.importRequest.IsValid() || !CanImportNodeAsImage(layer.importRequest))
			{
				continue;
			}

			const ArcGISRestServiceInfo* serviceInfo = GetServiceInfo(layer.importRequest);
			if (!serviceInfo)
			{
				continue;
			}

			const bool isTiled = IsTiledServiceForImport(*serviceInfo);
			const bool isImageServer = IsImageServerForImport(layer.importRequest);
			const std::string serviceRequestWkt = GetImageRequestWkt(*serviceInfo, isTiled);
			if (!SetCanvasCrsIfNeeded(serviceRequestWkt))
			{
				continue;
			}

			const std::string canvasWkt = mainCanvas->GetCrsWkt();
			if (!IsCrsDefinitionValid(serviceRequestWkt) || !IsCrsDefinitionValid(canvasWkt))
			{
				continue;
			}

			// 统一按服务自身 CRS 组织请求，再在本地重投影到 Canvas CRS。
			// 原因：ArcGIS REST 虽允许 bboxSR/imageSR 使用 WKID 或 SR JSON，但服务端不保证能正确识别每一个 EPSG/WKT；
			// 对 EPSG:27705 这类 ArcGIS Server 可能不支持的 CRS，直接让服务端输出该 CRS 会得到空白图。
			// 本地重投影可以把服务端 CRS 支持范围的不确定性收敛到客户端 PROJ/GDAL，行为更可控。
			const std::string requestWkt = serviceRequestWkt;

			GB_Rectangle requestViewExtent;
			if (!TryTransformRectangleBetweenCrs(canvasWkt, requestWkt, currentViewExtent, requestViewExtent))
			{
				continue;
			}

			double targetPixelSizeX = 0.0;
			double targetPixelSizeY = 0.0;
			TryGetTargetPixelSize(currentViewExtent, viewWidth, viewHeight, targetPixelSizeX, targetPixelSizeY);

			const bool needsReprojection = !AreCrsEquivalent(requestWkt, canvasWkt);
			const std::string imageFormat = ChooseImageFormat(*serviceInfo, isTiled);
			const std::string effectiveLayerId = GetEffectiveLayerId(layer.importRequest, *serviceInfo, isImageServer);
			if (effectiveLayerId.empty())
			{
				continue;
			}

			CalculateImageRequestItemsInput input;
			input.viewExtent = requestViewExtent;
			input.viewExtentWidthInPixels = viewWidth;
			input.viewExtentHeightInPixels = viewHeight;
			input.serviceUrl = layer.importRequest.serviceUrl;
			input.layerId = effectiveLayerId;
			input.imageFormat = imageFormat;
			input.serviceInfo = serviceInfo;
			input.isTiled = isTiled;
			input.isImageServer = isImageServer;
			input.dpi = ChooseDpi(*serviceInfo);

			const std::vector<ImageRequestItem> requestItems = CalculateLayerImageRequestItems(input);
			if (requestItems.empty())
			{
				continue;
			}

			const double layerNumber = static_cast<double>(layer.orderIndex);
			const GB_NetworkRequestOptions networkOptions = CreateNetworkOptionsFromConnectionSettings(layer.importRequest.connectionSettings);
			for (const ImageRequestItem& rawRequestItem : requestItems)
			{
				if (rawRequestItem.requestUrl.empty() || !rawRequestItem.imageExtent.IsValid())
				{
					continue;
				}

				ImageRequestItem requestItem = rawRequestItem;

				const std::string tileUid = BuildLayerTileUid(layer.layerUid, requestItem, canvasWkt);
				TargetTileRecord target;
				target.tileUid = tileUid;
				target.layerUid = layer.layerUid;
				target.requestUrl = requestItem.requestUrl;
				target.extent = requestItem.imageExtent;
				target.layerNumber = layerNumber;
				target.layerOrderIndex = layer.orderIndex;
				outTargetsByUid[target.tileUid] = target;

				TileTask task;
				task.generation = generation;
				task.sequence = nextTaskSequence++;
				task.layerOrderIndex = layer.orderIndex;
				task.layerNumber = layerNumber;
				task.layerUid = layer.layerUid;
				task.tileUid = tileUid;
				task.requestItem = requestItem;
				task.sourceWktUtf8 = requestWkt;
				task.targetWktUtf8 = canvasWkt;
				task.needsReprojection = needsReprojection;
				task.targetPixelSizeX = targetPixelSizeX;
				task.targetPixelSizeY = targetPixelSizeY;
				task.connectionSettings = layer.importRequest.connectionSettings;
				task.networkOptions = networkOptions;
				outTasks.push_back(std::move(task));
			}
		}
	}

	void ReconcileDisplayedTiles(const std::unordered_map<std::string, TargetTileRecord>& targetsByUid, std::vector<TileTask>& tasks)
	{
		if (!mainCanvas)
		{
			return;
		}

		for (auto iter = displayedTilesByUid.begin(); iter != displayedTilesByUid.end(); )
		{
			const auto targetIter = targetsByUid.find(iter->first);
			if (targetIter == targetsByUid.end())
			{
				MoveDisplayedTileToTransition(iter->first, std::move(iter->second));
				iter = displayedTilesByUid.erase(iter);
				continue;
			}

			DisplayedTileRecord& displayedTile = iter->second;
			const TargetTileRecord& target = targetIter->second;
			if (displayedTile.layerNumber != target.layerNumber)
			{
				std::vector<std::string> singleUid;
				singleUid.push_back(iter->first);
				mainCanvas->SetDrawablesLayerNumber(singleUid, target.layerNumber);

				displayedTile.layerNumber = target.layerNumber;
				displayedTile.tile.layerNumber = target.layerNumber;
				displayedTile.tile.visible = true;
			}
			iter++;
		}

		for (auto iter = transitionTilesByUid.begin(); iter != transitionTilesByUid.end(); )
		{
			const auto targetIter = targetsByUid.find(iter->first);
			if (targetIter == targetsByUid.end())
			{
				iter++;
				continue;
			}

			DisplayedTileRecord displayedTile = std::move(iter->second);
			const TargetTileRecord& target = targetIter->second;

			std::vector<std::string> singleUid;
			singleUid.push_back(iter->first);
			mainCanvas->SetDrawablesLayerNumber(singleUid, target.layerNumber);

			displayedTile.layerNumber = target.layerNumber;
			displayedTile.tile.layerNumber = target.layerNumber;
			displayedTile.tile.visible = true;
			displayedTilesByUid[iter->first] = std::move(displayedTile);
			iter = transitionTilesByUid.erase(iter);
		}

		if (!tasks.empty())
		{
			tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [this](const TileTask& task) {
				return displayedTilesByUid.find(task.tileUid) != displayedTilesByUid.end();
				}), tasks.end());
		}
	}

	void MoveDisplayedTileToTransition(const std::string& tileUid, DisplayedTileRecord&& displayedTile)
	{
		if (!mainCanvas || tileUid.empty())
		{
			return;
		}

		std::vector<std::string> singleUid;
		singleUid.push_back(tileUid);
		mainCanvas->SetDrawablesLayerNumber(singleUid, QMainCanvas::GetBottomLayerNumber());

		displayedTile.layerNumber = QMainCanvas::GetBottomLayerNumber();
		displayedTile.tile.layerNumber = QMainCanvas::GetBottomLayerNumber();
		displayedTile.tile.visible = true;
		transitionTilesByUid[tileUid] = std::move(displayedTile);
	}

	void BeginRefreshCompletionTracking(std::uint64_t generation, size_t pendingTaskCount)
	{
		trackedGeneration = generation;
		trackedPendingTaskCount = pendingTaskCount;
		trackedFinishedTaskCount = 0;

		if (trackedPendingTaskCount == 0)
		{
			RemoveAllTransitionTiles();
		}
	}

	void MarkTileTaskFinished(std::uint64_t generation)
	{
		if (generation != trackedGeneration || !IsTaskGenerationCurrent(generation) || trackedPendingTaskCount == 0)
		{
			return;
		}

		if (trackedFinishedTaskCount < trackedPendingTaskCount)
		{
			trackedFinishedTaskCount++;
		}

		if (trackedFinishedTaskCount >= trackedPendingTaskCount)
		{
			RemoveAllTransitionTiles();
		}
	}

	void EnqueueTasks(const std::vector<TileTask>& tasks)
	{
		if (tasks.empty())
		{
			return;
		}

		{
			std::lock_guard<std::mutex> lock(taskQueueMutex);
			if (isStopping)
			{
				return;
			}

			for (const TileTask& task : tasks)
			{
				taskQueue.push(task);
			}
		}
		taskQueueCondition.notify_all();
	}

	void RemoveAllDisplayedTiles()
	{
		if (!mainCanvas || displayedTilesByUid.empty())
		{
			return;
		}

		std::vector<std::string> uids;
		uids.reserve(displayedTilesByUid.size());
		for (const auto& item : displayedTilesByUid)
		{
			uids.push_back(item.first);
		}
		mainCanvas->RemoveDrawables(uids);
		displayedTilesByUid.clear();
	}

	void RemoveAllTransitionTiles()
	{
		if (!mainCanvas || transitionTilesByUid.empty())
		{
			transitionTilesByUid.clear();
			return;
		}

		std::vector<std::string> uids;
		uids.reserve(transitionTilesByUid.size());
		for (const auto& item : transitionTilesByUid)
		{
			uids.push_back(item.first);
		}
		mainCanvas->RemoveDrawables(uids);
		transitionTilesByUid.clear();
	}

	void StartWorkers()
	{
		const size_t threadCount = ChooseWorkerThreadCount();
		workerThreads.reserve(threadCount);
		for (size_t threadIndex = 0; threadIndex < threadCount; threadIndex++)
		{
			workerThreads.emplace_back([this]() {
				WorkerLoop();
				});
		}
	}

	void StopWorkers()
	{
		{
			std::lock_guard<std::mutex> lock(taskQueueMutex);
			isStopping = true;
			ClearPendingTasksNoLock();
		}
		taskQueueCondition.notify_all();

		for (std::thread& workerThread : workerThreads)
		{
			if (workerThread.joinable())
			{
				workerThread.join();
			}
		}
		workerThreads.clear();
	}

	void ClearPendingTasksNoLock()
	{
		while (!taskQueue.empty())
		{
			taskQueue.pop();
		}
	}

	void WorkerLoop()
	{
		while (true)
		{
			TileTask task;
			{
				std::unique_lock<std::mutex> lock(taskQueueMutex);
				taskQueueCondition.wait(lock, [this]() {
					return isStopping || !taskQueue.empty();
					});

				if (isStopping && taskQueue.empty())
				{
					return;
				}

				task = taskQueue.top();
				taskQueue.pop();
			}

			ProcessTileTask(task);
		}
	}

	bool IsTaskGenerationCurrent(std::uint64_t generation) const
	{
		return currentGeneration.load(std::memory_order_acquire) == generation;
	}

	std::string BuildRawImageCacheExtraKey(const TileTask& task) const
	{
		std::string keyText;
		keyText.reserve(512);
		keyText += "version=1\n";
		keyText += "referer=";
		keyText += task.networkOptions.refererUtf8;
		keyText += "\n";

		for (const std::string& header : task.networkOptions.headersUtf8)
		{
			keyText += "header=";
			keyText += header;
			keyText += "\n";
		}

		if (!task.connectionSettings.username.empty() || !task.connectionSettings.password.empty())
		{
			keyText += "credentialsHash=";
			keyText += GB_Md5Hash(task.connectionSettings.username + "\n" + task.connectionSettings.password);
			keyText += "\n";
		}

		return "LayerRefresherRawImageCacheV1:" + GB_Md5Hash(keyText);
	}

	TileImageCacheKey BuildRawImageCacheKey(const TileTask& task, const std::string& downloadUrl) const
	{
		ImageRequestItem cacheRequestItem = task.requestItem;
		cacheRequestItem.requestUrl = downloadUrl;
		cacheRequestItem.uid = task.requestItem.uid.empty() ? GB_Md5Hash(downloadUrl) : task.requestItem.uid;

		// 这里缓存的是服务端原始返回影像，而不是重投影后的显示影像。
		// 因此 targetWktUtf8 留空，使同一原始瓦片可被不同目标 CRS 复用。
		return TileImageCacheKey(cacheRequestItem, task.sourceWktUtf8, std::string(), BuildRawImageCacheExtraKey(task));
	}

	std::string GuessPreferredCacheFileExt(const TileTask& task, const GB_NetworkDownloadedFile& downloadedFile) const
	{
		std::string fileExt = GuessFileExtFromContentType(downloadedFile.contentTypeUtf8);
		if (!fileExt.empty())
		{
			return fileExt;
		}

		fileExt = GuessFileExtFromFileName(downloadedFile.fileNameUtf8);
		if (!fileExt.empty())
		{
			return fileExt;
		}

		return GuessFileExtFromImageFormatForLayerRefresher(task.requestItem.imageFormat);
	}

	static void QDebugCallback(const std::string& name, int64_t elapsedNs)
	{
		qDebug() << QString::fromStdString(name) << ":" << elapsedNs / 1000000.0 << "ms";
	}

	bool TryLoadRawImageFromCacheOrNetwork(const TileTask& task, const std::string& downloadUrl, const GB_ImageLoadOptions& loadOptions, GB_Image& outImage) const
	{
		outImage.Clear();

		const TileImageCacheKey cacheKey = BuildRawImageCacheKey(task, downloadUrl);
		if (TileImageCache::TryReadImage(cacheKey, outImage, loadOptions) && !outImage.IsEmpty())
		{
			return true;
		}

		if (!IsTaskGenerationCurrent(task.generation))
		{
			return false;
		}

		//const double sleepingTime = GB_RandomInt(50, 2000);
		//std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(sleepingTime)));
		//outImage.LoadFromFile("0aea755541b65721e72b2d76e9c691942c7431e5e0a8dfee02b537084600d519.jpg");

#if 0
		GB_NetworkResponse response;
		{
			//GB_ScopeTimer timer("download", &std::cerr, QDebugCallback);
			GB_Timer timer;
			timer.Start();
			response = GB_RequestUrlData(downloadUrl, task.networkOptions);
			const int64_t timeConsuming = timer.ElapsedMilliseconds();
			if (timeConsuming > 2000)
			{
				qDebug() << "Download slow:" << QString::fromStdString(downloadUrl) << timeConsuming << "ms";
			}

			if (!response.ok || response.body.empty())
			{
				return false;
			}
		}
#else
		const GB_NetworkResponse response = GB_RequestUrlData(downloadUrl, task.networkOptions);
		if (!response.ok || response.body.empty())
		{
			return false;
		}
#endif

		if (!outImage.LoadFromMemory(response.body.data(), response.body.size(), loadOptions) || outImage.IsEmpty())
		{
			outImage.Clear();
			return false;
		}

		const std::string preferredFileExt = GB_GuessFileExt(GB_StringToByteBuffer(response.body));
		TileImageCache::PutEncodedImage(cacheKey, response.body, preferredFileExt);
		return true;
	}

	void ProcessTileTask(const TileTask& task)
	{
		if (!IsTaskGenerationCurrent(task.generation))
		{
			return;
		}

		try
		{
			const std::string downloadUrl = PrefixRequestUrlIfNeeded(task.requestItem.requestUrl, task.connectionSettings);
			if (downloadUrl.empty())
			{
				PostTileTaskFinishedIfCurrent(task.generation);
				return;
			}

			if (!IsTaskGenerationCurrent(task.generation))
			{
				return;
			}

			GB_ImageLoadOptions loadOptions;
			loadOptions.colorMode = GB_ImageColorMode::BGRA;
			loadOptions.preserveBitDepth = false;

			GB_Image image;
			if (!TryLoadRawImageFromCacheOrNetwork(task, downloadUrl, loadOptions, image))
			{
				PostTileTaskFinishedIfCurrent(task.generation);
				return;
			}

			if (!IsTaskGenerationCurrent(task.generation))
			{
				return;
			}

			GB_Image displayImage;
			GB_Rectangle displayExtent;
			if (task.needsReprojection)
			{
				GeoImage sourceGeoImage(std::move(image), GeoBoundingBox(task.sourceWktUtf8, task.requestItem.imageExtent));
				if (!sourceGeoImage.IsValid())
				{
					PostTileTaskFinishedIfCurrent(task.generation);
					return;
				}

				GeoImageReprojectOptions reprojectOptions;
				reprojectOptions.interpolation = GB_ImageInterpolation::Linear;
				reprojectOptions.sampleGridCount = ReprojectSampleGridCount;
				reprojectOptions.targetPixelSizeX = task.targetPixelSizeX;
				reprojectOptions.targetPixelSizeY = task.targetPixelSizeY;
				reprojectOptions.maxOutputWidth = MaxReprojectOutputSideLength;
				reprojectOptions.maxOutputHeight = MaxReprojectOutputSideLength;
				reprojectOptions.maxOutputPixelCount = MaxReprojectOutputPixelCount;
				reprojectOptions.enableOpenMP = false;
				reprojectOptions.clampToCrsValidArea = true;

				GeoImage reprojectedImage;
#if 0
				{
					GB_ScopeTimer timer("reproject", &std::cerr, QDebugCallback);
					if (!GeoCrsTransform::ReprojectGeoImage(sourceGeoImage, task.targetWktUtf8, reprojectedImage, reprojectOptions) || !reprojectedImage.IsValid())
					{
						PostTileTaskFinishedIfCurrent(task.generation);
						return;
					}
				}
#else
				if (!GeoCrsTransform::ReprojectGeoImage(sourceGeoImage, task.targetWktUtf8, reprojectedImage, reprojectOptions) || !reprojectedImage.IsValid())
				{
					PostTileTaskFinishedIfCurrent(task.generation);
					return;
				}
#endif


				displayImage = std::move(reprojectedImage.image);
				displayExtent = reprojectedImage.boundingBox.rect;
			}
			else
			{
				displayImage = std::move(image);
				displayExtent = task.requestItem.imageExtent;
			}

			if (displayImage.IsEmpty() || !displayExtent.IsValid())
			{
				PostTileTaskFinishedIfCurrent(task.generation);
				return;
			}

			TileResult result;
			result.generation = task.generation;
			result.tileUid = task.tileUid;
			result.layerUid = task.layerUid;
			result.layerNumber = task.layerNumber;
			result.tile.image = std::move(displayImage);
			result.tile.extent = displayExtent;
			result.tile.uid = task.tileUid;
			result.tile.visible = true;
			result.tile.layerNumber = task.layerNumber;
			PostTileResult(std::move(result));
		}
		catch (...)
		{
			PostTileTaskFinishedIfCurrent(task.generation);
			return;
		}
	}

	void PostTileResult(TileResult result)
	{
		LayerRefresher* owner = ownerPointer.data();
		if (!owner)
		{
			return;
		}

		const QPointer<LayerRefresher> safeOwnerPointer = ownerPointer;
		QMetaObject::invokeMethod(owner, [safeOwnerPointer, result = std::move(result)]() mutable {
			LayerRefresher* refresher = safeOwnerPointer.data();
			if (!refresher || !refresher->impl)
			{
				return;
			}

			refresher->impl->AppendPendingTileResult(std::move(result));
			}, Qt::QueuedConnection);
	}

	void PostTileTaskFinishedIfCurrent(std::uint64_t generation)
	{
		if (!IsTaskGenerationCurrent(generation))
		{
			return;
		}

		LayerRefresher* owner = ownerPointer.data();
		if (!owner)
		{
			return;
		}

		const QPointer<LayerRefresher> safeOwnerPointer = ownerPointer;
		QMetaObject::invokeMethod(owner, [safeOwnerPointer, generation]() mutable {
			LayerRefresher* refresher = safeOwnerPointer.data();
			if (!refresher || !refresher->impl)
			{
				return;
			}

			refresher->impl->OnTileTaskFinished(generation);
			}, Qt::QueuedConnection);
	}

	void AppendPendingTileResult(TileResult&& result)
	{
		if (!IsTaskGenerationCurrent(result.generation))
		{
			return;
		}

		pendingTileResults.push_back(std::move(result));
		SchedulePendingTileResultFlush();
	}

	void SchedulePendingTileResultFlush()
	{
		if (isTileResultFlushScheduled)
		{
			return;
		}

		LayerRefresher* owner = ownerPointer.data();
		if (!owner)
		{
			return;
		}

		isTileResultFlushScheduled = true;
		const QPointer<LayerRefresher> safeOwnerPointer = ownerPointer;
		QTimer::singleShot(0, owner, [safeOwnerPointer]() mutable {
			LayerRefresher* refresher = safeOwnerPointer.data();
			if (!refresher || !refresher->impl)
			{
				return;
			}

			refresher->impl->FlushPendingTileResults();
			});
	}

	void FlushPendingTileResults()
	{
		isTileResultFlushScheduled = false;
		if (pendingTileResults.empty())
		{
			return;
		}

		std::vector<TileResult> results;
		results.swap(pendingTileResults);
		OnTileResultsReady(results);

		if (!pendingTileResults.empty())
		{
			SchedulePendingTileResultFlush();
		}
	}

	void OnTileTaskFinished(std::uint64_t generation)
	{
		if (!IsTaskGenerationCurrent(generation))
		{
			return;
		}

		MarkTileTaskFinished(generation);
	}

	void OnTileResultsReady(std::vector<TileResult>& results)
	{
		if (!mainCanvas)
		{
			return;
		}

		std::vector<std::string> uidsToRemove;
		std::vector<MapTile> tilesToAdd;
		uidsToRemove.reserve(results.size());
		tilesToAdd.reserve(results.size());

		for (TileResult& result : results)
		{
			if (!IsTaskGenerationCurrent(result.generation))
			{
				continue;
			}

			const auto targetIter = currentTargetsByUid.find(result.tileUid);
			if (targetIter != currentTargetsByUid.end())
			{
				const TargetTileRecord& target = targetIter->second;
				if (target.layerUid == result.layerUid && target.layerNumber == result.layerNumber && displayedTilesByUid.find(result.tileUid) == displayedTilesByUid.end())
				{
					result.tile.layerNumber = target.layerNumber;
					result.tile.visible = true;

					uidsToRemove.push_back(result.tileUid);
					transitionTilesByUid.erase(result.tileUid);
					tilesToAdd.push_back(result.tile);

					DisplayedTileRecord displayedTile;
					displayedTile.tile = result.tile;
					displayedTile.tile.layerNumber = target.layerNumber;
					displayedTile.layerUid = result.layerUid;
					displayedTile.layerNumber = target.layerNumber;
					displayedTilesByUid[result.tileUid] = std::move(displayedTile);
				}
			}

			MarkTileTaskFinished(result.generation);
		}

		if (!uidsToRemove.empty())
		{
			mainCanvas->RemoveDrawables(uidsToRemove);
		}

		if (!tilesToAdd.empty())
		{
			mainCanvas->AddMapTiles(tilesToAdd);
		}
	}

private:
	QPointer<LayerRefresher> ownerPointer;
	QPointer<QMainCanvas> mainCanvas;
	QPointer<QServiceBrowserPanel> serviceBrowserPanel;
	QPointer<QLayerManagerPanel> layerManagerPanel;

	std::vector<LayerSnapshot> layers;
	GB_Rectangle currentViewExtent;
	double currentApproximateMetersPerPixel = 0.0;
	bool hasViewExtent = false;
	bool isStartingRefresh = false;
	bool needsRestartAfterCurrentRefresh = false;

	std::unordered_map<std::string, TargetTileRecord> currentTargetsByUid;
	std::unordered_map<std::string, DisplayedTileRecord> displayedTilesByUid;
	std::unordered_map<std::string, DisplayedTileRecord> transitionTilesByUid;

	std::vector<TileResult> pendingTileResults;
	bool isTileResultFlushScheduled = false;

	std::atomic<std::uint64_t> currentGeneration;
	std::uint64_t nextTaskSequence;
	std::uint64_t trackedGeneration = 0;
	size_t trackedPendingTaskCount = 0;
	size_t trackedFinishedTaskCount = 0;

	std::vector<std::thread> workerThreads;
	std::priority_queue<TileTask, std::vector<TileTask>, TileTaskPriorityGreater> taskQueue;
	std::mutex taskQueueMutex;
	std::condition_variable taskQueueCondition;
	bool isStopping;
};

LayerRefresher* LayerRefresher::GetInstance()
{
	static LayerRefresher instance;
	return &instance;
}

void LayerRefresher::SetCanvasAndPanel(QMainCanvas* canvas, QServiceBrowserPanel* panel)
{
	impl->SetCanvasAndPanels(canvas, panel, nullptr);
}

void LayerRefresher::SetCanvasAndPanel(QMainCanvas* canvas, QLayerManagerPanel* layerManagerPanel)
{
	impl->SetCanvasAndPanels(canvas, nullptr, layerManagerPanel);
}

void LayerRefresher::SetCanvasAndPanels(QMainCanvas* canvas, QServiceBrowserPanel* serviceBrowserPanel, QLayerManagerPanel* layerManagerPanel)
{
	impl->SetCanvasAndPanels(canvas, serviceBrowserPanel, layerManagerPanel);
}

void LayerRefresher::StopRefresh()
{
	impl->StopRefresh();
}

LayerRefresher::LayerRefresher()
	: impl(new Impl(this))
{
}

LayerRefresher::~LayerRefresher()
{
}

void LayerRefresher::OnViewStateChanged(const GB_Rectangle& extent, double approximateMetersPerPixel)
{
	impl->OnViewStateChanged(extent, approximateMetersPerPixel);
}

void LayerRefresher::OnLayersChanged(const LayerManagerChangeInfo& changeInfo)
{
	impl->OnLayersChanged(changeInfo);
}

void LayerRefresher::OnArcGISRestLayerImportRequested(const LayerImportRequestInfo& request)
{
	impl->OnDirectLayerImportRequested(request);
}
