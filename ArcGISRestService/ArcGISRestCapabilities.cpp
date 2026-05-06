#include "ArcGISRestCapabilities.h"
#include "GeoBase/GB_FormatParser.h"
#include "GeoBase/GB_Utf8String.h"
#include "GeoBase/GB_Crypto.h"
#include "GeoCrsManager.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <map>

#define SET_ERROR_MESSAGE(msg) do { if (errorMessage) { *errorMessage = (msg); } } while(0)

namespace
{
	inline bool EndsWithPathSegment(const std::string& text, const std::string& suffix)
	{
		if (suffix.empty())
		{
			return false;
		}

		if (text.size() < suffix.size())
		{
			return false;
		}

		const size_t exactPos = text.size() - suffix.size();
		if (text.compare(exactPos, suffix.size(), suffix) == 0)
		{
			if (exactPos == 0 || text[exactPos - 1] == '/')
			{
				return true;
			}
		}

		if (text.size() < suffix.size() + 1)
		{
			return false;
		}

		if (text.back() == '/')
		{
			const size_t slashPos = text.size() - suffix.size() - 1;
			if (text.compare(slashPos, suffix.size(), suffix) == 0)
			{
				if (slashPos == 0 || text[slashPos - 1] == '/')
				{
					return true;
				}
			}
		}

		return false;
	}

	std::string AdjustBaseUrl(const std::string& baseUrl, const std::string& name)
	{
		std::string resultUrl = baseUrl;
		if (resultUrl.empty() || resultUrl.back() != '/')
		{
			resultUrl += '/';
		}

		const std::vector<std::string> parts = GB_Utf8Split(name, '/');
		std::string checkString = "";
		for (const std::string& part : parts)
		{
			if (!checkString.empty())
			{
				checkString += '/';
			}
			checkString += part;

			if (EndsWithPathSegment(resultUrl, checkString))
			{
				const size_t removeLength = checkString.size() + 1;

				if (resultUrl.size() >= removeLength)
				{
					resultUrl.erase(resultUrl.size() - removeLength);
				}
				else
				{
					resultUrl.clear();
				}
				break;
			}
		}
		return resultUrl;
	}

	inline ArcGISServiceType GetArcGISServiceTypeFromString(const std::string& typeString)
	{
		if (GB_Utf8Equals(typeString, "FeatureServer", false))
		{
			return ArcGISServiceType::FeatureServer;
		}
		else if (GB_Utf8Equals(typeString, "MapServer", false))
		{
			return ArcGISServiceType::MapServer;
		}
		else if (GB_Utf8Equals(typeString, "ImageServer", false))
		{
			return ArcGISServiceType::ImageServer;
		}
		else if (GB_Utf8Equals(typeString, "GlobeServer", false))
		{
			return ArcGISServiceType::GlobeServer;
		}
		else if (GB_Utf8Equals(typeString, "GPServer", false))
		{
			return ArcGISServiceType::GPServer;
		}
		else if (GB_Utf8Equals(typeString, "GeocodeServer", false))
		{
			return ArcGISServiceType::GeocodeServer;
		}
		else if (GB_Utf8Equals(typeString, "SceneServer", false))
		{
			return ArcGISServiceType::SceneServer;
		}
		return ArcGISServiceType::Unknown;
	}

	inline std::string GetWktFromSpatialReferenceMap(const GB_VariantMap& spatialReferenceMap)
	{
		const auto latestWkidIt = spatialReferenceMap.find("latestWkid");
		const auto wkidIt = spatialReferenceMap.find("wkid");
		const auto wktIt = spatialReferenceMap.find("wkt");

		std::string crsString = (latestWkidIt != spatialReferenceMap.end() ? latestWkidIt->second.ToString() : "");
		if (crsString.empty())
		{
			crsString = (wkidIt != spatialReferenceMap.end() ? wkidIt->second.ToString() : "");
		}

		std::shared_ptr<const GeoCrs> crs = nullptr;
		if (!crsString.empty())
		{
			crs = GeoCrsManager::GetFromDefinitionCached("EPSG:" + crsString);
			if (!crs || !crs->IsValid())
			{
				crs = GeoCrsManager::GetFromDefinitionCached("ESRI:" + crsString);
			}
		}
		else if (wktIt != spatialReferenceMap.end())
		{
			const std::string wktString = wktIt->second.ToString();
			crs = GeoCrsManager::GetFromDefinitionCached(wktString);
		}

		if (!crs || !crs->IsValid())
		{
			// 如果无法解析出有效坐标系，默认使用 EPSG:4326。
			return GB_ToWkt("EPSG:4326");
		}

		return crs->ExportToWktUtf8();
	}

	inline const std::vector<std::string>& GetSupportedImageFormats()
	{
		static const std::vector<std::string> supportedFormats = { "bmp", "gif", "jpeg", "jpg", "png", "tif", "tiff", "webp" };
		return supportedFormats;
	}

	bool FindBestSupportedImageFormat(const std::vector<std::string>& inputFormats, std::string& outFormat)
	{
		const std::vector<std::string>& supportedFormats = GetSupportedImageFormats();
		for (const std::string& inputFormat : inputFormats)
		{
			for (const std::string& supportedFormat : supportedFormats)
			{
				if (GB_Utf8StartsWith(inputFormat, supportedFormat, false))
				{
					outFormat = inputFormat;
					return true;
				}
			}
		}
		return false;
	}

	inline ArcGISRestSpatialReference ParseSpatialReference(const GB_VariantMap& spatialReferenceMap)
	{
		ArcGISRestSpatialReference spatialReference;
		spatialReference.rawJsonMap = spatialReferenceMap;
		if (spatialReferenceMap.find("wkid") != spatialReferenceMap.end())
		{
			spatialReference.wkid = spatialReferenceMap.at("wkid").ToInt();
		}
		if (spatialReferenceMap.find("latestWkid") != spatialReferenceMap.end())
		{
			spatialReference.latestWkid = spatialReferenceMap.at("latestWkid").ToInt();
		}
		if (spatialReferenceMap.find("vcsWkid") != spatialReferenceMap.end())
		{
			spatialReference.vcsWkid = spatialReferenceMap.at("vcsWkid").ToInt();
		}
		if (spatialReferenceMap.find("latestVcsWkid") != spatialReferenceMap.end())
		{
			spatialReference.latestVcsWkid = spatialReferenceMap.at("latestVcsWkid").ToInt();
		}
		if (spatialReferenceMap.find("xyTolerance") != spatialReferenceMap.end())
		{
			spatialReference.xyTolerance = spatialReferenceMap.at("xyTolerance").ToDouble();
		}
		if (spatialReferenceMap.find("zTolerance") != spatialReferenceMap.end())
		{
			spatialReference.zTolerance = spatialReferenceMap.at("zTolerance").ToDouble();
		}
		if (spatialReferenceMap.find("mTolerance") != spatialReferenceMap.end())
		{
			spatialReference.mTolerance = spatialReferenceMap.at("mTolerance").ToDouble();
		}
		if (spatialReferenceMap.find("falseX") != spatialReferenceMap.end())
		{
			spatialReference.falseX = spatialReferenceMap.at("falseX").ToDouble();
		}
		if (spatialReferenceMap.find("falseY") != spatialReferenceMap.end())
		{
			spatialReference.falseY = spatialReferenceMap.at("falseY").ToDouble();
		}
		if (spatialReferenceMap.find("xyUnits") != spatialReferenceMap.end())
		{
			spatialReference.xyUnits = spatialReferenceMap.at("xyUnits").ToDouble();
		}
		if (spatialReferenceMap.find("falseZ") != spatialReferenceMap.end())
		{
			spatialReference.falseZ = spatialReferenceMap.at("falseZ").ToDouble();
		}
		if (spatialReferenceMap.find("zUnits") != spatialReferenceMap.end())
		{
			spatialReference.zUnits = spatialReferenceMap.at("zUnits").ToDouble();
		}
		if (spatialReferenceMap.find("falseM") != spatialReferenceMap.end())
		{
			spatialReference.falseM = spatialReferenceMap.at("falseM").ToDouble();
		}
		if (spatialReferenceMap.find("mUnits") != spatialReferenceMap.end())
		{
			spatialReference.mUnits = spatialReferenceMap.at("mUnits").ToDouble();
		}
		spatialReference.wkt = GetWktFromSpatialReferenceMap(spatialReferenceMap);
		return spatialReference;
	}

	inline ArcGISRestEnvelope ParseEnvelope(const GB_VariantMap& envelopeMap)
	{
		ArcGISRestEnvelope envelope;
		envelope.rawJsonMap = envelopeMap;
		if (envelopeMap.find("xmin") != envelopeMap.end())
		{
			envelope.xmin = envelopeMap.at("xmin").ToDouble();
		}
		if (envelopeMap.find("ymin") != envelopeMap.end())
		{
			envelope.ymin = envelopeMap.at("ymin").ToDouble();
		}
		if (envelopeMap.find("xmax") != envelopeMap.end())
		{
			envelope.xmax = envelopeMap.at("xmax").ToDouble();
		}
		if (envelopeMap.find("ymax") != envelopeMap.end())
		{
			envelope.ymax = envelopeMap.at("ymax").ToDouble();
		}
		if (envelopeMap.find("zmin") != envelopeMap.end())
		{
			envelope.zmin = envelopeMap.at("zmin").ToDouble();
		}
		if (envelopeMap.find("zmax") != envelopeMap.end())
		{
			envelope.zmax = envelopeMap.at("zmax").ToDouble();
		}
		if (envelopeMap.find("mmin") != envelopeMap.end())
		{
			envelope.mmin = envelopeMap.at("mmin").ToDouble();
		}
		if (envelopeMap.find("mmax") != envelopeMap.end())
		{
			envelope.mmax = envelopeMap.at("mmax").ToDouble();
		}
		if (envelopeMap.find("idmin") != envelopeMap.end())
		{
			envelope.idmin = envelopeMap.at("idmin").ToDouble();
		}
		if (envelopeMap.find("idmax") != envelopeMap.end())
		{
			envelope.idmax = envelopeMap.at("idmax").ToDouble();
		}
		if (envelopeMap.find("spatialReference") != envelopeMap.end())
		{
			GB_VariantMap spatialReferenceMap;
			envelopeMap.at("spatialReference").AnyCast(spatialReferenceMap);
			envelope.spatialReference = ParseSpatialReference(spatialReferenceMap);
		}
		return envelope;
	}

	inline ArcGISMapServiceTimeInfo ParseTimeInfo(const GB_VariantMap& timeInfoMap)
	{
		ArcGISMapServiceTimeInfo timeInfo;
		timeInfo.rawJsonMap = timeInfoMap;
		if (timeInfoMap.find("timeExtent") != timeInfoMap.end())
		{
			std::vector<GB_Variant> timeExtentList;
			timeInfoMap.at("timeExtent").AnyCast(timeExtentList);
			if (timeExtentList.size() == 2)
			{
				timeInfo.hasTimeExtent = true;
				timeInfo.timeExtentStart = timeExtentList[0].ToInt64();
				timeInfo.timeExtentEnd = timeExtentList[1].ToInt64();
			}
		}

		if (timeInfoMap.find("timeReference") != timeInfoMap.end())
		{
			GB_VariantMap timeReferenceMap;
			timeInfoMap.at("timeReference").AnyCast(timeReferenceMap);
			timeInfo.timeReference.rawJsonMap = timeReferenceMap;
			timeInfo.hasTimeReference = true;
			timeInfo.timeReference.timeZone = timeReferenceMap["timeZone"].ToString();
			timeInfo.timeReference.respectsDaylightSaving = timeReferenceMap["respectsDaylightSaving"].ToBool();
		}
		if (timeInfoMap.find("timeRelation") != timeInfoMap.end())
		{
			timeInfo.timeRelation = timeInfoMap.at("timeRelation").ToString();
		}
		if (timeInfoMap.find("defaultTimeInterval") != timeInfoMap.end())
		{
			timeInfo.defaultTimeInterval = timeInfoMap.at("defaultTimeInterval").ToDouble();
		}
		if (timeInfoMap.find("defaultTimeIntervalUnits") != timeInfoMap.end())
		{
			timeInfo.defaultTimeIntervalUnits = timeInfoMap.at("defaultTimeIntervalUnits").ToString();
		}
		if (timeInfoMap.find("defaultTimeWindow") != timeInfoMap.end())
		{
			timeInfo.defaultTimeWindow = timeInfoMap.at("defaultTimeWindow").ToDouble();
		}
		if (timeInfoMap.find("defaultTimeWindowUnits") != timeInfoMap.end())
		{
			timeInfo.defaultTimeWindowUnits = timeInfoMap.at("defaultTimeWindowUnits").ToString();
		}
		if (timeInfoMap.find("hasLiveData") != timeInfoMap.end())
		{
			timeInfo.hasLiveData = timeInfoMap.at("hasLiveData").ToBool();
		}
		if (timeInfoMap.find("liveModeOffsetDirection") != timeInfoMap.end())
		{
			timeInfo.liveModeOffsetDirection = timeInfoMap.at("liveModeOffsetDirection").ToString();
		}
		return timeInfo;
	}

	inline ArcGISMapServiceDocumentInfo ParseDocumentInfo(const GB_VariantMap& documentInfoMap)
	{
		ArcGISMapServiceDocumentInfo documentInfo;
		documentInfo.rawJsonMap = documentInfoMap;
		if (documentInfoMap.find("Title") != documentInfoMap.end())
		{
			documentInfo.title = documentInfoMap.at("Title").ToString();
		}
		if (documentInfoMap.find("Author") != documentInfoMap.end())
		{
			documentInfo.author = documentInfoMap.at("Author").ToString();
		}
		if (documentInfoMap.find("Comments") != documentInfoMap.end())
		{
			documentInfo.comments = documentInfoMap.at("Comments").ToString();
		}
		if (documentInfoMap.find("Subject") != documentInfoMap.end())
		{
			documentInfo.subject = documentInfoMap.at("Subject").ToString();
		}
		if (documentInfoMap.find("Category") != documentInfoMap.end())
		{
			documentInfo.category = documentInfoMap.at("Category").ToString();
		}
		if (documentInfoMap.find("AntialiasingMode") != documentInfoMap.end())
		{
			documentInfo.antialiasingMode = documentInfoMap.at("AntialiasingMode").ToString();
		}
		if (documentInfoMap.find("TextAntialiasingMode") != documentInfoMap.end())
		{
			documentInfo.textAntialiasingMode = documentInfoMap.at("TextAntialiasingMode").ToString();
		}
		if (documentInfoMap.find("Version") != documentInfoMap.end())
		{
			documentInfo.version = documentInfoMap.at("Version").ToString();
		}
		if (documentInfoMap.find("Keywords") != documentInfoMap.end())
		{
			documentInfo.keywords = documentInfoMap.at("Keywords").ToString();
		}
		return documentInfo;
	}

	std::vector<ArcGISMapServiceDatumTransformation> ParseDatumTransformations(const GB_VariantList& datumTransformationList)
	{
		std::vector<ArcGISMapServiceDatumTransformation> datumTransformations;
		for (const GB_Variant& transformationVariant : datumTransformationList)
		{
			GB_VariantMap transformationMap;
			transformationVariant.AnyCast(transformationMap);

			ArcGISMapServiceDatumTransformation datumTransformation;
			datumTransformation.rawJsonMap = transformationMap;

			GB_VariantList geoTransformsList;
			transformationMap["geoTransforms"].AnyCast(geoTransformsList);
			for (const GB_Variant& geoTransform : geoTransformsList)
			{
				GB_VariantMap geoTransformMap;
				geoTransform.AnyCast(geoTransformMap);

				ArcGISMapServiceGeoTransformStep geoTransformStep;
				geoTransformStep.rawJsonMap = geoTransformMap;
				geoTransformStep.wkid = geoTransformMap["wkid"].ToInt();
				geoTransformStep.latestWkid = geoTransformMap["latestWkid"].ToInt();
				geoTransformStep.name = geoTransformMap["name"].ToString();
				geoTransformStep.wkt = geoTransformMap["wkt"].ToString();
				geoTransformStep.transformForward = geoTransformMap["transformForward"].ToBool();
				datumTransformation.geoTransforms.push_back(std::move(geoTransformStep));
			}
			datumTransformations.push_back(std::move(datumTransformation));
		}

		return datumTransformations;
	}

	inline ArcGISMapServiceArchivingInfo ParseArchivingInfo(const GB_VariantMap& archivingInfoMap)
	{
		ArcGISMapServiceArchivingInfo archivingInfo;
		archivingInfo.rawJsonMap = archivingInfoMap;
		if (archivingInfoMap.find("supportsHistoricMoment") != archivingInfoMap.end())
		{
			archivingInfo.hasSupportsHistoricMoment = true;
			archivingInfo.supportsHistoricMoment = archivingInfoMap.at("supportsHistoricMoment").ToBool();
		}
		if (archivingInfoMap.find("startArchivingMoment") != archivingInfoMap.end())
		{
			archivingInfo.hasStartArchivingMoment = true;
			archivingInfo.startArchivingMoment = archivingInfoMap.at("startArchivingMoment").ToInt64();
		}
		return archivingInfo;
	}

	inline ArcGISRestResourceType IdentifyResourceType(const GB_VariantMap& jsonMap)
	{
		const auto HasKey = [&jsonMap](const char* key) -> bool {
			return jsonMap.find(key) != jsonMap.end();
			};

		const auto IsDetailedLayerOrTableObject = [](const GB_VariantMap& objectMap) -> bool {
			if (objectMap.find("id") == objectMap.end())
			{
				return false;
			}

			// 注意：
			// Service 根资源里的 layers/tables 摘要项通常也会有：
			// id、name、type、minScale、maxScale、defaultVisibility、parentLayerId、subLayerIds
			// 所以这些字段不能作为“完整 layer/table 对象”的判据。
			//
			// /layers 或 /<layerId> 返回的完整对象，更可靠的特征应是下面这些字段：
			return objectMap.find("parentLayer") != objectMap.end() ||
				objectMap.find("subLayers") != objectMap.end() ||
				objectMap.find("extent") != objectMap.end() ||
				objectMap.find("fields") != objectMap.end() ||
				objectMap.find("geometryField") != objectMap.end() ||
				objectMap.find("relationships") != objectMap.end() ||
				objectMap.find("htmlPopupType") != objectMap.end() ||
				objectMap.find("displayField") != objectMap.end() ||
				objectMap.find("advancedQueryCapabilities") != objectMap.end() ||
				objectMap.find("supportedQueryFormats") != objectMap.end();
			};

		const auto HasDetailedLayerOrTableArray = [&jsonMap, &IsDetailedLayerOrTableObject](const char* key) -> bool {
			const auto iter = jsonMap.find(key);
			if (iter == jsonMap.end())
			{
				return false;
			}

			const GB_VariantList* itemList = iter->second.AnyCast<GB_VariantList>();
			if (itemList == nullptr)
			{
				return false;
			}

			for (const GB_Variant& item : *itemList)
			{
				const GB_VariantMap* objectMap = item.AnyCast<GB_VariantMap>();
				if (objectMap == nullptr)
				{
					continue;
				}

				if (IsDetailedLayerOrTableObject(*objectMap))
				{
					return true;
				}
			}

			return false;
			};

		// 1. 单个 /<layerId> 或 /<tableId>
		if (HasKey("id") && (HasKey("parentLayer") || HasKey("subLayers") || HasKey("fields") || HasKey("extent") || HasKey("geometryField") ||
			HasKey("relationships") || HasKey("htmlPopupType") || HasKey("displayField") || HasKey("advancedQueryCapabilities")))
		{
			return ArcGISRestResourceType::LayerOrTable;
		}

		// 2. Service 根资源
		// Map Service / Feature Service / Image Service 根资源本来就会返回服务级字段，
		// 同时也可能带 layers/tables 摘要数组，所以必须放在 AllLayersAndTables 前面。
		if (HasKey("mapName") || HasKey("serviceDescription") || HasKey("supportsDynamicLayers") || HasKey("singleFusedMapCache") ||
			HasKey("supportedImageFormatTypes") || HasKey("initialExtent") || HasKey("fullExtent") || HasKey("tileInfo") || HasKey("capabilities") ||
			HasKey("spatialReference"))
		{
			return ArcGISRestResourceType::Service;
		}

		// 3. /layers
		// 根对象本身通常只有 layers/tables，真正的“完整 layer/table 信息”在数组元素里。
		if (HasDetailedLayerOrTableArray("layers") || HasDetailedLayerOrTableArray("tables"))
		{
			return ArcGISRestResourceType::AllLayersAndTables;
		}

		// 4. /rest/services 或 /rest/services/<folder>
		if (HasKey("folders") || HasKey("services"))
		{
			return ArcGISRestResourceType::ServicesDirectory;
		}

		// 5. 若仅有 layers/tables，且没有任何服务根资源特征，则更倾向于 /layers。为了兼容 layers/tables 为空数组的情况。
		if (HasKey("layers") || HasKey("tables"))
		{
			return ArcGISRestResourceType::AllLayersAndTables;
		}

		return ArcGISRestResourceType::Unknown;
	}

	inline ArcGISRestLayerOrTableReference ParseLayerOrTableReference(const GB_VariantMap& layerOrTableMap)
	{
		ArcGISRestLayerOrTableReference subLayerInfo;
		subLayerInfo.rawJsonMap = layerOrTableMap;
		if (layerOrTableMap.find("id") != layerOrTableMap.end())
		{
			subLayerInfo.id = layerOrTableMap.at("id").ToString();
		}
		if (layerOrTableMap.find("name") != layerOrTableMap.end())
		{
			subLayerInfo.name = layerOrTableMap.at("name").ToString();
		}
		return subLayerInfo;
	}

	inline ArcGISRestLayerOrTableAdvancedQueryCapabilities ParseAdvancedQueryCapabilities(const GB_VariantMap& capabilitiesMap)
	{
		ArcGISRestLayerOrTableAdvancedQueryCapabilities capabilities;
		capabilities.rawJsonMap = capabilitiesMap;
		if (capabilitiesMap.find("useStandardizedQueries") != capabilitiesMap.end())
		{
			capabilities.useStandardizedQueries = capabilitiesMap.at("useStandardizedQueries").ToBool();
		}
		if (capabilitiesMap.find("supportsStatistics") != capabilitiesMap.end())
		{
			capabilities.supportsStatistics = capabilitiesMap.at("supportsStatistics").ToBool();
		}
		if (capabilitiesMap.find("supportsHavingClause") != capabilitiesMap.end())
		{
			capabilities.supportsHavingClause = capabilitiesMap.at("supportsHavingClause").ToBool();
		}
		if (capabilitiesMap.find("supportsCountDistinct") != capabilitiesMap.end())
		{
			capabilities.supportsCountDistinct = capabilitiesMap.at("supportsCountDistinct").ToBool();
		}
		if (capabilitiesMap.find("supportsOrderBy") != capabilitiesMap.end())
		{
			capabilities.supportsOrderBy = capabilitiesMap.at("supportsOrderBy").ToBool();
		}
		if (capabilitiesMap.find("supportsDistinct") != capabilitiesMap.end())
		{
			capabilities.supportsDistinct = capabilitiesMap.at("supportsDistinct").ToBool();
		}
		if (capabilitiesMap.find("supportsPagination") != capabilitiesMap.end())
		{
			capabilities.supportsPagination = capabilitiesMap.at("supportsPagination").ToBool();
		}
		if (capabilitiesMap.find("supportsTrueCurve") != capabilitiesMap.end())
		{
			capabilities.supportsTrueCurve = capabilitiesMap.at("supportsTrueCurve").ToBool();
		}
		if (capabilitiesMap.find("supportsReturningQueryExtent") != capabilitiesMap.end())
		{
			capabilities.supportsReturningQueryExtent = capabilitiesMap.at("supportsReturningQueryExtent").ToBool();
		}
		if (capabilitiesMap.find("supportsQueryWithDistance") != capabilitiesMap.end())
		{
			capabilities.supportsQueryWithDistance = capabilitiesMap.at("supportsQueryWithDistance").ToBool();
		}
		if (capabilitiesMap.find("supportsSqlExpression") != capabilitiesMap.end())
		{
			capabilities.supportsSqlExpression = capabilitiesMap.at("supportsSqlExpression").ToBool();
		}
		return capabilities;
	}

	inline ArcGISRestLayerOrTableInfo ParseLayerOrTableInfo(const GB_VariantMap& layerOrTableMap)
	{
		GB_VariantMap jsonMap = layerOrTableMap;

		ArcGISRestLayerOrTableInfo layerOrTable;
		layerOrTable.rawJsonMap = layerOrTableMap;
		layerOrTable.currentVersion = jsonMap["currentVersion"].ToString();
		layerOrTable.id = jsonMap["id"].ToString();
		layerOrTable.name = jsonMap["name"].ToString();
		layerOrTable.type = jsonMap["type"].ToString();
		layerOrTable.description = jsonMap["description"].ToString();
		layerOrTable.geometryType = jsonMap["geometryType"].ToString();

		if (jsonMap.find("sourceSpatialReference") != jsonMap.end())
		{
			GB_VariantMap sourceSpatialReferenceMap;
			jsonMap["sourceSpatialReference"].AnyCast(sourceSpatialReferenceMap);
			if (!sourceSpatialReferenceMap.empty())
			{
				layerOrTable.hasSourceSpatialReference = true;
				layerOrTable.sourceSpatialReference = ParseSpatialReference(sourceSpatialReferenceMap);
			}
		}

		layerOrTable.copyrightText = jsonMap["copyrightText"].ToString();

		if (jsonMap.find("parentLayer") != jsonMap.end())
		{
			GB_VariantMap parentLayerMap;
			jsonMap["parentLayer"].AnyCast(parentLayerMap);
			if (!parentLayerMap.empty())
			{
				layerOrTable.hasParentLayer = true;
				layerOrTable.parentLayer = ParseLayerOrTableReference(parentLayerMap);
			}
		}

		if (jsonMap.find("subLayers") != jsonMap.end())
		{
			GB_VariantList subLayersList;
			jsonMap["subLayers"].AnyCast(subLayersList);
			for (const GB_Variant& subLayer : subLayersList)
			{
				GB_VariantMap subLayerMap;
				subLayer.AnyCast(subLayerMap);
				layerOrTable.subLayers.push_back(ParseLayerOrTableReference(subLayerMap));
			}
		}

		layerOrTable.minScale = jsonMap["minScale"].ToDouble();
		layerOrTable.maxScale = jsonMap["maxScale"].ToDouble();
		layerOrTable.referenceScale = jsonMap["referenceScale"].ToDouble();
		layerOrTable.defaultVisibility = jsonMap["defaultVisibility"].ToBool();

		if (jsonMap.find("extent") != jsonMap.end())
		{
			GB_VariantMap extentMap;
			jsonMap["extent"].AnyCast(extentMap);
			if (!extentMap.empty())
			{
				layerOrTable.hasExtent = true;
				layerOrTable.extent = ParseEnvelope(extentMap);
			}
		}

		layerOrTable.hasAttachments = jsonMap["hasAttachments"].ToBool();
		layerOrTable.htmlPopupType = jsonMap["htmlPopupType"].ToString();
		layerOrTable.displayField = jsonMap["displayField"].ToString();
		layerOrTable.typeIdField = jsonMap["typeIdField"].ToString();
		layerOrTable.subtypeFieldName = jsonMap["subtypeFieldName"].ToString();
		if (jsonMap.find("subtypeField") != jsonMap.end())
		{
			layerOrTable.subtypeField = jsonMap["subtypeField"];
			layerOrTable.hasSubtypeField = (!layerOrTable.subtypeField.IsEmpty());
		}
		if (jsonMap.find("defaultSubtypeCode") != jsonMap.end())
		{
			layerOrTable.defaultSubtypeCode = jsonMap["defaultSubtypeCode"];
			layerOrTable.hasDefaultSubtypeCode = (!layerOrTable.defaultSubtypeCode.IsEmpty());
		}
		if (jsonMap.find("fields") != jsonMap.end())
		{
			layerOrTable.fields = jsonMap["fields"];
			layerOrTable.hasFields = (!layerOrTable.fields.IsEmpty());
		}
		if (jsonMap.find("geometryField") != jsonMap.end())
		{
			layerOrTable.geometryField = jsonMap["geometryField"];
			layerOrTable.hasGeometryField = (!layerOrTable.geometryField.IsEmpty());
		}
		if (jsonMap.find("indexes") != jsonMap.end())
		{
			layerOrTable.indexes = jsonMap["indexes"];
			layerOrTable.hasIndexes = (!layerOrTable.indexes.IsEmpty());
		}
		if (jsonMap.find("subtypes") != jsonMap.end())
		{
			layerOrTable.subtypes = jsonMap["subtypes"];
			layerOrTable.hasSubtypes = (!layerOrTable.subtypes.IsEmpty());
		}
		if (jsonMap.find("relationships") != jsonMap.end())
		{
			layerOrTable.relationships = jsonMap["relationships"];
			layerOrTable.hasRelationships = (!layerOrTable.relationships.IsEmpty());
		}

		layerOrTable.canModifyLayer = jsonMap["canModifyLayer"].ToBool();
		layerOrTable.canScaleSymbols = jsonMap["canScaleSymbols"].ToBool();
		layerOrTable.hasLabels = jsonMap["hasLabels"].ToBool();
		layerOrTable.capabilities = GB_Utf8Split(jsonMap["capabilities"].ToString(), ',');
		layerOrTable.supportsStatistics = jsonMap["supportsStatistics"].ToBool();
		layerOrTable.supportsAdvancedQueries = jsonMap["supportsAdvancedQueries"].ToBool();
		layerOrTable.supportedQueryFormats = GB_Utf8Split(jsonMap["supportedQueryFormats"].ToString(), ',');
		layerOrTable.isDataVersioned = jsonMap["isDataVersioned"].ToBool();

		if (jsonMap.find("ownershipBasedAccessControlForFeatures") != jsonMap.end())
		{
			layerOrTable.ownershipBasedAccessControlForFeatures = jsonMap["ownershipBasedAccessControlForFeatures"];
			layerOrTable.hasOwnershipBasedAccessControlForFeatures = (!layerOrTable.ownershipBasedAccessControlForFeatures.IsEmpty());
		}

		layerOrTable.useStandardizedQueries = jsonMap["useStandardizedQueries"].ToBool();

		if (jsonMap.find("advancedQueryCapabilities") != jsonMap.end())
		{
			GB_VariantMap advancedQueryCapabilitiesMap;
			jsonMap["advancedQueryCapabilities"].AnyCast(advancedQueryCapabilitiesMap);
			if (!advancedQueryCapabilitiesMap.empty())
			{
				layerOrTable.hasAdvancedQueryCapabilities = true;
				layerOrTable.advancedQueryCapabilities = ParseAdvancedQueryCapabilities(advancedQueryCapabilitiesMap);
			}
		}

		if (jsonMap.find("dateFieldsTimeReference") != jsonMap.end())
		{
			layerOrTable.dateFieldsTimeReference = jsonMap["dateFieldsTimeReference"];
			layerOrTable.hasDateFieldsTimeReference = (!layerOrTable.dateFieldsTimeReference.IsEmpty());
		}

		layerOrTable.supportsCoordinatesQuantization = jsonMap["supportsCoordinatesQuantization"].ToBool();
		return layerOrTable;
	}

	ArcGISMapServiceTileInfo ParseTileInfo(const GB_VariantMap& tileInfoMap)
	{
		ArcGISMapServiceTileInfo tileInfo;
		tileInfo.rawJsonMap = tileInfoMap;
		if (tileInfoMap.find("rows") != tileInfoMap.end())
		{
			tileInfo.rows = tileInfoMap.at("rows").ToInt();
		}
		if (tileInfoMap.find("cols") != tileInfoMap.end())
		{
			tileInfo.cols = tileInfoMap.at("cols").ToInt();
		}
		if (tileInfoMap.find("dpi") != tileInfoMap.end())
		{
			tileInfo.dpi = tileInfoMap.at("dpi").ToInt();
		}
		if (tileInfoMap.find("format") != tileInfoMap.end())
		{
			tileInfo.format = tileInfoMap.at("format").ToString();
		}
		if (tileInfoMap.find("compressionQuality") != tileInfoMap.end())
		{
			tileInfo.compressionQuality = tileInfoMap.at("compressionQuality").ToInt();
		}
		if (tileInfoMap.find("origin") != tileInfoMap.end())
		{
			GB_VariantMap originMap;
			tileInfoMap.at("origin").AnyCast(originMap);
			tileInfo.origin.x = originMap["x"].ToDouble();
			tileInfo.origin.y = originMap["y"].ToDouble();
		}
		if (tileInfoMap.find("spatialReference") != tileInfoMap.end())
		{
			GB_VariantMap spatialReferenceMap;
			tileInfoMap.at("spatialReference").AnyCast(spatialReferenceMap);
			tileInfo.spatialReference = ParseSpatialReference(spatialReferenceMap);
		}
		if (tileInfoMap.find("lods") != tileInfoMap.end())
		{
			GB_VariantList lodsList;
			tileInfoMap.at("lods").AnyCast(lodsList);
			for (const GB_Variant& lodVariant : lodsList)
			{
				GB_VariantMap lodMap;
				lodVariant.AnyCast(lodMap);

				ArcGISMapServiceTileLod tileLod;
				tileLod.rawJsonMap = lodMap;
				tileLod.level = lodMap["level"].ToInt();
				tileLod.resolution = lodMap["resolution"].ToDouble();
				tileLod.scale = lodMap["scale"].ToDouble();
				tileInfo.lods.push_back(std::move(tileLod));
			}
		}
		return tileInfo;
	}

	inline std::string CalculateTreeNodeUid(ArcGISRestServiceTreeNode::NodeType type, const std::string& text, const std::string& url)
	{
		const std::string nodeFeatureText = GB_Utf8Format("%d-%s-%s", type, text, url);
		return GB_Md5Hash(nodeFeatureText);
	}

	inline bool UrlEndsWithArcGISServiceType(const std::string& url, const std::string& serviceType)
	{
		std::string normalizedUrl = GB_UrlOperator::GetUrlBase(url);
		if (normalizedUrl.empty())
		{
			normalizedUrl = url;
		}

		while (!normalizedUrl.empty() && normalizedUrl.back() == '/')
		{
			normalizedUrl.pop_back();
		}

		return GB_Utf8EndsWith(normalizedUrl, "/" + serviceType, false);
	}

	inline bool IsImageServiceRootNode(const ArcGISRestServiceInfo& serviceInfo, const ArcGISRestServiceTreeNode& node)
	{
		if (node.type == ArcGISRestServiceTreeNode::NodeType::ImageService || UrlEndsWithArcGISServiceType(node.url, "ImageServer"))
		{
			return true;
		}

		const auto serviceDataTypeIter = serviceInfo.rawJsonMap.find("serviceDataType");
		if (serviceDataTypeIter != serviceInfo.rawJsonMap.end())
		{
			const std::string serviceDataType = serviceDataTypeIter->second.ToString();
			if (GB_Utf8Find(serviceDataType, "ImageService", false) >= 0)
			{
				return true;
			}
		}

		return false;
	}

	inline ArcGISRestServiceTreeNode::NodeType GetVectorLayerNodeType(const ArcGISMapServiceLayerEntry& layerEntry)
	{
		if (GB_Utf8Equals(layerEntry.geometryType, "esriGeometryPoint", false) || GB_Utf8Equals(layerEntry.geometryType, "esriGeometryMultipoint", false))
		{
			return ArcGISRestServiceTreeNode::NodeType::PointVectorLayer;
		}

		if (GB_Utf8Equals(layerEntry.geometryType, "esriGeometryPolyline", false))
		{
			return ArcGISRestServiceTreeNode::NodeType::LineVectorLayer;
		}

		if (GB_Utf8Equals(layerEntry.geometryType, "esriGeometryPolygon", false) || GB_Utf8Equals(layerEntry.geometryType, "esriGeometryEnvelope", false))
		{
			return ArcGISRestServiceTreeNode::NodeType::PolygonVectorLayer;
		}

		return ArcGISRestServiceTreeNode::NodeType::UnknownVectorLayer;
	}

	std::vector<ArcGISRestServiceTreeNode> BuildLayerNodeRecursively(const ArcGISMapServiceLayerEntry& layerEntry, std::unordered_set<std::string>& ancestorLayerIds, std::unordered_set<std::string>& builtLayerIds,
		const std::unordered_map<std::string, std::vector<const ArcGISMapServiceLayerEntry*>>& childLayersByParentId, bool hasVectorEntry, bool hasRasterEntry)
	{
		const std::string& layerId = layerEntry.id;
		builtLayerIds.insert(layerId);
		ancestorLayerIds.insert(layerId);
		if (childLayersByParentId.find(layerId) != childLayersByParentId.end())
		{
			// 有子图层，则该图层默认为 Raster 类型
			ArcGISRestServiceTreeNode thisNode;
			thisNode.text = layerEntry.name;
			thisNode.type = ArcGISRestServiceTreeNode::NodeType::RasterLayer;
			thisNode.url = layerEntry.url;
			thisNode.uid = CalculateTreeNodeUid(thisNode.type, thisNode.text, thisNode.url);
			for (const ArcGISMapServiceLayerEntry* childLayer : childLayersByParentId.at(layerId))
			{
				if (!childLayer || ancestorLayerIds.find(childLayer->id) != ancestorLayerIds.end())
				{
					continue;
				}

				std::unordered_set<std::string> childAncestorLayerIds = ancestorLayerIds;
				std::vector<ArcGISRestServiceTreeNode> childLayerNodes = BuildLayerNodeRecursively(*childLayer, childAncestorLayerIds, builtLayerIds, childLayersByParentId, hasVectorEntry, hasRasterEntry);

				for (ArcGISRestServiceTreeNode& childLayerNode : childLayerNodes)
				{
					childLayerNode.parentUid = thisNode.uid;
				}
				thisNode.children.insert(thisNode.children.end(), childLayerNodes.begin(), childLayerNodes.end());
			}
			std::sort(thisNode.children.begin(), thisNode.children.end(), [](const ArcGISRestServiceTreeNode& a, const ArcGISRestServiceTreeNode& b) {
				if (a.text == b.text)
				{
					return static_cast<int>(a.type) > static_cast<int>(b.type); // 同名时，RasterLayer < VectorLayer
				}
				return GB_Utf8CompareLogical(a.text, b.text) < 0;
				});
			return { thisNode };
		}

		// 没有子图层
		std::vector<ArcGISRestServiceTreeNode> layerNodes;
		if (hasRasterEntry)
		{
			ArcGISRestServiceTreeNode thisNode;
			thisNode.text = layerEntry.name;
			thisNode.type = ArcGISRestServiceTreeNode::NodeType::RasterLayer;
			thisNode.url = layerEntry.url;
			thisNode.uid = CalculateTreeNodeUid(thisNode.type, thisNode.text, thisNode.url);
			layerNodes.push_back(std::move(thisNode));
		}
		if (hasVectorEntry)
		{
			ArcGISRestServiceTreeNode thisNode;
			thisNode.text = layerEntry.name;
			const ArcGISRestServiceTreeNode::NodeType vectorLayerNodeType = GetVectorLayerNodeType(layerEntry);
			thisNode.type = vectorLayerNodeType;
			thisNode.url = layerEntry.url;
			thisNode.uid = CalculateTreeNodeUid(thisNode.type, thisNode.text, thisNode.url);
			layerNodes.push_back(std::move(thisNode));
		}
		return layerNodes;
	}
}

bool RequestArcGISRestJson(const ArcGISRestConnectionSettings& settings, std::string& outJson, const GB_NetworkRequestOptions& networkOptions, std::string* errorMessage)
{
	if (errorMessage)
	{
		errorMessage->clear();
	}

	if (settings.serviceUrl.empty())
	{
		SET_ERROR_MESSAGE("Service URL is empty.");
		return false;
	}

	const std::string jsonRequestUrl = settings.urlPrefix + GB_UrlOperator::SetUrlQueryValue(settings.serviceUrl, "f", "json");
	GB_NetworkResponse jsonRequestResult = GB_RequestUrlData(jsonRequestUrl, networkOptions);
	if (!jsonRequestResult.ok)
	{
		SET_ERROR_MESSAGE("Failed to request ArcGIS REST JSON: " + jsonRequestResult.errorMessageUtf8);
		return false;
	}
	outJson = std::move(jsonRequestResult.body);
	return true;
}

bool ParseArcGISRestJson(const std::string& json, const std::string& baseUrl, ArcGISRestServiceInfo& root, std::string* errorMessage)
{
	if (json.empty())
	{
		SET_ERROR_MESSAGE("Input JSON is empty.");
		return false;
	}

	if (baseUrl.empty())
	{
		SET_ERROR_MESSAGE("Base URL is empty.");
		return false;
	}

	GB_VariantMap jsonMap;
	std::string	parseError = "";
	if (!GB_JsonParser::ParseToVariantMap(json, jsonMap, &parseError))
	{
		SET_ERROR_MESSAGE("Failed to parse JSON: " + parseError);
		return false;
	}
	if (jsonMap.empty())
	{
		SET_ERROR_MESSAGE("Parsed JSON is empty.");
		return false;
	}
	if (jsonMap.find("error") != jsonMap.end())
	{
		std::string errorText = "";
		const GB_VariantMap* error = jsonMap.at("error").AnyCast<GB_VariantMap>();
		if (error && error->find("message") != error->end())
		{
			const std::string* message = error->at("message").AnyCast<std::string>();
			if (message)
			{
				errorText = *message;
			}
		}
		SET_ERROR_MESSAGE("Json contains error: " + errorText);
		return false;
	}

	root = ArcGISRestServiceInfo();
	root.rawJsonMap = jsonMap;
	root.resourceType = IdentifyResourceType(jsonMap);
	root.currentVersion = jsonMap["currentVersion"].ToString();

	// folders
	std::vector<GB_Variant> folderVariants;
	jsonMap["folders"].AnyCast(folderVariants);
	for (const GB_Variant& folderVariant : folderVariants)
	{
		std::string folderName = "";
		if (!folderVariant.AnyCast(folderName) || folderName.empty())
		{
			continue;
		}

		ArcGISRestFolderEntry folder;
		folder.name = folderName;
		folder.url = AdjustBaseUrl(baseUrl, folderName) + folderName;
		root.folders.push_back(std::move(folder));
	}

	// services
	std::vector<GB_Variant> serviceVariants;
	jsonMap["services"].AnyCast(serviceVariants);
	for (const GB_Variant& serviceVariant : serviceVariants)
	{
		GB_VariantMap serviceVariantMap;
		serviceVariant.AnyCast(serviceVariantMap);
		if (serviceVariantMap.find("name") == serviceVariantMap.end() || serviceVariantMap.find("type") == serviceVariantMap.end())
		{
			continue;
		}

		const std::string serviceName = serviceVariantMap.at("name").ToString();
		const std::string serviceTypeString = serviceVariantMap.at("type").ToString();
		const ArcGISServiceType serviceType = GetArcGISServiceTypeFromString(serviceTypeString);
		if (serviceType == ArcGISServiceType::Unknown || serviceType == ArcGISServiceType::GlobeServer || serviceType == ArcGISServiceType::GPServer ||
			serviceType == ArcGISServiceType::GeocodeServer)
		{
			continue;
		}

		std::string displayName = "";
		{
			const std::vector<std::string> nameParts = GB_Utf8Split(serviceName, '/');
			displayName = (nameParts.empty() ? serviceName : nameParts.back());
		}

		ArcGISServiceDirectoryEntry service;
		service.name = displayName;
		service.type = serviceType;
		service.url = AdjustBaseUrl(baseUrl, serviceName) + serviceName + '/' + serviceTypeString;
		root.services.push_back(std::move(service));
	}

	// layers
	GB_VariantMap spatialReferenceMap;
	jsonMap["spatialReference"].AnyCast(spatialReferenceMap);
	if (!spatialReferenceMap.empty())
	{
		root.hasSpatialReference = true;
		ArcGISRestSpatialReference spatialReference = ParseSpatialReference(spatialReferenceMap);
		root.spatialReference = std::move(spatialReference);
	}
	root.cimVersion = jsonMap["cimVersion"].ToString();
	root.serviceDescription = jsonMap["serviceDescription"].ToString();
	root.name = jsonMap["name"].ToString();
	root.mapName = jsonMap["mapName"].ToString();
	root.description = jsonMap["description"].ToString();
	root.copyrightText = jsonMap["copyrightText"].ToString();
	root.supportsDynamicLayers = jsonMap["supportsDynamicLayers"].ToBool();

	GB_VariantList layerInfoList;
	jsonMap["layers"].AnyCast(layerInfoList);
	for (const GB_Variant& layerInfo : layerInfoList)
	{
		GB_VariantMap layerInfoMap;
		layerInfo.AnyCast(layerInfoMap);

		ArcGISMapServiceLayerEntry layerEntry;
		layerEntry.rawJsonMap = layerInfoMap;
		layerEntry.id = layerInfoMap["id"].ToString();
		layerEntry.name = layerInfoMap["name"].ToString();

		layerEntry.url = baseUrl;
		if (GB_Utf8EndsWith(layerEntry.url, "/"))
		{
			layerEntry.url.pop_back();
		}
		layerEntry.url += "/" + layerEntry.id;

		layerEntry.parentLayerId = layerInfoMap["parentLayerId"].ToString();
		layerEntry.defaultVisibility = layerInfoMap["defaultVisibility"].ToBool();
		GB_VariantList subLayerIds;
		layerInfoMap["subLayerIds"].AnyCast(subLayerIds);
		layerEntry.hasSubLayerIds = (!subLayerIds.empty());
		for (const GB_Variant& subLayerId : subLayerIds)
		{
			layerEntry.subLayerIds.push_back(subLayerId.ToString());
		}
		layerEntry.minScale = layerInfoMap["minScale"].ToDouble();
		layerEntry.maxScale = layerInfoMap["maxScale"].ToDouble();
		layerEntry.type = layerInfoMap["type"].ToString();
		layerEntry.geometryType = layerInfoMap["geometryType"].ToString();
		if (layerInfoMap.find("supportsDynamicLegends") != layerInfoMap.end())
		{
			layerEntry.hasSupportsDynamicLegends = true;
			layerEntry.supportsDynamicLegends = layerInfoMap["supportsDynamicLegends"].ToBool();
		}
		root.layers.push_back(std::move(layerEntry));
	}

	GB_VariantList tableInfoList;
	jsonMap["tables"].AnyCast(tableInfoList);
	for (const GB_Variant& tableInfo : tableInfoList)
	{
		GB_VariantMap tableInfoMap;
		tableInfo.AnyCast(tableInfoMap);

		ArcGISMapServiceTableEntry tableEntry;
		tableEntry.rawJsonMap = tableInfoMap;
		tableEntry.id = tableInfoMap["id"].ToString();
		tableEntry.name = tableInfoMap["name"].ToString();
		tableEntry.type = tableInfoMap["type"].ToString();
		root.tables.push_back(std::move(tableEntry));
	}

	root.singleFusedMapCache = jsonMap["singleFusedMapCache"].ToBool();

	if (jsonMap.find("tileInfo") != jsonMap.end())
	{
		GB_VariantMap tileInfoMap;
		jsonMap["tileInfo"].AnyCast(tileInfoMap);
		if (!tileInfoMap.empty())
		{
			root.hasTileInfo = true;
			root.tileInfo = ParseTileInfo(tileInfoMap);
		}
	}

	if (jsonMap.find("storageInfo") != jsonMap.end())
	{
		GB_VariantMap storageInfoMap;
		jsonMap["storageInfo"].AnyCast(storageInfoMap);
		if (!storageInfoMap.empty())
		{
			root.hasStorageInfo = true;
			root.storageInfo.rawJsonMap = storageInfoMap;
			root.storageInfo.storageFormat = storageInfoMap["storageFormat"].ToString();
			root.storageInfo.packetSize = storageInfoMap["packetSize"].ToInt();
		}
	}

	if (jsonMap.find("initialExtent") != jsonMap.end())
	{
		root.hasInitialExtent = true;
		GB_VariantMap initialExtentMap;
		jsonMap["initialExtent"].AnyCast(initialExtentMap);
		root.initialExtent = ParseEnvelope(initialExtentMap);
	}

	if (jsonMap.find("fullExtent") != jsonMap.end())
	{
		root.hasFullExtent = true;
		GB_VariantMap fullExtentMap;
		jsonMap["fullExtent"].AnyCast(fullExtentMap);
		root.fullExtent = ParseEnvelope(fullExtentMap);
	}

	root.datesInUnknownTimezone = jsonMap["datesInUnknownTimezone"].ToBool();

	if (jsonMap.find("timeInfo") != jsonMap.end())
	{
		root.hasTimeInfo = true;
		GB_VariantMap timeInfoMap;
		jsonMap["timeInfo"].AnyCast(timeInfoMap);
		root.timeInfo = ParseTimeInfo(timeInfoMap);
	}

	root.minScale = jsonMap["minScale"].ToDouble();
	root.maxScale = jsonMap["maxScale"].ToDouble();
	root.units = jsonMap["units"].ToString();
	root.supportedImageFormatTypes = GB_Utf8Split(jsonMap["supportedImageFormatTypes"].ToString(), ',');

	GB_VariantMap documentInfoMap;
	jsonMap["documentInfo"].AnyCast(documentInfoMap);
	root.documentInfo = ParseDocumentInfo(documentInfoMap);

	root.capabilities = GB_Utf8Split(jsonMap["capabilities"].ToString(), ',');
	root.supportedQueryFormats = GB_Utf8Split(jsonMap["supportedQueryFormats"].ToString(), ',');
	root.exportTilesAllowed = jsonMap["exportTilesAllowed"].ToBool();
	root.maxExportTilesCount = jsonMap["maxExportTilesCount"].ToInt();

	root.referenceScale = jsonMap["referenceScale"].ToDouble();

	GB_VariantList datumTransformationList;
	jsonMap["datumTransformations"].AnyCast(datumTransformationList);
	root.datumTransformations = ParseDatumTransformations(datumTransformationList);

	root.supportsDatumTransformation = jsonMap["supportsDatumTransformation"].ToBool();

	GB_VariantMap archivingInfoMap;
	jsonMap["archivingInfo"].AnyCast(archivingInfoMap);
	root.archivingInfo = ParseArchivingInfo(archivingInfoMap);

	root.supportsClipping = jsonMap["supportsClipping"].ToBool();
	root.supportsSpatialFilter = jsonMap["supportsSpatialFilter"].ToBool();
	root.supportsTimeRelation = jsonMap["supportsTimeRelation"].ToBool();
	root.supportsQueryDataElements = jsonMap["supportsQueryDataElements"].ToBool();
	root.maxRecordCount = jsonMap["maxRecordCount"].ToInt();
	root.maxImageHeight = jsonMap["maxImageHeight"].ToInt();
	root.maxImageWidth = jsonMap["maxImageWidth"].ToInt();

	// TODO... tileServers
	root.supportedExtensions = jsonMap["supportedExtensions"].ToString();

	if (root.resourceType == ArcGISRestResourceType::LayerOrTable)
	{
		root.layerOrTable = ParseLayerOrTableInfo(jsonMap);
	}
	else if (root.resourceType == ArcGISRestResourceType::AllLayersAndTables)
	{
		GB_VariantList layerInfoList;
		jsonMap["layers"].AnyCast(layerInfoList);
		for (const GB_Variant& layerInfo : layerInfoList)
		{
			GB_VariantMap layerInfoMap;
			layerInfo.AnyCast(layerInfoMap);
			ArcGISRestLayerOrTableInfo layer = ParseLayerOrTableInfo(layerInfoMap);
			root.allLayers.push_back(std::move(layer));
		}

		GB_VariantList tableInfoList;
		jsonMap["tables"].AnyCast(tableInfoList);
		for (const GB_Variant& tableInfo : tableInfoList)
		{
			GB_VariantMap tableInfoMap;
			tableInfo.AnyCast(tableInfoMap);
			ArcGISRestLayerOrTableInfo table = ParseLayerOrTableInfo(tableInfoMap);
			root.allTables.push_back(std::move(table));
		}
	}
	return true;
}

std::string ArcGISRestServiceTreeNode::CalculateUid() const
{
	return CalculateTreeNodeUid(type, text, url);
}

bool ArcGISRestServiceTreeNode::Expandable() const
{
	if (!children.empty())
	{
		return true;
	}

	return (type == NodeType::Root || type == NodeType::Folder || type == NodeType::MapService ||
		type == NodeType::ImageService || type == NodeType::FeatureService || type == NodeType::AllLayers);
}

bool ArcGISRestServiceTreeNode::NeedRequestJson() const
{
	if (!children.empty())
	{
		return false;
	}
	return (type == NodeType::Root || type == NodeType::Folder || type == NodeType::MapService ||
		type == NodeType::ImageService || type == NodeType::FeatureService || type == NodeType::AllLayers);
}

bool ArcGISRestServiceTreeNode::Expand(const ArcGISRestConnectionSettings& settings, std::string* errorMessage)
{
	if (errorMessage)
	{
		errorMessage->clear();
	}

	if (!Expandable())
	{
		if (errorMessage)
		{
			*errorMessage = "Node type does not support expansion.";
		}
		return false;
	}

	if (!NeedRequestJson())
	{
		return true;
	}

	ArcGISRestConnectionSettings requestSetting = settings;
	requestSetting.serviceUrl = url;
	std::string json = "";
	std::string requestErrorMessage = "";
	if (!RequestArcGISRestJson(requestSetting, json, GB_NetworkRequestOptions(), &requestErrorMessage) || json.empty())
	{
		if (errorMessage)
		{
			*errorMessage = "Failed to request JSON for expanding node: " + requestErrorMessage;
		}
		return false;
	}

	ArcGISRestServiceInfo serviceInfo;
	std::string parseErrorMessage = "";
	if (!ParseArcGISRestJson(json, url, serviceInfo, &parseErrorMessage))
	{
		if (errorMessage)
		{
			*errorMessage = "Failed to parse JSON for expanding node: " + parseErrorMessage;
		}
		return false;
	}

	if (!BuildArcGISRestServiceTree(serviceInfo, *this))
	{
		if (errorMessage)
		{
			*errorMessage = "Failed to build service tree from parsed JSON.";
		}
		return false;
	}
	return true;
}

bool ArcGISRestServiceTreeNode::FindNode(const std::string& nodeUid) const
{
	const ArcGISRestServiceTreeNode* outNode = nullptr;
	return FindNode(nodeUid, outNode);
}

bool ArcGISRestServiceTreeNode::FindNode(const std::string& nodeUid, const ArcGISRestServiceTreeNode*& outNode) const
{
	if (uid == nodeUid)
	{
		outNode = this;
		return true;
	}
	for (const ArcGISRestServiceTreeNode& child : children)
	{
		if (child.FindNode(nodeUid, outNode))
		{
			return true;
		}
	}
	return false;
}

bool ArcGISRestServiceTreeNode::FindNode(const std::string& nodeUid, ArcGISRestServiceTreeNode*& outNode)
{
	if (uid == nodeUid)
	{
		outNode = this;
		return true;
	}
	for (ArcGISRestServiceTreeNode& child : children)
	{
		if (child.FindNode(nodeUid, outNode))
		{
			return true;
		}
	}
	return false;
}

std::vector<const ArcGISRestServiceTreeNode*> ArcGISRestServiceTreeNode::FindNodes(const std::string& nodeText) const
{
	std::vector<const ArcGISRestServiceTreeNode*> result;
	if (text == nodeText)
	{
		result.push_back(this);
	}
	for (const ArcGISRestServiceTreeNode& child : children)
	{
		std::vector<const ArcGISRestServiceTreeNode*> childResult = child.FindNodes(nodeText);
		result.insert(result.end(), childResult.begin(), childResult.end());
	}
	return result;
}

std::vector<ArcGISRestServiceTreeNode*> ArcGISRestServiceTreeNode::FindNodes(const std::string& nodeText)
{
	std::vector<ArcGISRestServiceTreeNode*> result;
	if (text == nodeText)
	{
		result.push_back(this);
	}
	for (ArcGISRestServiceTreeNode& child : children)
	{
		std::vector<ArcGISRestServiceTreeNode*> childResult = child.FindNodes(nodeText);
		result.insert(result.end(), childResult.begin(), childResult.end());
	}
	return result;
}

bool ArcGISRestServiceTreeNode::FindParentNode(const ArcGISRestServiceTreeNode& rootNode, const ArcGISRestServiceTreeNode*& outParentNode) const
{
	return rootNode.FindNode(parentUid, outParentNode);
}

bool ArcGISRestServiceTreeNode::FindServiceParentNode(const ArcGISRestServiceTreeNode& rootNode, const ArcGISRestServiceTreeNode*& outServiceParentNode) const
{
	outServiceParentNode = nullptr;
	const ArcGISRestServiceTreeNode* parentNode = nullptr;
	const ArcGISRestServiceTreeNode* curNode = this;
	while (curNode && curNode->FindParentNode(rootNode, parentNode) && parentNode)
	{
		if (parentNode->type == NodeType::Root)
		{
			return false;
		}

		if (parentNode->type == NodeType::MapService || parentNode->type == NodeType::ImageService || parentNode->type == NodeType::FeatureService)
		{
			outServiceParentNode = parentNode;
			return true;
		}
		curNode = parentNode;
	}
	return false;
}

bool BuildArcGISRestServiceTree(const ArcGISRestServiceInfo& serviceInfo, ArcGISRestServiceTreeNode& node)
{
	const static std::string allLayersNodeText = GB_STR("（全部图层）");
	using NodeType = ArcGISRestServiceTreeNode::NodeType;
	if (serviceInfo.resourceType == ArcGISRestResourceType::Unknown)
	{
		return false;
	}

	if (node.type == NodeType::Unknown)
	{
		node.type = NodeType::Root;
	}
	if (node.text.empty())
	{
		node.text = serviceInfo.mapName.empty() ? "Root" : serviceInfo.mapName;
	}

	if (serviceInfo.resourceType == ArcGISRestResourceType::LayerOrTable || serviceInfo.resourceType == ArcGISRestResourceType::AllLayersAndTables)
	{
		return false;
	}

	if (serviceInfo.resourceType == ArcGISRestResourceType::ServicesDirectory)
	{
		for (const ArcGISRestFolderEntry& folderEntry : serviceInfo.folders)
		{
			if (!folderEntry.name.empty() && !folderEntry.url.empty())
			{
				ArcGISRestServiceTreeNode folderNode;
				folderNode.type = NodeType::Folder;
				folderNode.text = folderEntry.name;
				folderNode.url = folderEntry.url;
				folderNode.uid = CalculateTreeNodeUid(folderNode.type, folderNode.text, folderNode.url);
				folderNode.parentUid = node.uid;
				node.children.push_back(folderNode);
			}
		}

		for (const ArcGISServiceDirectoryEntry& serviceEntry : serviceInfo.services)
		{
			if (!serviceEntry.name.empty() && !serviceEntry.url.empty())
			{
				ArcGISRestServiceTreeNode serviceNode;
				serviceNode.text = serviceEntry.name;
				serviceNode.url = serviceEntry.url;

				if (serviceEntry.type == ArcGISServiceType::FeatureServer)
				{
					serviceNode.type = NodeType::FeatureService;
					serviceNode.uid = CalculateTreeNodeUid(serviceNode.type, serviceNode.text, serviceNode.url);
					serviceNode.parentUid = node.uid;
					node.children.push_back(serviceNode);
				}
				else if (serviceEntry.type == ArcGISServiceType::ImageServer)
				{
					serviceNode.type = NodeType::ImageService;
					serviceNode.uid = CalculateTreeNodeUid(serviceNode.type, serviceNode.text, serviceNode.url);
					serviceNode.parentUid = node.uid;
					node.children.push_back(serviceNode);
				}
				else if (serviceEntry.type == ArcGISServiceType::MapServer)
				{
					serviceNode.type = NodeType::MapService;
					serviceNode.uid = CalculateTreeNodeUid(serviceNode.type, serviceNode.text, serviceNode.url);
					serviceNode.parentUid = node.uid;
					node.children.push_back(serviceNode);
				}
			}
		}

		std::sort(node.children.begin(), node.children.end(), [](const ArcGISRestServiceTreeNode& a, const ArcGISRestServiceTreeNode& b) {
			return GB_Utf8CompareLogical(a.text, b.text) < 0;
			});
	}
	else if (serviceInfo.resourceType == ArcGISRestResourceType::Service)
	{
		const bool isImageService = IsImageServiceRootNode(serviceInfo, node);

		// 根据 capabilities 字段判断服务是否具有 Vector（Query）和 Raster（Map）能力。
		// ImageServer 本身就是服务级栅格影像源，Query/Catalog 表示影像目录能力，不能当成普通矢量图层入口。
		bool hasVectorEntry = false;
		bool hasRasterEntry = isImageService;
		if (!isImageService)
		{
			for (const std::string& capability : serviceInfo.capabilities)
			{
				if (GB_Utf8Equals(capability, "Query", false))
				{
					hasVectorEntry = true;
				}
				else if (GB_Utf8Equals(capability, "Map", false))
				{
					hasRasterEntry = true;
				}
			}
		}

		// 构建 parentLayerId -> childLayerEntry 列表的映射，以便后续递归构建图层树
		std::unordered_map<std::string, std::vector<const ArcGISMapServiceLayerEntry*>> childLayersByParentId;
		std::vector<const ArcGISMapServiceLayerEntry*> rootLayers;
		{
			std::unordered_map<std::string, const ArcGISMapServiceLayerEntry*> layerById;
			layerById.reserve(serviceInfo.layers.size());
			for (const ArcGISMapServiceLayerEntry& layer : serviceInfo.layers)
			{
				if (!layer.id.empty())
				{
					layerById[layer.id] = &layer;
				}
			}

			childLayersByParentId.reserve(serviceInfo.layers.size());
			rootLayers.reserve(serviceInfo.layers.size());
			for (const ArcGISMapServiceLayerEntry& layer : serviceInfo.layers)
			{
				const bool hasValidParent = !layer.parentLayerId.empty() && layer.parentLayerId != "-1" &&
					layer.parentLayerId != layer.id && layerById.find(layer.parentLayerId) != layerById.end();
				if (hasValidParent)
				{
					childLayersByParentId[layer.parentLayerId].push_back(&layer);
				}
				else
				{
					rootLayers.push_back(&layer);
				}
			}
		}

		// 递归构建图层树
		std::unordered_set<std::string> builtLayerIds;
		builtLayerIds.reserve(serviceInfo.layers.size());
		for (const ArcGISMapServiceLayerEntry* rootLayerEntry : rootLayers)
		{
			if (!rootLayerEntry)
			{
				continue;
			}

			std::unordered_set<std::string> ancestorLayerIds;
			std::vector<ArcGISRestServiceTreeNode> childrenLayers = BuildLayerNodeRecursively(*rootLayerEntry, ancestorLayerIds, builtLayerIds, childLayersByParentId, hasVectorEntry, hasRasterEntry);
			for (ArcGISRestServiceTreeNode& child : childrenLayers)
			{
				child.parentUid = node.uid;
			}
			node.children.insert(node.children.end(), childrenLayers.begin(), childrenLayers.end());
		}

		std::sort(node.children.begin(), node.children.end(), [](const ArcGISRestServiceTreeNode& a, const ArcGISRestServiceTreeNode& b) {
			if (a.text == b.text)
			{
				return static_cast<int>(a.type) > static_cast<int>(b.type); // 同名时，RasterLayer < VectorLayer
			}
			return GB_Utf8CompareLogical(a.text, b.text) < 0;
			});

		// 如果有多个图层，并且服务支持导出图片，则在图层列表顶部添加一个“全部图层”的节点。
		// ImageServer 的可导入入口是服务本身，不应伪装为 MapServer 风格的 all-layers 子图层。
		if (!isImageService && serviceInfo.layers.size() > 1 && !serviceInfo.supportedImageFormatTypes.empty() && !node.children.empty())
		{
			ArcGISRestServiceTreeNode allLayersNode;
			allLayersNode.type = NodeType::AllLayers;
			allLayersNode.text = allLayersNodeText;

			const std::string& firstLayerUrl = node.children[0].url;
			const uint64_t lastSlashIndex = GB_Utf8FindLast(firstLayerUrl, "/");
			if (lastSlashIndex >= 0)
			{
				allLayersNode.url = GB_Utf8Substr(firstLayerUrl, 0, lastSlashIndex);
			}

			allLayersNode.uid = CalculateTreeNodeUid(allLayersNode.type, allLayersNode.text, allLayersNode.url);
			allLayersNode.parentUid = node.uid;
			node.children.insert(node.children.begin(), allLayersNode);
		}

		// ImageServer 没有 MapServer 意义上的 layerId。为了保持服务浏览面板与其它服务一致，
		// ImageService 服务节点本身仅作为可展开容器，真正可拖入画布的是下面这个服务级栅格子节点。
		// 子节点 URL 仍然指向 ImageServer 服务根 URL，后续导入流程会根据父服务类型识别为 ImageServer，
		// 并通过 /exportImage 请求影像，不会拼接 MapServer 风格的 layers=show:<layerId>。
		if (isImageService)
		{
			bool hasServiceLevelRasterNode = false;
			for (const ArcGISRestServiceTreeNode& child : node.children)
			{
				if (child.type == NodeType::RasterLayer && GB_Utf8Equals(child.url, node.url, false))
				{
					hasServiceLevelRasterNode = true;
					break;
				}
			}

			if (!hasServiceLevelRasterNode)
			{
				ArcGISRestServiceTreeNode imageLayerNode;
				imageLayerNode.type = NodeType::RasterLayer;
				imageLayerNode.text = !node.text.empty() ? node.text : serviceInfo.name;
				if (imageLayerNode.text.empty())
				{
					imageLayerNode.text = GB_STR("ImageServer 影像图层");
				}
				imageLayerNode.url = node.url;
				imageLayerNode.uid = CalculateTreeNodeUid(imageLayerNode.type, imageLayerNode.text, imageLayerNode.url);
				imageLayerNode.parentUid = node.uid;
				imageLayerNode.serviceInfo = serviceInfo;
				node.children.insert(node.children.begin(), std::move(imageLayerNode));
			}
		}
	}
	node.serviceInfo = serviceInfo;
	return true;
}

std::vector<ImageRequestItem> CalculateImageRequestItems(const CalculateImageRequestItemsInput& input)
{
	if (!input.viewExtent.IsValid() || input.viewExtentWidthInPixels <= 0 || input.viewExtentHeightInPixels <= 0 || input.serviceUrl.empty() ||
		(!input.isImageServer && input.layerId.empty()) || input.imageFormat.empty())
	{
		return {};
	}

	const ArcGISRestServiceInfo* serviceInfo = input.serviceInfo;
	const GB_Rectangle& viewExtent = input.viewExtent;

	std::string baseUrl = input.serviceUrl;
	if (GB_Utf8EndsWith(baseUrl, "/"))
	{
		baseUrl.pop_back();
	}

	std::vector<ImageRequestItem> requestItems;
	if (input.isTiled)
	{
		const double targetResolution = viewExtent.Width() / input.viewExtentWidthInPixels;
		if (!serviceInfo || !serviceInfo->hasTileInfo || serviceInfo->tileInfo.lods.empty())
		{
			return {};
		}

		const int tileWidthInPixels = serviceInfo->tileInfo.cols;
		const int tileHeightInPixels = serviceInfo->tileInfo.rows;
		if (tileWidthInPixels < 1 || tileHeightInPixels < 1)
		{
			return {};
		}

		const GB_Point2d& originPt = serviceInfo->tileInfo.origin;
		std::vector<ArcGISMapServiceTileLod> lods = serviceInfo->tileInfo.lods;
		std::sort(lods.begin(), lods.end(), [](const ArcGISMapServiceTileLod& a, const ArcGISMapServiceTileLod& b) {
			return a.resolution > b.resolution;
			});

		int level = 0;
		int foundLevel = -1;
		std::map<int, double> levelToResolutionMap;
		for (const ArcGISMapServiceTileLod& lod : lods)
		{
			level = lod.level;
			const double resolution = lod.resolution;
			levelToResolutionMap[level] = resolution;
			if (foundLevel < 0 && resolution <= 1.5 * targetResolution)
			{
				foundLevel = level;
			}
		}
		level = (foundLevel >= 0 ? foundLevel : lods.back().level);

		const double resolution = levelToResolutionMap[level];
		if (!std::isfinite(resolution) || resolution <= 0.0)
		{
			return {};
		}

		const double tileWidthInMapUnits = static_cast<double>(tileWidthInPixels) * resolution;
		const double tileHeightInMapUnits = static_cast<double>(tileHeightInPixels) * resolution;
		const int minX = static_cast<int>(std::floor((viewExtent.minX - originPt.x) / tileWidthInMapUnits));
		const int minY = static_cast<int>(std::floor((originPt.y - viewExtent.maxY) / tileHeightInMapUnits));
		const int maxX = static_cast<int>(std::ceil((viewExtent.maxX - originPt.x) / tileWidthInMapUnits)) - 1;
		const int maxY = static_cast<int>(std::ceil((originPt.y - viewExtent.minY) / tileHeightInMapUnits)) - 1;

		for (int iy = minY; iy <= maxY; iy++)
		{
			for (int ix = minX; ix <= maxX; ix++)
			{
				const std::string requestUrl = baseUrl + GB_Utf8Format("/tile/%d/%d/%d", level, iy, ix);

				const double tileMinX = originPt.x + static_cast<double>(ix) * tileWidthInMapUnits;
				const double tileMaxY = originPt.y - static_cast<double>(iy) * tileHeightInMapUnits;
				const double tileMaxX = tileMinX + tileWidthInMapUnits;
				const double tileMinY = tileMaxY - tileHeightInMapUnits;

				ImageRequestItem requestItem;
				requestItem.serviceUrl = input.serviceUrl;
				requestItem.layerId = input.layerId;
				requestItem.imageFormat = input.imageFormat;
				requestItem.requestUrl = requestUrl;
				requestItem.imageExtent.Set(tileMinX, tileMinY, tileMaxX, tileMaxY);
				requestItem.uid = GB_Md5Hash(requestItem.requestUrl);
				requestItems.push_back(std::move(requestItem));
			}
		}
	}
	else
	{
		baseUrl += (input.isImageServer ? "/exportImage" : "/export");

		const int maxImageWidth = ((serviceInfo && serviceInfo->maxImageWidth > 0) ? serviceInfo->maxImageWidth : input.viewExtentWidthInPixels);
		const int maxImageHeight = ((serviceInfo && serviceInfo->maxImageHeight > 0) ? serviceInfo->maxImageHeight : input.viewExtentHeightInPixels);
		if (maxImageWidth <= 0 || maxImageHeight <= 0)
		{
			return {};
		}

		const int numStepsInWidth = static_cast<int>(std::ceil(static_cast<double>(input.viewExtentWidthInPixels) / maxImageWidth));
		const int numStepsInHeight = static_cast<int>(std::ceil(static_cast<double>(input.viewExtentHeightInPixels) / maxImageHeight));
		for (int currentStepWidth = 0; currentStepWidth < numStepsInWidth; currentStepWidth++)
		{
			for (int currentStepHeight = 0; currentStepHeight < numStepsInHeight; currentStepHeight++)
			{
				const int usedWidthPixels = currentStepWidth * maxImageWidth;
				const int usedHeightPixels = currentStepHeight * maxImageHeight;
				const int imageWidth = std::min(maxImageWidth, input.viewExtentWidthInPixels - usedWidthPixels);
				const int imageHeight = std::min(maxImageHeight, input.viewExtentHeightInPixels - usedHeightPixels);
				if (imageWidth <= 0 || imageHeight <= 0)
				{
					continue;
				}

				const std::string imageSizeInfo = GB_Utf8Format("%d,%d", imageWidth, imageHeight);
				const double imageMinX = viewExtent.minX + viewExtent.Width() * usedWidthPixels / input.viewExtentWidthInPixels;
				const double imageMinY = viewExtent.minY + viewExtent.Height() * usedHeightPixels / input.viewExtentHeightInPixels;
				const double imageMaxX = viewExtent.minX + viewExtent.Width() * (usedWidthPixels + imageWidth) / input.viewExtentWidthInPixels;
				const double imageMaxY = viewExtent.minY + viewExtent.Height() * (usedHeightPixels + imageHeight) / input.viewExtentHeightInPixels;
				const std::string imageBBoxInfo = GB_Utf8Format("%lf,%lf,%lf,%lf", imageMinX, imageMinY, imageMaxX, imageMaxY);

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
	}
	return requestItems;
}
