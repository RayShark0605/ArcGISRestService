#include "QServiceBrowserPanel.h"
#include "QArcGISRestConnectionDialog.h"

#include "QMainCanvas.h"

#include "GeoBase/GB_FileSystem.h"
#include "GeoBase/GB_IO.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDialog>
#include <QDir>
#include <QGroupBox>
#include <QMetaObject>
#include <QHash>
#include <QAbstractItemView>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QRectF>
#include <QSplitter>
#include <QStyle>
#include <QStringList>
#include <QTextEdit>
#include <QTextOption>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <cmath>

namespace
{
	enum ServiceBrowserItemRole
	{
		RoleUid = Qt::UserRole + 1,
		RoleUrl,
		RoleText,
		RoleNodeType,
		RoleCanDrag,
		RoleCanShowContextMenu,
		RoleDetails,
		RoleIsArcGISRestNode,
		RoleIsCategoryNode,
		RoleHasLazyChildren,
		RoleIsLazyPlaceholder,
		RoleIsLoading
	};

	const char* ArcGISRestCategoryUidText = "arcgis_rest_services_category";

	const char* ArcGISRestServicesMemoryFileName = "CustomArcGISRestServices.bin";
	const char ArcGISRestServicesMemoryFileMagic[] = { 'M', 'W', 'A', 'R', 'S', 'V', 'C', '1' };
	const uint32_t ArcGISRestServicesMemoryFileVersion = 1;
	const uint32_t ArcGISRestServicesMemoryFileMaxConnectionCount = 100000;
	const uint32_t ArcGISRestServicesMemoryFileMaxHeaderCount = 10000;
	const uint32_t ArcGISRestServicesMemoryFileMaxStringByteCount = 64u * 1024u * 1024u;

	QString NormalizeFilePathText(QString filePath)
	{
		return filePath.replace(QLatin1Char('\\'), QLatin1Char('/'));
	}

	void AppendRawBytes(GB_ByteBuffer& buffer, const void* data, size_t byteCount)
	{
		if (!data || byteCount == 0)
		{
			return;
		}

		const char* const begin = static_cast<const char*>(data);
		buffer.insert(buffer.end(), begin, begin + byteCount);
	}

	bool ReadRawBytes(const GB_ByteBuffer& buffer, size_t& offset, size_t byteCount, std::string& outBytes)
	{
		outBytes.clear();
		if (byteCount == 0)
		{
			return true;
		}

		if (offset > buffer.size() || byteCount > buffer.size() - offset)
		{
			return false;
		}

		const char* const begin = reinterpret_cast<const char*>(buffer.data()) + offset;
		outBytes.assign(begin, byteCount);
		offset += byteCount;
		return true;
	}

	bool AppendMemoryFileString(GB_ByteBuffer& buffer, const std::string& text)
	{
		if (text.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
		{
			return false;
		}

		GB_ByteBufferIO::AppendUInt32LE(buffer, static_cast<uint32_t>(text.size()));
		AppendRawBytes(buffer, text.data(), text.size());
		return true;
	}

	bool ReadMemoryFileString(const GB_ByteBuffer& buffer, size_t& offset, std::string& outText)
	{
		outText.clear();

		uint32_t textSize = 0;
		if (!GB_ByteBufferIO::ReadUInt32LE(buffer, offset, textSize))
		{
			return false;
		}

		if (textSize > ArcGISRestServicesMemoryFileMaxStringByteCount)
		{
			return false;
		}

		return ReadRawBytes(buffer, offset, static_cast<size_t>(textSize), outText);
	}

	bool AppendArcGISRestConnectionSettings(GB_ByteBuffer& buffer, const ArcGISRestConnectionSettings& settings)
	{
		if (!AppendMemoryFileString(buffer, settings.displayName) ||
			!AppendMemoryFileString(buffer, settings.serviceUrl) ||
			!AppendMemoryFileString(buffer, settings.urlPrefix) ||
			!AppendMemoryFileString(buffer, settings.portalCommunityEndpoint) ||
			!AppendMemoryFileString(buffer, settings.portalContentEndpoint) ||
			!AppendMemoryFileString(buffer, settings.username) ||
			!AppendMemoryFileString(buffer, settings.password) ||
			!AppendMemoryFileString(buffer, settings.httpReferer))
		{
			return false;
		}

		if (settings.httpCustomHeaders.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
		{
			return false;
		}

		GB_ByteBufferIO::AppendUInt32LE(buffer, static_cast<uint32_t>(settings.httpCustomHeaders.size()));
		for (const std::pair<std::string, std::string>& header : settings.httpCustomHeaders)
		{
			if (!AppendMemoryFileString(buffer, header.first) || !AppendMemoryFileString(buffer, header.second))
			{
				return false;
			}
		}
		return true;
	}

	bool ReadArcGISRestConnectionSettings(const GB_ByteBuffer& buffer, size_t& offset, ArcGISRestConnectionSettings& outSettings)
	{
		ArcGISRestConnectionSettings settings;
		if (!ReadMemoryFileString(buffer, offset, settings.displayName) ||
			!ReadMemoryFileString(buffer, offset, settings.serviceUrl) ||
			!ReadMemoryFileString(buffer, offset, settings.urlPrefix) ||
			!ReadMemoryFileString(buffer, offset, settings.portalCommunityEndpoint) ||
			!ReadMemoryFileString(buffer, offset, settings.portalContentEndpoint) ||
			!ReadMemoryFileString(buffer, offset, settings.username) ||
			!ReadMemoryFileString(buffer, offset, settings.password) ||
			!ReadMemoryFileString(buffer, offset, settings.httpReferer))
		{
			return false;
		}

		uint32_t headerCount = 0;
		if (!GB_ByteBufferIO::ReadUInt32LE(buffer, offset, headerCount) || headerCount > ArcGISRestServicesMemoryFileMaxHeaderCount)
		{
			return false;
		}

		settings.httpCustomHeaders.reserve(headerCount);
		for (uint32_t headerIndex = 0; headerIndex < headerCount; headerIndex++)
		{
			std::string headerName;
			std::string headerValue;
			if (!ReadMemoryFileString(buffer, offset, headerName) || !ReadMemoryFileString(buffer, offset, headerValue))
			{
				return false;
			}
			settings.httpCustomHeaders.push_back(std::make_pair(headerName, headerValue));
		}

		outSettings = std::move(settings);
		return true;
	}

	bool SerializeArcGISRestConnectionSettingsList(const std::vector<ArcGISRestConnectionSettings>& settingsList, GB_ByteBuffer& outBuffer)
	{
		outBuffer.clear();
		if (settingsList.size() > static_cast<size_t>(ArcGISRestServicesMemoryFileMaxConnectionCount))
		{
			return false;
		}

		AppendRawBytes(outBuffer, ArcGISRestServicesMemoryFileMagic, sizeof(ArcGISRestServicesMemoryFileMagic));
		GB_ByteBufferIO::AppendUInt32LE(outBuffer, ArcGISRestServicesMemoryFileVersion);
		GB_ByteBufferIO::AppendUInt32LE(outBuffer, static_cast<uint32_t>(settingsList.size()));
		for (const ArcGISRestConnectionSettings& settings : settingsList)
		{
			if (!AppendArcGISRestConnectionSettings(outBuffer, settings))
			{
				outBuffer.clear();
				return false;
			}
		}
		return true;
	}

	bool DeserializeArcGISRestConnectionSettingsList(const GB_ByteBuffer& buffer, std::vector<ArcGISRestConnectionSettings>& outSettingsList)
	{
		outSettingsList.clear();
		if (buffer.empty())
		{
			return false;
		}

		size_t offset = 0;
		std::string magic;
		if (!ReadRawBytes(buffer, offset, sizeof(ArcGISRestServicesMemoryFileMagic), magic) ||
			magic != std::string(ArcGISRestServicesMemoryFileMagic, sizeof(ArcGISRestServicesMemoryFileMagic)))
		{
			return false;
		}

		uint32_t version = 0;
		if (!GB_ByteBufferIO::ReadUInt32LE(buffer, offset, version) || version != ArcGISRestServicesMemoryFileVersion)
		{
			return false;
		}

		uint32_t connectionCount = 0;
		if (!GB_ByteBufferIO::ReadUInt32LE(buffer, offset, connectionCount) || connectionCount > ArcGISRestServicesMemoryFileMaxConnectionCount)
		{
			return false;
		}

		std::vector<ArcGISRestConnectionSettings> settingsList;
		settingsList.reserve(connectionCount);
		for (uint32_t connectionIndex = 0; connectionIndex < connectionCount; connectionIndex++)
		{
			ArcGISRestConnectionSettings settings;
			if (!ReadArcGISRestConnectionSettings(buffer, offset, settings))
			{
				return false;
			}
			settingsList.push_back(std::move(settings));
		}

		if (offset != buffer.size())
		{
			return false;
		}

		outSettingsList.swap(settingsList);
		return true;
	}

	void ShowArcGISRestServicesMemoryFileError(QWidget* parent, const QString& mainText, const QString& detailText)
	{
		QMessageBox messageBox(parent);
		messageBox.setIcon(QMessageBox::Warning);
		messageBox.setWindowTitle(QStringLiteral("ArcGIS REST 服务记忆文件错误"));
		messageBox.setText(mainText);
		if (!detailText.isEmpty())
		{
			messageBox.setDetailedText(detailText);
		}
		messageBox.setStandardButtons(QMessageBox::Ok);
		messageBox.exec();
	}

	bool IsPlaceholderItem(const QTreeWidgetItem* item)
	{
		return item && item->data(0, RoleIsLazyPlaceholder).toBool();
	}

	bool IsNetworkExpandableArcGISRestNodeType(ArcGISRestServiceTreeNode::NodeType nodeType)
	{
		switch (nodeType)
		{
		case ArcGISRestServiceTreeNode::NodeType::Root:
		case ArcGISRestServiceTreeNode::NodeType::Folder:
		case ArcGISRestServiceTreeNode::NodeType::MapService:
		case ArcGISRestServiceTreeNode::NodeType::ImageService:
		case ArcGISRestServiceTreeNode::NodeType::FeatureService:
			return true;
		case ArcGISRestServiceTreeNode::NodeType::Unknown:
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

	bool IsArcGISRestNodeExpansionResolved(const ArcGISRestServiceTreeNode& node)
	{
		return node.serviceInfo.resourceType != ArcGISRestResourceType::Unknown;
	}

	bool ShouldShowLazyExpandableIndicator(const ArcGISRestServiceTreeNode& node, bool showLazyExpandableIndicators)
	{
		return showLazyExpandableIndicators &&
			node.children.empty() &&
			!node.url.empty() &&
			IsNetworkExpandableArcGISRestNodeType(node.type) &&
			!IsArcGISRestNodeExpansionResolved(node);
	}

	QString ToQString(const std::string& textUtf8)
	{
		return QString::fromUtf8(textUtf8.c_str());
	}

	std::string ToStdString(const QString& text)
	{
		return text.toUtf8().constData();
	}

	QString BoolToText(bool value)
	{
		return value ? QStringLiteral("是") : QStringLiteral("否");
	}


	QString TrimmedQStringFromStdString(const std::string& textUtf8)
	{
		return ToQString(textUtf8).trimmed();
	}

	std::string TrimmedStdString(const std::string& textUtf8)
	{
		return ToStdString(TrimmedQStringFromStdString(textUtf8));
	}

	std::string NormalizeArcGISRestServiceUrl(const std::string& serviceUrlUtf8)
	{
		std::string normalizedUrl = TrimmedStdString(serviceUrlUtf8);
		if (normalizedUrl.empty())
		{
			return std::string();
		}

		const std::string urlBase = GB_UrlOperator::GetUrlBase(normalizedUrl);
		if (!urlBase.empty())
		{
			normalizedUrl = urlBase;
		}

		while (!normalizedUrl.empty() && normalizedUrl.back() == '/')
		{
			normalizedUrl.pop_back();
		}
		return normalizedUrl;
	}


	std::string NormalizeArcGISRestRequestUrl(const std::string& serviceUrlUtf8)
	{
		std::string requestUrl = TrimmedStdString(serviceUrlUtf8);
		if (requestUrl.empty())
		{
			return std::string();
		}

		const size_t fragmentPos = requestUrl.find('#');
		if (fragmentPos != std::string::npos)
		{
			requestUrl.erase(fragmentPos);
		}

		requestUrl = GB_UrlOperator::RemoveUrlQueryKey(requestUrl, "f");
		return requestUrl;
	}

	QString GuessArcGISRestDisplayName(const ArcGISRestConnectionSettings& settings)
	{
		const QString displayName = TrimmedQStringFromStdString(settings.displayName);
		if (!displayName.isEmpty())
		{
			return displayName;
		}

		const std::string normalizedUrl = NormalizeArcGISRestServiceUrl(settings.serviceUrl);
		QString urlText = ToQString(normalizedUrl);
		if (urlText.isEmpty())
		{
			return QStringLiteral("ArcGIS REST 服务");
		}

		const int queryIndex = urlText.indexOf(QLatin1Char('?'));
		if (queryIndex >= 0)
		{
			urlText = urlText.left(queryIndex);
		}

		const int fragmentIndex = urlText.indexOf(QLatin1Char('#'));
		if (fragmentIndex >= 0)
		{
			urlText = urlText.left(fragmentIndex);
		}

		while (urlText.endsWith(QLatin1Char('/')))
		{
			urlText.chop(1);
		}

		QStringList urlParts = urlText.split(QLatin1Char('/'), QString::SkipEmptyParts);
		if (urlParts.isEmpty())
		{
			return QStringLiteral("ArcGIS REST 服务");
		}

		const QString lastPart = urlParts.last();
		const QString lowerLastPart = lastPart.toLower();
		if ((lowerLastPart == QStringLiteral("mapserver") || lowerLastPart == QStringLiteral("featureserver") || lowerLastPart == QStringLiteral("imageserver")) && urlParts.size() >= 2)
		{
			return urlParts.at(urlParts.size() - 2);
		}
		return lastPart;
	}

	ArcGISRestServiceTreeNode::NodeType GuessArcGISRestNodeTypeFromUrl(const std::string& serviceUrlUtf8)
	{
		QString urlText = ToQString(NormalizeArcGISRestServiceUrl(serviceUrlUtf8)).toLower();
		while (urlText.endsWith(QLatin1Char('/')))
		{
			urlText.chop(1);
		}

		if (urlText.endsWith(QStringLiteral("/mapserver")))
		{
			return ArcGISRestServiceTreeNode::NodeType::MapService;
		}
		if (urlText.endsWith(QStringLiteral("/featureserver")))
		{
			return ArcGISRestServiceTreeNode::NodeType::FeatureService;
		}
		if (urlText.endsWith(QStringLiteral("/imageserver")))
		{
			return ArcGISRestServiceTreeNode::NodeType::ImageService;
		}
		return ArcGISRestServiceTreeNode::NodeType::Root;
	}

	ArcGISRestServiceTreeNode::NodeType GetRootNodeTypeFromServiceInfo(const ArcGISRestServiceInfo& serviceInfo, const std::string& serviceUrlUtf8, ArcGISRestServiceTreeNode::NodeType fallbackType)
	{
		if (serviceInfo.resourceType != ArcGISRestResourceType::Service)
		{
			return ArcGISRestServiceTreeNode::NodeType::Root;
		}

		const ArcGISRestServiceTreeNode::NodeType typeFromUrl = GuessArcGISRestNodeTypeFromUrl(serviceUrlUtf8);
		if (typeFromUrl == ArcGISRestServiceTreeNode::NodeType::MapService || typeFromUrl == ArcGISRestServiceTreeNode::NodeType::FeatureService ||
			typeFromUrl == ArcGISRestServiceTreeNode::NodeType::ImageService)
		{
			return typeFromUrl;
		}

		if (fallbackType == ArcGISRestServiceTreeNode::NodeType::MapService || fallbackType == ArcGISRestServiceTreeNode::NodeType::FeatureService ||
			fallbackType == ArcGISRestServiceTreeNode::NodeType::ImageService)
		{
			return fallbackType;
		}

		return ArcGISRestServiceTreeNode::NodeType::Root;
	}

	GB_NetworkRequestOptions CreateNetworkOptionsFromConnectionSettings(const ArcGISRestConnectionSettings& settings)
	{
		GB_NetworkRequestOptions networkOptions;
		networkOptions.connectTimeoutMs = 5000;
		networkOptions.totalTimeoutMs = 30000;
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

	std::string JoinArcGISRestLayerIds(const ArcGISRestServiceInfo& serviceInfo)
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
				result += ',';
			}
			result += layer.id;
		}
		return result;
	}

	std::string ExtractFirstUrlPathSegment(const std::string& text)
	{
		if (text.empty())
		{
			return std::string();
		}

		const size_t slashIndex = text.find('/');
		if (slashIndex == std::string::npos)
		{
			return text;
		}
		return text.substr(0, slashIndex);
	}

	void ShowArcGISRestConnectionError(QWidget* parent, const QString& mainText, const QString& detailText)
	{
		QMessageBox messageBox(parent);
		messageBox.setIcon(QMessageBox::Critical);
		messageBox.setWindowTitle(QStringLiteral("ArcGIS REST 服务连接失败"));
		messageBox.setText(mainText);
		if (!detailText.isEmpty())
		{
			messageBox.setDetailedText(detailText);
		}
		messageBox.setStandardButtons(QMessageBox::Ok);
		messageBox.exec();
	}



	ArcGISRestConnectionSettings NormalizeArcGISRestConnectionSettings(ArcGISRestConnectionSettings settings)
	{
		settings.displayName = TrimmedStdString(settings.displayName);
		settings.serviceUrl = NormalizeArcGISRestRequestUrl(settings.serviceUrl);
		settings.urlPrefix = TrimmedStdString(settings.urlPrefix);
		settings.portalCommunityEndpoint = TrimmedStdString(settings.portalCommunityEndpoint);
		settings.portalContentEndpoint = TrimmedStdString(settings.portalContentEndpoint);
		settings.httpReferer = TrimmedStdString(settings.httpReferer);

		std::vector<std::pair<std::string, std::string>> headers;
		headers.reserve(settings.httpCustomHeaders.size());
		for (const std::pair<std::string, std::string>& header : settings.httpCustomHeaders)
		{
			const std::string headerName = TrimmedStdString(header.first);
			if (headerName.empty() && TrimmedStdString(header.second).empty())
			{
				continue;
			}

			headers.push_back(std::make_pair(headerName, header.second));
		}
		settings.httpCustomHeaders.swap(headers);
		return settings;
	}

	bool AreArcGISRestConnectionHeadersEqual(const std::vector<std::pair<std::string, std::string>>& firstHeaders,
		const std::vector<std::pair<std::string, std::string>>& secondHeaders)
	{
		if (firstHeaders.size() != secondHeaders.size())
		{
			return false;
		}

		for (size_t headerIndex = 0; headerIndex < firstHeaders.size(); headerIndex++)
		{
			if (TrimmedStdString(firstHeaders[headerIndex].first) != TrimmedStdString(secondHeaders[headerIndex].first) ||
				firstHeaders[headerIndex].second != secondHeaders[headerIndex].second)
			{
				return false;
			}
		}
		return true;
	}

	bool AreArcGISRestConnectionSettingsEqualExceptDisplayName(const ArcGISRestConnectionSettings& firstSettings,
		const ArcGISRestConnectionSettings& secondSettings)
	{
		const ArcGISRestConnectionSettings first = NormalizeArcGISRestConnectionSettings(firstSettings);
		const ArcGISRestConnectionSettings second = NormalizeArcGISRestConnectionSettings(secondSettings);

		return first.serviceUrl == second.serviceUrl &&
			first.urlPrefix == second.urlPrefix &&
			first.portalCommunityEndpoint == second.portalCommunityEndpoint &&
			first.portalContentEndpoint == second.portalContentEndpoint &&
			first.username == second.username &&
			first.password == second.password &&
			first.httpReferer == second.httpReferer &&
			AreArcGISRestConnectionHeadersEqual(first.httpCustomHeaders, second.httpCustomHeaders);
	}

	ArcGISRestServiceTreeNode CreateArcGISRestConnectionNodeFromSettings(const ArcGISRestConnectionSettings& settings, const std::string& serviceBaseUrl)
	{
		ArcGISRestServiceTreeNode connectionNode;
		connectionNode.type = GuessArcGISRestNodeTypeFromUrl(serviceBaseUrl);
		connectionNode.text = settings.displayName.empty() ? ToStdString(QStringLiteral("ArcGIS REST 服务")) : settings.displayName;
		connectionNode.url = serviceBaseUrl;
		connectionNode.uid = connectionNode.CalculateUid();
		connectionNode.parentUid = ToStdString(QServiceBrowserPanel::GetArcGISRestCategoryUid());
		return connectionNode;
	}

	class ServiceBrowserTreeWidget : public QTreeWidget
	{
	public:
		explicit ServiceBrowserTreeWidget(QWidget* parent = nullptr) : QTreeWidget(parent)
		{
		}

	protected:
		virtual QMimeData* mimeData(const QList<QTreeWidgetItem*> items) const override
		{
			QMimeData* mimeData = new QMimeData();
			if (items.size() != 1)
			{
				return mimeData;
			}

			const QTreeWidgetItem* item = items.first();
			if (!item || IsPlaceholderItem(item) || !item->data(0, RoleCanDrag).toBool())
			{
				return mimeData;
			}

			QJsonObject object;
			object.insert(QStringLiteral("uid"), item->data(0, RoleUid).toString());
			object.insert(QStringLiteral("url"), item->data(0, RoleUrl).toString());
			object.insert(QStringLiteral("text"), item->data(0, RoleText).toString());
			object.insert(QStringLiteral("nodeType"), item->data(0, RoleNodeType).toInt());

			const QJsonDocument document(object);
			mimeData->setData(QMainCanvas::GetServiceNodeMimeType(), document.toJson(QJsonDocument::Compact));
			mimeData->setText(item->data(0, RoleText).toString());
			return mimeData;
		}

		virtual void startDrag(Qt::DropActions supportedActions) override
		{
			const QList<QTreeWidgetItem*> items = selectedItems();
			if (items.size() != 1)
			{
				return;
			}

			QTreeWidgetItem* item = items.first();
			if (!item || IsPlaceholderItem(item) || !item->data(0, RoleCanDrag).toBool())
			{
				return;
			}

			QTreeWidget::startDrag(supportedActions);
		}
	};


}

bool LayerImportRequestInfo::IsValid() const
{
	return !nodeUid.empty() && !serviceUrl.empty() && !connectionSettings.serviceUrl.empty() && serviceInfo != nullptr &&
		nodeType != ArcGISRestServiceTreeNode::NodeType::Unknown &&
		IsArcGISRestServiceNodeType(serviceNodeType);
}

class QServiceBrowserPanel::ArcGISRestConnectionLoadThread : public QThread
{
private:
	struct ArcGISRestConnectionLoadResult
	{
		QString sourceUid = "";
		quint64 loadToken = 0;
		bool succeeded = false;
		QString errorMainText = "";
		QString errorDetailText = "";
		ArcGISRestConnectionSettings settings;
		std::string serviceBaseUrl = "";
		ArcGISRestServiceTreeNode connectionNode;
		ArcGISRestServiceTreeNode loadedNode;
		ArcGISRestConnectionLoadMode loadMode = ArcGISRestConnectionLoadMode::AddNew;
	};

public:
	ArcGISRestConnectionLoadThread(const QPointer<QServiceBrowserPanel>& panelPointer,
		const QString& sourceUid,
		quint64 loadToken,
		const ArcGISRestConnectionSettings& settings,
		const std::string& serviceBaseUrl,
		const ArcGISRestServiceTreeNode& connectionNode,
		ArcGISRestConnectionLoadMode loadMode)
		: QThread(nullptr), panelPointer(panelPointer), sourceUid(sourceUid), loadToken(loadToken), settings(settings),
		serviceBaseUrl(serviceBaseUrl), connectionNode(connectionNode), loadMode(loadMode)
	{
	}

protected:
	virtual void run() override
	{
		ArcGISRestConnectionLoadResult result;
		result.sourceUid = sourceUid;
		result.loadToken = loadToken;
		result.settings = settings;
		result.serviceBaseUrl = serviceBaseUrl;
		result.connectionNode = connectionNode;
		result.loadMode = loadMode;

		std::string jsonText = "";
		std::string requestErrorMessage = "";
		const GB_NetworkRequestOptions networkOptions = CreateNetworkOptionsFromConnectionSettings(settings);
		const bool requestSucceeded = RequestArcGISRestJson(settings, jsonText, networkOptions, &requestErrorMessage) && !jsonText.empty();
		if (!requestSucceeded)
		{
			result.errorMainText = QStringLiteral("请求 ArcGIS REST 服务 JSON 失败。");
			result.errorDetailText = ToQString(requestErrorMessage);
			PostResult(result);
			return;
		}

		ArcGISRestServiceInfo serviceInfo;
		std::string parseErrorMessage = "";
		if (!ParseArcGISRestJson(jsonText, serviceBaseUrl, serviceInfo, &parseErrorMessage))
		{
			result.errorMainText = QStringLiteral("解析 ArcGIS REST 服务 JSON 失败。");
			result.errorDetailText = ToQString(parseErrorMessage);
			PostResult(result);
			return;
		}

		ArcGISRestServiceTreeNode loadedNode = connectionNode;
		loadedNode.type = GetRootNodeTypeFromServiceInfo(serviceInfo, serviceBaseUrl, connectionNode.type);
		loadedNode.uid = loadedNode.CalculateUid();
		loadedNode.children.clear();
		loadedNode.serviceInfo = ArcGISRestServiceInfo();

		if (!BuildArcGISRestServiceTree(serviceInfo, loadedNode))
		{
			result.errorMainText = QStringLiteral("根据 ArcGIS REST 服务信息构建图层树失败。");
			PostResult(result);
			return;
		}

		result.succeeded = true;
		result.loadedNode = std::move(loadedNode);
		PostResult(result);
	}

private:

	void PostResult(const ArcGISRestConnectionLoadResult& result)
	{
		QServiceBrowserPanel* panel = panelPointer.data();
		if (!panel)
		{
			return;
		}

		const QPointer<QServiceBrowserPanel> safePanelPointer = panelPointer;
		QMetaObject::invokeMethod(panel, [safePanelPointer, result]() mutable {
			QServiceBrowserPanel* panel = safePanelPointer.data();
			if (!panel)
			{
				return;
			}

			if (!panel->IsArcGISRestConnectionLoadCurrent(result.sourceUid, result.loadToken))
			{
				return;
			}
			panel->EndArcGISRestConnectionLoad(result.sourceUid, result.loadToken);

			if (!result.succeeded)
			{
				QString statusText;
				if (result.loadMode == ArcGISRestConnectionLoadMode::AddNew)
				{
					statusText = QStringLiteral("状态：首次加载失败。服务连接已保留，可稍后刷新或编辑后重试。");
				}
				else if (result.loadMode == ArcGISRestConnectionLoadMode::RefreshExisting)
				{
					statusText = QStringLiteral("状态：刷新失败。服务连接已保留。");
				}
				else
				{
					statusText = QStringLiteral("状态：编辑后重新加载失败。服务连接已保留，可重新编辑或刷新。");
				}

				if (!result.errorMainText.isEmpty())
				{
					statusText += QStringLiteral("\n\n") + result.errorMainText.trimmed();
				}
				if (!result.errorDetailText.isEmpty())
				{
					statusText += QStringLiteral("\n\n详细错误：") + result.errorDetailText;
				}

				const QString currentUidBeforeRestore = panel->GetCurrentSelectableNodeUid();
				panel->RestoreArcGISRestNodeLazyExpansionState(result.connectionNode, statusText);
				panel->RestoreCurrentSelectableNodeByUid(currentUidBeforeRestore, result.sourceUid);

				ShowArcGISRestConnectionError(panel, result.errorMainText, result.errorDetailText);
				return;
			}

			const QString currentUidBeforeUpdate = panel->GetCurrentSelectableNodeUid();
			const QString loadedUid = ToQString(result.loadedNode.uid);
			const QString fallbackUid = (currentUidBeforeUpdate == result.sourceUid) ? loadedUid : QString();
			bool updateSucceeded = false;
			if (loadedUid == result.sourceUid)
			{
				updateSucceeded = panel->UpdateArcGISRestServiceNode(result.loadedNode, true);
			}
			else
			{
				panel->RemoveNode(result.sourceUid);
				updateSucceeded = panel->AddArcGISRestServiceNode(result.loadedNode, true);
			}

			if (!updateSucceeded)
			{
				panel->RemoveNode(loadedUid);
				ShowArcGISRestConnectionError(panel, QStringLiteral("更新 ArcGIS REST 服务浏览树失败。"), QString());
				return;
			}

			panel->MoveArcGISRestConnectionSettings(result.sourceUid, loadedUid);
			panel->SetArcGISRestConnectionSettings(loadedUid, result.settings);
			panel->RestoreCurrentSelectableNodeByUid(currentUidBeforeUpdate, fallbackUid);
			if (!result.loadedNode.children.empty())
			{
				panel->ExpandNodeByUid(loadedUid);
			}
			}, Qt::QueuedConnection);
	}

private:
	QPointer<QServiceBrowserPanel> panelPointer;
	QString sourceUid = "";
	quint64 loadToken = 0;
	ArcGISRestConnectionSettings settings;
	std::string serviceBaseUrl = "";
	ArcGISRestServiceTreeNode connectionNode;
	ArcGISRestConnectionLoadMode loadMode = ArcGISRestConnectionLoadMode::AddNew;
};

class QServiceBrowserPanel::ArcGISRestNodeExpandThread : public QThread
{
public:
	ArcGISRestNodeExpandThread(const QPointer<QServiceBrowserPanel>& panelPointer, const ArcGISRestServiceTreeNode& sourceNode,
		const ArcGISRestConnectionSettings& settings)
		: QThread(nullptr), panelPointer(panelPointer), sourceNode(sourceNode), settings(settings)
	{
	}

protected:
	virtual void run() override
	{
		ArcGISRestNodeExpandResult result;
		result.sourceNode = sourceNode;

		if (sourceNode.url.empty())
		{
			result.errorMainText = QStringLiteral("节点 URL 为空，无法展开。 ");
			PostResult(result);
			return;
		}

		ArcGISRestConnectionSettings requestSettings = settings;
		requestSettings.serviceUrl = NormalizeArcGISRestRequestUrl(sourceNode.url);

		std::string jsonText = "";
		std::string requestErrorMessage = "";
		const GB_NetworkRequestOptions networkOptions = CreateNetworkOptionsFromConnectionSettings(requestSettings);
		if (!RequestArcGISRestJson(requestSettings, jsonText, networkOptions, &requestErrorMessage) || jsonText.empty())
		{
			result.errorMainText = QStringLiteral("请求 ArcGIS REST 节点 JSON 失败。 ");
			result.errorDetailText = ToQString(requestErrorMessage);
			PostResult(result);
			return;
		}

		ArcGISRestServiceInfo serviceInfo;
		std::string parseErrorMessage = "";
		if (!ParseArcGISRestJson(jsonText, sourceNode.url, serviceInfo, &parseErrorMessage))
		{
			result.errorMainText = QStringLiteral("解析 ArcGIS REST 节点 JSON 失败。 ");
			result.errorDetailText = ToQString(parseErrorMessage);
			PostResult(result);
			return;
		}

		ArcGISRestServiceTreeNode expandedNode = sourceNode;
		expandedNode.children.clear();
		expandedNode.serviceInfo = ArcGISRestServiceInfo();
		if (!BuildArcGISRestServiceTree(serviceInfo, expandedNode))
		{
			result.errorMainText = QStringLiteral("根据 ArcGIS REST 节点信息构建图层树失败。 ");
			PostResult(result);
			return;
		}

		result.succeeded = true;
		result.expandedNode = std::move(expandedNode);
		PostResult(result);
	}

private:
	struct ArcGISRestNodeExpandResult
	{
		bool succeeded = false;
		QString errorMainText = "";
		QString errorDetailText = "";
		ArcGISRestServiceTreeNode sourceNode;
		ArcGISRestServiceTreeNode expandedNode;
	};

	void PostResult(const ArcGISRestNodeExpandResult& result)
	{
		QServiceBrowserPanel* panel = panelPointer.data();
		if (!panel)
		{
			return;
		}

		const QPointer<QServiceBrowserPanel> safePanelPointer = panelPointer;
		QMetaObject::invokeMethod(panel, [safePanelPointer, result]() mutable {
			QServiceBrowserPanel* panel = safePanelPointer.data();
			if (!panel)
			{
				return;
			}

			if (!result.succeeded)
			{
				QString statusText = QStringLiteral("状态：展开失败。 ");
				if (!result.errorMainText.isEmpty())
				{
					statusText += result.errorMainText.trimmed();
				}
				if (!result.errorDetailText.isEmpty())
				{
					statusText += QStringLiteral("\n\n详细错误：") + result.errorDetailText;
				}
				panel->RestoreArcGISRestNodeLazyExpansionState(result.sourceNode, statusText);
				ShowArcGISRestConnectionError(panel, result.errorMainText.trimmed(), result.errorDetailText);
				return;
			}

			const QString currentUidBeforeUpdate = panel->GetCurrentSelectableNodeUid();
			if (!panel->UpdateArcGISRestServiceNode(result.expandedNode, true))
			{
				panel->RestoreArcGISRestNodeLazyExpansionState(result.sourceNode, QStringLiteral("状态：展开成功，但更新服务浏览树失败。"));
				ShowArcGISRestConnectionError(panel, QStringLiteral("更新 ArcGIS REST 服务浏览树失败。"), QString());
				return;
			}

			const QString expandedUid = ToQString(result.expandedNode.uid);
			panel->RestoreCurrentSelectableNodeByUid(currentUidBeforeUpdate);
			if (!result.expandedNode.children.empty())
			{
				panel->ExpandNodeByUid(expandedUid);
			}
			}, Qt::QueuedConnection);
	}

private:
	QPointer<QServiceBrowserPanel> panelPointer;
	ArcGISRestServiceTreeNode sourceNode;
	ArcGISRestConnectionSettings settings;
};

QServiceBrowserPanel::QServiceBrowserPanel(QWidget* parent) : QDockWidget(QStringLiteral("服务浏览"), parent)
{
	qRegisterMetaType<LayerImportRequestInfo>("ArcGISRestLayerImportRequest");

	setObjectName(QStringLiteral("QServiceBrowserPanel"));
	setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
	setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

	QWidget* contentWidget = new QWidget(this);
	QVBoxLayout* mainLayout = new QVBoxLayout(contentWidget);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(0);

	QSplitter* splitter = new QSplitter(Qt::Vertical, contentWidget);
	mainLayout->addWidget(splitter);

	treeWidget = new ServiceBrowserTreeWidget(splitter);
	treeWidget->setHeaderHidden(true);
	treeWidget->setColumnCount(1);
	treeWidget->setRootIsDecorated(true);
	treeWidget->setItemsExpandable(true);
	treeWidget->setExpandsOnDoubleClick(true);
	treeWidget->setAnimated(false);
	treeWidget->setSelectionMode(QAbstractItemView::SingleSelection);
	treeWidget->setDragEnabled(true);
	treeWidget->setAcceptDrops(false);
	treeWidget->setDragDropMode(QAbstractItemView::DragOnly);
	treeWidget->setDefaultDropAction(Qt::CopyAction);
	treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
	treeWidget->header()->setStretchLastSection(true);

	QGroupBox* detailsGroupBox = new QGroupBox(QStringLiteral("详细信息"), splitter);
	QVBoxLayout* detailsLayout = new QVBoxLayout(detailsGroupBox);
	detailsLayout->setContentsMargins(6, 6, 6, 6);
	detailsLayout->setSpacing(0);

	detailsTextEdit = new QTextEdit(detailsGroupBox);
	detailsTextEdit->setReadOnly(true);
	detailsTextEdit->setAcceptRichText(false);
	detailsTextEdit->setLineWrapMode(QTextEdit::WidgetWidth);
	detailsTextEdit->setWordWrapMode(QTextOption::WrapAnywhere);
	detailsTextEdit->setMinimumHeight(80);
	detailsTextEdit->setPlainText(QStringLiteral("未选择节点。"));
	detailsLayout->addWidget(detailsTextEdit);

	splitter->addWidget(treeWidget);
	splitter->addWidget(detailsGroupBox);
	splitter->setStretchFactor(0, 4);
	splitter->setStretchFactor(1, 1);
	splitter->setSizes(QList<int>() << 500 << 160);

	setWidget(contentWidget);

	loadingAnimationTimer = new QTimer(this);
	loadingAnimationTimer->setInterval(80);
	connect(loadingAnimationTimer, &QTimer::timeout, this, &QServiceBrowserPanel::OnLoadingAnimationTimerTimeout);

	EnsureArcGISRestCategoryItem();
	if (!LoadArcGISRestServicesFromMemoryFile())
	{
		ShowArcGISRestServicesMemoryFileError(this,
			QStringLiteral("读取 ArcGIS REST 服务记忆文件失败。"),
			GetDefaultArcGISRestServicesMemoryFilePath());
	}

	connect(treeWidget, &QTreeWidget::currentItemChanged, this, &QServiceBrowserPanel::OnCurrentItemChanged);
	connect(treeWidget, &QTreeWidget::itemActivated, this, &QServiceBrowserPanel::OnItemActivated);
	connect(treeWidget, &QTreeWidget::itemDoubleClicked, this, &QServiceBrowserPanel::OnItemDoubleClicked);
	connect(treeWidget, &QTreeWidget::itemExpanded, this, &QServiceBrowserPanel::OnItemExpanded);
	connect(treeWidget, &QTreeWidget::customContextMenuRequested, this, &QServiceBrowserPanel::OnCustomContextMenuRequested);
}

QServiceBrowserPanel::~QServiceBrowserPanel()
{
	arcGISRestConnectionLoadTokenByUid.clear();
	arcGISRestConnectionSettingsByUid.clear();
	arcGISRestServiceInfoByUid.clear();
	itemByUid.clear();
}

QString QServiceBrowserPanel::GetArcGISRestCategoryUid()
{
	return QString::fromLatin1(ArcGISRestCategoryUidText);
}

QString QServiceBrowserPanel::GetDefaultArcGISRestServicesMemoryFilePath()
{
	QString localAppDataPath = qEnvironmentVariable("LOCALAPPDATA").trimmed();
	if (localAppDataPath.isEmpty())
	{
		const QString userProfilePath = qEnvironmentVariable("USERPROFILE").trimmed();
		if (!userProfilePath.isEmpty())
		{
			localAppDataPath = userProfilePath + QStringLiteral("/AppData/Local");
		}
	}
	if (localAppDataPath.isEmpty())
	{
		localAppDataPath = QDir::homePath() + QStringLiteral("/AppData/Local");
	}

	QString filePath = localAppDataPath;
	while (filePath.endsWith(QLatin1Char('/')) || filePath.endsWith(QLatin1Char('\\')))
	{
		filePath.chop(1);
	}
	filePath += QStringLiteral("/MapWeaver/") + QString::fromLatin1(ArcGISRestServicesMemoryFileName);
	return NormalizeFilePathText(filePath);
}

Qt::DockWidgetArea QServiceBrowserPanel::GetDefaultDockWidgetArea()
{
	return Qt::LeftDockWidgetArea;
}

QTreeWidget* QServiceBrowserPanel::GetTreeWidget() const
{
	return treeWidget;
}

QTextEdit* QServiceBrowserPanel::GetDetailsTextEdit() const
{
	return detailsTextEdit;
}

bool QServiceBrowserPanel::HasArcGISRestConnectionDisplayName(const QString& displayName, const QString& ignoredUid) const
{
	const QString normalizedDisplayName = displayName.trimmed();
	if (normalizedDisplayName.isEmpty() || !arcGISRestCategoryItem)
	{
		return false;
	}

	const QString normalizedIgnoredUid = ignoredUid.trimmed();
	for (int childIndex = 0; childIndex < arcGISRestCategoryItem->childCount(); childIndex++)
	{
		const QTreeWidgetItem* childItem = arcGISRestCategoryItem->child(childIndex);
		if (!childItem || !IsArcGISRestConnectionRootItem(childItem))
		{
			continue;
		}

		const QString childUid = childItem->data(0, RoleUid).toString().trimmed();
		if (!normalizedIgnoredUid.isEmpty() && childUid == normalizedIgnoredUid)
		{
			continue;
		}

		const QString childDisplayName = childItem->data(0, RoleText).toString().trimmed();
		if (QString::compare(childDisplayName, normalizedDisplayName, Qt::CaseInsensitive) == 0)
		{
			return true;
		}
	}

	return false;
}

void QServiceBrowserPanel::ClearArcGISRestServices()
{
	EnsureArcGISRestCategoryItem();
	RemoveChildrenAndUnregister(arcGISRestCategoryItem);
	arcGISRestConnectionLoadTokenByUid.clear();
	arcGISRestConnectionSettingsByUid.clear();
	arcGISRestServiceInfoByUid.clear();
	arcGISRestCategoryItem->setExpanded(true);
	UpdateLoadingAnimationTimerState();
	UpdateDetailsForItem(treeWidget ? treeWidget->currentItem() : nullptr);
}

void QServiceBrowserPanel::SetArcGISRestRootChildren(const std::vector<ArcGISRestServiceTreeNode>& nodes)
{
	EnsureArcGISRestCategoryItem();
	RemoveChildrenAndUnregister(arcGISRestCategoryItem);
	arcGISRestConnectionLoadTokenByUid.clear();
	arcGISRestConnectionSettingsByUid.clear();
	arcGISRestServiceInfoByUid.clear();

	if (treeWidget)
	{
		treeWidget->setUpdatesEnabled(false);
	}

	for (const ArcGISRestServiceTreeNode& node : nodes)
	{
		AddArcGISRestServiceNodeInternal(node, arcGISRestCategoryItem, true);
	}

	arcGISRestCategoryItem->setExpanded(true);

	if (treeWidget)
	{
		treeWidget->setUpdatesEnabled(true);
	}

	UpdateLoadingAnimationTimerState();
	UpdateDetailsForItem(treeWidget ? treeWidget->currentItem() : nullptr);
}

bool QServiceBrowserPanel::AddArcGISRestServiceNode(const ArcGISRestServiceTreeNode& node, bool recursive)
{
	EnsureArcGISRestCategoryItem();
	return AddArcGISRestServiceNodeInternal(node, nullptr, recursive);
}

bool QServiceBrowserPanel::UpdateArcGISRestServiceNode(const ArcGISRestServiceTreeNode& node, bool recursive)
{
	EnsureArcGISRestCategoryItem();
	return UpdateArcGISRestServiceNodeInternal(node, recursive);
}

bool QServiceBrowserPanel::RemoveNode(const QString& uid)
{
	if (uid.isEmpty() || uid == GetArcGISRestCategoryUid())
	{
		return false;
	}

	QTreeWidgetItem* item = FindItemByUid(uid);
	if (!item)
	{
		return false;
	}

	QTreeWidgetItem* const parentItem = item->parent();
	CancelArcGISRestConnectionLoadsRecursively(item);
	RemoveArcGISRestConnectionSettingsRecursively(item);
	UnregisterItemRecursively(item);

	if (parentItem)
	{
		const int childIndex = parentItem->indexOfChild(item);
		if (childIndex >= 0)
		{
			delete parentItem->takeChild(childIndex);
		}
	}
	else if (treeWidget)
	{
		const int topLevelIndex = treeWidget->indexOfTopLevelItem(item);
		if (topLevelIndex >= 0)
		{
			delete treeWidget->takeTopLevelItem(topLevelIndex);
		}
	}

	UpdateLoadingAnimationTimerState();
	UpdateDetailsForItem(treeWidget ? treeWidget->currentItem() : nullptr);
	return true;
}

bool QServiceBrowserPanel::SetNodeDraggable(const QString& uid, bool enabled)
{
	QTreeWidgetItem* item = FindItemByUid(uid);
	if (!item || IsPlaceholderItem(item))
	{
		return false;
	}

	item->setData(0, RoleCanDrag, enabled);
	ApplyDragFlag(item, enabled);
	UpdateDetailsForItem(treeWidget ? treeWidget->currentItem() : nullptr);
	return true;
}

bool QServiceBrowserPanel::SetNodeContextMenuEnabled(const QString& uid, bool enabled)
{
	QTreeWidgetItem* item = FindItemByUid(uid);
	if (!item || IsPlaceholderItem(item))
	{
		return false;
	}

	item->setData(0, RoleCanShowContextMenu, enabled);
	UpdateDetailsForItem(treeWidget ? treeWidget->currentItem() : nullptr);
	return true;
}

bool QServiceBrowserPanel::SetNodeDetailText(const QString& uid, const QString& detailText)
{
	QTreeWidgetItem* item = FindItemByUid(uid);
	if (!item || IsPlaceholderItem(item))
	{
		return false;
	}

	item->setData(0, RoleDetails, detailText);
	UpdateDetailsForItem(treeWidget ? treeWidget->currentItem() : nullptr);
	return true;
}

bool QServiceBrowserPanel::SelectNode(const QString& uid)
{
	QTreeWidgetItem* item = FindItemByUid(uid);
	if (!item || !treeWidget)
	{
		return false;
	}

	treeWidget->setCurrentItem(item);
	treeWidget->scrollToItem(item);
	return true;
}

bool QServiceBrowserPanel::ExpandNodeByUid(const QString& uid)
{
	QTreeWidgetItem* item = FindItemByUid(uid);
	if (!item)
	{
		return false;
	}

	item->setExpanded(true);
	return true;
}

bool QServiceBrowserPanel::GetSelectedNodeInfo(ServiceBrowserNodeInfo& outNodeInfo) const
{
	if (!treeWidget || !treeWidget->currentItem())
	{
		outNodeInfo = ServiceBrowserNodeInfo();
		return false;
	}

	outNodeInfo = GetNodeInfo(treeWidget->currentItem());
	return !outNodeInfo.uid.isEmpty();
}

bool QServiceBrowserPanel::BindMainCanvas(QMainCanvas* mainCanvas)
{
	if (!mainCanvas)
	{
		return false;
	}

	const QMetaObject::Connection connection = connect(mainCanvas,
		&QMainCanvas::LayerDropRequested,
		this,
		&QServiceBrowserPanel::HandleCanvasLayerDropRequested,
		Qt::UniqueConnection);
	return static_cast<bool>(connection);
}

bool QServiceBrowserPanel::ImportArcGISRestNodeByUid(const QString& uid)
{
	QTreeWidgetItem* item = FindItemByUid(uid);
	if (!item || IsPlaceholderItem(item))
	{
		return false;
	}

	return EmitArcGISRestLayerImportRequest(item);
}

void QServiceBrowserPanel::SetDefaultContextMenuEnabled(bool enabled)
{
	defaultContextMenuEnabled = enabled;
}

bool QServiceBrowserPanel::IsDefaultContextMenuEnabled() const
{
	return defaultContextMenuEnabled;
}

void QServiceBrowserPanel::SetContextMenuBuilder(const ContextMenuBuilder& builder)
{
	contextMenuBuilder = builder;
}

void QServiceBrowserPanel::SetShowLazyExpandableIndicators(bool enabled)
{
	showLazyExpandableIndicators = enabled;
}

bool QServiceBrowserPanel::IsShowLazyExpandableIndicatorsEnabled() const
{
	return showLazyExpandableIndicators;
}

void QServiceBrowserPanel::OnCurrentItemChanged(QTreeWidgetItem* currentItem, QTreeWidgetItem* previousItem)
{
	Q_UNUSED(previousItem);
	UpdateDetailsForItem(currentItem);

	if (!currentItem || IsPlaceholderItem(currentItem))
	{
		emit CurrentNodeChanged(QString(), QString(), QString(), 0);
		return;
	}

	const ServiceBrowserNodeInfo nodeInfo = GetNodeInfo(currentItem);
	emit CurrentNodeChanged(nodeInfo.uid, nodeInfo.url, nodeInfo.text, nodeInfo.nodeType);
}

void QServiceBrowserPanel::OnItemActivated(QTreeWidgetItem* item, int column)
{
	Q_UNUSED(column);
	EmitNodeSignal(&QServiceBrowserPanel::NodeActivated, item);
}

void QServiceBrowserPanel::OnItemDoubleClicked(QTreeWidgetItem* item, int column)
{
	Q_UNUSED(column);
	EmitNodeSignal(&QServiceBrowserPanel::NodeDoubleClicked, item);
}

void QServiceBrowserPanel::OnItemExpanded(QTreeWidgetItem* item)
{
	if (!item || IsPlaceholderItem(item) || item->data(0, RoleIsLoading).toBool() || !item->data(0, RoleHasLazyChildren).toBool())
	{
		return;
	}

	const ServiceBrowserNodeInfo nodeInfo = GetNodeInfo(item);
	const ArcGISRestServiceTreeNode::NodeType nodeType = static_cast<ArcGISRestServiceTreeNode::NodeType>(nodeInfo.nodeType);
	if (!nodeInfo.isArcGISRestNode || !IsNetworkExpandableArcGISRestNodeType(nodeType) || nodeInfo.url.isEmpty())
	{
		if (item->childCount() == 1 && IsPlaceholderItem(item->child(0)))
		{
			delete item->takeChild(0);
		}
		item->setData(0, RoleHasLazyChildren, false);
		UpdateDetailsForItem(treeWidget ? treeWidget->currentItem() : nullptr);
		return;
	}

	ArcGISRestServiceTreeNode sourceNode;
	sourceNode.type = nodeType;
	sourceNode.text = ToStdString(nodeInfo.text);
	sourceNode.url = ToStdString(nodeInfo.url);
	sourceNode.uid = ToStdString(nodeInfo.uid);

	QTreeWidgetItem* const parentItem = item->parent();
	if (parentItem && !IsPlaceholderItem(parentItem))
	{
		sourceNode.parentUid = ToStdString(parentItem->data(0, RoleUid).toString());
	}

	emit ArcGISRestNodeExpandRequested(nodeInfo.uid, nodeInfo.url, nodeInfo.text, nodeInfo.nodeType);

	ArcGISRestConnectionSettings settings;
	GetArcGISRestConnectionSettingsForNodeItem(item, settings);
	settings = NormalizeArcGISRestConnectionSettings(settings);
	settings.serviceUrl = NormalizeArcGISRestRequestUrl(sourceNode.url);

	SetItemLoadingState(item, true);
	ArcGISRestNodeExpandThread* expandThread = new ArcGISRestNodeExpandThread(QPointer<QServiceBrowserPanel>(this), sourceNode, settings);
	connect(expandThread, &QThread::finished, expandThread, &QObject::deleteLater);
	expandThread->start();
}

void QServiceBrowserPanel::OnCustomContextMenuRequested(const QPoint& position)
{
	if (!treeWidget)
	{
		return;
	}

	QTreeWidgetItem* item = treeWidget->itemAt(position);
	if (!item || IsPlaceholderItem(item))
	{
		return;
	}

	const ServiceBrowserNodeInfo nodeInfo = GetNodeInfo(item);
	if (!nodeInfo.canShowContextMenu)
	{
		return;
	}

	QMenu menu(this);
	const bool isArcGISRestCategoryNode = IsArcGISRestCategoryNode(nodeInfo);
	if (isArcGISRestCategoryNode)
	{
		if (defaultContextMenuEnabled)
		{
			AddDefaultContextMenuActions(&menu, nodeInfo, item);
		}
	}
	else if (IsArcGISRestConnectionRootItem(item))
	{
		AddArcGISRestConnectionContextMenuActions(&menu, nodeInfo, item);
	}
	else if (nodeInfo.isArcGISRestNode && FindArcGISRestConnectionRootItem(item))
	{
		AddArcGISRestChildNodeContextMenuActions(&menu, nodeInfo, item);
	}
	else
	{
		if (defaultContextMenuEnabled)
		{
			AddDefaultContextMenuActions(&menu, nodeInfo, item);
		}

		if (contextMenuBuilder)
		{
			if (!menu.actions().isEmpty())
			{
				menu.addSeparator();
			}
			contextMenuBuilder(&menu, nodeInfo);
		}
	}

	if (!menu.actions().isEmpty())
	{
		menu.exec(treeWidget->viewport()->mapToGlobal(position));
	}
}

void QServiceBrowserPanel::OnNewArcGISRestConnectionRequested()
{
	QArcGISRestConnectionDialog dialog(this);
	dialog.SetConnectionNameExistsChecker([this](const QString& connectionName) -> bool
		{
			return HasArcGISRestConnectionDisplayName(connectionName);
		});
	if (dialog.exec() != QDialog::Accepted)
	{
		return;
	}

	ArcGISRestConnectionSettings settings = NormalizeArcGISRestConnectionSettings(dialog.GetSettings());
	if (settings.displayName.empty())
	{
		settings.displayName = ToStdString(GuessArcGISRestDisplayName(settings));
	}

	const std::string serviceBaseUrl = NormalizeArcGISRestServiceUrl(settings.serviceUrl);
	if (settings.serviceUrl.empty() || serviceBaseUrl.empty())
	{
		ShowArcGISRestConnectionError(this, QStringLiteral("服务 URL 不能为空。"), QString());
		return;
	}

	EnsureArcGISRestCategoryItem();

	ArcGISRestServiceTreeNode connectionNode = CreateArcGISRestConnectionNodeFromSettings(settings, serviceBaseUrl);
	const QString provisionalUid = ToQString(connectionNode.uid);
	if (!AddArcGISRestServiceNode(connectionNode, false))
	{
		ShowArcGISRestConnectionError(this, QStringLiteral("无法在服务浏览树中创建 ArcGIS REST 服务连接节点。"), QString());
		return;
	}

	SetArcGISRestConnectionSettings(provisionalUid, settings);
	if (!SaveArcGISRestServicesToMemoryFile())
	{
		ShowArcGISRestServicesMemoryFileError(this,
			QStringLiteral("保存 ArcGIS REST 服务记忆文件失败。"),
			GetDefaultArcGISRestServicesMemoryFilePath());
	}

	QTreeWidgetItem* connectionItem = FindItemByUid(provisionalUid);
	SetItemLoadingState(connectionItem, true);
	SelectNode(provisionalUid);
	ExpandNodeByUid(provisionalUid);

	StartArcGISRestConnectionLoad(provisionalUid, settings, connectionNode, ArcGISRestConnectionLoadMode::AddNew);
}

void QServiceBrowserPanel::OnLoadingAnimationTimerTimeout()
{
	UpdateLoadingAnimationFrame();
	UpdateLoadingAnimationTimerState();
}

void QServiceBrowserPanel::HandleCanvasLayerDropRequested(const QString& nodeUid, const QString& url, const QString& text, int nodeType)
{
	Q_UNUSED(url);
	Q_UNUSED(text);
	Q_UNUSED(nodeType);

	ImportArcGISRestNodeByUid(nodeUid);
}

QTreeWidgetItem* QServiceBrowserPanel::CreateArcGISRestCategoryItem() const
{
	QTreeWidgetItem* item = new QTreeWidgetItem();
	item->setText(0, QStringLiteral("ArcGIS Rest 服务"));
	item->setIcon(0, QIcon(":/resources/Resources/ArcGIS_Rest_Service_128.ico"));
	item->setData(0, RoleUid, GetArcGISRestCategoryUid());
	item->setData(0, RoleUrl, QString());
	item->setData(0, RoleText, QStringLiteral("ArcGIS Rest 服务"));
	item->setData(0, RoleNodeType, static_cast<int>(ArcGISRestServiceTreeNode::NodeType::Root));
	item->setData(0, RoleCanDrag, false);
	item->setData(0, RoleCanShowContextMenu, true);
	item->setData(0, RoleDetails, QStringLiteral("ArcGIS Rest 服务根分类。"));
	item->setData(0, RoleIsArcGISRestNode, false);
	item->setData(0, RoleIsCategoryNode, true);
	item->setData(0, RoleHasLazyChildren, false);
	item->setData(0, RoleIsLazyPlaceholder, false);
	item->setData(0, RoleIsLoading, false);
	item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicatorWhenChildless);
	ApplyStandardItemFlags(item, false);
	return item;
}

QTreeWidgetItem* QServiceBrowserPanel::CreateTreeItemFromArcGISRestNode(const ArcGISRestServiceTreeNode& node, bool recursive)
{
	const QString uid = NormalizeUid(node);
	if (uid.isEmpty())
	{
		return nullptr;
	}

	const bool canDrag = CanDragArcGISRestNodeByDefault(node.type);
	const bool hasLazyChildren = ShouldShowLazyExpandableIndicator(node, showLazyExpandableIndicators);

	QTreeWidgetItem* item = new QTreeWidgetItem();
	item->setText(0, ToQString(node.text));
	item->setToolTip(0, ToQString(node.url));
	item->setIcon(0, IconForArcGISRestNodeType(node.type));
	item->setData(0, RoleUid, uid);
	item->setData(0, RoleUrl, ToQString(node.url));
	item->setData(0, RoleText, ToQString(node.text));
	item->setData(0, RoleNodeType, static_cast<int>(node.type));
	item->setData(0, RoleCanDrag, canDrag);
	item->setData(0, RoleCanShowContextMenu, true);
	item->setData(0, RoleDetails, CreateDetailTextForArcGISRestNode(node, canDrag, hasLazyChildren));
	item->setData(0, RoleIsArcGISRestNode, true);
	item->setData(0, RoleIsCategoryNode, false);
	item->setData(0, RoleHasLazyChildren, hasLazyChildren);
	item->setData(0, RoleIsLazyPlaceholder, false);
	item->setData(0, RoleIsLoading, false);
	item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicatorWhenChildless);
	ApplyStandardItemFlags(item, canDrag);

	if (recursive)
	{
		for (const ArcGISRestServiceTreeNode& childNode : node.children)
		{
			QTreeWidgetItem* childItem = CreateTreeItemFromArcGISRestNode(childNode, true);
			if (childItem)
			{
				item->addChild(childItem);
			}
		}
	}

	if (hasLazyChildren && item->childCount() == 0)
	{
		item->addChild(CreateLazyPlaceholderItem());
	}

	return item;
}

QTreeWidgetItem* QServiceBrowserPanel::CreateLazyPlaceholderItem() const
{
	QTreeWidgetItem* item = new QTreeWidgetItem();
	item->setText(0, QStringLiteral("正在等待展开..."));
	item->setData(0, RoleUid, QString());
	item->setData(0, RoleCanDrag, false);
	item->setData(0, RoleCanShowContextMenu, false);
	item->setData(0, RoleIsLazyPlaceholder, true);
	item->setData(0, RoleIsLoading, false);
	item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
	item->setFlags(Qt::ItemIsEnabled);
	return item;
}

QTreeWidgetItem* QServiceBrowserPanel::CreateLoadingPlaceholderItem() const
{
	QTreeWidgetItem* item = new QTreeWidgetItem();
	item->setText(0, QStringLiteral("正在加载服务信息..."));
	item->setIcon(0, CreateLoadingIcon(loadingAnimationFrame));
	item->setData(0, RoleUid, QString());
	item->setData(0, RoleCanDrag, false);
	item->setData(0, RoleCanShowContextMenu, false);
	item->setData(0, RoleIsLazyPlaceholder, true);
	item->setData(0, RoleIsLoading, true);
	item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
	item->setFlags(Qt::ItemIsEnabled);
	return item;
}

bool QServiceBrowserPanel::AddArcGISRestServiceNodeInternal(const ArcGISRestServiceTreeNode& node, QTreeWidgetItem* explicitParentItem, bool recursive)
{
	if (!treeWidget)
	{
		return false;
	}

	const QString uid = NormalizeUid(node);
	if (uid.isEmpty())
	{
		return false;
	}

	RemoveNode(uid);

	QTreeWidgetItem* parentItem = explicitParentItem;
	if (!parentItem && !node.parentUid.empty())
	{
		parentItem = FindItemByUid(ToQString(node.parentUid));
		if (!parentItem)
		{
			return false;
		}
	}
	if (!parentItem)
	{
		parentItem = arcGISRestCategoryItem;
	}
	if (!parentItem)
	{
		return false;
	}

	QTreeWidgetItem* item = CreateTreeItemFromArcGISRestNode(node, recursive);
	if (!item)
	{
		return false;
	}

	parentItem->addChild(item);
	RegisterItemRecursively(item);
	RegisterArcGISRestServiceInfoRecursively(node, recursive);
	parentItem->setExpanded(true);
	UpdateLoadingAnimationTimerState();
	return true;
}

bool QServiceBrowserPanel::UpdateArcGISRestServiceNodeInternal(const ArcGISRestServiceTreeNode& node, bool recursive)
{
	const QString uid = NormalizeUid(node);
	if (uid.isEmpty())
	{
		return false;
	}

	QTreeWidgetItem* oldItem = FindItemByUid(uid);
	if (!oldItem)
	{
		return AddArcGISRestServiceNodeInternal(node, nullptr, recursive);
	}

	QTreeWidgetItem* parentItem = oldItem->parent();
	if (!parentItem)
	{
		return false;
	}

	const bool wasExpanded = oldItem->isExpanded();
	const bool wasCurrent = treeWidget && treeWidget->currentItem() == oldItem;
	const int childIndex = parentItem->indexOfChild(oldItem);
	if (childIndex < 0)
	{
		return false;
	}

	QTreeWidgetItem* newItem = CreateTreeItemFromArcGISRestNode(node, recursive);
	if (!newItem)
	{
		return false;
	}

	UnregisterItemRecursively(oldItem);
	delete parentItem->takeChild(childIndex);
	parentItem->insertChild(childIndex, newItem);
	RegisterItemRecursively(newItem);
	RegisterArcGISRestServiceInfoRecursively(node, recursive);
	newItem->setExpanded(wasExpanded);
	if (wasCurrent && treeWidget)
	{
		treeWidget->setCurrentItem(newItem);
	}
	UpdateLoadingAnimationTimerState();
	UpdateDetailsForItem(treeWidget ? treeWidget->currentItem() : nullptr);
	return true;
}

QTreeWidgetItem* QServiceBrowserPanel::FindItemByUid(const QString& uid) const
{
	if (uid.isEmpty())
	{
		return nullptr;
	}

	return itemByUid.value(uid, nullptr);
}

QString QServiceBrowserPanel::GetCurrentSelectableNodeUid() const
{
	if (!treeWidget)
	{
		return QString();
	}

	QTreeWidgetItem* currentItem = treeWidget->currentItem();
	if (!currentItem || IsPlaceholderItem(currentItem))
	{
		return QString();
	}

	return currentItem->data(0, RoleUid).toString();
}

void QServiceBrowserPanel::RestoreCurrentSelectableNodeByUid(const QString& uid, const QString& fallbackUid)
{
	if (!treeWidget)
	{
		return;
	}

	QTreeWidgetItem* targetItem = nullptr;
	if (!uid.isEmpty())
	{
		targetItem = FindItemByUid(uid);
	}

	if ((!targetItem || IsPlaceholderItem(targetItem)) && !fallbackUid.isEmpty())
	{
		targetItem = FindItemByUid(fallbackUid);
	}

	if (!targetItem || IsPlaceholderItem(targetItem))
	{
		UpdateDetailsForItem(treeWidget->currentItem());
		return;
	}

	if (treeWidget->currentItem() != targetItem)
	{
		treeWidget->setCurrentItem(targetItem, 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Current);
	}

	UpdateDetailsForItem(targetItem);
}

void QServiceBrowserPanel::RegisterItemRecursively(QTreeWidgetItem* item)
{
	if (!item)
	{
		return;
	}

	const QString uid = item->data(0, RoleUid).toString();
	if (!uid.isEmpty() && !IsPlaceholderItem(item))
	{
		itemByUid.insert(uid, item);
	}

	for (int childIndex = 0; childIndex < item->childCount(); childIndex++)
	{
		RegisterItemRecursively(item->child(childIndex));
	}
}

void QServiceBrowserPanel::RegisterArcGISRestServiceInfoRecursively(const ArcGISRestServiceTreeNode& node, bool recursive)
{
	const QString uid = NormalizeUid(node);
	if (!uid.isEmpty() && node.serviceInfo.resourceType != ArcGISRestResourceType::Unknown)
	{
		arcGISRestServiceInfoByUid.insert(uid, std::make_shared<ArcGISRestServiceInfo>(node.serviceInfo));
	}

	if (!recursive)
	{
		return;
	}

	for (const ArcGISRestServiceTreeNode& childNode : node.children)
	{
		RegisterArcGISRestServiceInfoRecursively(childNode, true);
	}
}

void QServiceBrowserPanel::UnregisterItemRecursively(QTreeWidgetItem* item)
{
	if (!item)
	{
		return;
	}

	const QString uid = item->data(0, RoleUid).toString();
	if (!uid.isEmpty())
	{
		itemByUid.remove(uid);
		arcGISRestServiceInfoByUid.remove(uid);
	}

	for (int childIndex = 0; childIndex < item->childCount(); childIndex++)
	{
		UnregisterItemRecursively(item->child(childIndex));
	}
}

void QServiceBrowserPanel::RemoveChildrenAndUnregister(QTreeWidgetItem* item)
{
	if (!item)
	{
		return;
	}

	QList<QTreeWidgetItem*> children = item->takeChildren();
	for (QTreeWidgetItem* child : children)
	{
		CancelArcGISRestConnectionLoadsRecursively(child);
		RemoveArcGISRestConnectionSettingsRecursively(child);
		UnregisterItemRecursively(child);
		delete child;
	}
}

void QServiceBrowserPanel::SetItemLoadingState(QTreeWidgetItem* item, bool enabled)
{
	if (!item || IsPlaceholderItem(item))
	{
		return;
	}

	if (enabled)
	{
		RemoveChildrenAndUnregister(item);
		item->setIcon(0, CreateLoadingIcon(loadingAnimationFrame));
		item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicatorWhenChildless);
		item->setData(0, RoleCanDrag, false);
		item->setData(0, RoleCanShowContextMenu, false);
		item->setData(0, RoleHasLazyChildren, false);
		item->setData(0, RoleIsLoading, true);
		item->setData(0, RoleDetails, CreateDetailTextFromNodeInfo(GetNodeInfo(item)) + QStringLiteral("\n\n状态：正在请求网络并解析服务树。"));
		ApplyDragFlag(item, false);
		item->addChild(CreateLoadingPlaceholderItem());
		item->setExpanded(true);
	}
	else
	{
		item->setData(0, RoleIsLoading, false);
	}

	UpdateLoadingAnimationTimerState();
	UpdateDetailsForItem(treeWidget ? treeWidget->currentItem() : nullptr);
}

void QServiceBrowserPanel::RestoreArcGISRestNodeLazyExpansionState(const ArcGISRestServiceTreeNode& node, const QString& statusText)
{
	QTreeWidgetItem* item = FindItemByUid(NormalizeUid(node));
	if (!item || IsPlaceholderItem(item))
	{
		return;
	}

	const bool canDrag = CanDragArcGISRestNodeByDefault(node.type);
	const bool hasLazyChildren = ShouldShowLazyExpandableIndicator(node, showLazyExpandableIndicators);

	RemoveChildrenAndUnregister(item);
	item->setText(0, ToQString(node.text));
	item->setToolTip(0, ToQString(node.url));
	item->setIcon(0, IconForArcGISRestNodeType(node.type));
	item->setData(0, RoleUrl, ToQString(node.url));
	item->setData(0, RoleText, ToQString(node.text));
	item->setData(0, RoleNodeType, static_cast<int>(node.type));
	item->setData(0, RoleCanDrag, canDrag);
	item->setData(0, RoleCanShowContextMenu, true);
	item->setData(0, RoleHasLazyChildren, hasLazyChildren);
	item->setData(0, RoleIsLoading, false);
	item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicatorWhenChildless);

	QString detailText = CreateDetailTextForArcGISRestNode(node, canDrag, hasLazyChildren);
	if (!statusText.trimmed().isEmpty())
	{
		detailText += QStringLiteral("\n\n") + statusText.trimmed();
	}
	item->setData(0, RoleDetails, detailText);
	ApplyStandardItemFlags(item, canDrag);

	if (hasLazyChildren)
	{
		item->addChild(CreateLazyPlaceholderItem());
	}

	item->setExpanded(false);
	UpdateLoadingAnimationTimerState();
	UpdateDetailsForItem(treeWidget ? treeWidget->currentItem() : nullptr);
}

bool QServiceBrowserPanel::HasLoadingItems() const
{
	if (!treeWidget)
	{
		return false;
	}

	for (int itemIndex = 0; itemIndex < treeWidget->topLevelItemCount(); itemIndex++)
	{
		if (HasLoadingItemsRecursively(treeWidget->topLevelItem(itemIndex)))
		{
			return true;
		}
	}
	return false;
}

bool QServiceBrowserPanel::HasLoadingItemsRecursively(const QTreeWidgetItem* item) const
{
	if (!item)
	{
		return false;
	}

	if (item->data(0, RoleIsLoading).toBool())
	{
		return true;
	}

	for (int childIndex = 0; childIndex < item->childCount(); childIndex++)
	{
		if (HasLoadingItemsRecursively(item->child(childIndex)))
		{
			return true;
		}
	}
	return false;
}

void QServiceBrowserPanel::UpdateLoadingAnimationTimerState()
{
	if (!loadingAnimationTimer)
	{
		return;
	}

	if (HasLoadingItems())
	{
		if (!loadingAnimationTimer->isActive())
		{
			loadingAnimationTimer->start();
		}
	}
	else
	{
		if (loadingAnimationTimer->isActive())
		{
			loadingAnimationTimer->stop();
		}
	}
}

void QServiceBrowserPanel::UpdateLoadingAnimationFrame()
{
	if (!treeWidget)
	{
		return;
	}

	loadingAnimationFrame = (loadingAnimationFrame + 1) % 12;
	const QIcon loadingIcon = CreateLoadingIcon(loadingAnimationFrame);
	for (int itemIndex = 0; itemIndex < treeWidget->topLevelItemCount(); itemIndex++)
	{
		UpdateLoadingAnimationFrameRecursively(treeWidget->topLevelItem(itemIndex), loadingIcon);
	}
}

void QServiceBrowserPanel::UpdateLoadingAnimationFrameRecursively(QTreeWidgetItem* item, const QIcon& loadingIcon)
{
	if (!item)
	{
		return;
	}

	if (item->data(0, RoleIsLoading).toBool())
	{
		item->setIcon(0, loadingIcon);
	}

	for (int childIndex = 0; childIndex < item->childCount(); childIndex++)
	{
		UpdateLoadingAnimationFrameRecursively(item->child(childIndex), loadingIcon);
	}
}

QIcon QServiceBrowserPanel::CreateLoadingIcon(int frameIndex) const
{
	const int iconSize = 16;
	QPixmap pixmap(iconSize, iconSize);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing, true);
	QPen pen(QColor(90, 90, 90), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	painter.setPen(pen);

	const QRectF arcRect(3.0, 3.0, 10.0, 10.0);
	const int startAngle = -frameIndex * 30 * 16;
	const int spanAngle = 270 * 16;
	painter.drawArc(arcRect, startAngle, spanAngle);
	return QIcon(pixmap);
}

ServiceBrowserNodeInfo QServiceBrowserPanel::GetNodeInfo(QTreeWidgetItem* item) const
{
	ServiceBrowserNodeInfo nodeInfo;
	if (!item || IsPlaceholderItem(item))
	{
		return nodeInfo;
	}

	nodeInfo.uid = item->data(0, RoleUid).toString();
	nodeInfo.text = item->data(0, RoleText).toString();
	if (nodeInfo.text.isEmpty())
	{
		nodeInfo.text = item->text(0);
	}
	nodeInfo.url = item->data(0, RoleUrl).toString();
	nodeInfo.detailText = item->data(0, RoleDetails).toString();
	nodeInfo.nodeType = item->data(0, RoleNodeType).toInt();
	nodeInfo.canDrag = item->data(0, RoleCanDrag).toBool();
	nodeInfo.canShowContextMenu = item->data(0, RoleCanShowContextMenu).toBool();
	nodeInfo.isArcGISRestNode = item->data(0, RoleIsArcGISRestNode).toBool();
	nodeInfo.isCategoryNode = item->data(0, RoleIsCategoryNode).toBool();
	nodeInfo.hasLazyChildren = item->data(0, RoleHasLazyChildren).toBool();
	return nodeInfo;
}

void QServiceBrowserPanel::UpdateDetailsForItem(QTreeWidgetItem* item)
{
	if (!detailsTextEdit)
	{
		return;
	}

	if (!item || IsPlaceholderItem(item))
	{
		detailsTextEdit->setPlainText(QStringLiteral("未选择节点。"));
		return;
	}

	const ServiceBrowserNodeInfo nodeInfo = GetNodeInfo(item);
	QString detailText = nodeInfo.detailText;
	if (detailText.isEmpty())
	{
		detailText = CreateDetailTextFromNodeInfo(nodeInfo);
	}

	detailsTextEdit->setPlainText(detailText);
}

void QServiceBrowserPanel::EmitNodeSignal(void (QServiceBrowserPanel::* signalFunc)(const QString&, const QString&, const QString&, int), QTreeWidgetItem* item)
{
	if (!item || IsPlaceholderItem(item))
	{
		return;
	}

	const ServiceBrowserNodeInfo nodeInfo = GetNodeInfo(item);
	(this->*signalFunc)(nodeInfo.uid, nodeInfo.url, nodeInfo.text, nodeInfo.nodeType);
}

void QServiceBrowserPanel::ApplyDragFlag(QTreeWidgetItem* item, bool enabled) const
{
	if (!item)
	{
		return;
	}

	Qt::ItemFlags flags = item->flags();
	if (enabled)
	{
		flags |= Qt::ItemIsDragEnabled;
	}
	else
	{
		flags &= ~Qt::ItemIsDragEnabled;
	}
	item->setFlags(flags);
}

void QServiceBrowserPanel::ApplyStandardItemFlags(QTreeWidgetItem* item, bool canDrag) const
{
	if (!item)
	{
		return;
	}

	Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
	if (canDrag)
	{
		flags |= Qt::ItemIsDragEnabled;
	}
	item->setFlags(flags);
}

void QServiceBrowserPanel::AddDefaultContextMenuActions(QMenu* menu, const ServiceBrowserNodeInfo& nodeInfo, QTreeWidgetItem* item)
{
	if (!menu || !item)
	{
		return;
	}

	if (IsArcGISRestCategoryNode(nodeInfo))
	{
		QAction* newConnectionAction = menu->addAction(QStringLiteral("新建链接..."));
		connect(newConnectionAction, &QAction::triggered, this, &QServiceBrowserPanel::OnNewArcGISRestConnectionRequested);
		return;
	}

	if (item->childCount() > 0 || nodeInfo.hasLazyChildren)
	{
		QAction* expandAction = menu->addAction(QStringLiteral("展开"));
		expandAction->setEnabled(!item->isExpanded());
		connect(expandAction, &QAction::triggered, this, [item]() {
			item->setExpanded(true);
			});

		QAction* collapseAction = menu->addAction(QStringLiteral("折叠"));
		collapseAction->setEnabled(item->isExpanded());
		connect(collapseAction, &QAction::triggered, this, [item]() {
			item->setExpanded(false);
			});
	}

	if (nodeInfo.isArcGISRestNode && !nodeInfo.url.isEmpty())
	{
		if (!menu->actions().isEmpty())
		{
			menu->addSeparator();
		}
		QAction* refreshAction = menu->addAction(QStringLiteral("刷新"));
		connect(refreshAction, &QAction::triggered, this, [this, nodeInfo]() {
			emit NodeRefreshRequested(nodeInfo.uid, nodeInfo.url, nodeInfo.text, nodeInfo.nodeType);
			});
	}

	if (!menu->actions().isEmpty())
	{
		menu->addSeparator();
	}

	QAction* copyNameAction = menu->addAction(QStringLiteral("复制名称"));
	connect(copyNameAction, &QAction::triggered, this, [nodeInfo]() {
		QApplication::clipboard()->setText(nodeInfo.text);
		});

	QAction* copyUrlAction = menu->addAction(QStringLiteral("复制 URL"));
	copyUrlAction->setEnabled(!nodeInfo.url.isEmpty());
	connect(copyUrlAction, &QAction::triggered, this, [nodeInfo]() {
		QApplication::clipboard()->setText(nodeInfo.url);
		});

	QAction* copyUidAction = menu->addAction(QStringLiteral("复制 UID"));
	copyUidAction->setEnabled(!nodeInfo.uid.isEmpty());
	connect(copyUidAction, &QAction::triggered, this, [nodeInfo]() {
		QApplication::clipboard()->setText(nodeInfo.uid);
		});
}

void QServiceBrowserPanel::AddArcGISRestConnectionContextMenuActions(QMenu* menu, const ServiceBrowserNodeInfo& nodeInfo, QTreeWidgetItem* item)
{
	if (!menu || !item)
	{
		return;
	}

	const bool isLoading = item->data(0, RoleIsLoading).toBool();

	QAction* refreshAction = menu->addAction(QStringLiteral("刷新服务"));
	refreshAction->setEnabled(!isLoading);
	connect(refreshAction, &QAction::triggered, this, [this, item]() {
		RefreshArcGISRestConnection(item);
		});

	QAction* editAction = menu->addAction(QStringLiteral("编辑服务"));
	editAction->setEnabled(!isLoading);
	connect(editAction, &QAction::triggered, this, [this, item]() {
		EditArcGISRestConnection(item);
		});

	QAction* deleteAction = menu->addAction(QStringLiteral("删除服务"));
	deleteAction->setEnabled(!isLoading);
	connect(deleteAction, &QAction::triggered, this, [this, item]() {
		DeleteArcGISRestConnection(item);
		});

	Q_UNUSED(nodeInfo);
}

void QServiceBrowserPanel::AddArcGISRestChildNodeContextMenuActions(QMenu* menu, const ServiceBrowserNodeInfo& nodeInfo, QTreeWidgetItem* item)
{
	if (!menu || !item)
	{
		return;
	}

	const bool isLoading = item->data(0, RoleIsLoading).toBool();
	const ArcGISRestServiceTreeNode::NodeType nodeType = static_cast<ArcGISRestServiceTreeNode::NodeType>(nodeInfo.nodeType);
	const bool canRefreshService = nodeInfo.isArcGISRestNode && !nodeInfo.url.isEmpty() && IsNetworkExpandableArcGISRestNodeType(nodeType);
	if (canRefreshService)
	{
		QAction* const refreshAction = menu->addAction(QStringLiteral("刷新服务"));
		refreshAction->setEnabled(!isLoading);
		connect(refreshAction, &QAction::triggered, this, [this, item]() {
			RefreshArcGISRestChildNode(item);
			});
	}

	if (nodeInfo.canDrag)
	{
		if (!menu->actions().isEmpty())
		{
			menu->addSeparator();
		}

		QAction* const importAction = menu->addAction(QStringLiteral("导入"));
		importAction->setEnabled(!isLoading);
		connect(importAction, &QAction::triggered, this, [this, item]() {
			ImportArcGISRestNode(item);
			});
	}
}

bool QServiceBrowserPanel::IsArcGISRestCategoryNode(const ServiceBrowserNodeInfo& nodeInfo) const
{
	return nodeInfo.uid == GetArcGISRestCategoryUid();
}

bool QServiceBrowserPanel::IsArcGISRestConnectionRootItem(const QTreeWidgetItem* item) const
{
	if (!item || IsPlaceholderItem(item) || !arcGISRestCategoryItem)
	{
		return false;
	}

	return item->parent() == arcGISRestCategoryItem && item->data(0, RoleIsArcGISRestNode).toBool();
}

const QTreeWidgetItem* QServiceBrowserPanel::FindArcGISRestConnectionRootItem(const QTreeWidgetItem* item) const
{
	if (!item || IsPlaceholderItem(item) || !arcGISRestCategoryItem)
	{
		return nullptr;
	}

	const QTreeWidgetItem* currentItem = item;
	while (currentItem && currentItem->parent() && currentItem->parent() != arcGISRestCategoryItem)
	{
		currentItem = currentItem->parent();
	}

	if (!currentItem || !IsArcGISRestConnectionRootItem(currentItem))
	{
		return nullptr;
	}

	return currentItem;
}

bool QServiceBrowserPanel::GetArcGISRestConnectionSettingsForItem(const QTreeWidgetItem* item, ArcGISRestConnectionSettings& outSettings) const
{
	outSettings = ArcGISRestConnectionSettings();
	if (!item || !IsArcGISRestConnectionRootItem(item))
	{
		return false;
	}

	const QString uid = item->data(0, RoleUid).toString();
	const auto settingsIter = arcGISRestConnectionSettingsByUid.find(uid);
	if (settingsIter != arcGISRestConnectionSettingsByUid.end())
	{
		outSettings = settingsIter.value();
		outSettings.displayName = ToStdString(item->data(0, RoleText).toString());
		if (outSettings.serviceUrl.empty())
		{
			outSettings.serviceUrl = NormalizeArcGISRestRequestUrl(ToStdString(item->data(0, RoleUrl).toString()));
		}
		return true;
	}

	outSettings.displayName = ToStdString(item->data(0, RoleText).toString());
	outSettings.serviceUrl = NormalizeArcGISRestRequestUrl(ToStdString(item->data(0, RoleUrl).toString()));
	return !outSettings.serviceUrl.empty();
}

bool QServiceBrowserPanel::GetArcGISRestConnectionSettingsForNodeItem(const QTreeWidgetItem* item, ArcGISRestConnectionSettings& outSettings) const
{
	outSettings = ArcGISRestConnectionSettings();
	const QTreeWidgetItem* connectionRootItem = FindArcGISRestConnectionRootItem(item);
	if (!connectionRootItem)
	{
		return false;
	}

	return GetArcGISRestConnectionSettingsForItem(connectionRootItem, outSettings);
}

void QServiceBrowserPanel::SetArcGISRestConnectionSettings(const QString& uid, const ArcGISRestConnectionSettings& settings)
{
	if (uid.isEmpty())
	{
		return;
	}

	ArcGISRestConnectionSettings normalizedSettings = NormalizeArcGISRestConnectionSettings(settings);
	if (normalizedSettings.displayName.empty())
	{
		normalizedSettings.displayName = ToStdString(GuessArcGISRestDisplayName(normalizedSettings));
	}
	arcGISRestConnectionSettingsByUid.insert(uid, normalizedSettings);
}

void QServiceBrowserPanel::RemoveArcGISRestConnectionSettings(const QString& uid)
{
	if (uid.isEmpty())
	{
		return;
	}

	arcGISRestConnectionSettingsByUid.remove(uid);
}

void QServiceBrowserPanel::RemoveArcGISRestConnectionSettingsRecursively(const QTreeWidgetItem* item)
{
	if (!item)
	{
		return;
	}

	const QString uid = item->data(0, RoleUid).toString();
	RemoveArcGISRestConnectionSettings(uid);

	for (int childIndex = 0; childIndex < item->childCount(); childIndex++)
	{
		RemoveArcGISRestConnectionSettingsRecursively(item->child(childIndex));
	}
}

void QServiceBrowserPanel::MoveArcGISRestConnectionSettings(const QString& oldUid, const QString& newUid)
{
	if (oldUid.isEmpty() || newUid.isEmpty() || oldUid == newUid)
	{
		return;
	}

	const auto settingsIter = arcGISRestConnectionSettingsByUid.find(oldUid);
	if (settingsIter == arcGISRestConnectionSettingsByUid.end())
	{
		return;
	}

	arcGISRestConnectionSettingsByUid.insert(newUid, settingsIter.value());
	arcGISRestConnectionSettingsByUid.remove(oldUid);
}

quint64 QServiceBrowserPanel::BeginArcGISRestConnectionLoad(const QString& uid)
{
	if (uid.isEmpty())
	{
		return 0;
	}

	const quint64 loadToken = nextArcGISRestConnectionLoadToken++;
	if (nextArcGISRestConnectionLoadToken == 0)
	{
		nextArcGISRestConnectionLoadToken = 1;
	}

	arcGISRestConnectionLoadTokenByUid.insert(uid, loadToken);
	return loadToken;
}

bool QServiceBrowserPanel::IsArcGISRestConnectionLoadCurrent(const QString& uid, quint64 loadToken) const
{
	if (uid.isEmpty() || loadToken == 0)
	{
		return false;
	}

	const auto tokenIter = arcGISRestConnectionLoadTokenByUid.find(uid);
	return tokenIter != arcGISRestConnectionLoadTokenByUid.end() && tokenIter.value() == loadToken;
}

void QServiceBrowserPanel::EndArcGISRestConnectionLoad(const QString& uid, quint64 loadToken)
{
	if (IsArcGISRestConnectionLoadCurrent(uid, loadToken))
	{
		arcGISRestConnectionLoadTokenByUid.remove(uid);
	}
}

void QServiceBrowserPanel::CancelArcGISRestConnectionLoadsRecursively(const QTreeWidgetItem* item)
{
	if (!item)
	{
		return;
	}

	const QString uid = item->data(0, RoleUid).toString();
	if (!uid.isEmpty())
	{
		arcGISRestConnectionLoadTokenByUid.remove(uid);
	}

	for (int childIndex = 0; childIndex < item->childCount(); childIndex++)
	{
		CancelArcGISRestConnectionLoadsRecursively(item->child(childIndex));
	}
}

bool QServiceBrowserPanel::LoadArcGISRestServicesFromMemoryFile()
{
	EnsureArcGISRestCategoryItem();
	const QString filePath = GetDefaultArcGISRestServicesMemoryFilePath();
	const std::string filePathUtf8 = ToStdString(filePath);
	if (filePathUtf8.empty() || !GB_IsFileExists(filePathUtf8))
	{
		return true;
	}

	const GB_ByteBuffer fileData = GB_ReadBinaryFromFile(filePathUtf8);
	std::vector<ArcGISRestConnectionSettings> settingsList;
	if (!DeserializeArcGISRestConnectionSettingsList(fileData, settingsList))
	{
		return false;
	}

	if (treeWidget)
	{
		treeWidget->setUpdatesEnabled(false);
	}

	bool allSucceeded = true;
	for (ArcGISRestConnectionSettings settings : settingsList)
	{
		settings = NormalizeArcGISRestConnectionSettings(settings);
		if (settings.displayName.empty())
		{
			settings.displayName = ToStdString(GuessArcGISRestDisplayName(settings));
		}

		const std::string serviceBaseUrl = NormalizeArcGISRestServiceUrl(settings.serviceUrl);
		if (settings.serviceUrl.empty() || serviceBaseUrl.empty())
		{
			allSucceeded = false;
			continue;
		}

		ArcGISRestServiceTreeNode connectionNode = CreateArcGISRestConnectionNodeFromSettings(settings, serviceBaseUrl);
		if (!AddArcGISRestServiceNodeInternal(connectionNode, arcGISRestCategoryItem, false))
		{
			allSucceeded = false;
			continue;
		}
		SetArcGISRestConnectionSettings(ToQString(connectionNode.uid), settings);
	}

	if (arcGISRestCategoryItem)
	{
		arcGISRestCategoryItem->setExpanded(true);
	}

	if (treeWidget)
	{
		treeWidget->setUpdatesEnabled(true);
	}

	UpdateLoadingAnimationTimerState();
	UpdateDetailsForItem(treeWidget ? treeWidget->currentItem() : nullptr);
	return allSucceeded;
}

bool QServiceBrowserPanel::SaveArcGISRestServicesToMemoryFile() const
{
	GB_ByteBuffer fileData;
	const std::vector<ArcGISRestConnectionSettings> settingsList = CollectArcGISRestConnectionSettingsForMemoryFile();
	if (!SerializeArcGISRestConnectionSettingsList(settingsList, fileData))
	{
		return false;
	}

	const QString filePath = GetDefaultArcGISRestServicesMemoryFilePath();
	const std::string filePathUtf8 = ToStdString(filePath);
	if (filePathUtf8.empty())
	{
		return false;
	}

	const std::string directoryPathUtf8 = GB_GetDirectoryPath(filePathUtf8);
	if (!directoryPathUtf8.empty() && !GB_CreateDirectory(directoryPathUtf8))
	{
		return false;
	}

	return GB_WriteBinaryToFile(fileData, filePathUtf8);
}

std::vector<ArcGISRestConnectionSettings> QServiceBrowserPanel::CollectArcGISRestConnectionSettingsForMemoryFile() const
{
	std::vector<ArcGISRestConnectionSettings> settingsList;
	if (!arcGISRestCategoryItem)
	{
		return settingsList;
	}

	settingsList.reserve(static_cast<size_t>(arcGISRestCategoryItem->childCount()));
	for (int childIndex = 0; childIndex < arcGISRestCategoryItem->childCount(); childIndex++)
	{
		const QTreeWidgetItem* childItem = arcGISRestCategoryItem->child(childIndex);
		if (!childItem || !IsArcGISRestConnectionRootItem(childItem))
		{
			continue;
		}

		ArcGISRestConnectionSettings settings;
		if (!GetArcGISRestConnectionSettingsForItem(childItem, settings))
		{
			continue;
		}

		settings = NormalizeArcGISRestConnectionSettings(settings);
		settings.displayName = ToStdString(childItem->data(0, RoleText).toString());
		if (settings.displayName.empty())
		{
			settings.displayName = ToStdString(GuessArcGISRestDisplayName(settings));
		}
		if (settings.serviceUrl.empty())
		{
			settings.serviceUrl = NormalizeArcGISRestRequestUrl(ToStdString(childItem->data(0, RoleUrl).toString()));
		}

		settingsList.push_back(std::move(settings));
	}
	return settingsList;
}

void QServiceBrowserPanel::RefreshArcGISRestConnection(QTreeWidgetItem* item)
{
	if (!item || !IsArcGISRestConnectionRootItem(item) || item->data(0, RoleIsLoading).toBool())
	{
		return;
	}

	ArcGISRestConnectionSettings settings;
	if (!GetArcGISRestConnectionSettingsForItem(item, settings))
	{
		ShowArcGISRestConnectionError(this, QStringLiteral("无法获取该 ArcGIS REST 服务连接的原始连接信息。"), QString());
		return;
	}

	settings = NormalizeArcGISRestConnectionSettings(settings);
	if (settings.displayName.empty())
	{
		settings.displayName = ToStdString(item->data(0, RoleText).toString());
	}

	const std::string serviceBaseUrl = NormalizeArcGISRestServiceUrl(settings.serviceUrl);
	if (settings.serviceUrl.empty() || serviceBaseUrl.empty())
	{
		ShowArcGISRestConnectionError(this, QStringLiteral("服务 URL 不能为空。"), QString());
		return;
	}

	ArcGISRestServiceTreeNode connectionNode = CreateArcGISRestConnectionNodeFromSettings(settings, serviceBaseUrl);
	connectionNode.type = static_cast<ArcGISRestServiceTreeNode::NodeType>(item->data(0, RoleNodeType).toInt());
	connectionNode.uid = ToStdString(item->data(0, RoleUid).toString());
	connectionNode.parentUid = ToStdString(GetArcGISRestCategoryUid());

	SetArcGISRestConnectionSettings(item->data(0, RoleUid).toString(), settings);
	SetItemLoadingState(item, true);
	StartArcGISRestConnectionLoad(ToQString(connectionNode.uid), settings, connectionNode, ArcGISRestConnectionLoadMode::RefreshExisting);
}

void QServiceBrowserPanel::RefreshArcGISRestChildNode(QTreeWidgetItem* item)
{
	if (!item || IsPlaceholderItem(item) || IsArcGISRestConnectionRootItem(item) || item->data(0, RoleIsLoading).toBool())
	{
		return;
	}

	const ServiceBrowserNodeInfo nodeInfo = GetNodeInfo(item);
	const ArcGISRestServiceTreeNode::NodeType nodeType = static_cast<ArcGISRestServiceTreeNode::NodeType>(nodeInfo.nodeType);
	if (!nodeInfo.isArcGISRestNode || nodeInfo.url.isEmpty() || !IsNetworkExpandableArcGISRestNodeType(nodeType))
	{
		return;
	}

	ArcGISRestConnectionSettings settings;
	if (!GetArcGISRestConnectionSettingsForNodeItem(item, settings))
	{
		ShowArcGISRestConnectionError(this, QStringLiteral("无法获取该 ArcGIS REST 节点所属服务连接的原始连接信息。"), QString());
		return;
	}

	ArcGISRestServiceTreeNode sourceNode;
	sourceNode.type = nodeType;
	sourceNode.text = ToStdString(nodeInfo.text);
	sourceNode.url = NormalizeArcGISRestRequestUrl(ToStdString(nodeInfo.url));
	sourceNode.uid = ToStdString(nodeInfo.uid);

	QTreeWidgetItem* const parentItem = item->parent();
	if (parentItem && !IsPlaceholderItem(parentItem))
	{
		sourceNode.parentUid = ToStdString(parentItem->data(0, RoleUid).toString());
	}

	settings = NormalizeArcGISRestConnectionSettings(settings);
	settings.serviceUrl = sourceNode.url;

	SetItemLoadingState(item, true);
	ArcGISRestNodeExpandThread* expandThread = new ArcGISRestNodeExpandThread(QPointer<QServiceBrowserPanel>(this), sourceNode, settings);
	connect(expandThread, &QThread::finished, expandThread, &QObject::deleteLater);
	expandThread->start();
}

void QServiceBrowserPanel::ImportArcGISRestNode(QTreeWidgetItem* item)
{
	EmitArcGISRestLayerImportRequest(item);
}

bool QServiceBrowserPanel::EmitArcGISRestLayerImportRequest(QTreeWidgetItem* item)
{
	LayerImportRequestInfo request;
	QString errorMessage;
	if (!BuildArcGISRestLayerImportRequest(item, request, &errorMessage))
	{
		if (!errorMessage.trimmed().isEmpty())
		{
			ShowArcGISRestConnectionError(this, QStringLiteral("无法导入该 ArcGIS REST 图层。"), errorMessage);
		}
		return false;
	}

	emit LayerImportRequested(request);
	return true;
}

bool QServiceBrowserPanel::BuildArcGISRestLayerImportRequest(QTreeWidgetItem* item, LayerImportRequestInfo& outRequest, QString* errorMessage) const
{
	outRequest = LayerImportRequestInfo();
	if (errorMessage)
	{
		errorMessage->clear();
	}

	if (!item || IsPlaceholderItem(item))
	{
		if (errorMessage)
		{
			*errorMessage = QStringLiteral("当前节点无效。 ");
		}
		return false;
	}

	const ServiceBrowserNodeInfo nodeInfo = GetNodeInfo(item);
	if (!nodeInfo.isArcGISRestNode || !nodeInfo.canDrag)
	{
		if (errorMessage)
		{
			*errorMessage = QStringLiteral("当前节点不是可导入的 ArcGIS REST 图层节点。 ");
		}
		return false;
	}

	const QTreeWidgetItem* serviceItem = FindArcGISRestServiceItemForImport(item);
	if (!serviceItem)
	{
		if (errorMessage)
		{
			*errorMessage = QStringLiteral("无法找到当前图层所属的 MapServer / ImageServer / FeatureServer 服务节点。 ");
		}
		return false;
	}

	std::shared_ptr<const ArcGISRestServiceInfo> serviceInfoHolder = GetArcGISRestServiceInfoForItem(serviceItem);
	if (!serviceInfoHolder || serviceInfoHolder->resourceType != ArcGISRestResourceType::Service)
	{
		if (errorMessage)
		{
			*errorMessage = QStringLiteral("无法取得与服务 URL 匹配的 ArcGISRestServiceInfo。请先展开或刷新对应服务节点。 ");
		}
		return false;
	}

	const std::string serviceUrl = NormalizeArcGISRestServiceUrl(ToStdString(serviceItem->data(0, RoleUrl).toString()));
	if (serviceUrl.empty())
	{
		if (errorMessage)
		{
			*errorMessage = QStringLiteral("服务 URL 为空。 ");
		}
		return false;
	}

	ArcGISRestConnectionSettings connectionSettings;
	if (!GetArcGISRestConnectionSettingsForNodeItem(item, connectionSettings))
	{
		if (errorMessage)
		{
			*errorMessage = QStringLiteral("无法取得当前节点所属 ArcGIS REST 服务连接信息。 ");
		}
		return false;
	}

	// connectionSettings 表示用户添加/维护的 ArcGIS REST 根连接信息，
	// 其中 serviceUrl 必须保持为连接根 URL（例如 /arcgis/rest/services），
	// 不能覆盖成当前图层所属的 MapServer / FeatureServer / ImageServer URL。
	// 当前图层所属的具体服务 URL 写入 outRequest.serviceUrl。
	connectionSettings = NormalizeArcGISRestConnectionSettings(connectionSettings);

	const ArcGISRestServiceTreeNode::NodeType requestNodeType = static_cast<ArcGISRestServiceTreeNode::NodeType>(nodeInfo.nodeType);
	const ArcGISRestServiceTreeNode::NodeType serviceNodeType = static_cast<ArcGISRestServiceTreeNode::NodeType>(serviceItem->data(0, RoleNodeType).toInt());
	const std::string layerId = ExtractLayerIdForImport(item, serviceUrl, serviceInfoHolder.get());
	if (requestNodeType != ArcGISRestServiceTreeNode::NodeType::AllLayers && layerId.empty() && !IsArcGISRestServiceNodeType(requestNodeType))
	{
		if (errorMessage)
		{
			*errorMessage = QStringLiteral("无法从当前节点 URL 中解析图层 ID。 ");
		}
		return false;
	}

	outRequest.nodeUid = ToStdString(nodeInfo.uid);
	outRequest.nodeText = ToStdString(nodeInfo.text);
	outRequest.nodeUrl = NormalizeArcGISRestRequestUrl(ToStdString(nodeInfo.url));
	outRequest.serviceNodeUid = ToStdString(serviceItem->data(0, RoleUid).toString());
	outRequest.serviceUrl = serviceUrl;
	outRequest.connectionSettings = connectionSettings;
	outRequest.serviceInfoHolder = serviceInfoHolder;
	outRequest.serviceInfo = outRequest.serviceInfoHolder.get();
	outRequest.layerId = layerId;
	outRequest.nodeType = requestNodeType;
	outRequest.serviceNodeType = serviceNodeType;

	if (!outRequest.IsValid())
	{
		if (errorMessage)
		{
			*errorMessage = QStringLiteral("导入请求结构体不完整。 ");
		}
		outRequest = LayerImportRequestInfo();
		return false;
	}
	return true;
}

const QTreeWidgetItem* QServiceBrowserPanel::FindArcGISRestServiceItemForImport(const QTreeWidgetItem* item) const
{
	const QTreeWidgetItem* currentItem = item;
	while (currentItem && !IsPlaceholderItem(currentItem))
	{
		const ArcGISRestServiceTreeNode::NodeType currentNodeType = static_cast<ArcGISRestServiceTreeNode::NodeType>(currentItem->data(0, RoleNodeType).toInt());
		if (IsArcGISRestServiceNodeType(currentNodeType))
		{
			return currentItem;
		}

		currentItem = currentItem->parent();
	}
	return nullptr;
}

std::shared_ptr<const ArcGISRestServiceInfo> QServiceBrowserPanel::GetArcGISRestServiceInfoForItem(const QTreeWidgetItem* item) const
{
	if (!item || IsPlaceholderItem(item))
	{
		return std::shared_ptr<const ArcGISRestServiceInfo>();
	}

	const QString uid = item->data(0, RoleUid).toString();
	const auto serviceInfoIter = arcGISRestServiceInfoByUid.constFind(uid);
	if (serviceInfoIter == arcGISRestServiceInfoByUid.constEnd())
	{
		return std::shared_ptr<const ArcGISRestServiceInfo>();
	}
	return serviceInfoIter.value();
}

std::string QServiceBrowserPanel::ExtractLayerIdForImport(const QTreeWidgetItem* item, const std::string& serviceUrl, const ArcGISRestServiceInfo* serviceInfo) const
{
	if (!item)
	{
		return std::string();
	}

	const ArcGISRestServiceTreeNode::NodeType nodeType = static_cast<ArcGISRestServiceTreeNode::NodeType>(item->data(0, RoleNodeType).toInt());
	if (nodeType == ArcGISRestServiceTreeNode::NodeType::AllLayers)
	{
		return serviceInfo ? JoinArcGISRestLayerIds(*serviceInfo) : std::string();
	}

	const std::string normalizedServiceUrl = NormalizeArcGISRestServiceUrl(serviceUrl);
	const std::string normalizedNodeUrl = NormalizeArcGISRestServiceUrl(ToStdString(item->data(0, RoleUrl).toString()));
	if (normalizedServiceUrl.empty() || normalizedNodeUrl.empty())
	{
		return std::string();
	}

	const std::string prefix = normalizedServiceUrl + "/";
	if (normalizedNodeUrl.size() <= prefix.size() || normalizedNodeUrl.compare(0, prefix.size(), prefix) != 0)
	{
		return std::string();
	}

	return ExtractFirstUrlPathSegment(normalizedNodeUrl.substr(prefix.size()));
}

void QServiceBrowserPanel::EditArcGISRestConnection(QTreeWidgetItem* item)
{
	if (!item || !IsArcGISRestConnectionRootItem(item) || item->data(0, RoleIsLoading).toBool())
	{
		return;
	}

	ArcGISRestConnectionSettings oldSettings;
	if (!GetArcGISRestConnectionSettingsForItem(item, oldSettings))
	{
		ShowArcGISRestConnectionError(this, QStringLiteral("无法获取该 ArcGIS REST 服务连接的原始连接信息。"), QString());
		return;
	}

	oldSettings.displayName = ToStdString(item->data(0, RoleText).toString());

	const QString sourceUid = item->data(0, RoleUid).toString();

	QArcGISRestConnectionDialog dialog(this);
	dialog.setWindowTitle(QStringLiteral("编辑 ArcGIS REST Server 连接"));
	dialog.SetSettings(oldSettings);
	dialog.SetConnectionNameExistsChecker([this, sourceUid](const QString& connectionName) -> bool
		{
			return HasArcGISRestConnectionDisplayName(connectionName, sourceUid);
		});
	if (dialog.exec() != QDialog::Accepted)
	{
		return;
	}

	ArcGISRestConnectionSettings newSettings = NormalizeArcGISRestConnectionSettings(dialog.GetSettings());
	if (newSettings.displayName.empty())
	{
		newSettings.displayName = ToStdString(GuessArcGISRestDisplayName(newSettings));
	}

	if (AreArcGISRestConnectionSettingsEqualExceptDisplayName(oldSettings, newSettings))
	{
		UpdateArcGISRestConnectionDisplayNameOnly(item, newSettings);
		return;
	}

	const std::string serviceBaseUrl = NormalizeArcGISRestServiceUrl(newSettings.serviceUrl);
	if (newSettings.serviceUrl.empty() || serviceBaseUrl.empty())
	{
		ShowArcGISRestConnectionError(this, QStringLiteral("服务 URL 不能为空。"), QString());
		return;
	}

	ArcGISRestServiceTreeNode connectionNode = CreateArcGISRestConnectionNodeFromSettings(newSettings, serviceBaseUrl);
	connectionNode.uid = ToStdString(sourceUid);
	connectionNode.parentUid = ToStdString(GetArcGISRestCategoryUid());

	item->setText(0, ToQString(connectionNode.text));
	item->setToolTip(0, ToQString(connectionNode.url));
	item->setIcon(0, IconForArcGISRestNodeType(connectionNode.type));
	item->setData(0, RoleUrl, ToQString(connectionNode.url));
	item->setData(0, RoleText, ToQString(connectionNode.text));
	item->setData(0, RoleNodeType, static_cast<int>(connectionNode.type));
	item->setData(0, RoleDetails, CreateDetailTextForArcGISRestNode(connectionNode, false, false));

	SetArcGISRestConnectionSettings(sourceUid, newSettings);
	if (!SaveArcGISRestServicesToMemoryFile())
	{
		ShowArcGISRestServicesMemoryFileError(this,
			QStringLiteral("保存 ArcGIS REST 服务记忆文件失败。"),
			GetDefaultArcGISRestServicesMemoryFilePath());
	}
	SetItemLoadingState(item, true);
	SelectNode(sourceUid);
	ExpandNodeByUid(sourceUid);
	StartArcGISRestConnectionLoad(sourceUid, newSettings, connectionNode, ArcGISRestConnectionLoadMode::EditExisting);
}

void QServiceBrowserPanel::DeleteArcGISRestConnection(QTreeWidgetItem* item)
{
	if (!item || !IsArcGISRestConnectionRootItem(item))
	{
		return;
	}

	const QString serviceName = item->data(0, RoleText).toString();
	const QMessageBox::StandardButton result = QMessageBox::question(this,
		QStringLiteral("删除 ArcGIS REST 服务"),
		QStringLiteral("确定要删除“%1”吗？").arg(serviceName),
		QMessageBox::Yes | QMessageBox::No,
		QMessageBox::No);

	if (result != QMessageBox::Yes)
	{
		return;
	}

	if (RemoveNode(item->data(0, RoleUid).toString()) && !SaveArcGISRestServicesToMemoryFile())
	{
		ShowArcGISRestServicesMemoryFileError(this,
			QStringLiteral("保存 ArcGIS REST 服务记忆文件失败。"),
			GetDefaultArcGISRestServicesMemoryFilePath());
	}
}

void QServiceBrowserPanel::StartArcGISRestConnectionLoad(const QString& sourceUid, const ArcGISRestConnectionSettings& settings,
	const ArcGISRestServiceTreeNode& connectionNode, ArcGISRestConnectionLoadMode mode)
{
	if (sourceUid.isEmpty())
	{
		return;
	}

	const quint64 loadToken = BeginArcGISRestConnectionLoad(sourceUid);
	if (loadToken == 0)
	{
		return;
	}

	ArcGISRestConnectionLoadThread* loadThread = new ArcGISRestConnectionLoadThread(QPointer<QServiceBrowserPanel>(this),
		sourceUid,
		loadToken,
		settings,
		connectionNode.url,
		connectionNode,
		mode);
	connect(loadThread, &QThread::finished, loadThread, &QObject::deleteLater);
	loadThread->start();
}

bool QServiceBrowserPanel::UpdateArcGISRestConnectionDisplayNameOnly(QTreeWidgetItem* item, const ArcGISRestConnectionSettings& settings)
{
	if (!item || !IsArcGISRestConnectionRootItem(item))
	{
		return false;
	}

	const QString uid = item->data(0, RoleUid).toString();
	const QString displayName = ToQString(settings.displayName);
	if (displayName.isEmpty())
	{
		return false;
	}

	item->setText(0, displayName);
	item->setData(0, RoleText, displayName);

	QString detailText = item->data(0, RoleDetails).toString();
	if (!detailText.isEmpty())
	{
		QStringList detailBlocks = detailText.split(QStringLiteral("\n\n"));
		if (!detailBlocks.isEmpty() && detailBlocks.first().startsWith(QStringLiteral("名称：")))
		{
			detailBlocks.first() = QStringLiteral("名称：%1").arg(displayName);
			detailText = detailBlocks.join(QStringLiteral("\n\n"));
		}
		else
		{
			detailText = QStringLiteral("名称：%1\n\n%2").arg(displayName, detailText);
		}
		item->setData(0, RoleDetails, detailText);
	}

	ArcGISRestConnectionSettings normalizedSettings = NormalizeArcGISRestConnectionSettings(settings);
	normalizedSettings.displayName = ToStdString(displayName);
	SetArcGISRestConnectionSettings(uid, normalizedSettings);
	if (!SaveArcGISRestServicesToMemoryFile())
	{
		ShowArcGISRestServicesMemoryFileError(this,
			QStringLiteral("保存 ArcGIS REST 服务记忆文件失败。"),
			GetDefaultArcGISRestServicesMemoryFilePath());
	}

	UpdateDetailsForItem(treeWidget ? treeWidget->currentItem() : nullptr);
	if (treeWidget && treeWidget->currentItem() == item)
	{
		emit CurrentNodeChanged(uid, item->data(0, RoleUrl).toString(), displayName, item->data(0, RoleNodeType).toInt());
	}
	return true;
}

void QServiceBrowserPanel::EnsureArcGISRestCategoryItem()
{
	if (!treeWidget)
	{
		return;
	}

	if (arcGISRestCategoryItem)
	{
		return;
	}

	arcGISRestCategoryItem = CreateArcGISRestCategoryItem();
	treeWidget->addTopLevelItem(arcGISRestCategoryItem);
	RegisterItemRecursively(arcGISRestCategoryItem);
	arcGISRestCategoryItem->setExpanded(true);
}

QString QServiceBrowserPanel::NormalizeUid(const ArcGISRestServiceTreeNode& node) const
{
	if (!node.uid.empty())
	{
		return ToQString(node.uid);
	}

	return ToQString(node.CalculateUid());
}

QString QServiceBrowserPanel::CreateDetailTextForArcGISRestNode(const ArcGISRestServiceTreeNode& node, bool canDrag, bool hasLazyChildren) const
{
	QStringList lines;
	lines << QStringLiteral("名称：%1").arg(ToQString(node.text));
	lines << QStringLiteral("类型：%1").arg(NodeTypeToDisplayText(static_cast<int>(node.type)));
	lines << QStringLiteral("Url：%1").arg(ToQString(node.url));
	lines << QStringLiteral("Uid：%1").arg(NormalizeUid(node));
	lines << QStringLiteral("可导入：%1").arg(BoolToText(canDrag));

	if (!node.serviceInfo.currentVersion.empty())
	{
		lines << QStringLiteral("当前版本：%1").arg(ToQString(node.serviceInfo.currentVersion));
	}
	if (!node.serviceInfo.mapName.empty())
	{
		lines << QStringLiteral("地图名称：%1").arg(ToQString(node.serviceInfo.mapName));
	}
	if (!node.serviceInfo.serviceDescription.empty())
	{
		lines << QStringLiteral("服务描述：%1").arg(ToQString(node.serviceInfo.serviceDescription));
	}
	if (!node.serviceInfo.description.empty())
	{
		lines << QStringLiteral("描述：%1").arg(ToQString(node.serviceInfo.description));
	}
	if (!node.serviceInfo.copyrightText.empty())
	{
		lines << QStringLiteral("版权信息：%1").arg(ToQString(node.serviceInfo.copyrightText));
	}
	if (!node.serviceInfo.capabilities.empty())
	{
		QStringList capabilityTexts;
		for (const std::string& capability : node.serviceInfo.capabilities)
		{
			capabilityTexts << ToQString(capability).trimmed();
		}
		lines << QStringLiteral("能力：%1").arg(capabilityTexts.join(QStringLiteral(", ")));
	}

	return lines.join(QStringLiteral("\n\n"));
}

QString QServiceBrowserPanel::CreateDetailTextFromNodeInfo(const ServiceBrowserNodeInfo& nodeInfo) const
{
	QStringList lines;
	lines << QStringLiteral("名称：%1").arg(nodeInfo.text);
	lines << QStringLiteral("类型：%1").arg(NodeTypeToDisplayText(nodeInfo.nodeType));
	lines << QStringLiteral("URL：%1").arg(nodeInfo.url);
	lines << QStringLiteral("UID：%1").arg(nodeInfo.uid);
	lines << QStringLiteral("可拖入主画布：%1").arg(BoolToText(nodeInfo.canDrag));
	lines << QStringLiteral("可显示右键菜单：%1").arg(BoolToText(nodeInfo.canShowContextMenu));
	return lines.join(QStringLiteral("\n\n"));
}

bool QServiceBrowserPanel::CanDragArcGISRestNodeByDefault(ArcGISRestServiceTreeNode::NodeType nodeType) const
{
	switch (nodeType)
	{
	case ArcGISRestServiceTreeNode::NodeType::AllLayers:
	case ArcGISRestServiceTreeNode::NodeType::UnknownVectorLayer:
	case ArcGISRestServiceTreeNode::NodeType::PointVectorLayer:
	case ArcGISRestServiceTreeNode::NodeType::LineVectorLayer:
	case ArcGISRestServiceTreeNode::NodeType::PolygonVectorLayer:
	case ArcGISRestServiceTreeNode::NodeType::RasterLayer:
		return true;
	case ArcGISRestServiceTreeNode::NodeType::Unknown:
	case ArcGISRestServiceTreeNode::NodeType::Root:
	case ArcGISRestServiceTreeNode::NodeType::Folder:
	case ArcGISRestServiceTreeNode::NodeType::MapService:
	case ArcGISRestServiceTreeNode::NodeType::ImageService:
	case ArcGISRestServiceTreeNode::NodeType::FeatureService:
	case ArcGISRestServiceTreeNode::NodeType::Table:
	default:
		return false;
	}
}

QString QServiceBrowserPanel::NodeTypeToDisplayText(int nodeType) const
{
	switch (static_cast<ArcGISRestServiceTreeNode::NodeType>(nodeType))
	{
	case ArcGISRestServiceTreeNode::NodeType::Root:
		return QStringLiteral("根节点");
	case ArcGISRestServiceTreeNode::NodeType::Folder:
		return QStringLiteral("文件夹");
	case ArcGISRestServiceTreeNode::NodeType::MapService:
		return QStringLiteral("MapServer 服务");
	case ArcGISRestServiceTreeNode::NodeType::ImageService:
		return QStringLiteral("ImageServer 服务");
	case ArcGISRestServiceTreeNode::NodeType::FeatureService:
		return QStringLiteral("FeatureServer 服务");
	case ArcGISRestServiceTreeNode::NodeType::AllLayers:
		return QStringLiteral("全部图层");
	case ArcGISRestServiceTreeNode::NodeType::UnknownVectorLayer:
		return QStringLiteral("矢量图层");
	case ArcGISRestServiceTreeNode::NodeType::PointVectorLayer:
		return QStringLiteral("点矢量图层");
	case ArcGISRestServiceTreeNode::NodeType::LineVectorLayer:
		return QStringLiteral("线矢量图层");
	case ArcGISRestServiceTreeNode::NodeType::PolygonVectorLayer:
		return QStringLiteral("面矢量图层");
	case ArcGISRestServiceTreeNode::NodeType::RasterLayer:
		return QStringLiteral("栅格/地图图层");
	case ArcGISRestServiceTreeNode::NodeType::Table:
		return QStringLiteral("表");
	case ArcGISRestServiceTreeNode::NodeType::Unknown:
	default:
		return QStringLiteral("未知节点");
	}
}

QIcon QServiceBrowserPanel::IconForArcGISRestNodeType(ArcGISRestServiceTreeNode::NodeType nodeType) const
{
	switch (nodeType)
	{
	case ArcGISRestServiceTreeNode::NodeType::Root:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Root_32.ico");
	case ArcGISRestServiceTreeNode::NodeType::Folder:
		return style()->standardIcon(QStyle::SP_DirClosedIcon);
	case ArcGISRestServiceTreeNode::NodeType::MapService:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_MapService_32.ico");
	case ArcGISRestServiceTreeNode::NodeType::ImageService:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_ImageService_32.ico");
	case ArcGISRestServiceTreeNode::NodeType::FeatureService:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_FeatureService_32.ico");
	case ArcGISRestServiceTreeNode::NodeType::Table:
		return style()->standardIcon(QStyle::SP_FileDialogDetailedView);
	case ArcGISRestServiceTreeNode::NodeType::AllLayers:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_AllLayers_64.ico");
	case ArcGISRestServiceTreeNode::NodeType::UnknownVectorLayer:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Unknown_Layer_64.ico");
	case ArcGISRestServiceTreeNode::NodeType::PointVectorLayer:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Point_Layer_64.ico");
	case ArcGISRestServiceTreeNode::NodeType::LineVectorLayer:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Line_Layer_64.ico");
	case ArcGISRestServiceTreeNode::NodeType::PolygonVectorLayer:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Polygon_Layer_64.ico");
	case ArcGISRestServiceTreeNode::NodeType::RasterLayer:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Raster_Layer_64.ico");
	case ArcGISRestServiceTreeNode::NodeType::Unknown:
	default:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Unknown_Layer_64.ico");
	}
}
