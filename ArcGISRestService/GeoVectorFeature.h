#ifndef GEO_VECTOR_FEATURE_H
#define GEO_VECTOR_FEATURE_H

#include "ArcGISRestServicePort.h"
#include "GeoVectorField.h"
#include "GeoVectorGeometry.h"
#include "GeoBase/GB_Variant.h"

#include <cstddef>
#include <string>

/**
 * @brief 无效 FeatureId。
 *
 * 说明：
 * - fid 表示当前图层内部用于快速定位 GeoVectorFeature 的 FeatureId；
 * - fid 不是 ArcGIS 属性表中的 OBJECTID，也不应与 OBJECTID 字段强绑定；
 * - fid < 0 统一视为未分配或临时要素，GeoInvalidFid 是默认无效值。
 */
constexpr long long GeoInvalidFid = -1;

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief GIS 矢量要素。
 *
 * 一个 GeoVectorFeature 由三部分组成：
 * - uid：进程内/工程内用于跟踪该要素对象的 GUID 文本，由 GB_GenerateUuid() 生成，由类内部私有管理；
 * - fid：图层内部 FeatureId，用于图层内部快速索引、选择、缓存和更新；
 * - geometry + attributes：要素的几何与属性值。
 *
 * fid 与 ArcGIS OBJECTID 的关系：
 * - fid 是本模块的图层内部要素编号，语义接近 QGIS QgsFeatureId；
 * - ArcGIS OBJECTID 是数据源属性字段中的唯一行标识，应该作为普通字段值保存在 attributes 中；
 * - 当导入 ArcGIS Feature/Map 服务时，可以把 OBJECTID 字段值复制或映射为 fid 以提升查找效率，
 *   但这只是图层实现层面的策略，不是 GeoVectorFeature 本身的强约束。
 *
 * uid 的拷贝/移动语义：
 * - 默认构造和显式 ResetFidAndUid() / CloneAsNewFeature() 会生成新的 uid；
 * - 拷贝构造、拷贝赋值保持 uid 不变，表示复制的是同一个逻辑要素的值快照；
 * - 移动构造、移动赋值转移 uid，不重新生成，避免 std::vector 扩容等移动操作导致要素身份变化；
 * - 若需要创建一个“内容相同但身份全新”的要素，应调用 CloneAsNewFeature()。
 *
 * 属性布局约定：
 * - attributes 按字段集合 GeoVectorFields 的下标顺序保存属性值；
 * - GeoVectorFeature 自身不持有字段定义，因此按字段名访问属性时必须传入所属图层的 GeoVectorFields；
 * - attributes 可以短于字段集合，缺失的尾部字段按空 GB_Variant 处理；
 * - attributes 长于字段集合通常表示属性布局不兼容。
 */
class ARCGIS_RESTSERVICE_PORT GeoVectorFeature
{
public:
    /** @brief 图层内部 FeatureId。fid < 0 表示临时或未分配要素。 */
    long long fid = GeoInvalidFid;

    /** @brief 要素几何。 */
    GeoVectorGeometry geometry;

    /** @brief 要素属性值列表，顺序与所属图层的 GeoVectorFields 保持一致。 */
    GB_VariantList attributes;

    /** @brief 构造一个临时空要素，并生成新的 uid。 */
    GeoVectorFeature();

    /** @brief 以几何构造临时要素，并生成新的 uid。 */
    explicit GeoVectorFeature(const GeoVectorGeometry& featureGeometry);

    /** @brief 以几何构造临时要素（移动版本），并生成新的 uid。 */
    explicit GeoVectorFeature(GeoVectorGeometry&& featureGeometry);

    /** @brief 以 fid、几何和属性构造要素，并生成新的 uid。 */
    GeoVectorFeature(long long featureId, const GeoVectorGeometry& featureGeometry, const GB_VariantList& featureAttributes = GB_VariantList());

    /** @brief 以 fid、几何和属性构造要素（移动版本），并生成新的 uid。 */
    GeoVectorFeature(long long featureId, GeoVectorGeometry&& featureGeometry, GB_VariantList&& featureAttributes);

    /** @brief 拷贝构造。uid / fid / geometry / attributes 均按值复制，不重新生成 uid。 */
    GeoVectorFeature(const GeoVectorFeature& other);

    /** @brief 移动构造。uid / fid / geometry / attributes 均转移，不重新生成 uid。 */
    GeoVectorFeature(GeoVectorFeature&& other) noexcept;

    ~GeoVectorFeature();

    /** @brief 拷贝赋值。uid 保持与源对象一致，不重新生成。 */
    GeoVectorFeature& operator=(const GeoVectorFeature& other);

    /** @brief 移动赋值。uid 从源对象转移，不重新生成。 */
    GeoVectorFeature& operator=(GeoVectorFeature&& other) noexcept;

    /** @brief 交换两个要素的全部内容。 */
    void Swap(GeoVectorFeature& other) noexcept;

    /** @brief 生成一个新的 uid。失败时返回空字符串。 */
    static std::string GenerateUid();

    /** @brief 获取当前要素 uid。uid 由类内部管理，调用方不应直接修改。 */
    const std::string& GetUid() const;

    /** @brief 判断 uid 是否非空。 */
    bool HasValidUid() const;

    /** @brief 重新生成 uid。成功返回 true；生成失败时保持原 uid 不变并返回 false。 */
    bool RegenerateUid();

    /** @brief 判断 fid 是否有效。当前约定 fid >= 0 为有效。 */
    bool HasValidFid() const;

    /** @brief 判断是否为临时要素。当前约定 fid < 0 为临时或未分配要素。 */
    bool IsTemporaryFeature() const;

    /** @brief 设置图层内部 FeatureId。 */
    void SetFid(long long featureId);

    /** @brief 将 fid 设置为 GeoInvalidFid。 */
    void InvalidateFid();

    /** @brief 将 fid 置为无效，并重新生成 uid。 */
    void ResetFidAndUid();

    /**
     * @brief 克隆为一个新要素。
     *
     * 返回值会复制当前 geometry 与 attributes，但 fid 会被置为 GeoInvalidFid，uid 会重新生成。
     * 该接口适用于“复制粘贴要素”“由模板创建新要素”等场景。
     */
    GeoVectorFeature CloneAsNewFeature() const;

    /** @brief 清空几何和属性，fid 置为无效，并重新生成 uid。 */
    void Reset();

    /**
     * @brief 判断要素状态是否可用。
     *
     * @param requireValidGeometry 是否要求 geometry.IsValid()。临时要素 fid 无效仍可视为可用。
     */
    bool IsValid(bool requireValidGeometry = true) const;

    /** @brief 判断当前几何对象是否有效。 */
    bool HasValidGeometry() const;

    /** @brief 判断当前几何对象是否有效且非空。 */
    bool HasNonEmptyGeometry() const;

    /** @brief 当前几何是否为空。Unknown 也视为空。 */
    bool IsEmptyGeometry() const;

    /** @brief 获取几何类型。 */
    GeoVectorGeometryType GetGeometryType() const;

    /** @brief 获取要素几何外包矩形。无有效几何时返回无效矩形。 */
    GB_Rectangle BoundingRectangle() const;

    /** @brief 获取属性值数量。 */
    size_t GetAttributeCount() const;

    /** @brief 判断属性下标是否在 attributes 范围内。 */
    bool IsAttributeIndexValid(size_t attributeIndex) const;

    /** @brief 判断指定属性槽位是否存在且不是空 GB_Variant。 */
    bool HasAttribute(size_t attributeIndex) const;

    /** @brief 清空所有属性值。 */
    void ClearAttributes();

    /** @brief 以拷贝方式整体设置属性列表。 */
    void SetAttributes(const GB_VariantList& featureAttributes);

    /** @brief 以移动方式整体设置属性列表。 */
    void SetAttributes(GB_VariantList&& featureAttributes);

    /** @brief 调整属性数量。新增槽位使用 fillValue 填充。 */
    bool ResizeAttributes(size_t attributeCount, const GB_Variant& fillValue = GB_Variant());

    /** @brief 确保属性数量至少为 minAttributeCount。新增槽位使用 fillValue 填充。 */
    bool EnsureAttributeCount(size_t minAttributeCount, const GB_Variant& fillValue = GB_Variant());

    /** @brief 获取指定下标属性的只读指针。越界返回 nullptr。 */
    const GB_Variant* GetAttributePtr(size_t attributeIndex) const;

    /** @brief 获取指定下标属性的可写指针。越界返回 nullptr。 */
    GB_Variant* GetMutableAttributePtr(size_t attributeIndex);

    /** @brief 尝试获取指定下标的属性值。 */
    bool TryGetAttribute(size_t attributeIndex, GB_Variant& outValue) const;

    /** @brief 获取指定下标的属性值。越界时返回 defaultValue。 */
    GB_Variant GetAttribute(size_t attributeIndex, const GB_Variant& defaultValue = GB_Variant()) const;

    /** @brief 设置指定下标的属性值。越界时返回 false。 */
    bool SetAttribute(size_t attributeIndex, const GB_Variant& value);

    /** @brief 设置指定下标的属性值（移动版本）。越界时返回 false。 */
    bool SetAttribute(size_t attributeIndex, GB_Variant&& value);

    /** @brief 追加一个属性值。 */
    void AddAttribute(const GB_Variant& value);

    /** @brief 追加一个属性值（移动版本）。 */
    void AddAttribute(GB_Variant&& value);

    /** @brief 判断字段集合中是否存在指定字段名。 */
    bool HasField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, bool caseSensitive = false) const;

    /** @brief 获取指定字段名在字段集合中的索引；不存在返回 -1。 */
    int IndexOfField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, bool caseSensitive = false) const;

    /** @brief 查找指定字段名的字段定义。不存在返回 nullptr。 */
    const GeoVectorField* FindField(const GeoVectorFields& fields, const std::string& fieldNameUtf8, bool caseSensitive = false) const;

    /** @brief 判断指定字段是否存在对应属性槽位。字段存在且索引小于 attributes.size() 时返回 true。 */
    bool HasAttribute(const GeoVectorFields& fields, const std::string& fieldNameUtf8, bool caseSensitive = false) const;

    /** @brief 获取指定字段名属性的只读指针。字段不存在或属性槽位不存在时返回 nullptr。 */
    const GB_Variant* GetAttributePtr(const GeoVectorFields& fields, const std::string& fieldNameUtf8, bool caseSensitive = false) const;

    /** @brief 获取指定字段名属性的可写指针。字段不存在或属性槽位不存在时返回 nullptr。 */
    GB_Variant* GetMutableAttributePtr(const GeoVectorFields& fields, const std::string& fieldNameUtf8, bool caseSensitive = false);

    /** @brief 尝试按字段名获取属性值。字段不存在或属性槽位不存在时返回 false。 */
    bool TryGetAttribute(const GeoVectorFields& fields, const std::string& fieldNameUtf8, GB_Variant& outValue, bool caseSensitive = false) const;

    /** @brief 按字段名获取属性值。失败时返回 defaultValue。 */
    GB_Variant GetAttribute(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const GB_Variant& defaultValue = GB_Variant(), bool caseSensitive = false) const;

    /** @brief 按字段名设置属性值。字段存在但属性数量不足时会自动扩展。 */
    bool SetAttribute(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const GB_Variant& value, bool caseSensitive = false);

    /** @brief 按字段名设置属性值（移动版本）。字段存在但属性数量不足时会自动扩展。 */
    bool SetAttribute(const GeoVectorFields& fields, const std::string& fieldNameUtf8, GB_Variant&& value, bool caseSensitive = false);

    /** @brief 按字段定义转换并设置属性值。字段存在但属性数量不足时会自动扩展。 */
    bool SetConvertedAttribute(const GeoVectorFields& fields, const std::string& fieldNameUtf8, const GB_Variant& value, bool allowNull = true, bool caseSensitive = false);

    /** @brief 按字段下标转换并设置属性值。字段下标或属性下标非法时返回 false。 */
    bool SetConvertedAttribute(const GeoVectorFields& fields, size_t fieldIndex, const GB_Variant& value, bool allowNull = true);

    /** @brief 确保属性数量至少等于字段数量。新增槽位为空 GB_Variant。 */
    bool EnsureAttributeCountForFields(const GeoVectorFields& fields);

    /** @brief 将属性数量调整为字段数量。多余属性会被截断，缺失属性会补空。 */
    bool NormalizeAttributeCountToFields(const GeoVectorFields& fields);

    /**
     * @brief 判断属性布局是否与字段集合兼容。
     *
     * @param allowMissingTrailingAttributes true 表示允许 attributes 短于 fields；false 表示数量必须相等。
     */
    bool IsAttributeLayoutCompatibleWithFields(const GeoVectorFields& fields, bool allowMissingTrailingAttributes = true) const;

    /**
     * @brief 校验已有属性是否能按字段定义转换，并满足 nullable / maxLength 约束。
     *
     * @param allowMissingTrailingAttributes true 表示允许 attributes 短于 fields；false 表示数量必须相等。
     * @param allowNull true 表示允许空属性通过 nullable 字段校验；nullable=false 的字段仍不允许空值。
     */
    bool ValidateAttributesAgainstFields(const GeoVectorFields& fields, bool allowMissingTrailingAttributes = true, bool allowNull = true) const;

    /**
     * @brief 将已有属性按字段定义转换为推荐存储类型。
     *
     * 转换采用先构造临时属性列表再提交的方式；任一字段转换失败时，当前 attributes 保持不变。
     */
    bool ConvertAttributesToFieldStorage(const GeoVectorFields& fields, bool allowMissingTrailingAttributes = true, bool allowNull = true);

    /**
     * @brief 按字段名生成属性映射表。
     *
     * @param includeMissingAttributes true 表示字段存在但属性槽位缺失时仍输出空 GB_Variant。
     */
    GB_VariantMap ToAttributeMap(const GeoVectorFields& fields, bool includeMissingAttributes = false) const;

    /**
     * @brief 从字段名到属性值的映射表设置 attributes。
     *
     * 输出属性顺序与 fields 保持一致；attributeMap 中不存在的字段会填空，无法匹配到 fields 的额外键会被忽略。
     * 当 convertValues=true 时，会按字段定义执行类型转换和 nullable / maxLength 校验。
     */
    bool SetAttributesFromMap(const GeoVectorFields& fields, const GB_VariantMap& attributeMap, bool convertValues = true, bool caseSensitive = false);

    /** @brief 尝试获取字段集合中 ObjectId 字段对应的属性值。注意该值不是 fid。 */
    bool TryGetObjectIdAttribute(const GeoVectorFields& fields, GB_Variant& outValue, bool caseSensitive = false) const;

    /** @brief 尝试获取字段集合中 ObjectId 字段对应的属性值，并转为 int64。注意该值不是 fid。 */
    bool TryGetObjectIdAttributeAsInt64(const GeoVectorFields& fields, long long& outValue, bool caseSensitive = false) const;

    /** @brief 尝试获取字段集合中 GlobalId 字段对应的属性值。 */
    bool TryGetGlobalIdAttribute(const GeoVectorFields& fields, std::string& outValue, bool caseSensitive = false) const;

    /** @brief 严格比较 uid、fid、geometry 和 attributes。 */
    bool operator==(const GeoVectorFeature& other) const;

    /** @brief 严格比较 uid、fid、geometry 和 attributes。 */
    bool operator!=(const GeoVectorFeature& other) const;

private:
    /** @brief 要素全局唯一标识文本，通常为 UUID/GUID 字符串。 */
    std::string uid = "";
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
