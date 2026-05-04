#ifndef GEO_CRS_H
#define GEO_CRS_H

#include "ArcGISRestServicePort.h"
#include "GeoBase/Geometry/GB_Point2d.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class GeoBoundingBox;
class OGRSpatialReference;
class GB_Rectangle;

struct GeoCrsOgrSrsDeleter
{
    void operator()(OGRSpatialReference* srs) const noexcept;
};

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

class ARCGIS_RESTSERVICE_PORT GeoCrs
{
public:
    GeoCrs();

    GeoCrs(const GeoCrs& other);

    GeoCrs(GeoCrs&& other) noexcept;

    GeoCrs& operator=(const GeoCrs& other);

    GeoCrs& operator=(GeoCrs&& other) noexcept;

    virtual ~GeoCrs();

    static GeoCrs CreateFromEpsgCode(int epsgCode);

    static GeoCrs CreateFromWkt(const std::string& wktUtf8);

    static GeoCrs CreateFromUserInput(const std::string& definitionUtf8, bool allowNetworkAccess = true, bool allowFileAccess = true);

    // 获取唯一标识符
    // - 若 CRS 自身带 EPSG 权威码，则返回 "EPSG:<code>"。
    // - 否则返回 "WKT2_2018_HASH:<fnv1a64>"（基于 WKT2_2018 导出文本）。
    // - 注意：该 UID 不会触发 AutoIdentifyEPSG/FindBestMatch 推断，避免依赖外部 proj.db 状态而导致 UID 不稳定。
    std::string GetUidUtf8() const;

    // 判断两个坐标系是否相同
    bool operator==(const GeoCrs& other) const;

    bool operator!=(const GeoCrs& other) const;

    // 设置为 EPSG 坐标系。成功后替换当前内容；失败时保持当前对象不变。
    bool SetFromEpsgCode(int epsgCode);

    // 设置为 WKT 坐标系。成功后替换当前内容；失败时保持当前对象不变。
    bool SetFromWkt(const std::string& wktUtf8);

    // 设置为 GDAL/OGR SetFromUserInput() 支持的 CRS 定义。成功后替换当前内容；失败时保持当前对象不变。
    bool SetFromUserInput(const std::string& definitionUtf8, bool allowNetworkAccess = false, bool allowFileAccess = false);

    // 重置为空坐标系。返回 true 表示成功（内部 OGRSpatialReference 可用），false 表示失败。
    bool Reset();

    bool IsEmpty() const;

    bool IsValid() const;

    std::string GetNameUtf8() const;

    // 获取当前坐标系使用的参考椭球名称。
    // - 对 WKT2 坐标系，优先读取 ELLIPSOID 节点；
    // - 对 WKT1 坐标系，退化读取 SPHEROID 节点；
    // - 若坐标系为空或节点不存在，返回空字符串。
    std::string GetReferenceEllipsoidNameUtf8() const;

    bool IsGeographic() const;

    bool IsProjected() const;

    bool IsLocal() const;

    // 当前是否使用传统 GIS 坐标轴顺序。
    bool IsTraditionalGisAxisOrderEnabled() const;

    // 设置坐标轴顺序：
    //  - enable=true：传统 GIS 顺序 (X=经度/Easting, Y=纬度/Northing)
    //  - enable=false：遵循 CRS 权威机构定义的轴顺序（GDAL 3.0+ 默认行为）
    void SetTraditionalGisAxisOrder(bool enable);

    enum class WktFormat
    {
        Default,
        Wkt1Gdal,
        Wkt1Esri,
        Wkt2_2015,
        Wkt2_2018,
        Wkt2
    };

    std::string ExportToWktUtf8(WktFormat format = WktFormat::Wkt2_2018, bool multiline = false) const;

    std::string ExportToPrettyWktUtf8(bool simplify = false) const;

    std::string ExportToProj4Utf8() const;

    std::string ExportToProjJsonUtf8() const;

    int TryGetEpsgCode(bool tryAutoIdentify = true, bool tryFindBestMatch = false, int minMatchConfidence = 90) const;

    std::string ToEpsgStringUtf8() const;

    // 尝试获取根节点坐标系权威码字符串。
    // - 优先读取 WKT/对象自身的根节点 AUTHORITY/ID；
    // - tryAutoIdentify=true 时，会额外尝试 AutoIdentifyEPSG() 补识别 EPSG；
    // - tryFindBestMatch=true 时，会按 EPSG、ESRI 的顺序尝试 FindBestMatch()。
    // 返回示例："EPSG:4326"、"ESRI:102100"；失败返回空串。
    std::string ToAuthorityStringUtf8(bool tryAutoIdentify = true, bool tryFindBestMatch = true, int minMatchConfidence = 90) const;

    std::string ToOgcUrnStringUtf8() const;

    struct UnitsInfo
    {
        std::string nameUtf8 = "";
        double toSI = 1.0; // 线单位：乘它得到“米”；角单位：乘它得到“弧度”
    };

    UnitsInfo GetLinearUnits() const;

    UnitsInfo GetAngularUnits() const;

    // 获取/计算该 CRS 的 metersPerUnit（每 1 个坐标单位对应多少米）。
    // - 对于投影/本地 CRS：返回线单位到米的转换系数（例如英尺->米）。
    // - 对于地理 CRS：返回角单位对应的弧长（基于椭球长半轴，近似等于赤道处的米/度），
    //   即 metersPerUnit = angularUnitToRadians * semiMajorAxis。
    // - 若 CRS 为空或无法计算，返回 0。
    double GetMetersPerUnit() const;

    struct LonLatAreaSegment
    {
        double west = 0;
        double south = 0;
        double east = 0;
        double north = 0;
    };

    // 获取坐标系的经纬度有效范围分段表示（最多 2 段），用于处理跨越日期变更线的 CRS。
    // 返回的每段范围均满足 west <= east，单位为度，且使用 EPSG:4326 的经度/纬度含义。
    std::vector<LonLatAreaSegment> GetValidAreaLonLatSegments() const;

    // 获取坐标系在它本身下的范围。
    // 注意：本函数返回的是该范围的轴对齐包络矩形；若需要跨日期变更线分段、非矩形边界等更精确表达，
    // 请使用 GetValidAreaPolygons()。
    GeoBoundingBox GetValidArea() const;

    // 获取坐标系在它本身下的有效范围多边形。
    // - 返回值：外层 vector 表示多个分段区域；内层 vector 表示单个闭合多边形环。
    // - 坐标单位与轴顺序遵循当前 GeoCrs 对象本身：默认使用传统 GIS 顺序 (X=经度/Easting, Y=纬度/Northing)；
    //   当 SetTraditionalGisAxisOrder(false) 后，会尽量遵循 CRS 权威轴序。
    // - edgeSampleCount 表示每条经纬度边界边的采样点数量，最小会夹取为 2；值越大，投影后曲线边界越精细。
    // - 当底层 CRS 缺少 area of use 或坐标变换失败到无法形成有效多边形时，返回空数组。
    std::vector<std::vector<GB_Point2d>> GetValidAreaPolygons(int edgeSampleCount = 129) const;

    // 获取坐标系的经纬度范围（EPSG:4326）。
    // 注意：GeoBoundingBox 只能表示一个矩形。当 CRS 的有效范围跨越日期变更线时，
    // 本函数会返回保守的全球经度范围 [-180, 180]；如需更精确的分段范围，请使用 GetValidAreaLonLatSegments()。
    GeoBoundingBox GetValidAreaLonLat() const;

    const OGRSpatialReference* GetConst() const;

    // 更安全的只读访问：返回内部对象的引用。
    const OGRSpatialReference& GetConstRef() const;

    // 克隆一份 OGRSpatialReference。
    // - 返回对象由 GeoCrsOgrSrsDeleter 负责释放；
    // - 克隆会保留当前 GeoCrs 的轴顺序策略；
    // - 适合需要跨锁、跨线程或长期持有 OGRSpatialReference 的内部调用方使用。
    std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> CloneOgrSpatialReference() const;

    // 注意：返回的引用为内部对象，修改会直接影响 GeoCrs。
    // 本类对 const 接口提供内部互斥保护，允许并发只读；但通过 GetRef()/Get() 取得的可写引用/指针
    // 不提供跨线程安全保证，且不应在多个线程中长期持有并同时读写。
    OGRSpatialReference& GetRef();

    // 注意：返回的指针为内部对象（借用，不转移所有权）。不要对返回指针调用 Release()/delete。
    // 若需要可写访问，更推荐使用 GetRef()。
    OGRSpatialReference* Get();

private:
    void InvalidateCaches() const;
    void InvalidateCachesNoLock() const;

    bool ResetNoLock();

    bool IsEmptyNoLock() const;

    bool IsValidNoLock() const;

    bool IsAxisOrderReversedByAuthorityNoLock() const;

    OGRSpatialReference* EnsureSpatialReferenceNoLock();

    int TryGetEpsgCodeNoLock(bool tryAutoIdentify, bool tryFindBestMatch, int minMatchConfidence) const;

    std::string ToAuthorityStringUtf8NoLock(bool tryAutoIdentify, bool tryFindBestMatch, int minMatchConfidence) const;

    std::string ExportToWktUtf8NoLock(WktFormat format, bool multiline) const;

    std::vector<LonLatAreaSegment> GetValidAreaLonLatSegmentsNoLock() const;

    GeoBoundingBox GetValidAreaLonLatNoLock() const;

    GeoBoundingBox GetValidAreaNoLock() const;

    std::vector<std::vector<GB_Point2d>> GetValidAreaPolygonsNoLock(int edgeSampleCount) const;

private:
    std::unique_ptr<OGRSpatialReference, GeoCrsOgrSrsDeleter> spatialReference;

    // GDAL 3.0+ 起坐标轴顺序默认遵循 CRS 定义（例如 EPSG:4326 为纬度/经度）。
    // 本类默认使用传统 GIS 顺序以减少“经纬度顺序”踩坑。
    bool useTraditionalGisAxisOrder = true;

    // 互斥：保护 spatialReference 与所有缓存字段，使 const 接口在并发读场景下安全。
    mutable std::recursive_mutex mutex;

    // ---- 缓存：避免重复进行 Validate / AutoIdentifyEPSG / FindBestMatch 等可能较重的逻辑 ----
    // IsValid() 的结果缓存
    mutable bool hasCachedIsValid = false;
    mutable bool cachedIsValid = false;

    // 默认参数（tryAutoIdentify=true, tryFindBestMatch=false, minMatchConfidence=90）下的 EPSG 结果缓存
    mutable bool hasCachedDefaultEpsgCode = false;
    mutable int cachedDefaultEpsgCode = 0; // 0 表示未能得到 EPSG code 或者为空

    // GetUidUtf8() 的缓存：
    //  cachedUidKind:
    //      -2：未计算
    //      -1：空/不可用
    //       0：使用 cachedUidWktHash (WKT2_2018 的哈希)
    //     > 0：EPSG code
    mutable int cachedUidKind = -2;
    mutable std::uint64_t cachedUidWktHash = 0;

    // GetMetersPerUnit() 的缓存
    mutable bool hasCachedMetersPerUnit = false;
    mutable double cachedMetersPerUnit = 0.0;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
