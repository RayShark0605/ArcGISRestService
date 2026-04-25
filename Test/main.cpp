#include "ArcGISRestCapabilities.h"
#include <iostream>

int main(int argc, char* argv[])
{
	ArcGISRestConnectionSettings settings;
	settings.serviceUrl = "https://sampleserver6.arcgisonline.com/arcgis/rest/services";

	std::string json = "";
	if (!RequestArcGISRestJson(settings, json) || json.empty())
	{
		std::cout << "Failed to request ArcGIS REST JSON." << std::endl;
		return 1;
	}

	ArcGISRestServiceInfo rootInfo;
	if (!ParseArcGISRestJson(json, settings.serviceUrl, rootInfo))
	{
		std::cout << "Failed to parse ArcGIS REST JSON." << std::endl;
		return 1;
	}

	ArcGISRestServiceTreeNode rootNode;
	if (!BuildArcGISRestServiceTree(rootInfo, rootNode))
	{
		return 1;
	}

	const std::vector<ArcGISRestServiceTreeNode*> militaryNodes = rootNode.FindNodes("NYTimes_Covid19Cases_USCounties");
	if (militaryNodes.size() < 1)
	{
		std::cout << "Failed to find node." << std::endl;
		return 1;
	}

	if (!militaryNodes[0]->Expand(settings))
	{
		std::cout << "Failed to expand node." << std::endl;
		return 1;
	}

	const std::vector<ArcGISRestServiceTreeNode*> unitsNodes = rootNode.FindNodes("[place_holder]");
	if (unitsNodes.size() < 2)
	{
		std::cout << "Failed to find node." << std::endl;
		return 1;
	}

	const ArcGISRestServiceTreeNode* serviceParentNode = nullptr;
	if (!unitsNodes[1]->FindServiceParentNode(rootNode, serviceParentNode))
	{
		std::cout << "Failed to find parent service info." << std::endl;
		return 1;
	}

	//CalculateImageRequestItemsInput input;
	//input.viewExtent.Set(-23179900.597176231, -11427170.097761590, 21910215.030743435, 11036497.615786836);
	//input.viewExtentWidthInPixels = 1385;
	//input.viewExtentHeightInPixels = 690;
	//input.imageFormat = "PNG32";
	//input.layerId = "0";
	//input.serviceUrl = "https://sampleserver6.arcgisonline.com/arcgis/rest/services/World_Street_Map/MapServer";
	//input.serviceInfo = &root;
	//input.isTiled = true;
	//std::vector<ImageRequestItem> requestItems = CalculateImageRequestItems(input);



	return 0;
}