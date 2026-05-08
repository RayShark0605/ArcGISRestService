#ifndef GEO_IO_H
#define GEO_IO_H

#include "ArcGISRestServicePort.h"
#include "GeoBoundingBox.h"
#include "GeoVectorFeature.h"
#include "GeoVectorField.h"
#include "GeoVectorGeometry.h"
#include "GeoBase/GB_Variant.h"
#include "GeoBase/Geometry/GB_Rectangle.h"
#include <string>
#include <vector>

class ARCGIS_RESTSERVICE_PORT GeoIO_Shp
{
public:
#pragma region SourceFile
	enum class SourceFileRole
	{
		Unknown = 0,
		Shp,
		Shx,
		Dbf,
		Prj,
		Cpg,
		Qix,
		Sbn,
		Sbx,
		ShpXml,
		OtherSidecar
	};

	struct SourceFile
	{
		SourceFileRole role = SourceFileRole::Unknown;
		std::string filePathUtf8 = "";
		bool exists = false;
		std::uint64_t fileSizeBytes = 0;
	};

	struct SourceFileSet
	{
		std::vector<SourceFile> files;

		std::string GetFilePathUtf8(SourceFileRole role) const;
		bool HasFile(SourceFileRole role) const;
	};
#pragma endregion

#pragma region Diagnostic
	enum class ReadDiagnosticLevel
	{
		Info = 0,
		Warning,
		Error
	};

	struct ReadDiagnostic
	{
		ReadDiagnosticLevel level = ReadDiagnosticLevel::Info;
		std::string codeUtf8 = "";
		std::string messageUtf8 = "";
		long long featureFid = GeoInvalidFid;
		int fieldIndex = -1;
	};
#pragma endregion

#pragma region LayerSourceInfo
	struct EncodingInfo
	{
		bool hasCpgFile = false;
		std::string cpgValueUtf8 = "";

		bool hasLdidValue = false;
		int ldidValue = 0;

		std::string encodingFromCpgUtf8 = "";
		std::string encodingFromLdidUtf8 = "";
		std::string sourceEncodingUtf8 = "";
		std::string usedEncodingUtf8 = "";

		bool encodingOverriddenByUser = false;
		bool recodeToUtf8 = true;
	};

	struct MetadataItem
	{
		std::string domainUtf8 = "";
		std::string keyUtf8 = "";
		std::string valueUtf8 = "";
	};

	enum class SourceType
	{
		Unknown = 0,
		EsriShapefile,
		StandaloneDbf
	};

	struct LayerSourceInfo
	{
		SourceType sourceType = SourceType::Unknown;
		std::string driverNameUtf8 = "";
		std::string datasetPathUtf8 = "";
		std::string layerNameUtf8 = "";
		int layerIndex = 0;
		SourceFileSet sourceFiles;
		EncodingInfo encodingInfo;
		std::vector<MetadataItem> metadataItems;
	};
#pragma endregion

#pragma region LayerGeometryInfo
	enum class ShapefileGeometryKind
	{
		Unknown = 0,
		NullShape,
		Point,
		MultiPoint,
		Polyline,
		Polygon,
		MultiPatch
	};

	struct LayerGeometryInfo
	{
		GeoVectorGeometryType geometryType = GeoVectorGeometryType::Unknown;
		ShapefileGeometryKind shapefileGeometryKind = ShapefileGeometryKind::Unknown;
		int sourceWkbGeometryTypeCode = 0;
		std::string sourceWkbGeometryTypeNameUtf8 = "";
		bool hasZ = false;
		bool hasM = false;
		bool hasNullGeometry = false;
		bool hasMixedSingleAndMultipartGeometry = false;
		bool droppedZ = false;
		bool droppedM = false;
		bool hasUnsupportedGeometry = false;
	};
#pragma endregion

#pragma region ShpData
	struct LayerSpatialInfo
	{
		bool hasCrs = false;
		std::string crsWktUtf8 = "";
		std::string authorityCodeUtf8 = "";
		bool hasExtent = false;
		GB_Rectangle extent;
		GeoBoundingBox boundingBox;
	};

	struct LayerStatistics
	{
		long long featureCount = -1;
		std::size_t loadedFeatureCount = 0;
		std::size_t fieldCount = 0;
		std::size_t nullGeometryFeatureCount = 0;
		std::size_t unsupportedGeometryFeatureCount = 0;
	};

	struct ShpData
	{
		std::string layerNameUtf8 = "";
		std::string displayNameUtf8 = "";
		LayerSourceInfo sourceInfo;
		LayerGeometryInfo geometryInfo;
		LayerSpatialInfo spatialInfo;
		LayerStatistics statistics;
		GeoVectorFields fields;
		std::vector<GeoVectorFeature> features;
		std::vector<MetadataItem> metadataItems;
		std::vector<ReadDiagnostic> diagnostics;

		bool IsValid() const;
		void Reset();
	};
#pragma endregion

	








	

	

	











































};










#endif