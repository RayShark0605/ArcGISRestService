#ifndef GEO_VECTOR_FIELD_H
#define GEO_VECTOR_FIELD_H

#include "ArcGISRestServicePort.h"
#include "GeoBase/GB_Variant.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief GIS 矢量图层字段类型。
 * 
 * 对应关系示例：
 * - esriFieldTypeOID              -> ObjectId
 * - esriFieldTypeGlobalID         -> GlobalId
 * - esriFieldTypeGUID             -> Guid
 * - esriFieldTypeSmallInteger     -> Int16
 * - esriFieldTypeInteger          -> Int32
 * - esriFieldTypeBigInteger       -> Int64
 * - esriFieldTypeSingle           -> Float
 * - esriFieldTypeDouble           -> Double
 * - esriFieldTypeString           -> String
 * - esriFieldTypeDate             -> DateTime
 * - esriFieldTypeDateOnly         -> Date
 * - esriFieldTypeTimeOnly         -> Time
 * - esriFieldTypeTimestampOffset  -> TimestampOffset
 * - esriFieldTypeBlob             -> Blob
 * - esriFieldTypeRaster           -> Raster
 * - esriFieldTypeGeometry         -> Geometry
 * - esriFieldTypeXML              -> Xml
 */
enum class GeoVectorFieldType
{
    Unknown = 0,
    ObjectId,
    GlobalId,
    Guid,
    Int16,
    Int32,
    Int64,
    Float,
    Double,
    String,
    DateTime,
    Date,
    Time,
    TimestampOffset,
    Blob,
    Raster,
    Geometry,
    Xml
};

/**
 * @brief 字段值的推荐存储类型。
 */
enum class GeoVectorFieldValueStorageType
{
    Unknown = 0,

    /** @brief 32 位整数，用于 Int16 / Int32。 */
    Int32,

    /** @brief 64 位整数，用于 ObjectId / Int64 / DateTime。DateTime 通常表示 Unix epoch 毫秒。 */
    Int64,

    /** @brief 双精度浮点数，用于 Float / Double。 */
    Double,

    /** @brief UTF-8 字符串，用于 GlobalId / Guid / String / Date / Time / TimestampOffset / Xml。 */
    String,

    /** @brief 二进制字节数组，用于 Blob。 */
    Binary,

    /** @brief 保留原始 GB_Variant，用于 Unknown / Raster / Geometry 等暂不统一展开的类型。 */
    RawVariant
};

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief GIS 矢量图层的字段定义。
 *
 * 设计约定：
 * - nameUtf8 是字段真实名称，应在同一字段集合中保持唯一；
 * - aliasUtf8 是展示名称，可为空；为空时 UI 可退化显示 nameUtf8；
 * - sourceTypeTextUtf8 保留数据源原始字段类型文本，例如 "esriFieldTypeString"；
 * - maxLength 主要用于 String / Guid / GlobalId / Xml 等文本类字段，0 表示未知或不限制；
 * - nullable 表示属性值是否允许为空；
 * - rawJsonMap 可保留 ArcGIS fields[] 中的完整原始对象，便于后续扩展 domain/defaultValue/editable 等信息。
 */
struct ARCGIS_RESTSERVICE_PORT GeoVectorField
{
    std::string nameUtf8 = "";
    GeoVectorFieldType type = GeoVectorFieldType::Unknown;
    std::string aliasUtf8 = "";
    std::string sourceTypeTextUtf8 = "";
    int maxLength = 0;
    bool nullable = true;
    GB_VariantMap rawJsonMap;

    /** @brief 判断字段定义是否可用。要求字段名非空、字段类型不是 Unknown，且 maxLength 非负。 */
    bool IsValid() const;

    /** @brief 是否是以文本形式表达或存储的字段。 */
    bool IsStringField() const;

    /** @brief 是否是整数类字段。 */
    bool IsIntegerField() const;

    /** @brief 是否是浮点类字段。 */
    bool IsFloatingPointField() const;

    /** @brief 是否是数值类字段。 */
    bool IsNumericField() const;

    /** @brief 是否是日期或时间类字段。 */
    bool IsDateOrTimeField() const;

    /** @brief 是否是二进制字段。 */
    bool IsBinaryField() const;

    /** @brief 是否是 ObjectId / GlobalId / Guid 这类标识字段。 */
    bool IsIdField() const;

    /** @brief 获取展示名称。aliasUtf8 非空时返回 aliasUtf8，否则返回 nameUtf8。 */
    std::string GetDisplayNameUtf8() const;
};

using GeoVectorFields = std::vector<GeoVectorField>;

/**
 * @brief GeoVectorField / GeoVectorFields 的常用静态工具函数集合。
 */
class ARCGIS_RESTSERVICE_PORT GeoVectorFieldsHelper
{
public:
    /** @brief 获取字段类型对应的推荐存储类型。 */
    static GeoVectorFieldValueStorageType GetGeoVectorFieldValueStorageType(GeoVectorFieldType fieldType);

    /** @brief 将字段类型转为稳定的可读字符串，例如 "Int32"。 */
    static std::string FieldTypeToString(GeoVectorFieldType fieldType);

    /** @brief 将推荐存储类型转为稳定的可读字符串，例如 "Int64"。 */
    static std::string FieldValueStorageTypeToString(GeoVectorFieldValueStorageType storageType);

    /** @brief 根据 ArcGIS 字段类型字符串解析字段类型。比较时不区分 ASCII 大小写。 */
    static GeoVectorFieldType FieldTypeFromArcGISString(const std::string& fieldTypeTextUtf8);

    /** @brief 将字段类型转为 ArcGIS 字段类型字符串。Unknown 返回空字符串。 */
    static std::string FieldTypeToArcGISString(GeoVectorFieldType fieldType);

    /** @brief 判断字段名是否非空且不包含控制字符。 */
    static bool IsValidFieldName(const std::string& fieldNameUtf8);

    /**
     * @brief 规范化字段名，用作查找键。
     *
     * 当 caseSensitive=false 时，仅对 ASCII 字母执行小写化；不会做 Unicode 大小写折叠，
     * 也不会修改字段名中的非 ASCII 字节。
     */
    static std::string NormalizeFieldName(const std::string& fieldNameUtf8, bool caseSensitive = false);

    /** @brief 判断两个字段名是否相同。默认不区分 ASCII 大小写。 */
    static bool FieldNameEquals(const std::string& leftFieldNameUtf8, const std::string& rightFieldNameUtf8, bool caseSensitive = false);

    /** @brief 判断字段集合中是否存在指定字段名。 */
    static bool ContainsField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, bool caseSensitive = false);

    /** @brief 查找指定字段名的索引；不存在时返回 -1。 */
    static int IndexOfField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, bool caseSensitive = false);

    /** @brief 获取指定字段名的只读字段指针；不存在时返回 nullptr。 */
    static const GeoVectorField* FindField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, bool caseSensitive = false);

    /** @brief 获取指定字段名的可写字段指针；不存在时返回 nullptr。 */
    static GeoVectorField* FindField(GeoVectorFields& fields, const std::string& fieldNameUtf8, bool caseSensitive = false);

    /** @brief 尝试复制指定字段名的字段定义。 */
    static bool TryGetField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, GeoVectorField& outField, bool caseSensitive = false);

    /** @brief 获取指定字段名的字段类型；不存在时返回 defaultType。 */
    static GeoVectorFieldType GetFieldType(const GeoVectorFields& fields, const std::string& fieldNameUtf8, GeoVectorFieldType defaultType = GeoVectorFieldType::Unknown, bool caseSensitive = false);

    /** @brief 尝试获取指定字段名的字段类型。 */
    static bool TryGetFieldType(const GeoVectorFields& fields, const std::string& fieldNameUtf8, GeoVectorFieldType& outFieldType, bool caseSensitive = false);

    /** @brief 获取字段名列表。 */
    static std::vector<std::string> GetFieldNames(const GeoVectorFields& fields, bool onlyValidFields = false);

    /** @brief 获取字段展示名列表。aliasUtf8 为空时使用 nameUtf8。 */
    static std::vector<std::string> GetFieldDisplayNames(const GeoVectorFields& fields, bool onlyValidFields = false);

    /** @brief 筛选指定字段类型的字段。 */
    static GeoVectorFields FilterByType(const GeoVectorFields& fields, GeoVectorFieldType fieldType);

    /** @brief 筛选多个字段类型的字段。 */
    static GeoVectorFields FilterByTypes(const GeoVectorFields& fields, const std::vector<GeoVectorFieldType>& fieldTypes);

    /** @brief 筛选整数类字段。 */
    static GeoVectorFields FilterIntegerFields(const GeoVectorFields& fields);

    /** @brief 筛选浮点类字段。 */
    static GeoVectorFields FilterFloatingPointFields(const GeoVectorFields& fields);

    /** @brief 筛选数值类字段。 */
    static GeoVectorFields FilterNumericFields(const GeoVectorFields& fields);

    /** @brief 筛选文本类字段。 */
    static GeoVectorFields FilterStringFields(const GeoVectorFields& fields);

    /** @brief 筛选日期或时间类字段。 */
    static GeoVectorFields FilterDateOrTimeFields(const GeoVectorFields& fields);

    /** @brief 查找第一个指定类型的字段；不存在时返回 nullptr。 */
    static const GeoVectorField* FindFirstFieldOfType(const GeoVectorFields& fields, GeoVectorFieldType fieldType);

    /** @brief 查找第一个属于多个指定类型之一的字段；不存在时返回 nullptr。 */
    static const GeoVectorField* FindFirstFieldOfTypes(const GeoVectorFields& fields, const std::vector<GeoVectorFieldType>& fieldTypes);

    /** @brief 查找 ObjectId 字段。优先按类型匹配；不存在时按常见字段名 OBJECTID / OID / FID 回退匹配。 */
    static const GeoVectorField* FindObjectIdField(const GeoVectorFields& fields, bool caseSensitive = false);

    /** @brief 查找 GlobalId 字段。优先按类型匹配；不存在时按常见字段名 GLOBALID 回退匹配。 */
    static const GeoVectorField* FindGlobalIdField(const GeoVectorFields& fields, bool caseSensitive = false);

    /** @brief 查找 Geometry 字段。 */
    static const GeoVectorField* FindGeometryField(const GeoVectorFields& fields);

    /**
     * @brief 判断字段集合中是否存在重复字段名。
     *
     * @param duplicatedNamesUtf8 可选输出重复字段名。输出的是首次发现重复时的原始字段名。
     */
    static bool HasDuplicatedFieldNames(const GeoVectorFields& fields, bool caseSensitive = false, std::vector<std::string>* duplicatedNamesUtf8 = nullptr);

    /** @brief 返回移除无效字段后的字段集合，保留原有顺序。 */
    static GeoVectorFields RemoveInvalidFields(const GeoVectorFields& fields);

    /**
     * @brief 校验字段集合。
     *
     * @param allowUnknownType 是否允许字段类型为 Unknown。
     * @param allowDuplicatedNames 是否允许重复字段名。
     * @param caseSensitive 字段名重复判断时是否区分大小写。
     * @param errorMessageUtf8 可选错误信息输出。
     */
    static bool ValidateFields(const GeoVectorFields& fields, bool allowUnknownType = false, bool allowDuplicatedNames = false, bool caseSensitive = false, std::string* errorMessageUtf8 = nullptr);

    /**
     * @brief 构建字段名到索引的查找表。
     *
     * 当存在重复字段名时返回 false，并清空 outFieldIndexMap。
     */
    static bool TryBuildNameToIndexMap(const GeoVectorFields& fields, std::unordered_map<std::string, size_t>& outFieldIndexMap, bool caseSensitive = false, bool onlyValidFields = true);

    /** @brief 尝试将 ArcGIS fields[] 中的单个字段对象解析为 GeoVectorField。 */
    static bool TryParseArcGISField(const GB_VariantMap& fieldMap, GeoVectorField& outField);

    /** @brief 尝试将 GB_Variant 中保存的字段对象解析为 GeoVectorField。要求实际类型为 GB_VariantMap。 */
    static bool TryParseArcGISField(const GB_Variant& fieldVariant, GeoVectorField& outField);

    /** @brief 解析 ArcGIS fields[]。要求 fieldsVariant 实际类型为 GB_VariantList。非法项会被跳过。 */
    static GeoVectorFields ParseArcGISFields(const GB_Variant& fieldsVariant);

    /** @brief 解析 ArcGIS fields[]。非法项会被跳过。 */
    static GeoVectorFields ParseArcGISFields(const GB_VariantList& fieldList);

    /**
     * @brief 按字段类型将属性值转换为推荐存储类型。
     *
     * 说明：
     * - inputValue 为空时，outValue 也置为空并返回 true；
     * - Unknown / Raster / Geometry 会直接保留原始 GB_Variant；
     * - Int16 会额外检查 [-32768, 32767] 范围；
     * - Float / Double 均转为 double，以减少属性表计算时的分支。
     */
    static bool TryConvertValueToStorageType(GeoVectorFieldType fieldType, const GB_Variant& inputValue, GB_Variant& outValue);

    /**
     * @brief 按字段定义转换属性值，并检查 nullable 与 maxLength。
     *
     * @param allowNull 当 inputValue 为空时是否允许返回 true；字段自身 nullable=false 时仍会返回 false。
     */
    static bool TryConvertValueToField(const GeoVectorField& field, const GB_Variant& inputValue, GB_Variant& outValue, bool allowNull = true);

    /** @brief 判断属性值是否能按字段类型转换为推荐存储类型。 */
    static bool IsValueCompatibleWithFieldType(GeoVectorFieldType fieldType, const GB_Variant& inputValue, bool allowNull = true);

    /** @brief 判断属性值是否能按字段定义转换，并满足 nullable 与 maxLength 约束。 */
    static bool IsValueCompatibleWithField(const GeoVectorField& field, const GB_Variant& inputValue, bool allowNull = true);
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
