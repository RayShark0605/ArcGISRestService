#ifndef GEO_VECTOR_GEOMETRY_H
#define GEO_VECTOR_GEOMETRY_H

#include "ArcGISRestServicePort.h"
#include "GeoBase/GB_Variant.h"
#include "GeoBase/Geometry/GB_Point2d.h"
#include "GeoBase/Geometry/GB_Polyline.h"
#include "GeoBase/Geometry/GB_Rectangle.h"

#include <cstddef>
#include <string>
#include <vector>

/**
 * @brief GIS 矢量要素几何类型。
 */
enum class ARCGIS_RESTSERVICE_PORT GeoVectorGeometryType
{
    Unknown = 0,
    Point,
    Polyline,
    Polygon
};

/**
 * @brief GIS 矢量要素的几何部分。
 *
 * 设计约定：
 * - 一个 GeoVectorGeometry 对应一个 GIS 矢量要素的几何部分；
 * - 点要素可包含多个点 part，内部数据为 std::vector<GB_Point2d>；
 * - 线要素可包含多条线 part，内部数据为 std::vector<GB_Polyline>；
 * - 面要素可包含多个面 part，内部数据为 std::vector<GB_Polyline>，每个 part 都以闭合多段线表示一个多边形；
 * - geometryType 负责区分线和面，因为二者的底层存储类型均为 std::vector<GB_Polyline>；
 * - 空的已知类型几何是合法对象；Unknown 表示未设置类型，不视为有效几何。
 */
class ARCGIS_RESTSERVICE_PORT GeoVectorGeometry
{
public:
    using PointDataType = std::vector<GB_Point2d>;
    using PolylineDataType = std::vector<GB_Polyline>;
    using PolygonDataType = std::vector<GB_Polyline>;

    /** @brief 构造 Unknown 空几何。 */
    GeoVectorGeometry();

    /** @brief 以点 part 数组构造点几何。 */
    explicit GeoVectorGeometry(const PointDataType& points);

    /** @brief 以点 part 数组构造点几何（移动版本）。 */
    explicit GeoVectorGeometry(PointDataType&& points);

    /**
     * @brief 构造线或面几何。
     * @param geometryType 只能为 GeoVectorGeometryType::Polyline 或 GeoVectorGeometryType::Polygon。
     * @param parts 线 part 或面 part。
     * @param closePolygonParts 当 geometryType 为 Polygon 时，是否自动补闭合点。
     */
    GeoVectorGeometry(GeoVectorGeometryType geometryType, const PolylineDataType& parts, bool closePolygonParts = true);
    GeoVectorGeometry(GeoVectorGeometryType geometryType, PolylineDataType&& parts, bool closePolygonParts = true);

    /** @brief 创建点几何。 */
    static GeoVectorGeometry CreatePointGeometry(const PointDataType& points);

    /** @brief 创建点几何（移动版本）。 */
    static GeoVectorGeometry CreatePointGeometry(PointDataType&& points);

    /** @brief 创建线几何。 */
    static GeoVectorGeometry CreatePolylineGeometry(const PolylineDataType& polylines);

    /** @brief 创建线几何（移动版本）。 */
    static GeoVectorGeometry CreatePolylineGeometry(PolylineDataType&& polylines);

    /** @brief 创建面几何。 */
    static GeoVectorGeometry CreatePolygonGeometry(const PolygonDataType& polygons, bool closePolygonParts = true);

    /** @brief 创建面几何（移动版本）。 */
    static GeoVectorGeometry CreatePolygonGeometry(PolygonDataType&& polygons, bool closePolygonParts = true);

    /** @brief 将几何类型转为可读字符串。 */
    static std::string GeometryTypeToString(GeoVectorGeometryType geometryType);

    /** @brief 根据 ArcGIS REST 几何类型字符串获取几何类型。 */
    static GeoVectorGeometryType GeometryTypeFromArcGISString(const std::string& geometryTypeText);

    /** @brief 将几何类型转为 ArcGIS REST 几何类型字符串。 */
    static std::string GeometryTypeToArcGISString(GeoVectorGeometryType geometryType);

    /** @brief 清空当前对象，类型重置为 Unknown。 */
    void Reset();

    /**
     * @brief 设置为指定类型的空几何。
     * @return geometryType 为 Unknown / Point / Polyline / Polygon 时返回 true，其它情况返回 false。
     */
    bool SetEmptyGeometry(GeoVectorGeometryType geometryType);

    /** @brief 获取当前几何类型。 */
    GeoVectorGeometryType GetGeometryType() const;

    /** @brief 获取当前几何类型字符串。 */
    std::string GetGeometryTypeString() const;

    /** @brief 当前是否为点几何。 */
    bool IsPoint() const;

    /** @brief 当前是否为线几何。 */
    bool IsPolyline() const;

    /** @brief 当前是否为面几何。 */
    bool IsPolygon() const;

    /** @brief 当前是否为空几何。Unknown 也视为空。 */
    bool IsEmpty() const;

    /**
     * @brief 当前对象是否满足类型与数据一致性约定。
     * @note Unknown 返回 false；已知类型的空几何可以是有效对象。
     */
    bool IsValid() const;

    /** @brief 获取 part 数量。Point 返回点数量；Polyline / Polygon 返回 GB_Polyline 数量。 */
    size_t GetPartCount() const;

    /** @brief 获取总顶点数。Point 返回点数量；Polyline / Polygon 返回所有 part 的顶点数总和。 */
    size_t GetVertexCount() const;

    /** @brief 获取几何外包矩形。空几何或无效几何返回无效矩形。 */
    GB_Rectangle BoundingRectangle() const;

    /** @brief 以点数组设置点几何。失败时保持当前对象不变。 */
    bool SetPoints(const PointDataType& points);

    /** @brief 以点数组设置点几何（移动版本）。失败时保持当前对象不变。 */
    bool SetPoints(PointDataType&& points);

    /** @brief 以多段线数组设置线几何。失败时保持当前对象不变。 */
    bool SetPolylines(const PolylineDataType& polylines);

    /** @brief 以多段线数组设置线几何（移动版本）。失败时保持当前对象不变。 */
    bool SetPolylines(PolylineDataType&& polylines);

    /**
     * @brief 以闭合多段线数组设置面几何。失败时保持当前对象不变。
     * @param closePolygonParts 是否在 part 未闭合时自动追加首点使其闭合。
     */
    bool SetPolygons(const PolygonDataType& polygons, bool closePolygonParts = true);

    /** @brief 以闭合多段线数组设置面几何（移动版本）。失败时保持当前对象不变。 */
    bool SetPolygons(PolygonDataType&& polygons, bool closePolygonParts = true);

    /**
     * @brief 根据类型和 GB_Variant 设置几何数据。失败时保持当前对象不变。
     * @note data 中的实际类型必须与 geometryType 匹配。
     */
    bool SetGeometryData(GeoVectorGeometryType geometryType, const GB_Variant& data, bool closePolygonParts = true);

    /** @brief 根据类型和 GB_Variant 设置几何数据（移动版本）。失败时保持当前对象不变。 */
    bool SetGeometryData(GeoVectorGeometryType geometryType, GB_Variant&& data, bool closePolygonParts = true);

    /** @brief 追加一个点 part。当前为 Unknown 时会自动转为 Point。 */
    bool AddPoint(const GB_Point2d& point);

    /** @brief 追加一条线 part。当前为 Unknown 时会自动转为 Polyline。 */
    bool AddPolyline(const GB_Polyline& polyline);

    /** @brief 追加一个面 part。当前为 Unknown 时会自动转为 Polygon。 */
    bool AddPolygon(const GB_Polyline& polygon, bool closePolygonPart = true);

    /** @brief 获取点几何数据。若当前不是 Point，则返回空数组引用。 */
    const PointDataType& GetPoints() const;

    /** @brief 获取可写点几何数据。若当前不是 Point，则返回 nullptr。 */
    PointDataType* GetMutablePoints();

    /** @brief 获取只读点几何数据指针。若当前不是 Point，则返回 nullptr。 */
    const PointDataType* GetPointsPtr() const;

    /** @brief 获取线几何数据。若当前不是 Polyline，则返回空数组引用。 */
    const PolylineDataType& GetPolylines() const;

    /** @brief 获取可写线几何数据。若当前不是 Polyline，则返回 nullptr。 */
    PolylineDataType* GetMutablePolylines();

    /** @brief 获取只读线几何数据指针。若当前不是 Polyline，则返回 nullptr。 */
    const PolylineDataType* GetPolylinesPtr() const;

    /** @brief 获取面几何数据。若当前不是 Polygon，则返回空数组引用。 */
    const PolygonDataType& GetPolygons() const;

    /** @brief 获取可写面几何数据。若当前不是 Polygon，则返回 nullptr。 */
    PolygonDataType* GetMutablePolygons();

    /** @brief 获取只读面几何数据指针。若当前不是 Polygon，则返回 nullptr。 */
    const PolygonDataType* GetPolygonsPtr() const;

    /** @brief 获取原始 GB_Variant 数据，只读。 */
    const GB_Variant& GetGeometryData() const;

    /** @brief 获取指定点 part。失败返回 NaN 点。 */
    GB_Point2d GetPoint(size_t index) const;

    /** @brief 尝试获取指定点 part。 */
    bool TryGetPoint(size_t index, GB_Point2d& outPoint) const;

    /** @brief 获取指定线 part。失败返回 GB_Polyline::Invalid。 */
    GB_Polyline GetPolyline(size_t index) const;

    /** @brief 尝试获取指定线 part。 */
    bool TryGetPolyline(size_t index, GB_Polyline& outPolyline) const;

    /** @brief 获取指定面 part。失败返回 GB_Polyline::Invalid。 */
    GB_Polyline GetPolygon(size_t index) const;

    /** @brief 尝试获取指定面 part。 */
    bool TryGetPolygon(size_t index, GB_Polyline& outPolygon) const;

    /** @brief 严格比较几何类型和顶点坐标。 */
    bool operator==(const GeoVectorGeometry& other) const;

    /** @brief 严格比较几何类型和顶点坐标。 */
    bool operator!=(const GeoVectorGeometry& other) const;

    /** @brief 按容差比较几何类型和顶点坐标。 */
    bool IsNearEqual(const GeoVectorGeometry& other, double tolerance = GB_Epsilon) const;

private:
    GeoVectorGeometryType geometryType = GeoVectorGeometryType::Unknown;
    GB_Variant geometryData;
};

#endif
