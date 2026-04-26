#include "LayerRefresher.h"
#include "QServiceBrowserPanel.h"
#include "QMainCanvas.h"
#include "GeoCrsManager.h"
#include "GeoBoundingBox.h"
#include "GeoBase/GB_Network.h"
#include "GeoBase/GB_ThreadPool.h"
#include "GeoBase/CV/GB_Image.h"

#include <QMetaObject>
#include <QThread>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <future>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
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

	GB_NetworkRequestOptions CreateNetworkOptionsFromConnectionSettings(const ArcGISRestConnectionSettings& settings)
	{
		GB_NetworkRequestOptions networkOptions;
		networkOptions.connectTimeoutMs = 5000;
		networkOptions.totalTimeoutMs = 0;
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

	bool ShouldRequestNodeJsonForImport(const LayerImportRequestInfo& request)
	{
		if (request.nodeType == ArcGISRestServiceTreeNode::NodeType::AllLayers)
		{
			return false;
		}

		if (IsArcGISRestServiceNodeType(request.nodeType))
		{
			return false;
		}

		return !request.nodeUrl.empty() && !request.layerId.empty();
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

	bool TryGetImportBoundingBox(const LayerImportRequestInfo& request, const ArcGISRestServiceInfo& nodeInfo, const ArcGISRestServiceInfo& serviceInfo, GeoBoundingBox& outBBox)
	{
		const std::string fallbackWkt = GetServiceFallbackWkt(serviceInfo);

		if (nodeInfo.layerOrTable.hasExtent && TryCreateBoundingBoxFromEnvelope(nodeInfo.layerOrTable.extent, fallbackWkt, outBBox))
		{
			return true;
		}

		if (nodeInfo.hasFullExtent && TryCreateBoundingBoxFromEnvelope(nodeInfo.fullExtent, fallbackWkt, outBBox))
		{
			return true;
		}

		if (nodeInfo.hasInitialExtent && TryCreateBoundingBoxFromEnvelope(nodeInfo.initialExtent, fallbackWkt, outBBox))
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

	constexpr size_t MaxParallelImageDownloadThreadCount = 8;

	size_t ChooseParallelImageDownloadThreadCount(size_t requestItemCount)
	{
		if (requestItemCount <= 1)
		{
			return requestItemCount;
		}

		const unsigned int hardwareThreadCount = std::thread::hardware_concurrency();
		size_t preferredThreadCount = (hardwareThreadCount > 0) ? static_cast<size_t>(hardwareThreadCount) : static_cast<size_t>(4);

		// 瓦片下载属于网络 I/O + 图像解码混合任务。线程数略高于低核心数机器的 CPU 核数可以隐藏网络等待，
		// 但不应无上限放大，避免对远端 ArcGIS 服务和本地解码造成过高瞬时压力。
		preferredThreadCount = std::max<size_t>(preferredThreadCount, 4);
		preferredThreadCount = std::min<size_t>(preferredThreadCount, MaxParallelImageDownloadThreadCount);
		return std::max<size_t>(1, std::min<size_t>(requestItemCount, preferredThreadCount));
	}

	struct ArcGISRestImageDownloadContext
	{
		LayerImportRequestInfo request;
		ArcGISRestServiceInfo serviceInfo;
		GB_Rectangle viewExtent;
		int viewExtentWidthInPixels = 0;
		int viewExtentHeightInPixels = 0;
		std::string imageFormat = "";
		bool isTiled = false;
		bool isImageServer = false;
		int dpi = 96;
		GB_NetworkRequestOptions networkOptions;
	};

	std::string PrefixRequestUrlIfNeeded(const std::string& requestUrl, const ArcGISRestConnectionSettings& connectionSettings)
	{
		const std::string urlPrefix = TrimmedStdString(connectionSettings.urlPrefix);
		if (urlPrefix.empty())
		{
			return requestUrl;
		}

		return urlPrefix + requestUrl;
	}
}

class LayerRefresher::ArcGISRestImageDownloadThread : public QThread
{
public:
	ArcGISRestImageDownloadThread(const QPointer<LayerRefresher>& refresherPointer, const ArcGISRestImageDownloadContext& context)
		: QThread(nullptr), refresherPointer(refresherPointer), context(context)
	{
	}

protected:
	virtual void run() override
	{
		if (isInterruptionRequested())
		{
			return;
		}

		CalculateImageRequestItemsInput input;
		input.viewExtent = context.viewExtent;
		input.viewExtentWidthInPixels = context.viewExtentWidthInPixels;
		input.viewExtentHeightInPixels = context.viewExtentHeightInPixels;
		input.serviceUrl = context.request.serviceUrl;
		input.layerId = context.request.layerId;
		input.imageFormat = context.imageFormat;
		input.serviceInfo = &context.serviceInfo;
		input.isTiled = context.isTiled;
		input.isImageServer = context.isImageServer;
		input.dpi = context.dpi;

		const std::vector<ImageRequestItem> requestItems = CalculateImageRequestItems(input);
		if (requestItems.empty())
		{
			return;
		}

		const size_t threadCount = ChooseParallelImageDownloadThreadCount(requestItems.size());
		if (threadCount == 0)
		{
			return;
		}

		try
		{
			GB_ThreadPool threadPool(threadCount);
			std::vector<std::future<bool>> futures;
			futures.reserve(requestItems.size());

			for (size_t itemIndex = 0; itemIndex < requestItems.size(); itemIndex++)
			{
				if (isInterruptionRequested())
				{
					break;
				}

				const ImageRequestItem requestItem = requestItems[itemIndex];
				if (requestItem.requestUrl.empty() || !requestItem.imageExtent.IsValid())
				{
					continue;
				}

				futures.push_back(threadPool.Enqueue([this, requestItem]() -> bool {
					return DownloadAndPostTile(requestItem);
					}));
			}

			for (std::future<bool>& future : futures)
			{
				try
				{
					future.get();
				}
				catch (...)
				{
					// 单个瓦片失败不应中断整个刷新流程。
				}
			}
		}
		catch (...)
		{
			return;
		}
	}

private:
	bool DownloadAndPostTile(const ImageRequestItem& requestItem)
	{
		if (isInterruptionRequested())
		{
			return false;
		}

		try
		{
			const std::string downloadUrl = PrefixRequestUrlIfNeeded(requestItem.requestUrl, context.request.connectionSettings);
			if (downloadUrl.empty())
			{
				return false;
			}

			const GB_NetworkResponse response = GB_RequestUrlData(downloadUrl, context.networkOptions);
			if (!response.ok || response.body.empty() || isInterruptionRequested())
			{
				return false;
			}

			GB_ImageLoadOptions loadOptions;
			loadOptions.colorMode = GB_ImageColorMode::BGRA;
			loadOptions.preserveBitDepth = false;

			GB_Image image;
			if (!image.LoadFromMemory(response.body.data(), response.body.size(), loadOptions) || image.IsEmpty() || isInterruptionRequested())
			{
				return false;
			}

			MapTile tile;
			tile.image = std::move(image);
			tile.extent = requestItem.imageExtent;
			tile.uid = requestItem.uid;
			tile.visible = true;
			PostTile(tile);
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	void PostTile(const MapTile& tile)
	{
		std::lock_guard<std::mutex> lock(postTileMutex);

		LayerRefresher* refresher = refresherPointer.data();
		if (!refresher)
		{
			return;
		}

		const QPointer<LayerRefresher> safeRefresherPointer = refresherPointer;
		QMetaObject::invokeMethod(refresher, [safeRefresherPointer, tile]() mutable {
			LayerRefresher* refresher = safeRefresherPointer.data();
			if (!refresher || !refresher->mainCanvas)
			{
				return;
			}

			refresher->mainCanvas->AddMapTile(tile);
			}, Qt::QueuedConnection);
	}

private:
	std::mutex postTileMutex;
	QPointer<LayerRefresher> refresherPointer;
	ArcGISRestImageDownloadContext context;
};

class LayerRefresher::ArcGISRestLayerImportThread : public QThread
{
private:
	struct ArcGISRestLayerImportResult
	{
		bool succeeded = false;
		LayerImportRequestInfo request;
		ArcGISRestServiceInfo nodeInfo;
		GeoBoundingBox layerBBox;
		std::string errorMessage = "";
	};

public:
	ArcGISRestLayerImportThread(const QPointer<LayerRefresher>& refresherPointer, const LayerImportRequestInfo& request)
		: QThread(nullptr), refresherPointer(refresherPointer), request(request)
	{
	}

protected:
	virtual void run() override
	{
		ArcGISRestLayerImportResult result;
		result.request = request;

		if (isInterruptionRequested())
		{
			return;
		}

		if (!request.IsValid())
		{
			result.errorMessage = "Invalid ArcGIS REST layer import request.";
			PostResult(result);
			return;
		}

		if (!CanImportNodeAsImage(request))
		{
			result.errorMessage = "Only MapServer and ImageServer nodes can be imported as map images currently.";
			PostResult(result);
			return;
		}

		if (!request.serviceInfo)
		{
			result.errorMessage = "ArcGIS REST service info is empty.";
			PostResult(result);
			return;
		}

		ArcGISRestServiceInfo nodeInfo = *request.serviceInfo;
		if (ShouldRequestNodeJsonForImport(request))
		{
			ArcGISRestConnectionSettings requestLayerSettings = request.connectionSettings;
			requestLayerSettings.serviceUrl = request.nodeUrl;

			std::string nodeJson = "";
			std::string requestJsonError = "";
			const GB_NetworkRequestOptions networkOptions = CreateNetworkOptionsFromConnectionSettings(requestLayerSettings);
			if (!RequestArcGISRestJson(requestLayerSettings, nodeJson, networkOptions, &requestJsonError))
			{
				result.errorMessage = "Failed to request ArcGIS REST layer JSON: " + requestJsonError;
				PostResult(result);
				return;
			}

			if (isInterruptionRequested())
			{
				return;
			}

			std::string parseError = "";
			if (!ParseArcGISRestJson(nodeJson, request.nodeUrl, nodeInfo, &parseError))
			{
				result.errorMessage = "Failed to parse ArcGIS REST layer JSON: " + parseError;
				PostResult(result);
				return;
			}
		}

		GeoBoundingBox layerBBox;
		if (!TryGetImportBoundingBox(request, nodeInfo, *request.serviceInfo, layerBBox))
		{
			result.errorMessage = "ArcGIS REST layer extent is invalid or missing.";
			PostResult(result);
			return;
		}

		result.succeeded = true;
		result.nodeInfo = std::move(nodeInfo);
		result.layerBBox = layerBBox;
		PostResult(result);
	}

private:
	void PostResult(const ArcGISRestLayerImportResult& result)
	{
		LayerRefresher* refresher = refresherPointer.data();
		if (!refresher)
		{
			return;
		}

		const QPointer<LayerRefresher> safeRefresherPointer = refresherPointer;
		QMetaObject::invokeMethod(refresher, [safeRefresherPointer, result]() mutable {
			LayerRefresher* refresher = safeRefresherPointer.data();
			if (!refresher || !refresher->serviceBrowserPanel || !refresher->mainCanvas)
			{
				return;
			}

			if (!result.succeeded)
			{
				return;
			}

			const std::string& layerWkt = result.layerBBox.wktUtf8;
			if (!refresher->SetCanvasCrsIfNeeded(layerWkt))
			{
				return;
			}

			if (!refresher->mainCanvas->HasDrawables())
			{
				refresher->mainCanvas->ZoomToExtent(result.layerBBox.rect, 0.05);
			}

			GB_Rectangle viewExtent;
			if (!refresher->mainCanvas->TryGetCurrentViewExtent(viewExtent))
			{
				return;
			}

			const int viewWidth = std::max(1, refresher->mainCanvas->width());
			const int viewHeight = std::max(1, refresher->mainCanvas->height());
			const ArcGISRestServiceInfo& serviceInfo = *result.request.serviceInfo;
			const bool isTiled = IsTiledServiceForImport(serviceInfo);
			const bool isImageServer = IsImageServerForImport(result.request);

			ArcGISRestImageDownloadContext context;
			context.request = result.request;
			context.serviceInfo = serviceInfo;
			context.viewExtent = viewExtent;
			context.viewExtentWidthInPixels = viewWidth;
			context.viewExtentHeightInPixels = viewHeight;
			context.isTiled = isTiled;
			context.isImageServer = isImageServer;
			context.imageFormat = ChooseImageFormat(serviceInfo, isTiled);
			context.dpi = ChooseDpi(serviceInfo);
			context.networkOptions = CreateNetworkOptionsFromConnectionSettings(result.request.connectionSettings);

			ArcGISRestImageDownloadThread* const downloadThread = new ArcGISRestImageDownloadThread(QPointer<LayerRefresher>(refresher), context);
			connect(downloadThread, &QThread::finished, downloadThread, &QObject::deleteLater);
			downloadThread->start();
			}, Qt::QueuedConnection);
	}

private:
	QPointer<LayerRefresher> refresherPointer;
	LayerImportRequestInfo request;
};

LayerRefresher* LayerRefresher::GetInstance()
{
	static LayerRefresher instance;
	return &instance;
}

void LayerRefresher::SetCanvasAndPanel(QMainCanvas* canvas, QServiceBrowserPanel* panel)
{
	QServiceBrowserPanel* const oldPanel = serviceBrowserPanel.data();
	if (oldPanel)
	{
		disconnect(oldPanel, &QServiceBrowserPanel::LayerImportRequested, this, &LayerRefresher::OnArcGISRestLayerImportRequested);
	}

	mainCanvas = canvas;
	serviceBrowserPanel = panel;

	if (mainCanvas && serviceBrowserPanel)
	{
		connect(serviceBrowserPanel.data(), &QServiceBrowserPanel::LayerImportRequested, this, &LayerRefresher::OnArcGISRestLayerImportRequested, Qt::UniqueConnection);
	}
}

LayerRefresher::LayerRefresher()
{
}

LayerRefresher::~LayerRefresher()
{
}

bool LayerRefresher::SetCanvasCrsIfNeeded(const std::string& wkt) const
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

void LayerRefresher::OnArcGISRestLayerImportRequested(const LayerImportRequestInfo& request)
{
	if (!serviceBrowserPanel || !mainCanvas || !request.IsValid())
	{
		return;
	}

	ArcGISRestLayerImportThread* const importThread = new ArcGISRestLayerImportThread(QPointer<LayerRefresher>(this), request);
	connect(importThread, &QThread::finished, importThread, &QObject::deleteLater);
	importThread->start();
}
