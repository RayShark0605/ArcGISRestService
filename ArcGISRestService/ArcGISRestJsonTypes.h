#ifndef ARCGIS_REST_JSON_TYPES_H
#define ARCGIS_REST_JSON_TYPES_H

#include "ArcGISRestServicePort.h"
#include "GeoBase/GB_Variant.h"
#include "GeoBase/GB_Math.h"
#include "GeoBase/Geometry/GB_Point2d.h"

#include <string>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

enum class ArcGISServiceType
{
    Unknown = 0,
    FeatureServer,
    MapServer,
    ImageServer,
    GlobeServer,
    GPServer,
    GeocodeServer,
    SceneServer
};

struct ArcGISServiceDirectoryEntry
{
    ArcGISServiceType type = ArcGISServiceType::Unknown;
    std::string name = "";
    std::string url = "";
};

struct ArcGISRestFolderEntry
{
    std::string name = "";
    std::string url = "";
};

// MapServer 根资源中的 layers[] 项
// 注意：这不是 /layers 端点返回的“完整 Layer/Table 资源”，
// 而是 Map Service 根资源上的“图层摘要项”。
struct ArcGISMapServiceLayerEntry
{
    std::string id = "";
    std::string name = "";
    std::string url = "";

    std::string parentLayerId = "";
    bool defaultVisibility = false;

    // JSON 中 subLayerIds 既可能是 null，也可能是数组。
    bool hasSubLayerIds = false;
    std::vector<std::string> subLayerIds;

    double minScale = 0;
    double maxScale = 0;

    // 这些字段在很多真实服务返回中会出现，但并非所有服务都保证带出。
    std::string type = "";
    std::string geometryType = "";

    bool hasSupportsDynamicLegends = false;
    bool supportsDynamicLegends = false;

    GB_VariantMap rawJsonMap;
};

// MapServer 根资源中的 tables[] 项
struct ArcGISMapServiceTableEntry
{
    std::string id = "";
    std::string name = "";
    std::string type = "";

    GB_VariantMap rawJsonMap;
};

struct ArcGISRestSpatialReference
{
    int wkid = 0;
    int latestWkid = 0;

    int vcsWkid = 0;
    int latestVcsWkid = 0;

    std::string wkt = "";

    double xyTolerance = GB_QuietNan;
    double zTolerance = GB_QuietNan;
    double mTolerance = GB_QuietNan;

    double falseX = GB_QuietNan;
    double falseY = GB_QuietNan;
    double xyUnits = GB_QuietNan;

    double falseZ = GB_QuietNan;
    double zUnits = GB_QuietNan;

    double falseM = GB_QuietNan;
    double mUnits = GB_QuietNan;
    GB_VariantMap rawJsonMap;
};

struct ArcGISMapServiceTileLod
{
    int level = -1;
    double resolution = GB_QuietNan;
    double scale = GB_QuietNan;

    GB_VariantMap rawJsonMap;
};

struct ArcGISMapServiceTileInfo
{
    int rows = 0;
    int cols = 0;
    int dpi = 0;

    std::string format = "";
    int compressionQuality = 0;

    GB_Point2d origin;
    ArcGISRestSpatialReference spatialReference;

    std::vector<ArcGISMapServiceTileLod> lods;
    GB_VariantMap rawJsonMap;
};

struct ArcGISMapServiceStorageInfo
{
    std::string storageFormat = "";
    int packetSize = 0;

    GB_VariantMap rawJsonMap;
};

struct ArcGISRestEnvelope
{
    double xmin = GB_QuietNan;
    double ymin = GB_QuietNan;
    double xmax = GB_QuietNan;
    double ymax = GB_QuietNan;

    double zmin = GB_QuietNan;
    double zmax = GB_QuietNan;
    double mmin = GB_QuietNan;
    double mmax = GB_QuietNan;

    double idmin = GB_QuietNan;
    double idmax = GB_QuietNan;

    ArcGISRestSpatialReference spatialReference;
    GB_VariantMap rawJsonMap;
};

struct ArcGISMapServiceTimeReference
{
    std::string timeZone = "";
    bool respectsDaylightSaving = false;

    GB_VariantMap rawJsonMap;
};

struct ArcGISMapServiceTimeInfo
{
    bool hasTimeExtent = false;
    std::int64_t timeExtentStart = 0;
    std::int64_t timeExtentEnd = 0;

    bool hasTimeReference = false;
    ArcGISMapServiceTimeReference timeReference;

    std::string timeRelation = "";

    double defaultTimeInterval = 0;
    std::string defaultTimeIntervalUnits = "";

    double defaultTimeWindow = 0.0;
    std::string defaultTimeWindowUnits = "";

    bool hasLiveData = false;
    std::string liveModeOffsetDirection = "";

    GB_VariantMap rawJsonMap;
};

struct ArcGISMapServiceDocumentInfo
{
    std::string title = "";
    std::string author = "";
    std::string comments = "";
    std::string subject = "";
    std::string category = "";
    std::string antialiasingMode = "";
    std::string textAntialiasingMode = "";
    std::string version = "";
    std::string keywords = "";

    GB_VariantMap rawJsonMap;
};

struct ArcGISMapServiceGeoTransformStep
{
    int wkid = 0;
    int latestWkid = 0;

    std::string name = "";
    std::string wkt = "";

    bool transformForward = true;

    GB_VariantMap rawJsonMap;
};

struct ArcGISMapServiceDatumTransformation
{
    std::vector<ArcGISMapServiceGeoTransformStep> geoTransforms;
    GB_VariantMap rawJsonMap;
};

struct ArcGISMapServiceArchivingInfo
{
    bool hasSupportsHistoricMoment = false;
    bool supportsHistoricMoment = false;

    bool hasStartArchivingMoment = false;
    std::int64_t startArchivingMoment = -1;

    GB_VariantMap rawJsonMap;
};

struct ArcGISRestLayerOrTableReference
{
    std::string id = "";
    std::string name = "";

    GB_VariantMap rawJsonMap;
};

struct ArcGISRestLayerOrTableAdvancedQueryCapabilities
{
    bool useStandardizedQueries = false;
    bool supportsStatistics = false;
    bool supportsHavingClause = false;
    bool supportsCountDistinct = false;
    bool supportsOrderBy = false;
    bool supportsDistinct = false;
    bool supportsPagination = false;
    bool supportsTrueCurve = false;
    bool supportsReturningQueryExtent = false;
    bool supportsQueryWithDistance = false;
    bool supportsSqlExpression = false;

    GB_VariantMap rawJsonMap;
};

struct ArcGISRestLayerOrTableInfo
{
    std::string currentVersion = "";

    std::string id = "";
    std::string name = "";
    std::string type = "";
    std::string description = "";
    std::string geometryType = "";

    bool hasSourceSpatialReference = false;
    ArcGISRestSpatialReference sourceSpatialReference;

    std::string copyrightText = "";

    bool hasParentLayer = false;
    ArcGISRestLayerOrTableReference parentLayer;

    // 这里对应 /<id> 或 /layers 返回中的 subLayers 数组，
    // 元素通常是 { "id": ..., "name": ... } 这种轻量引用对象。
    std::vector<ArcGISRestLayerOrTableReference> subLayers;

    double minScale = 0;
    double maxScale = 0;
    double referenceScale = 0;
    bool defaultVisibility = false;

    bool hasExtent = false;
    ArcGISRestEnvelope extent;

    bool hasAttachments = false;
    std::string htmlPopupType = "";
    std::string displayField = "";
    std::string typeIdField = "";
    std::string subtypeFieldName = "";

    // 下面这些字段在不同服务中可能是 null / object / array，
    // 暂时用 GB_Variant 保留原始形态，后续若有需要再继续细分类型。
    bool hasSubtypeField = false;
    GB_Variant subtypeField;

    bool hasDefaultSubtypeCode = false;
    GB_Variant defaultSubtypeCode;

    bool hasFields = false;
    GB_Variant fields;

    bool hasGeometryField = false;
    GB_Variant geometryField;

    bool hasIndexes = false;
    GB_Variant indexes;

    bool hasSubtypes = false;
    GB_Variant subtypes;

    bool hasRelationships = false;
    GB_Variant relationships;

    bool canModifyLayer = false;
    bool canScaleSymbols = false;
    bool hasLabels = false;

    std::vector<std::string> capabilities;

    bool supportsStatistics = false;
    bool supportsAdvancedQueries = false;

    std::vector<std::string> supportedQueryFormats;

    bool isDataVersioned = false;

    bool hasOwnershipBasedAccessControlForFeatures = false;
    GB_Variant ownershipBasedAccessControlForFeatures;

    bool useStandardizedQueries = false;

    bool hasAdvancedQueryCapabilities = false;
    ArcGISRestLayerOrTableAdvancedQueryCapabilities advancedQueryCapabilities;

    bool hasDateFieldsTimeReference = false;
    GB_Variant dateFieldsTimeReference;

    bool supportsCoordinatesQuantization = false;

    GB_VariantMap rawJsonMap;
};

enum class ArcGISRestResourceType
{
    Unknown = 0,
    
    // /arcgis/rest/services
    // /arcgis/rest/services/<folderName>
    ServicesDirectory,

    // /arcgis/rest/services/<serviceName>/<ServiceType>
    Service,

    // /arcgis/rest/services/<serviceName>/<ServiceType>/<layerOrTableId>
    LayerOrTable,

    // /arcgis/rest/services/<serviceName>/<ServiceType>/layers
    AllLayersAndTables
};

struct ArcGISRestServiceInfo
{
    // 当前解析出来的 ArcGIS REST JSON 所属资源层级
    ArcGISRestResourceType resourceType = ArcGISRestResourceType::Unknown;

    std::string currentVersion = "";

    // Services Directory 资源
    std::vector<ArcGISRestFolderEntry> folders;
    std::vector<ArcGISServiceDirectoryEntry> services;

    // Service 根资源（MapServer / FeatureServer / ImageServer ...）
    // 注意：layers / tables 表示“服务根资源上的摘要列表”，不是 /layers 端点返回的完整 Layer/Table 资源。
    std::vector<ArcGISMapServiceLayerEntry> layers;
    std::vector<ArcGISMapServiceTableEntry> tables;

    std::string cimVersion = "";
    std::string serviceDescription = "";
    std::string mapName = "";
    std::string description = "";
    std::string copyrightText = "";
    bool supportsDynamicLayers = false;

    bool hasSpatialReference = false;
    ArcGISRestSpatialReference spatialReference;

    bool singleFusedMapCache = false;

    bool hasTileInfo = false;
    ArcGISMapServiceTileInfo tileInfo;

    bool hasStorageInfo = false;
    ArcGISMapServiceStorageInfo storageInfo;

    bool hasInitialExtent = false;
    ArcGISRestEnvelope initialExtent;

    bool hasFullExtent = false;
    ArcGISRestEnvelope fullExtent;

    bool datesInUnknownTimezone = false;

    bool hasTimeInfo = false;
    ArcGISMapServiceTimeInfo timeInfo;

    double minScale = 0;
    double maxScale = 0;

    std::string units = "";
    std::vector<std::string> supportedImageFormatTypes;

    ArcGISMapServiceDocumentInfo documentInfo;

    std::vector<std::string> capabilities;
    std::vector<std::string> supportedQueryFormats;

    bool exportTilesAllowed = false;
    int maxExportTilesCount = 0;

    double referenceScale = 0;

    std::vector<ArcGISMapServiceDatumTransformation> datumTransformations;
    bool supportsDatumTransformation = false;

    ArcGISMapServiceArchivingInfo archivingInfo;

    bool supportsClipping = false;
    bool supportsSpatialFilter = false;
    bool supportsTimeRelation = false;
    bool supportsQueryDataElements = false;

    int maxRecordCount = 0;
    int maxImageHeight = 0;
    int maxImageWidth = 0;

    std::vector<std::string> tileServers;
    std::string supportedExtensions = "";

    // LayerOrTable 资源：/MapServer/<id>
    ArcGISRestLayerOrTableInfo layerOrTable;

    // AllLayersAndTables 资源：/MapServer/layers
    std::vector<ArcGISRestLayerOrTableInfo> allLayers;
    std::vector<ArcGISRestLayerOrTableInfo> allTables;

    GB_VariantMap rawJsonMap;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
