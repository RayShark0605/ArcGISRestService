#include "GeoIO.h"

#include "GeoCrsManager.h"
#include "GeoBase/GB_FileSystem.h"
#include "GeoBase/GB_IO.h"
#include "GeoBase/GB_Utf8String.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>
#include <cpl_string.h>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

namespace
{
    struct GdalDatasetDeleter
    {
        void operator()(GDALDataset* dataset) const
        {
            if (dataset != nullptr)
            {
                GDALClose(dataset);
            }
        }
    };

    struct OgrFeatureDeleter
    {
        void operator()(OGRFeature* feature) const
        {
            if (feature != nullptr)
            {
                OGRFeature::DestroyFeature(feature);
            }
        }
    };

    struct OgrSpatialReferenceDeleter
    {
        void operator()(OGRSpatialReference* spatialReference) const
        {
            if (spatialReference != nullptr)
            {
                OGRSpatialReference::DestroySpatialReference(spatialReference);
            }
        }
    };

    class CslStringListGuard
    {
    public:
        CslStringListGuard()
        {
        }

        ~CslStringListGuard()
        {
            CSLDestroy(items_);
        }

        CslStringListGuard(const CslStringListGuard&) = delete;
        CslStringListGuard& operator=(const CslStringListGuard&) = delete;

        void SetNameValue(const char* name, const char* value)
        {
            items_ = CSLSetNameValue(items_, name, value);
        }

        char** Get() const
        {
            return items_;
        }

    private:
        char** items_ = nullptr;
    };

    class ScopedGdalConfigOption
    {
    public:
        ScopedGdalConfigOption(const char* name, const char* value) : name_(name == nullptr ? "" : name)
        {
            const char* oldValue = CPLGetConfigOption(name_.c_str(), nullptr);
            if (oldValue != nullptr)
            {
                hadOldValue_ = true;
                oldValue_ = oldValue;
            }
            CPLSetConfigOption(name_.c_str(), value);
        }

        ~ScopedGdalConfigOption()
        {
            if (name_.empty())
            {
                return;
            }

            CPLSetConfigOption(name_.c_str(), hadOldValue_ ? oldValue_.c_str() : nullptr);
        }

        ScopedGdalConfigOption(const ScopedGdalConfigOption&) = delete;
        ScopedGdalConfigOption& operator=(const ScopedGdalConfigOption&) = delete;

    private:
        std::string name_;
        bool hadOldValue_ = false;
        std::string oldValue_ = "";
    };

    void EnsureGdalRegistered()
    {
        static std::once_flag registerOnceFlag;
        std::call_once(registerOnceFlag, []()
            {
                GDALAllRegister();
            });
    }

    void SetErrorMessage(std::string* errorMessageUtf8, const std::string& messageUtf8)
    {
        if (errorMessageUtf8 != nullptr)
        {
            *errorMessageUtf8 = messageUtf8;
        }
    }

    std::string ToLowerAsciiString(const std::string& text)
    {
        std::string result = text;
        for (size_t i = 0; i < result.size(); i++)
        {
            if (result[i] >= 'A' && result[i] <= 'Z')
            {
                result[i] = static_cast<char>(result[i] - 'A' + 'a');
            }
        }
        return result;
    }

    bool EndsWithAsciiNoCase(const std::string& text, const std::string& suffix)
    {
        if (text.size() < suffix.size())
        {
            return false;
        }

        const std::string textSuffix = text.substr(text.size() - suffix.size());
        return ToLowerAsciiString(textSuffix) == ToLowerAsciiString(suffix);
    }

    std::string NormalizePathSeparators(const std::string& pathUtf8)
    {
        std::string result = pathUtf8;
        for (size_t i = 0; i < result.size(); i++)
        {
            if (result[i] == '\\')
            {
                result[i] = '/';
            }
        }
        return result;
    }

    std::string GetFileNameWithExtension(const std::string& filePathUtf8)
    {
        const std::string normalizedPath = NormalizePathSeparators(filePathUtf8);
        const size_t slashPosition = normalizedPath.find_last_of('/');
        if (slashPosition == std::string::npos)
        {
            return normalizedPath;
        }
        return normalizedPath.substr(slashPosition + 1);
    }

    std::string GetDirectoryPath(const std::string& filePathUtf8)
    {
        const std::string normalizedPath = NormalizePathSeparators(filePathUtf8);
        const size_t slashPosition = normalizedPath.find_last_of('/');
        if (slashPosition == std::string::npos)
        {
            return "";
        }
        return normalizedPath.substr(0, slashPosition + 1);
    }

    std::string GetFileStem(const std::string& filePathUtf8)
    {
        const std::string fileName = GetFileNameWithExtension(filePathUtf8);
        if (EndsWithAsciiNoCase(fileName, ".shp.xml"))
        {
            return fileName.substr(0, fileName.size() - 8);
        }

        const size_t dotPosition = fileName.find_last_of('.');
        if (dotPosition == std::string::npos)
        {
            return fileName;
        }
        return fileName.substr(0, dotPosition);
    }

    std::string GetFileExtensionLower(const std::string& filePathUtf8)
    {
        const std::string fileName = GetFileNameWithExtension(filePathUtf8);
        if (EndsWithAsciiNoCase(fileName, ".shp.xml"))
        {
            return ".shp.xml";
        }

        const size_t dotPosition = fileName.find_last_of('.');
        if (dotPosition == std::string::npos)
        {
            return "";
        }
        return ToLowerAsciiString(fileName.substr(dotPosition));
    }

    std::string GetBasePathWithoutKnownExtension(const std::string& shpOrDbfFilePathUtf8)
    {
        std::string normalizedPath = NormalizePathSeparators(shpOrDbfFilePathUtf8);
        if (EndsWithAsciiNoCase(normalizedPath, ".shp.xml"))
        {
            normalizedPath.erase(normalizedPath.size() - 8);
            return normalizedPath;
        }

        const std::string extension = GetFileExtensionLower(normalizedPath);
        if (extension == ".shp" || extension == ".shx" || extension == ".dbf" || extension == ".prj" || extension == ".cpg" ||
            extension == ".qix" || extension == ".sbn" || extension == ".sbx")
        {
            normalizedPath.erase(normalizedPath.size() - extension.size());
        }
        return normalizedPath;
    }

    GeoIO_Shp::SourceFile MakeSourceFile(const GeoIO_Shp::SourceFileRole role, const std::string& filePathUtf8)
    {
        GeoIO_Shp::SourceFile sourceFile;
        sourceFile.role = role;
        sourceFile.filePathUtf8 = filePathUtf8;
        sourceFile.exists = GB_IsFileExists(filePathUtf8);
        sourceFile.fileSizeBytes = sourceFile.exists ? static_cast<std::uint64_t>(GB_GetFileSizeByte(filePathUtf8)) : 0;
        return sourceFile;
    }

    void AddDiagnostic(std::vector<GeoIO_Shp::ReadDiagnostic>& diagnostics,
        const GeoIO_Shp::ReadDiagnosticLevel level,
        const std::string& codeUtf8,
        const std::string& messageUtf8,
        const long long featureFid = GeoInvalidFid,
        const int fieldIndex = -1)
    {
        GeoIO_Shp::ReadDiagnostic diagnostic;
        diagnostic.level = level;
        diagnostic.codeUtf8 = codeUtf8;
        diagnostic.messageUtf8 = messageUtf8;
        diagnostic.featureFid = featureFid;
        diagnostic.fieldIndex = fieldIndex;
        diagnostics.push_back(std::move(diagnostic));
    }

    std::string GetMetadataValue(char** metadata, const char* key)
    {
        if (metadata == nullptr || key == nullptr)
        {
            return "";
        }

        const char* value = CSLFetchNameValue(metadata, key);
        return value == nullptr ? "" : std::string(value);
    }

    void AppendMetadataItems(GDALMajorObject* object, const std::string& domainPrefixUtf8, std::vector<GeoIO_Shp::MetadataItem>& metadataItems)
    {
        if (object == nullptr)
        {
            return;
        }

        char** domainList = object->GetMetadataDomainList();
        if (domainList == nullptr)
        {
            char** metadata = object->GetMetadata();
            for (int i = 0; metadata != nullptr && metadata[i] != nullptr; i++)
            {
                char* key = nullptr;
                const char* value = CPLParseNameValue(metadata[i], &key);
                if (key != nullptr && value != nullptr)
                {
                    GeoIO_Shp::MetadataItem item;
                    item.domainUtf8 = domainPrefixUtf8;
                    item.keyUtf8 = key;
                    item.valueUtf8 = value;
                    metadataItems.push_back(std::move(item));
                }
                CPLFree(key);
            }
            return;
        }

        for (int domainIndex = 0; domainList[domainIndex] != nullptr; domainIndex++)
        {
            const std::string domain = domainList[domainIndex];
            char** metadata = object->GetMetadata(domain.empty() ? nullptr : domain.c_str());
            for (int itemIndex = 0; metadata != nullptr && metadata[itemIndex] != nullptr; itemIndex++)
            {
                char* key = nullptr;
                const char* value = CPLParseNameValue(metadata[itemIndex], &key);
                if (key != nullptr && value != nullptr)
                {
                    GeoIO_Shp::MetadataItem item;
                    item.domainUtf8 = domainPrefixUtf8.empty() ? domain : (domainPrefixUtf8 + ":" + domain);
                    item.keyUtf8 = key;
                    item.valueUtf8 = value;
                    metadataItems.push_back(std::move(item));
                }
                CPLFree(key);
            }
        }

        CSLDestroy(domainList);
    }

    GeoIO_Shp::EncodingInfo ReadEncodingInfo(OGRLayer* layer, const GeoIO_Shp::SourceFileSet& sourceFiles, const GeoIO_Shp::ReadOptions& options)
    {
        GeoIO_Shp::EncodingInfo encodingInfo;
        encodingInfo.hasCpgFile = sourceFiles.HasFile(GeoIO_Shp::SourceFileRole::Cpg);
        encodingInfo.recodeToUtf8 = options.recodeTextToUtf8;
        encodingInfo.encodingOverriddenByUser = !options.overrideEncodingUtf8.empty();
        encodingInfo.usedEncodingUtf8 = options.overrideEncodingUtf8;

        if (encodingInfo.hasCpgFile)
        {
            encodingInfo.cpgValueUtf8 = GB_ReadUtf8FromFile(sourceFiles.GetFilePathUtf8(GeoIO_Shp::SourceFileRole::Cpg), "utf-8");
        }

        if (layer == nullptr)
        {
            return encodingInfo;
        }

        char** metadata = layer->GetMetadata("SHAPEFILE");
        encodingInfo.cpgValueUtf8 = GetMetadataValue(metadata, "CPG_VALUE").empty() ? encodingInfo.cpgValueUtf8 : GetMetadataValue(metadata, "CPG_VALUE");
        encodingInfo.encodingFromCpgUtf8 = GetMetadataValue(metadata, "ENCODING_FROM_CPG");
        encodingInfo.encodingFromLdidUtf8 = GetMetadataValue(metadata, "ENCODING_FROM_LDID");
        encodingInfo.sourceEncodingUtf8 = GetMetadataValue(metadata, "SOURCE_ENCODING");

        const std::string ldidValue = GetMetadataValue(metadata, "LDID_VALUE");
        if (!ldidValue.empty())
        {
            encodingInfo.hasLdidValue = true;
            encodingInfo.ldidValue = std::atoi(ldidValue.c_str());
        }

        if (encodingInfo.usedEncodingUtf8.empty())
        {
            encodingInfo.usedEncodingUtf8 = encodingInfo.sourceEncodingUtf8;
        }
        if (encodingInfo.usedEncodingUtf8.empty())
        {
            encodingInfo.usedEncodingUtf8 = encodingInfo.encodingFromCpgUtf8;
        }
        if (encodingInfo.usedEncodingUtf8.empty())
        {
            encodingInfo.usedEncodingUtf8 = encodingInfo.encodingFromLdidUtf8;
        }

        return encodingInfo;
    }

    GeoVectorFieldType FieldTypeFromOgrFieldDefn(const OGRFieldDefn* fieldDefn)
    {
        if (fieldDefn == nullptr)
        {
            return GeoVectorFieldType::Unknown;
        }

        const OGRFieldType fieldType = fieldDefn->GetType();
        const OGRFieldSubType fieldSubType = fieldDefn->GetSubType();

        if (fieldType == OFTInteger && fieldSubType == OFSTBoolean)
        {
            return GeoVectorFieldType::Bool;
        }
        if (fieldType == OFTInteger && fieldSubType == OFSTInt16)
        {
            return GeoVectorFieldType::Int16;
        }
        if (fieldType == OFTReal && fieldSubType == OFSTFloat32)
        {
            return GeoVectorFieldType::Float;
        }

        switch (fieldType)
        {
        case OFTInteger:
            return GeoVectorFieldType::Int32;
        case OFTInteger64:
            return GeoVectorFieldType::Int64;
        case OFTReal:
            return GeoVectorFieldType::Double;
        case OFTString:
        case OFTWideString:
            return GeoVectorFieldType::String;
        case OFTDate:
            return GeoVectorFieldType::Date;
        case OFTTime:
            return GeoVectorFieldType::Time;
        case OFTDateTime:
            return GeoVectorFieldType::DateTime;
        case OFTBinary:
            return GeoVectorFieldType::Blob;
        default:
            return GeoVectorFieldType::Unknown;
        }
    }

    GeoVectorField ConvertFieldDefn(const OGRFieldDefn* fieldDefn)
    {
        GeoVectorField field;
        if (fieldDefn == nullptr)
        {
            return field;
        }

        field.nameUtf8 = fieldDefn->GetNameRef() == nullptr ? "" : fieldDefn->GetNameRef();
        field.aliasUtf8 = field.nameUtf8;
        field.type = FieldTypeFromOgrFieldDefn(fieldDefn);
        field.sourceTypeTextUtf8 = OGRFieldDefn::GetFieldTypeName(fieldDefn->GetType());
        field.sourceSubTypeTextUtf8 = OGRFieldDefn::GetFieldSubTypeName(fieldDefn->GetSubType());
        field.width = fieldDefn->GetWidth();
        field.precision = fieldDefn->GetPrecision();
        field.maxLength = (field.type == GeoVectorFieldType::String && field.width > 0) ? field.width : 0;
        field.nullable = fieldDefn->IsNullable() != FALSE;
        field.unique = fieldDefn->IsUnique() != FALSE;
        field.ignored = fieldDefn->IsIgnored() != FALSE;
        field.editable = true;

        const char* defaultValue = fieldDefn->GetDefault();
        if (defaultValue != nullptr && defaultValue[0] != '\0')
        {
            field.hasDefaultValue = true;
            field.defaultValue = std::string(defaultValue);
        }

        field.domainNameUtf8 = fieldDefn->GetDomainName();
        field.commentUtf8 = fieldDefn->GetComment();

        field.rawSourceMap["name"] = field.nameUtf8;
        field.rawSourceMap["type"] = field.sourceTypeTextUtf8;
        field.rawSourceMap["subType"] = field.sourceSubTypeTextUtf8;
        field.rawSourceMap["width"] = field.width;
        field.rawSourceMap["precision"] = field.precision;
        field.rawSourceMap["nullable"] = field.nullable;
        field.rawSourceMap["unique"] = field.unique;
        field.rawSourceMap["ignored"] = field.ignored;
        field.rawSourceMap["domainName"] = field.domainNameUtf8;
        field.rawSourceMap["comment"] = field.commentUtf8;

        return field;
    }

    GeoVectorFields ReadFieldsFromLayerDefn(const OGRFeatureDefn* layerDefn)
    {
        GeoVectorFields fields;
        if (layerDefn == nullptr)
        {
            return fields;
        }

        const int fieldCount = layerDefn->GetFieldCount();
        fields.reserve(static_cast<size_t>(std::max(fieldCount, 0)));
        for (int fieldIndex = 0; fieldIndex < fieldCount; fieldIndex++)
        {
            fields.push_back(ConvertFieldDefn(layerDefn->GetFieldDefn(fieldIndex)));
        }
        return fields;
    }

    GeoIO_Shp::ShapefileGeometryKind GetShapefileGeometryKind(const OGRwkbGeometryType geometryType)
    {
        const OGRwkbGeometryType flatType = OGR_GT_Flatten(geometryType);
        switch (flatType)
        {
        case wkbNone:
            return GeoIO_Shp::ShapefileGeometryKind::NullShape;
        case wkbPoint:
            return GeoIO_Shp::ShapefileGeometryKind::Point;
        case wkbMultiPoint:
            return GeoIO_Shp::ShapefileGeometryKind::MultiPoint;
        case wkbLineString:
        case wkbMultiLineString:
            return GeoIO_Shp::ShapefileGeometryKind::Polyline;
        case wkbPolygon:
        case wkbMultiPolygon:
            return GeoIO_Shp::ShapefileGeometryKind::Polygon;
        case wkbTIN:
        case wkbPolyhedralSurface:
            return GeoIO_Shp::ShapefileGeometryKind::MultiPatch;
        default:
            return GeoIO_Shp::ShapefileGeometryKind::Unknown;
        }
    }

    GeoVectorGeometryType GetGeoGeometryType(const OGRwkbGeometryType geometryType)
    {
        const OGRwkbGeometryType flatType = OGR_GT_Flatten(geometryType);
        switch (flatType)
        {
        case wkbPoint:
        case wkbMultiPoint:
            return GeoVectorGeometryType::Point;
        case wkbLineString:
        case wkbMultiLineString:
            return GeoVectorGeometryType::Polyline;
        case wkbPolygon:
        case wkbMultiPolygon:
            return GeoVectorGeometryType::Polygon;
        default:
            return GeoVectorGeometryType::Unknown;
        }
    }

    GeoIO_Shp::LayerGeometryInfo ReadLayerGeometryInfo(OGRLayer* layer)
    {
        GeoIO_Shp::LayerGeometryInfo geometryInfo;
        if (layer == nullptr)
        {
            return geometryInfo;
        }

        const OGRwkbGeometryType geometryType = layer->GetGeomType();
        geometryInfo.sourceWkbGeometryTypeCode = static_cast<int>(geometryType);
        geometryInfo.sourceWkbGeometryTypeNameUtf8 = OGRGeometryTypeToName(geometryType);
        geometryInfo.shapefileGeometryKind = GetShapefileGeometryKind(geometryType);
        geometryInfo.geometryType = GetGeoGeometryType(geometryType);
        geometryInfo.hasZ = OGR_GT_HasZ(geometryType) != FALSE;
        geometryInfo.hasM = OGR_GT_HasM(geometryType) != FALSE;
        geometryInfo.droppedZ = geometryInfo.hasZ;
        geometryInfo.droppedM = geometryInfo.hasM;
        return geometryInfo;
    }

    std::string ExportSpatialReferenceToWktUtf8(const OGRSpatialReference* spatialReference)
    {
        if (spatialReference == nullptr)
        {
            return "";
        }

        OGRSpatialReference clonedSpatialReference(*spatialReference);
        clonedSpatialReference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        char* wkt = nullptr;
        if (clonedSpatialReference.exportToWkt(&wkt) != OGRERR_NONE || wkt == nullptr)
        {
            CPLFree(wkt);
            return "";
        }

        std::string result = wkt;
        CPLFree(wkt);
        return result;
    }

    GeoIO_Shp::LayerSpatialInfo ReadSpatialInfo(OGRLayer* layer)
    {
        GeoIO_Shp::LayerSpatialInfo spatialInfo;
        if (layer == nullptr)
        {
            return spatialInfo;
        }

        const OGRSpatialReference* spatialReference = layer->GetSpatialRef();
        spatialInfo.crsWktUtf8 = ExportSpatialReferenceToWktUtf8(spatialReference);
        spatialInfo.hasCrs = !spatialInfo.crsWktUtf8.empty();
        if (spatialInfo.hasCrs)
        {
            spatialInfo.authorityCodeUtf8 = GeoCrsManager::WktToAuthorityCodeUtf8(spatialInfo.crsWktUtf8);
        }

        OGREnvelope envelope;
        if (layer->GetExtent(&envelope, TRUE) == OGRERR_NONE)
        {
            spatialInfo.extent.Set(envelope.MinX, envelope.MinY, envelope.MaxX, envelope.MaxY);
            spatialInfo.hasExtent = spatialInfo.extent.IsValid();
            if (spatialInfo.hasExtent && spatialInfo.hasCrs)
            {
                spatialInfo.boundingBox.Set(spatialInfo.crsWktUtf8, spatialInfo.extent);
            }
        }

        return spatialInfo;
    }

    bool IsMultipartGeometry(const GeoVectorGeometry& geometry)
    {
        return geometry.GetPartCount() > 1;
    }

    std::vector<GB_Point2d> ConvertLineStringToPoints(const OGRLineString* lineString)
    {
        std::vector<GB_Point2d> points;
        if (lineString == nullptr)
        {
            return points;
        }

        const int pointCount = lineString->getNumPoints();
        points.reserve(static_cast<size_t>(std::max(pointCount, 0)));
        for (int pointIndex = 0; pointIndex < pointCount; pointIndex++)
        {
            points.push_back(GB_Point2d(lineString->getX(pointIndex), lineString->getY(pointIndex)));
        }
        return points;
    }

    bool AppendPointGeometry(const OGRGeometry* geometry, GeoVectorGeometry::PointDataType& points)
    {
        if (geometry == nullptr)
        {
            return false;
        }

        const OGRwkbGeometryType flatType = OGR_GT_Flatten(geometry->getGeometryType());
        if (flatType == wkbPoint)
        {
            const OGRPoint* point = geometry->toPoint();
            if (point == nullptr || point->IsEmpty())
            {
                return false;
            }

            points.push_back(GB_Point2d(point->getX(), point->getY()));
            return true;
        }

        if (flatType == wkbMultiPoint)
        {
            const OGRGeometryCollection* collection = geometry->toGeometryCollection();
            if (collection == nullptr)
            {
                return false;
            }

            for (int geometryIndex = 0; geometryIndex < collection->getNumGeometries(); geometryIndex++)
            {
                AppendPointGeometry(collection->getGeometryRef(geometryIndex), points);
            }
            return true;
        }

        return false;
    }

    bool AppendPolylineGeometry(const OGRGeometry* geometry, GeoVectorGeometry::PolylineDataType& polylines)
    {
        if (geometry == nullptr)
        {
            return false;
        }

        const OGRwkbGeometryType flatType = OGR_GT_Flatten(geometry->getGeometryType());
        if (flatType == wkbLineString)
        {
            const OGRLineString* lineString = geometry->toLineString();
            if (lineString == nullptr || lineString->IsEmpty())
            {
                return false;
            }

            GB_Polyline polyline(ConvertLineStringToPoints(lineString));
            if (!polyline.IsValid())
            {
                return false;
            }

            polylines.push_back(std::move(polyline));
            return true;
        }

        if (flatType == wkbMultiLineString)
        {
            const OGRGeometryCollection* collection = geometry->toGeometryCollection();
            if (collection == nullptr)
            {
                return false;
            }

            for (int geometryIndex = 0; geometryIndex < collection->getNumGeometries(); geometryIndex++)
            {
                AppendPolylineGeometry(collection->getGeometryRef(geometryIndex), polylines);
            }
            return true;
        }

        return false;
    }

    bool AppendPolygonRing(const OGRLineString* ring, GeoVectorGeometry::PolygonDataType& polygons)
    {
        if (ring == nullptr || ring->IsEmpty())
        {
            return false;
        }

        std::vector<GB_Point2d> points = ConvertLineStringToPoints(ring);
        if (points.size() < 3)
        {
            return false;
        }

        if (!points.front().IsNearEqual(points.back(), GB_Epsilon))
        {
            points.push_back(points.front());
        }
        else
        {
            points.back() = points.front();
        }

        GB_Polyline polygon(std::move(points));
        if (!polygon.IsValid() || !polygon.IsClosed())
        {
            return false;
        }

        polygons.push_back(std::move(polygon));
        return true;
    }

    bool AppendPolygonGeometry(const OGRGeometry* geometry, GeoVectorGeometry::PolygonDataType& polygons, bool& hasInteriorRing)
    {
        if (geometry == nullptr)
        {
            return false;
        }

        const OGRwkbGeometryType flatType = OGR_GT_Flatten(geometry->getGeometryType());
        if (flatType == wkbPolygon)
        {
            const OGRPolygon* polygon = geometry->toPolygon();
            if (polygon == nullptr || polygon->IsEmpty())
            {
                return false;
            }

            AppendPolygonRing(polygon->getExteriorRing(), polygons);
            const int interiorRingCount = polygon->getNumInteriorRings();
            if (interiorRingCount > 0)
            {
                hasInteriorRing = true;
            }
            for (int ringIndex = 0; ringIndex < interiorRingCount; ringIndex++)
            {
                AppendPolygonRing(polygon->getInteriorRing(ringIndex), polygons);
            }
            return true;
        }

        if (flatType == wkbMultiPolygon)
        {
            const OGRGeometryCollection* collection = geometry->toGeometryCollection();
            if (collection == nullptr)
            {
                return false;
            }

            for (int geometryIndex = 0; geometryIndex < collection->getNumGeometries(); geometryIndex++)
            {
                AppendPolygonGeometry(collection->getGeometryRef(geometryIndex), polygons, hasInteriorRing);
            }
            return true;
        }

        return false;
    }

    bool ConvertOgrGeometryToGeoGeometry(const OGRGeometry* ogrGeometry,
        GeoVectorGeometry& outGeometry,
        GeoIO_Shp::LayerGeometryInfo& geometryInfo,
        std::vector<GeoIO_Shp::ReadDiagnostic>& diagnostics,
        const long long featureFid)
    {
        outGeometry.Reset();
        if (ogrGeometry == nullptr || ogrGeometry->IsEmpty())
        {
            outGeometry.SetEmptyGeometry(geometryInfo.geometryType);
            geometryInfo.hasNullGeometry = true;
            return true;
        }

        const OGRwkbGeometryType ogrGeometryType = ogrGeometry->getGeometryType();
        if (OGR_GT_HasZ(ogrGeometryType) != FALSE)
        {
            geometryInfo.hasZ = true;
            geometryInfo.droppedZ = true;
        }
        if (OGR_GT_HasM(ogrGeometryType) != FALSE)
        {
            geometryInfo.hasM = true;
            geometryInfo.droppedM = true;
        }

        const GeoVectorGeometryType geoGeometryType = GetGeoGeometryType(ogrGeometryType);
        if (geometryInfo.geometryType == GeoVectorGeometryType::Unknown && geoGeometryType != GeoVectorGeometryType::Unknown)
        {
            geometryInfo.geometryType = geoGeometryType;
        }

        switch (geoGeometryType)
        {
        case GeoVectorGeometryType::Point:
        {
            GeoVectorGeometry::PointDataType points;
            if (!AppendPointGeometry(ogrGeometry, points))
            {
                return false;
            }
            outGeometry.SetPoints(std::move(points));
            break;
        }
        case GeoVectorGeometryType::Polyline:
        {
            GeoVectorGeometry::PolylineDataType polylines;
            if (!AppendPolylineGeometry(ogrGeometry, polylines))
            {
                return false;
            }
            outGeometry.SetPolylines(std::move(polylines));
            break;
        }
        case GeoVectorGeometryType::Polygon:
        {
            bool hasInteriorRing = false;
            GeoVectorGeometry::PolygonDataType polygons;
            if (!AppendPolygonGeometry(ogrGeometry, polygons, hasInteriorRing))
            {
                return false;
            }
            if (hasInteriorRing)
            {
                AddDiagnostic(diagnostics,
                    GeoIO_Shp::ReadDiagnosticLevel::Warning,
                    "PolygonInteriorRingFlattened",
                    "GeoVectorGeometry 当前没有单独的内环/洞结构，读取时已把 Polygon 内环作为独立闭合面 part 保存。",
                    featureFid);
            }
            outGeometry.SetPolygons(std::move(polygons), true);
            break;
        }
        case GeoVectorGeometryType::Unknown:
        default:
            return false;
        }

        if (outGeometry.GetPartCount() > 1 && OGR_GT_Flatten(ogrGeometryType) != wkbMultiPoint && OGR_GT_Flatten(ogrGeometryType) != wkbMultiLineString &&
            OGR_GT_Flatten(ogrGeometryType) != wkbMultiPolygon)
        {
            geometryInfo.hasMixedSingleAndMultipartGeometry = true;
        }

        return outGeometry.IsValid();
    }

    std::string FieldValueToDateString(const OGRFeature* feature, const int fieldIndex)
    {
        if (feature == nullptr)
        {
            return "";
        }

        int year = 0;
        int month = 0;
        int day = 0;
        int hour = 0;
        int minute = 0;
        float second = 0.0f;
        int timeZoneFlag = 0;
        if (feature->GetFieldAsDateTime(fieldIndex, &year, &month, &day, &hour, &minute, &second, &timeZoneFlag) == FALSE)
        {
            const char* text = feature->GetFieldAsString(fieldIndex);
            return text == nullptr ? "" : std::string(text);
        }

        std::ostringstream stream;
        stream << std::setfill('0') << std::setw(4) << year << "-" << std::setw(2) << month << "-" << std::setw(2) << day;
        return stream.str();
    }

    std::string FieldValueToTimeString(const OGRFeature* feature, const int fieldIndex)
    {
        if (feature == nullptr)
        {
            return "";
        }

        int year = 0;
        int month = 0;
        int day = 0;
        int hour = 0;
        int minute = 0;
        float second = 0.0f;
        int timeZoneFlag = 0;
        if (feature->GetFieldAsDateTime(fieldIndex, &year, &month, &day, &hour, &minute, &second, &timeZoneFlag) == FALSE)
        {
            const char* text = feature->GetFieldAsString(fieldIndex);
            return text == nullptr ? "" : std::string(text);
        }

        std::ostringstream stream;
        stream << std::setfill('0') << std::setw(2) << hour << ":" << std::setw(2) << minute << ":" << std::setw(2) << static_cast<int>(second);
        return stream.str();
    }

    std::string FieldValueToDateTimeString(const OGRFeature* feature, const int fieldIndex)
    {
        if (feature == nullptr)
        {
            return "";
        }

        int year = 0;
        int month = 0;
        int day = 0;
        int hour = 0;
        int minute = 0;
        float second = 0.0f;
        int timeZoneFlag = 0;
        if (feature->GetFieldAsDateTime(fieldIndex, &year, &month, &day, &hour, &minute, &second, &timeZoneFlag) == FALSE)
        {
            const char* text = feature->GetFieldAsString(fieldIndex);
            return text == nullptr ? "" : std::string(text);
        }

        std::ostringstream stream;
        stream << std::setfill('0') << std::setw(4) << year << "-" << std::setw(2) << month << "-" << std::setw(2) << day
            << "T" << std::setw(2) << hour << ":" << std::setw(2) << minute << ":" << std::setw(2) << static_cast<int>(second);
        return stream.str();
    }

    GB_Variant ReadFieldValue(const OGRFeature* feature, const int fieldIndex, const OGRFieldDefn* fieldDefn)
    {
        if (feature == nullptr || fieldDefn == nullptr || !feature->IsFieldSetAndNotNull(fieldIndex))
        {
            return GB_Variant();
        }

        const OGRFieldType fieldType = fieldDefn->GetType();
        const OGRFieldSubType fieldSubType = fieldDefn->GetSubType();
        switch (fieldType)
        {
        case OFTInteger:
            if (fieldSubType == OFSTBoolean)
            {
                return feature->GetFieldAsInteger(fieldIndex) != 0;
            }
            return feature->GetFieldAsInteger(fieldIndex);
        case OFTInteger64:
            return static_cast<long long>(feature->GetFieldAsInteger64(fieldIndex));
        case OFTReal:
            return feature->GetFieldAsDouble(fieldIndex);
        case OFTString:
        case OFTWideString:
        {
            const char* text = feature->GetFieldAsString(fieldIndex);
            return std::string(text == nullptr ? "" : text);
        }
        case OFTDate:
            return FieldValueToDateString(feature, fieldIndex);
        case OFTTime:
            return FieldValueToTimeString(feature, fieldIndex);
        case OFTDateTime:
            return FieldValueToDateTimeString(feature, fieldIndex);
        case OFTBinary:
        {
            int byteCount = 0;
            const GByte* bytes = feature->GetFieldAsBinary(fieldIndex, &byteCount);
            if (bytes == nullptr || byteCount <= 0)
            {
                return GB_Variant();
            }

            GB_ByteBuffer buffer(bytes, bytes + byteCount);
            return buffer;
        }
        default:
        {
            const char* text = feature->GetFieldAsString(fieldIndex);
            return std::string(text == nullptr ? "" : text);
        }
        }
    }

    bool ReadFeatureAttributes(const OGRFeature* ogrFeature, const OGRFeatureDefn* layerDefn, GB_VariantList& outAttributes)
    {
        outAttributes.clear();
        if (ogrFeature == nullptr || layerDefn == nullptr)
        {
            return false;
        }

        const int fieldCount = layerDefn->GetFieldCount();
        outAttributes.reserve(static_cast<size_t>(std::max(fieldCount, 0)));
        for (int fieldIndex = 0; fieldIndex < fieldCount; fieldIndex++)
        {
            outAttributes.push_back(ReadFieldValue(ogrFeature, fieldIndex, layerDefn->GetFieldDefn(fieldIndex)));
        }
        return true;
    }

    bool IsStandaloneDbfPath(const std::string& filePathUtf8)
    {
        return GetFileExtensionLower(filePathUtf8) == ".dbf";
    }

    OGRLayer* FindLayer(GDALDataset* dataset, const GeoIO_Shp::ReadOptions& options, std::string* errorMessageUtf8)
    {
        if (dataset == nullptr)
        {
            SetErrorMessage(errorMessageUtf8, "GDAL 数据集为空。 ");
            return nullptr;
        }

        if (!options.layerNameUtf8.empty())
        {
            OGRLayer* layer = dataset->GetLayerByName(options.layerNameUtf8.c_str());
            if (layer == nullptr)
            {
                SetErrorMessage(errorMessageUtf8, "找不到指定图层：" + options.layerNameUtf8);
            }
            return layer;
        }

        if (options.layerIndex < 0 || options.layerIndex >= dataset->GetLayerCount())
        {
            std::ostringstream stream;
            stream << "图层下标越界：" << options.layerIndex << "，数据集图层数量为 " << dataset->GetLayerCount() << "。";
            SetErrorMessage(errorMessageUtf8, stream.str());
            return nullptr;
        }

        return dataset->GetLayer(options.layerIndex);
    }

    OGRFieldType GetWritableOgrFieldType(const GeoVectorField& field, bool& isSupported)
    {
        isSupported = true;
        switch (field.type)
        {
        case GeoVectorFieldType::Bool:
        case GeoVectorFieldType::Int16:
        case GeoVectorFieldType::Int32:
            return OFTInteger;
        case GeoVectorFieldType::ObjectId:
        case GeoVectorFieldType::Int64:
            return OFTInteger64;
        case GeoVectorFieldType::Float:
        case GeoVectorFieldType::Double:
            return OFTReal;
        case GeoVectorFieldType::Date:
            return OFTDate;
        case GeoVectorFieldType::GlobalId:
        case GeoVectorFieldType::Guid:
        case GeoVectorFieldType::String:
        case GeoVectorFieldType::DateTime:
        case GeoVectorFieldType::Time:
        case GeoVectorFieldType::TimestampOffset:
        case GeoVectorFieldType::Xml:
            return OFTString;
        case GeoVectorFieldType::Blob:
        case GeoVectorFieldType::Raster:
        case GeoVectorFieldType::Geometry:
        case GeoVectorFieldType::Unknown:
        default:
            isSupported = false;
            return OFTString;
        }
    }

    void ConfigureOgrFieldDefn(const GeoVectorField& field, OGRFieldDefn& fieldDefn)
    {
        if (field.type == GeoVectorFieldType::Bool)
        {
            fieldDefn.SetSubType(OFSTBoolean);
        }
        else if (field.type == GeoVectorFieldType::Int16)
        {
            fieldDefn.SetSubType(OFSTInt16);
        }
        else if (field.type == GeoVectorFieldType::Float)
        {
            fieldDefn.SetSubType(OFSTFloat32);
        }

        int width = field.width;
        int precision = field.precision;
        if (width <= 0)
        {
            switch (field.type)
            {
            case GeoVectorFieldType::Bool:
                width = 1;
                break;
            case GeoVectorFieldType::Int16:
                width = 6;
                break;
            case GeoVectorFieldType::Int32:
                width = 11;
                break;
            case GeoVectorFieldType::ObjectId:
            case GeoVectorFieldType::Int64:
                width = 20;
                break;
            case GeoVectorFieldType::Float:
            case GeoVectorFieldType::Double:
                width = 24;
                break;
            case GeoVectorFieldType::Date:
                width = 10;
                break;
            case GeoVectorFieldType::String:
            case GeoVectorFieldType::GlobalId:
            case GeoVectorFieldType::Guid:
            case GeoVectorFieldType::DateTime:
            case GeoVectorFieldType::Time:
            case GeoVectorFieldType::TimestampOffset:
            case GeoVectorFieldType::Xml:
            default:
                width = field.GetTextLengthLimit() > 0 ? field.GetTextLengthLimit() : 254;
                width = std::min(width, 254);
                break;
            }
        }

        if (precision <= 0 && (field.type == GeoVectorFieldType::Float || field.type == GeoVectorFieldType::Double))
        {
            precision = 15;
        }

        if (width > 0)
        {
            fieldDefn.SetWidth(width);
        }
        if (precision > 0)
        {
            fieldDefn.SetPrecision(precision);
        }
        fieldDefn.SetNullable(field.nullable ? TRUE : FALSE);
        fieldDefn.SetUnique(field.unique ? TRUE : FALSE);
        fieldDefn.SetIgnored(field.ignored ? TRUE : FALSE);
        if (field.hasDefaultValue)
        {
            const std::string defaultValue = field.defaultValue.ToString();
            if (!defaultValue.empty())
            {
                fieldDefn.SetDefault(defaultValue.c_str());
            }
        }
    }

    GeoVectorGeometryType InferGeometryTypeFromData(const GeoIO_Shp::ShpData& data, const GeoIO_Shp::WriteOptions& options)
    {
        if (options.geometryType != GeoVectorGeometryType::Unknown)
        {
            return options.geometryType;
        }
        if (data.geometryInfo.geometryType != GeoVectorGeometryType::Unknown)
        {
            return data.geometryInfo.geometryType;
        }

        for (size_t featureIndex = 0; featureIndex < data.features.size(); featureIndex++)
        {
            const GeoVectorGeometryType geometryType = data.features[featureIndex].GetGeometryType();
            if (geometryType != GeoVectorGeometryType::Unknown)
            {
                return geometryType;
            }
        }

        return GeoVectorGeometryType::Unknown;
    }

    bool HasMultipartFeatureOfType(const GeoIO_Shp::ShpData& data, const GeoVectorGeometryType geometryType)
    {
        for (size_t featureIndex = 0; featureIndex < data.features.size(); featureIndex++)
        {
            if (data.features[featureIndex].GetGeometryType() == geometryType && data.features[featureIndex].geometry.GetPartCount() > 1)
            {
                return true;
            }
        }
        return false;
    }

    OGRwkbGeometryType GetOgrLayerGeometryType(const GeoIO_Shp::ShpData& data, const GeoIO_Shp::WriteOptions& options)
    {
        const GeoVectorGeometryType geometryType = InferGeometryTypeFromData(data, options);
        switch (geometryType)
        {
        case GeoVectorGeometryType::Point:
            return HasMultipartFeatureOfType(data, GeoVectorGeometryType::Point) ? wkbMultiPoint : wkbPoint;
        case GeoVectorGeometryType::Polyline:
            return HasMultipartFeatureOfType(data, GeoVectorGeometryType::Polyline) ? wkbMultiLineString : wkbLineString;
        case GeoVectorGeometryType::Polygon:
            return HasMultipartFeatureOfType(data, GeoVectorGeometryType::Polygon) ? wkbMultiPolygon : wkbPolygon;
        case GeoVectorGeometryType::Unknown:
        default:
            return wkbNone;
        }
    }

    std::string GetShptLayerCreationValue(const GeoVectorGeometryType geometryType, const bool useMultiPoint)
    {
        switch (geometryType)
        {
        case GeoVectorGeometryType::Point:
            return useMultiPoint ? "MULTIPOINT" : "POINT";
        case GeoVectorGeometryType::Polyline:
            return "ARC";
        case GeoVectorGeometryType::Polygon:
            return "POLYGON";
        case GeoVectorGeometryType::Unknown:
        default:
            return "NULL";
        }
    }

    OGRSpatialReference* CreateSpatialReference(const std::string& wktUtf8)
    {
        if (wktUtf8.empty())
        {
            return nullptr;
        }

        std::unique_ptr<OGRSpatialReference, OgrSpatialReferenceDeleter> spatialReference(new OGRSpatialReference());
        spatialReference->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        const char* wktText = wktUtf8.c_str();
        if (spatialReference->importFromWkt(&wktText) != OGRERR_NONE)
        {
            return nullptr;
        }
        return spatialReference.release();
    }

    OGRGeometry* CreateOgrLineStringFromPolyline(const GB_Polyline& polyline)
    {
        const std::vector<GB_Point2d> vertices = polyline.GetVertices();
        if (vertices.empty())
        {
            return nullptr;
        }

        OGRLineString* lineString = new OGRLineString();
        for (size_t vertexIndex = 0; vertexIndex < vertices.size(); vertexIndex++)
        {
            if (!vertices[vertexIndex].IsValid())
            {
                delete lineString;
                return nullptr;
            }
            lineString->addPoint(vertices[vertexIndex].x, vertices[vertexIndex].y);
        }
        return lineString;
    }

    OGRLinearRing* CreateOgrLinearRingFromPolyline(const GB_Polyline& polyline)
    {
        std::vector<GB_Point2d> vertices = polyline.GetVertices();
        if (vertices.size() < 3)
        {
            return nullptr;
        }

        if (!vertices.front().IsNearEqual(vertices.back(), GB_Epsilon))
        {
            vertices.push_back(vertices.front());
        }
        else
        {
            vertices.back() = vertices.front();
        }

        if (vertices.size() < 4)
        {
            return nullptr;
        }

        OGRLinearRing* ring = new OGRLinearRing();
        for (size_t vertexIndex = 0; vertexIndex < vertices.size(); vertexIndex++)
        {
            if (!vertices[vertexIndex].IsValid())
            {
                delete ring;
                return nullptr;
            }
            ring->addPoint(vertices[vertexIndex].x, vertices[vertexIndex].y);
        }
        return ring;
    }

    OGRGeometry* CreateOgrPolygonFromPolyline(const GB_Polyline& polygonPart)
    {
        OGRLinearRing* ring = CreateOgrLinearRingFromPolyline(polygonPart);
        if (ring == nullptr)
        {
            return nullptr;
        }

        OGRPolygon* polygon = new OGRPolygon();
        polygon->addRingDirectly(ring);
        return polygon;
    }

    OGRGeometry* CreateOgrGeometryFromGeoGeometry(const GeoVectorGeometry& geometry)
    {
        if (!geometry.IsValid() || geometry.IsEmpty())
        {
            return nullptr;
        }

        switch (geometry.GetGeometryType())
        {
        case GeoVectorGeometryType::Point:
        {
            const GeoVectorGeometry::PointDataType& points = geometry.GetPoints();
            if (points.empty())
            {
                return nullptr;
            }
            if (points.size() == 1)
            {
                return new OGRPoint(points[0].x, points[0].y);
            }

            OGRMultiPoint* multiPoint = new OGRMultiPoint();
            for (size_t pointIndex = 0; pointIndex < points.size(); pointIndex++)
            {
                if (!points[pointIndex].IsValid())
                {
                    delete multiPoint;
                    return nullptr;
                }
                multiPoint->addGeometryDirectly(new OGRPoint(points[pointIndex].x, points[pointIndex].y));
            }
            return multiPoint;
        }
        case GeoVectorGeometryType::Polyline:
        {
            const GeoVectorGeometry::PolylineDataType& polylines = geometry.GetPolylines();
            if (polylines.empty())
            {
                return nullptr;
            }
            if (polylines.size() == 1)
            {
                return CreateOgrLineStringFromPolyline(polylines[0]);
            }

            OGRMultiLineString* multiLineString = new OGRMultiLineString();
            for (size_t polylineIndex = 0; polylineIndex < polylines.size(); polylineIndex++)
            {
                OGRGeometry* lineString = CreateOgrLineStringFromPolyline(polylines[polylineIndex]);
                if (lineString == nullptr)
                {
                    delete multiLineString;
                    return nullptr;
                }
                multiLineString->addGeometryDirectly(lineString);
            }
            return multiLineString;
        }
        case GeoVectorGeometryType::Polygon:
        {
            const GeoVectorGeometry::PolygonDataType& polygons = geometry.GetPolygons();
            if (polygons.empty())
            {
                return nullptr;
            }
            if (polygons.size() == 1)
            {
                return CreateOgrPolygonFromPolyline(polygons[0]);
            }

            OGRMultiPolygon* multiPolygon = new OGRMultiPolygon();
            for (size_t polygonIndex = 0; polygonIndex < polygons.size(); polygonIndex++)
            {
                OGRGeometry* polygon = CreateOgrPolygonFromPolyline(polygons[polygonIndex]);
                if (polygon == nullptr)
                {
                    delete multiPolygon;
                    return nullptr;
                }
                multiPolygon->addGeometryDirectly(polygon);
            }
            return multiPolygon;
        }
        case GeoVectorGeometryType::Unknown:
        default:
            return nullptr;
        }
    }

    bool IsGeometryCompatible(const GeoVectorGeometryType layerGeometryType, const GeoVectorGeometryType featureGeometryType)
    {
        if (featureGeometryType == GeoVectorGeometryType::Unknown)
        {
            return true;
        }
        return layerGeometryType == featureGeometryType;
    }

    bool TryParseDateString(const std::string& text, int& outYear, int& outMonth, int& outDay)
    {
        outYear = 0;
        outMonth = 0;
        outDay = 0;
        if (text.size() < 10 || text[4] != '-' || text[7] != '-')
        {
            return false;
        }

        outYear = std::atoi(text.substr(0, 4).c_str());
        outMonth = std::atoi(text.substr(5, 2).c_str());
        outDay = std::atoi(text.substr(8, 2).c_str());
        return outYear > 0 && outMonth >= 1 && outMonth <= 12 && outDay >= 1 && outDay <= 31;
    }

    void SetOgrFieldValue(OGRFeature* ogrFeature, const int targetFieldIndex, const OGRFieldDefn* targetFieldDefn, const GB_Variant& value)
    {
        if (ogrFeature == nullptr || targetFieldDefn == nullptr)
        {
            return;
        }

        if (value.IsEmpty())
        {
            ogrFeature->SetFieldNull(targetFieldIndex);
            return;
        }

        bool ok = false;
        switch (targetFieldDefn->GetType())
        {
        case OFTInteger:
        {
            const int intValue = value.ToInt(&ok);
            if (ok)
            {
                ogrFeature->SetField(targetFieldIndex, intValue);
            }
            else
            {
                ogrFeature->SetFieldNull(targetFieldIndex);
            }
            break;
        }
        case OFTInteger64:
        {
            const long long int64Value = value.ToInt64(&ok);
            if (ok)
            {
                ogrFeature->SetField(targetFieldIndex, static_cast<GIntBig>(int64Value));
            }
            else
            {
                ogrFeature->SetFieldNull(targetFieldIndex);
            }
            break;
        }
        case OFTReal:
        {
            const double doubleValue = value.ToDouble(&ok);
            if (ok)
            {
                ogrFeature->SetField(targetFieldIndex, doubleValue);
            }
            else
            {
                ogrFeature->SetFieldNull(targetFieldIndex);
            }
            break;
        }
        case OFTDate:
        {
            const std::string dateText = value.ToString(&ok);
            int year = 0;
            int month = 0;
            int day = 0;
            if (ok && TryParseDateString(dateText, year, month, day))
            {
                ogrFeature->SetField(targetFieldIndex, year, month, day, 0, 0, 0.0f, 0);
            }
            else
            {
                ogrFeature->SetFieldNull(targetFieldIndex);
            }
            break;
        }
        case OFTString:
        case OFTWideString:
        default:
        {
            const std::string textValue = value.ToString(&ok);
            if (ok)
            {
                ogrFeature->SetField(targetFieldIndex, textValue.c_str());
            }
            else
            {
                ogrFeature->SetFieldNull(targetFieldIndex);
            }
            break;
        }
        }
    }

    bool WriteCpgFileIfNeeded(const std::string& shpFilePathUtf8, const GeoIO_Shp::WriteOptions& options)
    {
        if (!options.writeCpgFile || options.encodingUtf8.empty())
        {
            return true;
        }

        const std::string cpgFilePath = GetBasePathWithoutKnownExtension(shpFilePathUtf8) + ".cpg";
        return GB_WriteUtf8ToFile(cpgFilePath, options.encodingUtf8, false, false);
    }

    bool DeleteExistingShapefileSet(const std::string& filePathUtf8, GDALDriver* driver)
    {
        if (driver != nullptr && GB_IsFileExists(filePathUtf8))
        {
            if (driver->Delete(filePathUtf8.c_str()) == CE_None)
            {
                return true;
            }
        }

        GeoIO_Shp::SourceFileSet sourceFileSet = GeoIO_Shp::CollectSourceFileSet(filePathUtf8);
        bool ok = true;
        for (size_t fileIndex = 0; fileIndex < sourceFileSet.files.size(); fileIndex++)
        {
            if (sourceFileSet.files[fileIndex].exists)
            {
                ok = GB_DeleteFile(sourceFileSet.files[fileIndex].filePathUtf8) && ok;
            }
        }
        return ok;
    }
}

std::string GeoIO_Shp::SourceFileSet::GetFilePathUtf8(const SourceFileRole role) const
{
    for (size_t i = 0; i < files.size(); i++)
    {
        if (files[i].role == role)
        {
            return files[i].filePathUtf8;
        }
    }
    return "";
}

bool GeoIO_Shp::SourceFileSet::HasFile(const SourceFileRole role) const
{
    for (size_t i = 0; i < files.size(); i++)
    {
        if (files[i].role == role && files[i].exists)
        {
            return true;
        }
    }
    return false;
}

bool GeoIO_Shp::ShpData::IsValid() const
{
    return sourceInfo.sourceType != SourceType::Unknown && !layerNameUtf8.empty();
}

void GeoIO_Shp::ShpData::Reset()
{
    *this = ShpData();
}

GeoIO_Shp::SourceFileSet GeoIO_Shp::CollectSourceFileSet(const std::string& shpOrDbfFilePathUtf8)
{
    SourceFileSet sourceFileSet;
    const std::string basePath = GetBasePathWithoutKnownExtension(shpOrDbfFilePathUtf8);
    sourceFileSet.files.reserve(9);
    sourceFileSet.files.push_back(MakeSourceFile(SourceFileRole::Shp, basePath + ".shp"));
    sourceFileSet.files.push_back(MakeSourceFile(SourceFileRole::Shx, basePath + ".shx"));
    sourceFileSet.files.push_back(MakeSourceFile(SourceFileRole::Dbf, basePath + ".dbf"));
    sourceFileSet.files.push_back(MakeSourceFile(SourceFileRole::Prj, basePath + ".prj"));
    sourceFileSet.files.push_back(MakeSourceFile(SourceFileRole::Cpg, basePath + ".cpg"));
    sourceFileSet.files.push_back(MakeSourceFile(SourceFileRole::Qix, basePath + ".qix"));
    sourceFileSet.files.push_back(MakeSourceFile(SourceFileRole::Sbn, basePath + ".sbn"));
    sourceFileSet.files.push_back(MakeSourceFile(SourceFileRole::Sbx, basePath + ".sbx"));
    sourceFileSet.files.push_back(MakeSourceFile(SourceFileRole::ShpXml, basePath + ".shp.xml"));
    return sourceFileSet;
}

bool GeoIO_Shp::IsSupportedFilePath(const std::string& filePathUtf8)
{
    const std::string extension = GetFileExtensionLower(filePathUtf8);
    return extension == ".shp" || extension == ".dbf";
}

bool GeoIO_Shp::Read(const std::string& filePathUtf8, ShpData& outData, const ReadOptions& options, std::string* errorMessageUtf8)
{
    outData.Reset();
    SetErrorMessage(errorMessageUtf8, "");

    if (filePathUtf8.empty())
    {
        SetErrorMessage(errorMessageUtf8, "输入文件路径为空。 ");
        return false;
    }
    if (!IsSupportedFilePath(filePathUtf8))
    {
        SetErrorMessage(errorMessageUtf8, "仅支持读取 .shp 或 .dbf 文件：" + filePathUtf8);
        return false;
    }

    EnsureGdalRegistered();

    std::unique_ptr<ScopedGdalConfigOption> organizePolygonsOption;
    if (options.organizePolygons)
    {
        organizePolygonsOption.reset(new ScopedGdalConfigOption("OGR_ORGANIZE_POLYGONS", "DEFAULT"));
    }

    CslStringListGuard openOptions;
    if (!options.overrideEncodingUtf8.empty())
    {
        openOptions.SetNameValue("ENCODING", options.overrideEncodingUtf8.c_str());
    }
    else if (!options.recodeTextToUtf8)
    {
        openOptions.SetNameValue("ENCODING", "");
    }

    std::unique_ptr<GDALDataset, GdalDatasetDeleter> dataset(static_cast<GDALDataset*>(GDALOpenEx(filePathUtf8.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, openOptions.Get(), nullptr)));
    if (!dataset)
    {
        SetErrorMessage(errorMessageUtf8, "GDAL 无法打开矢量数据源：" + filePathUtf8);
        return false;
    }

    OGRLayer* layer = FindLayer(dataset.get(), options, errorMessageUtf8);
    if (layer == nullptr)
    {
        outData.Reset();
        return false;
    }

    ShpData data;
    data.sourceInfo.sourceType = IsStandaloneDbfPath(filePathUtf8) ? SourceType::StandaloneDbf : SourceType::EsriShapefile;
    data.sourceInfo.driverNameUtf8 = dataset->GetDriver() == nullptr ? "" : dataset->GetDriver()->GetDescription();
    data.sourceInfo.datasetPathUtf8 = filePathUtf8;
    data.sourceInfo.layerIndex = options.layerIndex;
    data.sourceInfo.layerNameUtf8 = layer->GetName() == nullptr ? "" : layer->GetName();
    data.layerNameUtf8 = data.sourceInfo.layerNameUtf8.empty() ? GetFileStem(filePathUtf8) : data.sourceInfo.layerNameUtf8;
    data.displayNameUtf8 = data.layerNameUtf8;

    if (options.collectSourceFiles)
    {
        data.sourceInfo.sourceFiles = CollectSourceFileSet(filePathUtf8);
    }

    data.geometryInfo = ReadLayerGeometryInfo(layer);
    data.spatialInfo = ReadSpatialInfo(layer);
    data.sourceInfo.encodingInfo = ReadEncodingInfo(layer, data.sourceInfo.sourceFiles, options);

    if (options.loadMetadata)
    {
        AppendMetadataItems(dataset.get(), "dataset", data.sourceInfo.metadataItems);
        AppendMetadataItems(layer, "layer", data.sourceInfo.metadataItems);
        data.metadataItems = data.sourceInfo.metadataItems;
    }

    const OGRFeatureDefn* layerDefn = layer->GetLayerDefn();
    if (options.loadFields)
    {
        data.fields = ReadFieldsFromLayerDefn(layerDefn);
    }
    data.statistics.fieldCount = data.fields.size();
    data.statistics.featureCount = static_cast<long long>(layer->GetFeatureCount(FALSE));

    if (!options.loadFeatures)
    {
        outData = std::move(data);
        return true;
    }

    layer->ResetReading();
    while (true)
    {
        std::unique_ptr<OGRFeature, OgrFeatureDeleter> ogrFeature(layer->GetNextFeature());
        if (!ogrFeature)
        {
            break;
        }

        if (options.maxFeatureCount > 0 && data.features.size() >= options.maxFeatureCount)
        {
            AddDiagnostic(data.diagnostics,
                ReadDiagnosticLevel::Info,
                "MaxFeatureCountReached",
                "读取数量已达到 ReadOptions::maxFeatureCount，后续要素未继续读取。 ");
            break;
        }

        const long long featureFid = static_cast<long long>(ogrFeature->GetFID());
        GeoVectorFeature feature;
        if (options.useGdalFeatureIdAsFid)
        {
            feature.SetFid(featureFid);
        }

        if (!ReadFeatureAttributes(ogrFeature.get(), layerDefn, feature.attributes))
        {
            AddDiagnostic(data.diagnostics, ReadDiagnosticLevel::Warning, "ReadAttributesFailed", "读取要素属性失败。", featureFid);
        }

        GeoVectorGeometry geometry;
        const OGRGeometry* ogrGeometry = ogrFeature->GetGeometryRef();
        const bool isNullGeometry = (ogrGeometry == nullptr || ogrGeometry->IsEmpty());
        if (isNullGeometry)
        {
            data.statistics.nullGeometryFeatureCount++;
            data.geometryInfo.hasNullGeometry = true;
            if (!options.includeNullGeometryFeatures)
            {
                continue;
            }
            geometry.SetEmptyGeometry(data.geometryInfo.geometryType);
        }
        else if (!ConvertOgrGeometryToGeoGeometry(ogrGeometry, geometry, data.geometryInfo, data.diagnostics, featureFid))
        {
            data.geometryInfo.hasUnsupportedGeometry = true;
            data.statistics.unsupportedGeometryFeatureCount++;
            AddDiagnostic(data.diagnostics,
                ReadDiagnosticLevel::Warning,
                "UnsupportedGeometry",
                std::string("暂不支持的几何类型：") + OGRGeometryTypeToName(ogrGeometry->getGeometryType()),
                featureFid);

            if (options.skipUnsupportedGeometries)
            {
                continue;
            }
            geometry.SetEmptyGeometry(data.geometryInfo.geometryType);
        }

        feature.geometry = std::move(geometry);
        data.features.push_back(std::move(feature));
    }

    data.statistics.loadedFeatureCount = data.features.size();
    outData = std::move(data);
    return true;
}

bool GeoIO_Shp::Write(const std::string& filePathUtf8, const ShpData& data, const WriteOptions& options, std::string* errorMessageUtf8)
{
    SetErrorMessage(errorMessageUtf8, "");

    if (filePathUtf8.empty())
    {
        SetErrorMessage(errorMessageUtf8, "输出文件路径为空。 ");
        return false;
    }
    if (GetFileExtensionLower(filePathUtf8) != ".shp")
    {
        SetErrorMessage(errorMessageUtf8, "Shapefile 写出路径必须以 .shp 结尾：" + filePathUtf8);
        return false;
    }

    const GeoVectorGeometryType layerGeometryType = InferGeometryTypeFromData(data, options);
    if (layerGeometryType == GeoVectorGeometryType::Unknown && !data.features.empty())
    {
        SetErrorMessage(errorMessageUtf8, "无法从输入数据推断 Shapefile 几何类型。 ");
        return false;
    }

    EnsureGdalRegistered();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (driver == nullptr)
    {
        SetErrorMessage(errorMessageUtf8, "当前 GDAL 环境未注册 ESRI Shapefile 驱动。 ");
        return false;
    }

    if (GB_IsFileExists(filePathUtf8))
    {
        if (!options.overwrite)
        {
            SetErrorMessage(errorMessageUtf8, "目标 Shapefile 已存在，且 WriteOptions::overwrite=false：" + filePathUtf8);
            return false;
        }
        if (!DeleteExistingShapefileSet(filePathUtf8, driver))
        {
            SetErrorMessage(errorMessageUtf8, "删除已有 Shapefile 文件集失败：" + filePathUtf8);
            return false;
        }
    }

    const std::string outputDirectory = GetDirectoryPath(filePathUtf8);
    if (!outputDirectory.empty() && !GB_CreateDirectory(outputDirectory))
    {
        SetErrorMessage(errorMessageUtf8, "创建输出目录失败：" + outputDirectory);
        return false;
    }

    std::unique_ptr<GDALDataset, GdalDatasetDeleter> dataset(driver->Create(filePathUtf8.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    if (!dataset)
    {
        SetErrorMessage(errorMessageUtf8, "创建 Shapefile 数据源失败：" + filePathUtf8);
        return false;
    }

    const OGRwkbGeometryType ogrLayerGeometryType = GetOgrLayerGeometryType(data, options);
    const bool useMultiPoint = (ogrLayerGeometryType == wkbMultiPoint || HasMultipartFeatureOfType(data, GeoVectorGeometryType::Point));
    const std::string shptValue = GetShptLayerCreationValue(layerGeometryType, useMultiPoint);

    CslStringListGuard layerCreationOptions;
    if (!shptValue.empty())
    {
        layerCreationOptions.SetNameValue("SHPT", shptValue.c_str());
    }
    if (!options.encodingUtf8.empty())
    {
        layerCreationOptions.SetNameValue("ENCODING", options.encodingUtf8.c_str());
    }

    const std::string layerName = !options.layerNameUtf8.empty() ? options.layerNameUtf8 : (!data.layerNameUtf8.empty() ? data.layerNameUtf8 : GetFileStem(filePathUtf8));
    const std::string crsWkt = !options.crsWktUtf8.empty() ? options.crsWktUtf8 : data.spatialInfo.crsWktUtf8;
    std::unique_ptr<OGRSpatialReference, OgrSpatialReferenceDeleter> spatialReference(CreateSpatialReference(crsWkt));

    OGRLayer* layer = dataset->CreateLayer(layerName.c_str(), spatialReference.get(), ogrLayerGeometryType, layerCreationOptions.Get());
    if (layer == nullptr)
    {
        SetErrorMessage(errorMessageUtf8, "创建 Shapefile 图层失败：" + layerName);
        return false;
    }

    std::vector<int> fieldIndexMap(data.fields.size(), -1);
    for (size_t fieldIndex = 0; fieldIndex < data.fields.size(); fieldIndex++)
    {
        const GeoVectorField& field = data.fields[fieldIndex];
        bool isSupported = true;
        const OGRFieldType ogrFieldType = GetWritableOgrFieldType(field, isSupported);
        if (!isSupported)
        {
            if (options.skipUnsupportedFields)
            {
                continue;
            }
            SetErrorMessage(errorMessageUtf8, "Shapefile 不支持字段类型：" + field.nameUtf8 + " / " + GeoVectorFieldsHelper::FieldTypeToString(field.type));
            return false;
        }

        OGRFieldDefn fieldDefn(field.nameUtf8.c_str(), ogrFieldType);
        ConfigureOgrFieldDefn(field, fieldDefn);
        if (layer->CreateField(&fieldDefn, options.approximateFieldDefinition ? TRUE : FALSE) != OGRERR_NONE)
        {
            SetErrorMessage(errorMessageUtf8, "创建字段失败：" + field.nameUtf8);
            return false;
        }

        const OGRFeatureDefn* layerDefn = layer->GetLayerDefn();
        fieldIndexMap[fieldIndex] = layerDefn == nullptr ? -1 : (layerDefn->GetFieldCount() - 1);
    }

    OGRFeatureDefn* layerDefn = layer->GetLayerDefn();
    if (layerDefn == nullptr)
    {
        SetErrorMessage(errorMessageUtf8, "获取输出图层字段定义失败。 ");
        return false;
    }

    for (size_t featureIndex = 0; featureIndex < data.features.size(); featureIndex++)
    {
        const GeoVectorFeature& sourceFeature = data.features[featureIndex];
        if (!IsGeometryCompatible(layerGeometryType, sourceFeature.GetGeometryType()))
        {
            SetErrorMessage(errorMessageUtf8, "要素几何类型与输出 Shapefile 图层几何类型不一致。 ");
            return false;
        }

        if (sourceFeature.geometry.IsEmpty() && options.skipEmptyGeometryFeatures)
        {
            continue;
        }

        std::unique_ptr<OGRFeature, OgrFeatureDeleter> ogrFeature(OGRFeature::CreateFeature(layerDefn));
        if (!ogrFeature)
        {
            SetErrorMessage(errorMessageUtf8, "创建 OGRFeature 失败。 ");
            return false;
        }

        if (options.preserveFeatureId && sourceFeature.HasValidFid())
        {
            ogrFeature->SetFID(static_cast<GIntBig>(sourceFeature.fid));
        }

        for (size_t sourceFieldIndex = 0; sourceFieldIndex < fieldIndexMap.size(); sourceFieldIndex++)
        {
            const int targetFieldIndex = fieldIndexMap[sourceFieldIndex];
            if (targetFieldIndex < 0)
            {
                continue;
            }

            const OGRFieldDefn* targetFieldDefn = layerDefn->GetFieldDefn(targetFieldIndex);
            const GB_Variant value = sourceFeature.GetAttribute(sourceFieldIndex);
            SetOgrFieldValue(ogrFeature.get(), targetFieldIndex, targetFieldDefn, value);
        }

        std::unique_ptr<OGRGeometry> ogrGeometry(CreateOgrGeometryFromGeoGeometry(sourceFeature.geometry));
        if (ogrGeometry)
        {
            if (ogrFeature->SetGeometryDirectly(ogrGeometry.release()) != OGRERR_NONE)
            {
                SetErrorMessage(errorMessageUtf8, "设置要素几何失败。 ");
                return false;
            }
        }

        if (layer->CreateFeature(ogrFeature.get()) != OGRERR_NONE)
        {
            SetErrorMessage(errorMessageUtf8, "写入要素失败。 ");
            return false;
        }
    }

    if (options.createSpatialIndex && layerGeometryType != GeoVectorGeometryType::Unknown)
    {
        const std::string sql = "CREATE SPATIAL INDEX ON " + layerName;
        OGRLayer* resultLayer = dataset->ExecuteSQL(sql.c_str(), nullptr, nullptr);
        if (resultLayer != nullptr)
        {
            dataset->ReleaseResultSet(resultLayer);
        }
    }

    layer->SyncToDisk();
    dataset->FlushCache(true);

    if (!WriteCpgFileIfNeeded(filePathUtf8, options))
    {
        SetErrorMessage(errorMessageUtf8, "写出 .cpg 文件失败。 ");
        return false;
    }

    return true;
}
