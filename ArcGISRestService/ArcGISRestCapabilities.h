#pragma once
#include <string>
#include <vector>
#include "GeoBase/GB_Network.h"
#include "GeoBase/Geometry/GB_Rectangle.h"
#include "ArcGISRestServicePort.h"
#include "ArcGISRestJsonTypes.h"

struct ArcGISRestConnectionSettings
{
	std::string displayName = "";
	std::string serviceUrl = "";
	std::string urlPrefix = "";
	std::string portalCommunityEndpoint = "";
	std::string portalContentEndpoint = "";
	std::string username = "";
	std::string password = "";
	std::string httpReferer = "";
	std::vector<std::pair<std::string, std::string>> httpCustomHeaders;
};

ARCGIS_RESTSERVICE_PORT bool RequestArcGISRestJson(const ArcGISRestConnectionSettings& settings, std::string& outJson, const GB_NetworkRequestOptions& networkOptions = GB_NetworkRequestOptions(), std::string* errorMessage = nullptr);

ARCGIS_RESTSERVICE_PORT bool ParseArcGISRestJson(const std::string& json, const std::string& baseUrl, ArcGISRestServiceInfo& serviceInfo, std::string* errorMessage = nullptr);

struct ArcGISRestServiceTreeNode
{
	enum class NodeType
	{
		Unknown = 0,
		Root,
		Folder,
		MapService,
		ImageService,
		FeatureService,
		AllLayers,
		UnknownVectorLayer,
		PointVectorLayer,
		LineVectorLayer,
		PolygonVectorLayer,
		RasterLayer,
		Table
	};
	NodeType type = NodeType::Unknown;
	std::string text = "";
	std::string url = "";
	ArcGISRestServiceInfo serviceInfo;
	std::string uid = "";
	std::string parentUid = "";
	std::vector<ArcGISRestServiceTreeNode> children;

	ARCGIS_RESTSERVICE_PORT std::string CalculateUid() const;
	ARCGIS_RESTSERVICE_PORT bool Expandable() const;
	ARCGIS_RESTSERVICE_PORT bool NeedRequestJson() const;
	ARCGIS_RESTSERVICE_PORT bool Expand(const ArcGISRestConnectionSettings& settings = ArcGISRestConnectionSettings(), std::string* errorMessage = nullptr);
	ARCGIS_RESTSERVICE_PORT bool FindNode(const std::string& nodeUid) const;
	ARCGIS_RESTSERVICE_PORT bool FindNode(const std::string& nodeUid, const ArcGISRestServiceTreeNode*& outNode) const;
	ARCGIS_RESTSERVICE_PORT bool FindNode(const std::string& nodeUid, ArcGISRestServiceTreeNode*& outNode);
	ARCGIS_RESTSERVICE_PORT std::vector<const ArcGISRestServiceTreeNode*> FindNodes(const std::string& nodeText) const;
	ARCGIS_RESTSERVICE_PORT std::vector<ArcGISRestServiceTreeNode*> FindNodes(const std::string& nodeText);
	ARCGIS_RESTSERVICE_PORT bool FindParentNode(const ArcGISRestServiceTreeNode& rootNode, const ArcGISRestServiceTreeNode*& outParentNode) const;
	ARCGIS_RESTSERVICE_PORT bool FindServiceParentNode(const ArcGISRestServiceTreeNode& rootNode, const ArcGISRestServiceTreeNode*& outServiceParentNode) const;
};

ARCGIS_RESTSERVICE_PORT bool BuildArcGISRestServiceTree(const ArcGISRestServiceInfo& serviceInfo, ArcGISRestServiceTreeNode& node);

struct CalculateImageRequestItemsInput
{
	GB_Rectangle viewExtent;
	int viewExtentWidthInPixels = 0;
	int viewExtentHeightInPixels = 0;

	std::string serviceUrl = "";
	std::string layerId = "";
	std::string imageFormat = "";
	const ArcGISRestServiceInfo* serviceInfo = nullptr;

	bool isTiled = false;
	bool isImageServer = false;
	int dpi = 96;
};

struct ImageRequestItem
{
	std::string serviceUrl = "";
	std::string layerId = "";
	std::string imageFormat = "";
	std::string requestUrl = "";

	GB_Rectangle imageExtent;
	std::string uid = "";
};

ARCGIS_RESTSERVICE_PORT std::vector<ImageRequestItem> CalculateImageRequestItems(const CalculateImageRequestItemsInput& input);