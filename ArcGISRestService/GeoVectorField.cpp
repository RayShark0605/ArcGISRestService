#include "GeoVectorField.h"
#include "GeoBase/GB_Utf8String.h"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

#define SET_ERROR_MESSAGE(msg) do { if (errorMessageUtf8) { *errorMessageUtf8 = (msg); } } while (0)

namespace
{
    inline char ToLowerAscii(const char character)
    {
        if (character >= 'A' && character <= 'Z')
        {
            return static_cast<char>(character - 'A' + 'a');
        }

        return character;
    }

    inline bool IsAsciiSpace(const char character)
    {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n' || character == '\f' || character == '\v';
    }

    std::string TrimAscii(const std::string& text)
    {
        size_t beginIndex = 0;
        while (beginIndex < text.size() && IsAsciiSpace(text[beginIndex]))
        {
            beginIndex++;
        }

        size_t endIndex = text.size();
        while (endIndex > beginIndex && IsAsciiSpace(text[endIndex - 1]))
        {
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    std::string ToLowerAsciiString(const std::string& text)
    {
        std::string lowerText = text;
        for (size_t i = 0; i < lowerText.size(); i++)
        {
            lowerText[i] = ToLowerAscii(lowerText[i]);
        }
        return lowerText;
    }

    std::string NormalizeTypeText(const std::string& text)
    {
        std::string result;
        const std::string trimmedText = TrimAscii(text);
        result.reserve(trimmedText.size());
        for (size_t i = 0; i < trimmedText.size(); i++)
        {
            const char character = ToLowerAscii(trimmedText[i]);
            if (character == '_' || character == '-' || character == ' ' || character == '\t' || character == '\r' || character == '\n')
            {
                continue;
            }
            result.push_back(character);
        }
        return result;
    }

    bool FieldTypeExistsInList(const std::vector<GeoVectorFieldType>& fieldTypes, const GeoVectorFieldType fieldType)
    {
        for (size_t i = 0; i < fieldTypes.size(); i++)
        {
            if (fieldTypes[i] == fieldType)
            {
                return true;
            }
        }
        return false;
    }

    bool IsFieldNameFallbackMatched(const GeoVectorField& field, const std::vector<std::string>& fallbackNamesUtf8, const bool caseSensitive)
    {
        for (size_t i = 0; i < fallbackNamesUtf8.size(); i++)
        {
            if (GeoVectorFieldsHelper::FieldNameEquals(field.nameUtf8, fallbackNamesUtf8[i], caseSensitive))
            {
                return true;
            }
        }
        return false;
    }

    inline bool TryGetMapStringValue(const GB_VariantMap& valueMap, const std::string& key, std::string& outValue)
    {
        const GB_VariantMap::const_iterator iter = valueMap.find(key);
        if (iter == valueMap.end())
        {
            return false;
        }

        bool ok = false;
        const std::string value = iter->second.ToString(&ok);
        if (!ok)
        {
            return false;
        }

        outValue = value;
        return true;
    }

    inline bool TryGetMapIntValue(const GB_VariantMap& valueMap, const std::string& key, int& outValue)
    {
        const GB_VariantMap::const_iterator iter = valueMap.find(key);
        if (iter == valueMap.end())
        {
            return false;
        }

        bool ok = false;
        const int value = iter->second.ToInt(&ok);
        if (!ok)
        {
            return false;
        }

        outValue = value;
        return true;
    }

    inline bool TryGetMapBoolValue(const GB_VariantMap& valueMap, const std::string& key, bool& outValue)
    {
        const GB_VariantMap::const_iterator iter = valueMap.find(key);
        if (iter == valueMap.end())
        {
            return false;
        }

        bool ok = false;
        const bool value = iter->second.ToBool(&ok);
        if (!ok)
        {
            return false;
        }

        outValue = value;
        return true;
    }

    inline bool IsControlCharacter(const char character)
    {
        const unsigned char byteValue = static_cast<unsigned char>(character);
        return byteValue < 32 || byteValue == 127;
    }

    inline bool CheckTextLength(const GeoVectorField& field, const GB_Variant& value)
    {
        if (field.maxLength <= 0)
        {
            return true;
        }

        bool ok = false;
        const std::string text = value.ToString(&ok);
        if (!ok)
        {
            return false;
        }

        const size_t textLength = GB_IsUtf8(text) ? GB_GetUtf8Length(text) : text.size();
        return textLength <= static_cast<size_t>(field.maxLength);
    }
}

bool GeoVectorField::IsValid() const
{
    return GeoVectorFieldsHelper::IsValidFieldName(nameUtf8) && type != GeoVectorFieldType::Unknown && maxLength >= 0;
}

bool GeoVectorField::IsStringField() const
{
    return GeoVectorFieldsHelper::GetGeoVectorFieldValueStorageType(type) == GeoVectorFieldValueStorageType::String;
}

bool GeoVectorField::IsIntegerField() const
{
    return type == GeoVectorFieldType::ObjectId || type == GeoVectorFieldType::Int16 || type == GeoVectorFieldType::Int32 || type == GeoVectorFieldType::Int64;
}

bool GeoVectorField::IsFloatingPointField() const
{
    return type == GeoVectorFieldType::Float || type == GeoVectorFieldType::Double;
}

bool GeoVectorField::IsNumericField() const
{
    return IsIntegerField() || IsFloatingPointField();
}

bool GeoVectorField::IsDateOrTimeField() const
{
    return type == GeoVectorFieldType::DateTime || type == GeoVectorFieldType::Date || type == GeoVectorFieldType::Time || type == GeoVectorFieldType::TimestampOffset;
}

bool GeoVectorField::IsBinaryField() const
{
    return type == GeoVectorFieldType::Blob;
}

bool GeoVectorField::IsIdField() const
{
    return type == GeoVectorFieldType::ObjectId || type == GeoVectorFieldType::GlobalId || type == GeoVectorFieldType::Guid;
}

std::string GeoVectorField::GetDisplayNameUtf8() const
{
    return aliasUtf8.empty() ? nameUtf8 : aliasUtf8;
}

GeoVectorFieldValueStorageType GeoVectorFieldsHelper::GetGeoVectorFieldValueStorageType(const GeoVectorFieldType fieldType)
{
    switch (fieldType)
    {
    case GeoVectorFieldType::Int16:
    case GeoVectorFieldType::Int32:
        return GeoVectorFieldValueStorageType::Int32;
    case GeoVectorFieldType::ObjectId:
    case GeoVectorFieldType::Int64:
    case GeoVectorFieldType::DateTime:
        return GeoVectorFieldValueStorageType::Int64;
    case GeoVectorFieldType::Float:
    case GeoVectorFieldType::Double:
        return GeoVectorFieldValueStorageType::Double;
    case GeoVectorFieldType::GlobalId:
    case GeoVectorFieldType::Guid:
    case GeoVectorFieldType::String:
    case GeoVectorFieldType::Date:
    case GeoVectorFieldType::Time:
    case GeoVectorFieldType::TimestampOffset:
    case GeoVectorFieldType::Xml:
        return GeoVectorFieldValueStorageType::String;
    case GeoVectorFieldType::Blob:
        return GeoVectorFieldValueStorageType::Binary;
    case GeoVectorFieldType::Unknown:
    case GeoVectorFieldType::Raster:
    case GeoVectorFieldType::Geometry:
    default:
        return GeoVectorFieldValueStorageType::RawVariant;
    }
}

std::string GeoVectorFieldsHelper::FieldTypeToString(const GeoVectorFieldType fieldType)
{
    switch (fieldType)
    {
    case GeoVectorFieldType::ObjectId:
        return "ObjectId";
    case GeoVectorFieldType::GlobalId:
        return "GlobalId";
    case GeoVectorFieldType::Guid:
        return "Guid";
    case GeoVectorFieldType::Int16:
        return "Int16";
    case GeoVectorFieldType::Int32:
        return "Int32";
    case GeoVectorFieldType::Int64:
        return "Int64";
    case GeoVectorFieldType::Float:
        return "Float";
    case GeoVectorFieldType::Double:
        return "Double";
    case GeoVectorFieldType::String:
        return "String";
    case GeoVectorFieldType::DateTime:
        return "DateTime";
    case GeoVectorFieldType::Date:
        return "Date";
    case GeoVectorFieldType::Time:
        return "Time";
    case GeoVectorFieldType::TimestampOffset:
        return "TimestampOffset";
    case GeoVectorFieldType::Blob:
        return "Blob";
    case GeoVectorFieldType::Raster:
        return "Raster";
    case GeoVectorFieldType::Geometry:
        return "Geometry";
    case GeoVectorFieldType::Xml:
        return "Xml";
    case GeoVectorFieldType::Unknown:
    default:
        return "Unknown";
    }
}

std::string GeoVectorFieldsHelper::FieldValueStorageTypeToString(const GeoVectorFieldValueStorageType storageType)
{
    switch (storageType)
    {
    case GeoVectorFieldValueStorageType::Int32:
        return "Int32";
    case GeoVectorFieldValueStorageType::Int64:
        return "Int64";
    case GeoVectorFieldValueStorageType::Double:
        return "Double";
    case GeoVectorFieldValueStorageType::String:
        return "String";
    case GeoVectorFieldValueStorageType::Binary:
        return "Binary";
    case GeoVectorFieldValueStorageType::RawVariant:
        return "RawVariant";
    case GeoVectorFieldValueStorageType::Unknown:
    default:
        return "Unknown";
    }
}

GeoVectorFieldType GeoVectorFieldsHelper::FieldTypeFromArcGISString(const std::string& fieldTypeTextUtf8)
{
    const std::string typeText = NormalizeTypeText(fieldTypeTextUtf8);

    if (typeText == "esrifieldtypeoid" || typeText == "oid" || typeText == "objectid" || typeText == "objectidfield")
    {
        return GeoVectorFieldType::ObjectId;
    }
    if (typeText == "esrifieldtypeglobalid" || typeText == "globalid")
    {
        return GeoVectorFieldType::GlobalId;
    }
    if (typeText == "esrifieldtypeguid" || typeText == "guid")
    {
        return GeoVectorFieldType::Guid;
    }
    if (typeText == "esrifieldtypesmallinteger" || typeText == "smallinteger" || typeText == "int16" || typeText == "short")
    {
        return GeoVectorFieldType::Int16;
    }
    if (typeText == "esrifieldtypeinteger" || typeText == "integer" || typeText == "int" || typeText == "int32")
    {
        return GeoVectorFieldType::Int32;
    }
    if (typeText == "esrifieldtypebiginteger" || typeText == "biginteger" || typeText == "int64" || typeText == "long" || typeText == "longlong")
    {
        return GeoVectorFieldType::Int64;
    }
    if (typeText == "esrifieldtypesingle" || typeText == "single" || typeText == "float" || typeText == "float32")
    {
        return GeoVectorFieldType::Float;
    }
    if (typeText == "esrifieldtypedouble" || typeText == "double" || typeText == "float64")
    {
        return GeoVectorFieldType::Double;
    }
    if (typeText == "esrifieldtypestring" || typeText == "string" || typeText == "text")
    {
        return GeoVectorFieldType::String;
    }
    if (typeText == "esrifieldtypedate" || typeText == "datetime" || typeText == "dateandtime")
    {
        return GeoVectorFieldType::DateTime;
    }
    if (typeText == "esrifieldtypedateonly" || typeText == "date" || typeText == "dateonly")
    {
        return GeoVectorFieldType::Date;
    }
    if (typeText == "esrifieldtypetimeonly" || typeText == "time" || typeText == "timeonly")
    {
        return GeoVectorFieldType::Time;
    }
    if (typeText == "esrifieldtypetimestampoffset" || typeText == "timestampoffset" || typeText == "datetimeoffset")
    {
        return GeoVectorFieldType::TimestampOffset;
    }
    if (typeText == "esrifieldtypeblob" || typeText == "blob" || typeText == "binary")
    {
        return GeoVectorFieldType::Blob;
    }
    if (typeText == "esrifieldtyperaster" || typeText == "raster")
    {
        return GeoVectorFieldType::Raster;
    }
    if (typeText == "esrifieldtypegeometry" || typeText == "geometry" || typeText == "shape")
    {
        return GeoVectorFieldType::Geometry;
    }
    if (typeText == "esrifieldtypexml" || typeText == "xml")
    {
        return GeoVectorFieldType::Xml;
    }

    return GeoVectorFieldType::Unknown;
}

std::string GeoVectorFieldsHelper::FieldTypeToArcGISString(const GeoVectorFieldType fieldType)
{
    switch (fieldType)
    {
    case GeoVectorFieldType::ObjectId:
        return "esriFieldTypeOID";
    case GeoVectorFieldType::GlobalId:
        return "esriFieldTypeGlobalID";
    case GeoVectorFieldType::Guid:
        return "esriFieldTypeGUID";
    case GeoVectorFieldType::Int16:
        return "esriFieldTypeSmallInteger";
    case GeoVectorFieldType::Int32:
        return "esriFieldTypeInteger";
    case GeoVectorFieldType::Int64:
        return "esriFieldTypeBigInteger";
    case GeoVectorFieldType::Float:
        return "esriFieldTypeSingle";
    case GeoVectorFieldType::Double:
        return "esriFieldTypeDouble";
    case GeoVectorFieldType::String:
        return "esriFieldTypeString";
    case GeoVectorFieldType::DateTime:
        return "esriFieldTypeDate";
    case GeoVectorFieldType::Date:
        return "esriFieldTypeDateOnly";
    case GeoVectorFieldType::Time:
        return "esriFieldTypeTimeOnly";
    case GeoVectorFieldType::TimestampOffset:
        return "esriFieldTypeTimestampOffset";
    case GeoVectorFieldType::Blob:
        return "esriFieldTypeBlob";
    case GeoVectorFieldType::Raster:
        return "esriFieldTypeRaster";
    case GeoVectorFieldType::Geometry:
        return "esriFieldTypeGeometry";
    case GeoVectorFieldType::Xml:
        return "esriFieldTypeXML";
    case GeoVectorFieldType::Unknown:
    default:
        return "";
    }
}

bool GeoVectorFieldsHelper::IsValidFieldName(const std::string& fieldNameUtf8)
{
    if (fieldNameUtf8.empty())
    {
        return false;
    }

    for (size_t i = 0; i < fieldNameUtf8.size(); i++)
    {
        if (IsControlCharacter(fieldNameUtf8[i]))
        {
            return false;
        }
    }

    return true;
}

std::string GeoVectorFieldsHelper::NormalizeFieldName(const std::string& fieldNameUtf8, const bool caseSensitive)
{
    return caseSensitive ? fieldNameUtf8 : ToLowerAsciiString(fieldNameUtf8);
}

bool GeoVectorFieldsHelper::FieldNameEquals(const std::string& leftFieldNameUtf8, const std::string& rightFieldNameUtf8, const bool caseSensitive)
{
    if (caseSensitive)
    {
        return leftFieldNameUtf8 == rightFieldNameUtf8;
    }

    if (leftFieldNameUtf8.size() != rightFieldNameUtf8.size())
    {
        return false;
    }

    for (size_t i = 0; i < leftFieldNameUtf8.size(); i++)
    {
        if (ToLowerAscii(leftFieldNameUtf8[i]) != ToLowerAscii(rightFieldNameUtf8[i]))
        {
            return false;
        }
    }

    return true;
}

bool GeoVectorFieldsHelper::ContainsField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const bool caseSensitive)
{
    return IndexOfField(fields, fieldNameUtf8, caseSensitive) >= 0;
}

int GeoVectorFieldsHelper::IndexOfField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const bool caseSensitive)
{
    for (size_t i = 0; i < fields.size(); i++)
    {
        if (FieldNameEquals(fields[i].nameUtf8, fieldNameUtf8, caseSensitive))
        {
            if (i > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                return -1;
            }
            return static_cast<int>(i);
        }
    }

    return -1;
}

const GeoVectorField* GeoVectorFieldsHelper::FindField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const bool caseSensitive)
{
    const int fieldIndex = IndexOfField(fields, fieldNameUtf8, caseSensitive);
    if (fieldIndex < 0)
    {
        return nullptr;
    }

    return &fields[static_cast<size_t>(fieldIndex)];
}

GeoVectorField* GeoVectorFieldsHelper::FindField(GeoVectorFields& fields, const std::string& fieldNameUtf8, const bool caseSensitive)
{
    const int fieldIndex = IndexOfField(fields, fieldNameUtf8, caseSensitive);
    if (fieldIndex < 0)
    {
        return nullptr;
    }

    return &fields[static_cast<size_t>(fieldIndex)];
}

bool GeoVectorFieldsHelper::TryGetField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, GeoVectorField& outField, const bool caseSensitive)
{
    const GeoVectorField* field = FindField(fields, fieldNameUtf8, caseSensitive);
    if (field == nullptr)
    {
        outField = GeoVectorField();
        return false;
    }

    outField = *field;
    return true;
}

GeoVectorFieldType GeoVectorFieldsHelper::GetFieldType(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const GeoVectorFieldType defaultType, const bool caseSensitive)
{
    const GeoVectorField* field = FindField(fields, fieldNameUtf8, caseSensitive);
    return field == nullptr ? defaultType : field->type;
}

bool GeoVectorFieldsHelper::TryGetFieldType(const GeoVectorFields& fields, const std::string& fieldNameUtf8, GeoVectorFieldType& outFieldType, const bool caseSensitive)
{
    const GeoVectorField* field = FindField(fields, fieldNameUtf8, caseSensitive);
    if (field == nullptr)
    {
        outFieldType = GeoVectorFieldType::Unknown;
        return false;
    }

    outFieldType = field->type;
    return true;
}

std::vector<std::string> GeoVectorFieldsHelper::GetFieldNames(const GeoVectorFields& fields, const bool onlyValidFields)
{
    std::vector<std::string> fieldNames;
    fieldNames.reserve(fields.size());

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (!onlyValidFields || fields[i].IsValid())
        {
            fieldNames.push_back(fields[i].nameUtf8);
        }
    }

    return fieldNames;
}

std::vector<std::string> GeoVectorFieldsHelper::GetFieldDisplayNames(const GeoVectorFields& fields, const bool onlyValidFields)
{
    std::vector<std::string> fieldNames;
    fieldNames.reserve(fields.size());

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (!onlyValidFields || fields[i].IsValid())
        {
            fieldNames.push_back(fields[i].GetDisplayNameUtf8());
        }
    }

    return fieldNames;
}

GeoVectorFields GeoVectorFieldsHelper::FilterByType(const GeoVectorFields& fields, const GeoVectorFieldType fieldType)
{
    GeoVectorFields resultFields;
    resultFields.reserve(fields.size());

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (fields[i].type == fieldType)
        {
            resultFields.push_back(fields[i]);
        }
    }

    return resultFields;
}

GeoVectorFields GeoVectorFieldsHelper::FilterByTypes(const GeoVectorFields& fields, const std::vector<GeoVectorFieldType>& fieldTypes)
{
    GeoVectorFields resultFields;
    resultFields.reserve(fields.size());

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (FieldTypeExistsInList(fieldTypes, fields[i].type))
        {
            resultFields.push_back(fields[i]);
        }
    }

    return resultFields;
}

GeoVectorFields GeoVectorFieldsHelper::FilterIntegerFields(const GeoVectorFields& fields)
{
    GeoVectorFields resultFields;
    resultFields.reserve(fields.size());

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (fields[i].IsIntegerField())
        {
            resultFields.push_back(fields[i]);
        }
    }

    return resultFields;
}

GeoVectorFields GeoVectorFieldsHelper::FilterFloatingPointFields(const GeoVectorFields& fields)
{
    GeoVectorFields resultFields;
    resultFields.reserve(fields.size());

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (fields[i].IsFloatingPointField())
        {
            resultFields.push_back(fields[i]);
        }
    }

    return resultFields;
}

GeoVectorFields GeoVectorFieldsHelper::FilterNumericFields(const GeoVectorFields& fields)
{
    GeoVectorFields resultFields;
    resultFields.reserve(fields.size());

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (fields[i].IsNumericField())
        {
            resultFields.push_back(fields[i]);
        }
    }

    return resultFields;
}

GeoVectorFields GeoVectorFieldsHelper::FilterStringFields(const GeoVectorFields& fields)
{
    GeoVectorFields resultFields;
    resultFields.reserve(fields.size());

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (fields[i].IsStringField())
        {
            resultFields.push_back(fields[i]);
        }
    }

    return resultFields;
}

GeoVectorFields GeoVectorFieldsHelper::FilterDateOrTimeFields(const GeoVectorFields& fields)
{
    GeoVectorFields resultFields;
    resultFields.reserve(fields.size());

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (fields[i].IsDateOrTimeField())
        {
            resultFields.push_back(fields[i]);
        }
    }

    return resultFields;
}

const GeoVectorField* GeoVectorFieldsHelper::FindFirstFieldOfType(const GeoVectorFields& fields, const GeoVectorFieldType fieldType)
{
    for (size_t i = 0; i < fields.size(); i++)
    {
        if (fields[i].type == fieldType)
        {
            return &fields[i];
        }
    }

    return nullptr;
}

const GeoVectorField* GeoVectorFieldsHelper::FindFirstFieldOfTypes(const GeoVectorFields& fields, const std::vector<GeoVectorFieldType>& fieldTypes)
{
    for (size_t i = 0; i < fields.size(); i++)
    {
        if (FieldTypeExistsInList(fieldTypes, fields[i].type))
        {
            return &fields[i];
        }
    }

    return nullptr;
}

const GeoVectorField* GeoVectorFieldsHelper::FindObjectIdField(const GeoVectorFields& fields, const bool caseSensitive)
{
    const GeoVectorField* field = FindFirstFieldOfType(fields, GeoVectorFieldType::ObjectId);
    if (field != nullptr)
    {
        return field;
    }

    const std::vector<std::string> fallbackNames = { "OBJECTID", "OID", "FID" };
    for (size_t i = 0; i < fields.size(); i++)
    {
        if (IsFieldNameFallbackMatched(fields[i], fallbackNames, caseSensitive))
        {
            return &fields[i];
        }
    }

    return nullptr;
}

const GeoVectorField* GeoVectorFieldsHelper::FindGlobalIdField(const GeoVectorFields& fields, const bool caseSensitive)
{
    const GeoVectorField* field = FindFirstFieldOfType(fields, GeoVectorFieldType::GlobalId);
    if (field != nullptr)
    {
        return field;
    }

    const std::vector<std::string> fallbackNames = { "GLOBALID", "GLOBAL_ID" };
    for (size_t i = 0; i < fields.size(); i++)
    {
        if (IsFieldNameFallbackMatched(fields[i], fallbackNames, caseSensitive))
        {
            return &fields[i];
        }
    }

    return nullptr;
}

const GeoVectorField* GeoVectorFieldsHelper::FindGeometryField(const GeoVectorFields& fields)
{
    return FindFirstFieldOfType(fields, GeoVectorFieldType::Geometry);
}

bool GeoVectorFieldsHelper::HasDuplicatedFieldNames(const GeoVectorFields& fields, const bool caseSensitive, std::vector<std::string>* duplicatedNamesUtf8)
{
    if (duplicatedNamesUtf8 != nullptr)
    {
        duplicatedNamesUtf8->clear();
    }

    bool hasDuplicatedName = false;
    std::unordered_set<std::string> existedNames;
    std::unordered_set<std::string> reportedNames;

    for (size_t i = 0; i < fields.size(); i++)
    {
        const std::string normalizedName = NormalizeFieldName(fields[i].nameUtf8, caseSensitive);
        if (normalizedName.empty())
        {
            continue;
        }

        if (existedNames.find(normalizedName) != existedNames.end())
        {
            hasDuplicatedName = true;
            if (duplicatedNamesUtf8 != nullptr && reportedNames.find(normalizedName) == reportedNames.end())
            {
                duplicatedNamesUtf8->push_back(fields[i].nameUtf8);
                reportedNames.insert(normalizedName);
            }
        }
        else
        {
            existedNames.insert(normalizedName);
        }
    }

    return hasDuplicatedName;
}

GeoVectorFields GeoVectorFieldsHelper::RemoveInvalidFields(const GeoVectorFields& fields)
{
    GeoVectorFields resultFields;
    resultFields.reserve(fields.size());

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (fields[i].IsValid())
        {
            resultFields.push_back(fields[i]);
        }
    }

    return resultFields;
}

bool GeoVectorFieldsHelper::ValidateFields(const GeoVectorFields& fields, const bool allowUnknownType, const bool allowDuplicatedNames, const bool caseSensitive, std::string* errorMessageUtf8)
{
    SET_ERROR_MESSAGE("");

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (!IsValidFieldName(fields[i].nameUtf8))
        {
            SET_ERROR_MESSAGE(GB_STR("字段名为空或包含控制字符。"));
            return false;
        }

        if (fields[i].maxLength < 0)
        {
            SET_ERROR_MESSAGE(GB_STR("字段 maxLength 不能为负数。"));
            return false;
        }

        if (!allowUnknownType && fields[i].type == GeoVectorFieldType::Unknown)
        {
            SET_ERROR_MESSAGE(GB_STR("字段类型不能为 Unknown。"));
            return false;
        }
    }

    if (!allowDuplicatedNames && HasDuplicatedFieldNames(fields, caseSensitive, nullptr))
    {
        SET_ERROR_MESSAGE(GB_STR("字段集合中存在重复字段名。"));
        return false;
    }

    return true;
}

bool GeoVectorFieldsHelper::TryBuildNameToIndexMap(const GeoVectorFields& fields, std::unordered_map<std::string, size_t>& outFieldIndexMap, const bool caseSensitive, const bool onlyValidFields)
{
    std::unordered_map<std::string, size_t> fieldIndexMap;
    fieldIndexMap.reserve(fields.size());

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (onlyValidFields && !fields[i].IsValid())
        {
            continue;
        }

        const std::string normalizedName = NormalizeFieldName(fields[i].nameUtf8, caseSensitive);
        if (normalizedName.empty())
        {
            continue;
        }

        if (fieldIndexMap.find(normalizedName) != fieldIndexMap.end())
        {
            outFieldIndexMap.clear();
            return false;
        }

        fieldIndexMap[normalizedName] = i;
    }

    outFieldIndexMap = std::move(fieldIndexMap);
    return true;
}

bool GeoVectorFieldsHelper::TryParseArcGISField(const GB_VariantMap& fieldMap, GeoVectorField& outField)
{
    GeoVectorField field;
    field.rawJsonMap = fieldMap;

    TryGetMapStringValue(fieldMap, "name", field.nameUtf8);
    TryGetMapStringValue(fieldMap, "alias", field.aliasUtf8);
    TryGetMapStringValue(fieldMap, "type", field.sourceTypeTextUtf8);
    TryGetMapIntValue(fieldMap, "length", field.maxLength);
    TryGetMapIntValue(fieldMap, "maxLength", field.maxLength);
    TryGetMapBoolValue(fieldMap, "nullable", field.nullable);

    field.type = FieldTypeFromArcGISString(field.sourceTypeTextUtf8);

    if (field.aliasUtf8.empty())
    {
        field.aliasUtf8 = field.nameUtf8;
    }

    if (!IsValidFieldName(field.nameUtf8))
    {
        outField = GeoVectorField();
        return false;
    }

    if (field.maxLength < 0)
    {
        field.maxLength = 0;
    }

    outField = std::move(field);
    return true;
}

bool GeoVectorFieldsHelper::TryParseArcGISField(const GB_Variant& fieldVariant, GeoVectorField& outField)
{
    const GB_VariantMap* fieldMap = fieldVariant.AnyCast<GB_VariantMap>();
    if (fieldMap == nullptr)
    {
        outField = GeoVectorField();
        return false;
    }

    return TryParseArcGISField(*fieldMap, outField);
}

GeoVectorFields GeoVectorFieldsHelper::ParseArcGISFields(const GB_Variant& fieldsVariant)
{
    const GB_VariantList* fieldList = fieldsVariant.AnyCast<GB_VariantList>();
    if (fieldList == nullptr)
    {
        return GeoVectorFields();
    }

    return ParseArcGISFields(*fieldList);
}

GeoVectorFields GeoVectorFieldsHelper::ParseArcGISFields(const GB_VariantList& fieldList)
{
    GeoVectorFields fields;
    fields.reserve(fieldList.size());

    for (size_t i = 0; i < fieldList.size(); i++)
    {
        GeoVectorField field;
        if (TryParseArcGISField(fieldList[i], field))
        {
            fields.push_back(std::move(field));
        }
    }

    return fields;
}

bool GeoVectorFieldsHelper::TryConvertValueToStorageType(const GeoVectorFieldType fieldType, const GB_Variant& inputValue, GB_Variant& outValue)
{
    if (inputValue.IsEmpty())
    {
        outValue.Reset();
        return true;
    }

    switch (GetGeoVectorFieldValueStorageType(fieldType))
    {
    case GeoVectorFieldValueStorageType::Int32:
    {
        bool ok = false;
        const int value = inputValue.ToInt(&ok);
        if (!ok)
        {
            return false;
        }

        if (fieldType == GeoVectorFieldType::Int16 && (value < std::numeric_limits<short>::min() || value > std::numeric_limits<short>::max()))
        {
            return false;
        }

        outValue = value;
        return true;
    }
    case GeoVectorFieldValueStorageType::Int64:
    {
        bool ok = false;
        const long long value = inputValue.ToInt64(&ok);
        if (!ok)
        {
            return false;
        }

        outValue = value;
        return true;
    }
    case GeoVectorFieldValueStorageType::Double:
    {
        bool ok = false;
        const double value = inputValue.ToDouble(&ok);
        if (!ok)
        {
            return false;
        }

        outValue = value;
        return true;
    }
    case GeoVectorFieldValueStorageType::String:
    {
        bool ok = false;
        const std::string value = inputValue.ToString(&ok);
        if (!ok)
        {
            return false;
        }

        outValue = value;
        return true;
    }
    case GeoVectorFieldValueStorageType::Binary:
    {
        bool ok = false;
        const GB_ByteBuffer value = inputValue.ToBinary(&ok);
        if (!ok)
        {
            return false;
        }

        outValue = value;
        return true;
    }
    case GeoVectorFieldValueStorageType::RawVariant:
        outValue = inputValue;
        return true;
    case GeoVectorFieldValueStorageType::Unknown:
    default:
        return false;
    }
}

bool GeoVectorFieldsHelper::TryConvertValueToField(const GeoVectorField& field, const GB_Variant& inputValue, GB_Variant& outValue, const bool allowNull)
{
    if (inputValue.IsEmpty())
    {
        if (!allowNull || !field.nullable)
        {
            return false;
        }

        outValue.Reset();
        return true;
    }

    GB_Variant convertedValue;
    if (!TryConvertValueToStorageType(field.type, inputValue, convertedValue))
    {
        return false;
    }

    if (GetGeoVectorFieldValueStorageType(field.type) == GeoVectorFieldValueStorageType::String && !CheckTextLength(field, convertedValue))
    {
        return false;
    }

    outValue = std::move(convertedValue);
    return true;
}

bool GeoVectorFieldsHelper::IsValueCompatibleWithFieldType(const GeoVectorFieldType fieldType, const GB_Variant& inputValue, const bool allowNull)
{
    if (inputValue.IsEmpty())
    {
        return allowNull;
    }

    GB_Variant convertedValue;
    return TryConvertValueToStorageType(fieldType, inputValue, convertedValue);
}

bool GeoVectorFieldsHelper::IsValueCompatibleWithField(const GeoVectorField& field, const GB_Variant& inputValue, const bool allowNull)
{
    GB_Variant convertedValue;
    return TryConvertValueToField(field, inputValue, convertedValue, allowNull);
}

#undef SET_ERROR_MESSAGE
