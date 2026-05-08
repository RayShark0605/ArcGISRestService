#include "GeoVectorFeature.h"
#include "GeoBase/GB_Math.h"

#include <algorithm>
#include <utility>

namespace
{
    const GB_Variant* FindAttributeMapValue(const GB_VariantMap& attributeMap, const std::string& fieldNameUtf8, const bool caseSensitive)
    {
        if (caseSensitive)
        {
            const GB_VariantMap::const_iterator iter = attributeMap.find(fieldNameUtf8);
            return iter == attributeMap.end() ? nullptr : &iter->second;
        }

        for (GB_VariantMap::const_iterator iter = attributeMap.begin(); iter != attributeMap.end(); ++iter)
        {
            if (GeoVectorFieldsHelper::FieldNameEquals(iter->first, fieldNameUtf8, false))
            {
                return &iter->second;
            }
        }

        return nullptr;
    }
}

GeoVectorFeature::GeoVectorFeature()
{
    ResetFidAndUid();
}

GeoVectorFeature::GeoVectorFeature(const GeoVectorGeometry& featureGeometry) : geometry(featureGeometry)
{
    ResetFidAndUid();
}

GeoVectorFeature::GeoVectorFeature(GeoVectorGeometry&& featureGeometry) : geometry(std::move(featureGeometry))
{
    ResetFidAndUid();
}

GeoVectorFeature::GeoVectorFeature(const long long featureId, const GeoVectorGeometry& featureGeometry, const GB_VariantList& featureAttributes) : fid(featureId), geometry(featureGeometry), attributes(featureAttributes)
{
    uid = GenerateUid();
}

GeoVectorFeature::GeoVectorFeature(const long long featureId, GeoVectorGeometry&& featureGeometry, GB_VariantList&& featureAttributes) : fid(featureId), geometry(std::move(featureGeometry)), attributes(std::move(featureAttributes))
{
    uid = GenerateUid();
}

GeoVectorFeature::GeoVectorFeature(const GeoVectorFeature& other) : uid(other.uid), fid(other.fid), geometry(other.geometry), attributes(other.attributes)
{
}

GeoVectorFeature::GeoVectorFeature(GeoVectorFeature&& other) noexcept : uid(std::move(other.uid)), fid(other.fid), geometry(std::move(other.geometry)), attributes(std::move(other.attributes))
{
    other.fid = GeoInvalidFid;
}

GeoVectorFeature::~GeoVectorFeature()
{
}

GeoVectorFeature& GeoVectorFeature::operator=(const GeoVectorFeature& other)
{
    if (this == &other)
    {
        return *this;
    }

    uid = other.uid;
    fid = other.fid;
    geometry = other.geometry;
    attributes = other.attributes;
    return *this;
}

GeoVectorFeature& GeoVectorFeature::operator=(GeoVectorFeature&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    uid = std::move(other.uid);
    fid = other.fid;
    geometry = std::move(other.geometry);
    attributes = std::move(other.attributes);

    other.fid = GeoInvalidFid;
    return *this;
}

void GeoVectorFeature::Swap(GeoVectorFeature& other) noexcept
{
    std::swap(uid, other.uid);
    std::swap(fid, other.fid);
    std::swap(geometry, other.geometry);
    std::swap(attributes, other.attributes);
}

std::string GeoVectorFeature::GenerateUid()
{
    return GB_GenerateUuid();
}

const std::string& GeoVectorFeature::GetUid() const
{
    return uid;
}

bool GeoVectorFeature::HasValidUid() const
{
    return !uid.empty();
}

bool GeoVectorFeature::RegenerateUid()
{
    const std::string newUid = GenerateUid();
    if (newUid.empty())
    {
        return false;
    }

    uid = newUid;
    return true;
}

bool GeoVectorFeature::HasValidFid() const
{
    return fid >= 0;
}

bool GeoVectorFeature::IsTemporaryFeature() const
{
    return !HasValidFid();
}

void GeoVectorFeature::SetFid(const long long featureId)
{
    fid = featureId;
}

void GeoVectorFeature::InvalidateFid()
{
    fid = GeoInvalidFid;
}

void GeoVectorFeature::ResetFidAndUid()
{
    fid = GeoInvalidFid;
    uid = GenerateUid();
}

GeoVectorFeature GeoVectorFeature::CloneAsNewFeature() const
{
    GeoVectorFeature newFeature(*this);
    newFeature.ResetFidAndUid();
    return newFeature;
}

void GeoVectorFeature::Reset()
{
    ResetFidAndUid();
    geometry.Reset();
    attributes.clear();
}

bool GeoVectorFeature::IsValid(const bool requireValidGeometry) const
{
    if (!HasValidUid())
    {
        return false;
    }

    if (requireValidGeometry && !geometry.IsValid())
    {
        return false;
    }

    return true;
}

bool GeoVectorFeature::HasValidGeometry() const
{
    return geometry.IsValid();
}

bool GeoVectorFeature::HasNonEmptyGeometry() const
{
    return geometry.IsValid() && !geometry.IsEmpty();
}

bool GeoVectorFeature::IsEmptyGeometry() const
{
    return geometry.IsEmpty();
}

GeoVectorGeometryType GeoVectorFeature::GetGeometryType() const
{
    return geometry.GetGeometryType();
}

GB_Rectangle GeoVectorFeature::BoundingRectangle() const
{
    return geometry.BoundingRectangle();
}

size_t GeoVectorFeature::GetAttributeCount() const
{
    return attributes.size();
}

bool GeoVectorFeature::IsAttributeIndexValid(const size_t attributeIndex) const
{
    return attributeIndex < attributes.size();
}

bool GeoVectorFeature::HasAttribute(const size_t attributeIndex) const
{
    return attributeIndex < attributes.size() && !attributes[attributeIndex].IsEmpty();
}

void GeoVectorFeature::ClearAttributes()
{
    attributes.clear();
}

void GeoVectorFeature::SetAttributes(const GB_VariantList& featureAttributes)
{
    attributes = featureAttributes;
}

void GeoVectorFeature::SetAttributes(GB_VariantList&& featureAttributes)
{
    attributes = std::move(featureAttributes);
}

bool GeoVectorFeature::ResizeAttributes(const size_t attributeCount, const GB_Variant& fillValue)
{
    attributes.resize(attributeCount, fillValue);
    return true;
}

bool GeoVectorFeature::EnsureAttributeCount(const size_t minAttributeCount, const GB_Variant& fillValue)
{
    if (attributes.size() >= minAttributeCount)
    {
        return true;
    }

    attributes.resize(minAttributeCount, fillValue);
    return true;
}

const GB_Variant* GeoVectorFeature::GetAttributePtr(const size_t attributeIndex) const
{
    if (attributeIndex >= attributes.size())
    {
        return nullptr;
    }

    return &attributes[attributeIndex];
}

GB_Variant* GeoVectorFeature::GetMutableAttributePtr(const size_t attributeIndex)
{
    if (attributeIndex >= attributes.size())
    {
        return nullptr;
    }

    return &attributes[attributeIndex];
}

bool GeoVectorFeature::TryGetAttribute(const size_t attributeIndex, GB_Variant& outValue) const
{
    const GB_Variant* value = GetAttributePtr(attributeIndex);
    if (value == nullptr)
    {
        outValue.Reset();
        return false;
    }

    outValue = *value;
    return true;
}

GB_Variant GeoVectorFeature::GetAttribute(const size_t attributeIndex, const GB_Variant& defaultValue) const
{
    const GB_Variant* value = GetAttributePtr(attributeIndex);
    return value == nullptr ? defaultValue : *value;
}

bool GeoVectorFeature::SetAttribute(const size_t attributeIndex, const GB_Variant& value)
{
    if (attributeIndex >= attributes.size())
    {
        return false;
    }

    attributes[attributeIndex] = value;
    return true;
}

bool GeoVectorFeature::SetAttribute(const size_t attributeIndex, GB_Variant&& value)
{
    if (attributeIndex >= attributes.size())
    {
        return false;
    }

    attributes[attributeIndex] = std::move(value);
    return true;
}

void GeoVectorFeature::AddAttribute(const GB_Variant& value)
{
    attributes.push_back(value);
}

void GeoVectorFeature::AddAttribute(GB_Variant&& value)
{
    attributes.push_back(std::move(value));
}

bool GeoVectorFeature::HasField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const bool caseSensitive) const
{
    return GeoVectorFieldsHelper::ContainsField(fields, fieldNameUtf8, caseSensitive);
}

int GeoVectorFeature::IndexOfField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const bool caseSensitive) const
{
    return GeoVectorFieldsHelper::IndexOfField(fields, fieldNameUtf8, caseSensitive);
}

const GeoVectorField* GeoVectorFeature::FindField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const bool caseSensitive) const
{
    return GeoVectorFieldsHelper::FindField(fields, fieldNameUtf8, caseSensitive);
}

bool GeoVectorFeature::HasAttribute(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const bool caseSensitive) const
{
    const int fieldIndex = IndexOfField(fields, fieldNameUtf8, caseSensitive);
    return fieldIndex >= 0 && static_cast<size_t>(fieldIndex) < attributes.size();
}

const GB_Variant* GeoVectorFeature::GetAttributePtr(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const bool caseSensitive) const
{
    const int fieldIndex = IndexOfField(fields, fieldNameUtf8, caseSensitive);
    if (fieldIndex < 0)
    {
        return nullptr;
    }

    return GetAttributePtr(static_cast<size_t>(fieldIndex));
}

GB_Variant* GeoVectorFeature::GetMutableAttributePtr(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const bool caseSensitive)
{
    const int fieldIndex = IndexOfField(fields, fieldNameUtf8, caseSensitive);
    if (fieldIndex < 0)
    {
        return nullptr;
    }

    return GetMutableAttributePtr(static_cast<size_t>(fieldIndex));
}

bool GeoVectorFeature::TryGetAttribute(const GeoVectorFields& fields, const std::string& fieldNameUtf8, GB_Variant& outValue, const bool caseSensitive) const
{
    const int fieldIndex = IndexOfField(fields, fieldNameUtf8, caseSensitive);
    if (fieldIndex < 0)
    {
        outValue.Reset();
        return false;
    }

    return TryGetAttribute(static_cast<size_t>(fieldIndex), outValue);
}

GB_Variant GeoVectorFeature::GetAttribute(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const GB_Variant& defaultValue, const bool caseSensitive) const
{
    const int fieldIndex = IndexOfField(fields, fieldNameUtf8, caseSensitive);
    if (fieldIndex < 0)
    {
        return defaultValue;
    }

    return GetAttribute(static_cast<size_t>(fieldIndex), defaultValue);
}

bool GeoVectorFeature::SetAttribute(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const GB_Variant& value, const bool caseSensitive)
{
    const int fieldIndex = IndexOfField(fields, fieldNameUtf8, caseSensitive);
    if (fieldIndex < 0)
    {
        return false;
    }

    const size_t attributeIndex = static_cast<size_t>(fieldIndex);
    EnsureAttributeCount(attributeIndex + 1);
    attributes[attributeIndex] = value;
    return true;
}

bool GeoVectorFeature::SetAttribute(const GeoVectorFields& fields, const std::string& fieldNameUtf8, GB_Variant&& value, const bool caseSensitive)
{
    const int fieldIndex = IndexOfField(fields, fieldNameUtf8, caseSensitive);
    if (fieldIndex < 0)
    {
        return false;
    }

    const size_t attributeIndex = static_cast<size_t>(fieldIndex);
    EnsureAttributeCount(attributeIndex + 1);
    attributes[attributeIndex] = std::move(value);
    return true;
}

bool GeoVectorFeature::SetConvertedAttribute(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const GB_Variant& value, const bool allowNull, const bool caseSensitive)
{
    const int fieldIndex = IndexOfField(fields, fieldNameUtf8, caseSensitive);
    if (fieldIndex < 0)
    {
        return false;
    }

    return SetConvertedAttribute(fields, static_cast<size_t>(fieldIndex), value, allowNull);
}

bool GeoVectorFeature::SetConvertedAttribute(const GeoVectorFields& fields, const size_t fieldIndex, const GB_Variant& value, const bool allowNull)
{
    if (fieldIndex >= fields.size())
    {
        return false;
    }

    GB_Variant convertedValue;
    if (!GeoVectorFieldsHelper::TryConvertValueToField(fields[fieldIndex], value, convertedValue, allowNull))
    {
        return false;
    }

    EnsureAttributeCount(fieldIndex + 1);
    attributes[fieldIndex] = std::move(convertedValue);
    return true;
}

bool GeoVectorFeature::EnsureAttributeCountForFields(const GeoVectorFields& fields)
{
    return EnsureAttributeCount(fields.size());
}

bool GeoVectorFeature::NormalizeAttributeCountToFields(const GeoVectorFields& fields)
{
    attributes.resize(fields.size());
    return true;
}

bool GeoVectorFeature::IsAttributeLayoutCompatibleWithFields(const GeoVectorFields& fields, const bool allowMissingTrailingAttributes) const
{
    if (allowMissingTrailingAttributes)
    {
        return attributes.size() <= fields.size();
    }

    return attributes.size() == fields.size();
}

bool GeoVectorFeature::ValidateAttributesAgainstFields(const GeoVectorFields& fields, const bool allowMissingTrailingAttributes, const bool allowNull) const
{
    if (!IsAttributeLayoutCompatibleWithFields(fields, allowMissingTrailingAttributes))
    {
        return false;
    }

    const size_t checkCount = std::min(attributes.size(), fields.size());
    for (size_t i = 0; i < checkCount; i++)
    {
        if (!GeoVectorFieldsHelper::IsValueCompatibleWithField(fields[i], attributes[i], allowNull))
        {
            return false;
        }
    }

    if (!allowMissingTrailingAttributes && attributes.size() < fields.size())
    {
        for (size_t i = attributes.size(); i < fields.size(); i++)
        {
            const GB_Variant emptyValue;
            if (!GeoVectorFieldsHelper::IsValueCompatibleWithField(fields[i], emptyValue, allowNull))
            {
                return false;
            }
        }
    }

    return true;
}

bool GeoVectorFeature::ConvertAttributesToFieldStorage(const GeoVectorFields& fields, const bool allowMissingTrailingAttributes, const bool allowNull)
{
    if (!IsAttributeLayoutCompatibleWithFields(fields, allowMissingTrailingAttributes))
    {
        return false;
    }

    GB_VariantList convertedAttributes = attributes;
    if (!allowMissingTrailingAttributes)
    {
        convertedAttributes.resize(fields.size());
    }

    const size_t checkCount = std::min(convertedAttributes.size(), fields.size());
    for (size_t i = 0; i < checkCount; i++)
    {
        GB_Variant convertedValue;
        if (!GeoVectorFieldsHelper::TryConvertValueToField(fields[i], convertedAttributes[i], convertedValue, allowNull))
        {
            return false;
        }
        convertedAttributes[i] = std::move(convertedValue);
    }

    attributes = std::move(convertedAttributes);
    return true;
}

GB_VariantMap GeoVectorFeature::ToAttributeMap(const GeoVectorFields& fields, const bool includeMissingAttributes) const
{
    GB_VariantMap attributeMap;
    const size_t fieldCount = fields.size();

    for (size_t i = 0; i < fieldCount; i++)
    {
        if (!includeMissingAttributes && i >= attributes.size())
        {
            continue;
        }

        if (!GeoVectorFieldsHelper::IsValidFieldName(fields[i].nameUtf8))
        {
            continue;
        }

        attributeMap[fields[i].nameUtf8] = (i < attributes.size() ? attributes[i] : GB_Variant());
    }

    return attributeMap;
}

bool GeoVectorFeature::SetAttributesFromMap(const GeoVectorFields& fields, const GB_VariantMap& attributeMap, const bool convertValues, const bool caseSensitive)
{
    GB_VariantList newAttributes;
    newAttributes.resize(fields.size());

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (!GeoVectorFieldsHelper::IsValidFieldName(fields[i].nameUtf8))
        {
            return false;
        }

        const GB_Variant* inputValue = FindAttributeMapValue(attributeMap, fields[i].nameUtf8, caseSensitive);
        if (inputValue == nullptr)
        {
            continue;
        }

        if (convertValues)
        {
            GB_Variant convertedValue;
            if (!GeoVectorFieldsHelper::TryConvertValueToField(fields[i], *inputValue, convertedValue, true))
            {
                return false;
            }
            newAttributes[i] = std::move(convertedValue);
        }
        else
        {
            newAttributes[i] = *inputValue;
        }
    }

    attributes = std::move(newAttributes);
    return true;
}

bool GeoVectorFeature::TryGetObjectIdAttribute(const GeoVectorFields& fields, GB_Variant& outValue, const bool caseSensitive) const
{
    const GeoVectorField* objectIdField = GeoVectorFieldsHelper::FindObjectIdField(fields, caseSensitive);
    if (objectIdField == nullptr)
    {
        outValue.Reset();
        return false;
    }

    return TryGetAttribute(fields, objectIdField->nameUtf8, outValue, caseSensitive);
}

bool GeoVectorFeature::TryGetObjectIdAttributeAsInt64(const GeoVectorFields& fields, long long& outValue, const bool caseSensitive) const
{
    GB_Variant objectIdValue;
    if (!TryGetObjectIdAttribute(fields, objectIdValue, caseSensitive))
    {
        outValue = 0;
        return false;
    }

    bool ok = false;
    const long long objectId = objectIdValue.ToInt64(&ok);
    if (!ok)
    {
        outValue = 0;
        return false;
    }

    outValue = objectId;
    return true;
}

bool GeoVectorFeature::TryGetGlobalIdAttribute(const GeoVectorFields& fields, std::string& outValue, const bool caseSensitive) const
{
    const GeoVectorField* globalIdField = GeoVectorFieldsHelper::FindGlobalIdField(fields, caseSensitive);
    if (globalIdField == nullptr)
    {
        outValue.clear();
        return false;
    }

    GB_Variant globalIdValue;
    if (!TryGetAttribute(fields, globalIdField->nameUtf8, globalIdValue, caseSensitive))
    {
        outValue.clear();
        return false;
    }

    bool ok = false;
    const std::string globalId = globalIdValue.ToString(&ok);
    if (!ok)
    {
        outValue.clear();
        return false;
    }

    outValue = globalId;
    return true;
}

bool GeoVectorFeature::operator==(const GeoVectorFeature& other) const
{
    return uid == other.uid && fid == other.fid && geometry == other.geometry && attributes == other.attributes;
}

bool GeoVectorFeature::operator!=(const GeoVectorFeature& other) const
{
    return !(*this == other);
}
