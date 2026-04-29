#ifndef GEO_CRS_TRANSFORM_H
#define GEO_CRS_TRANSFORM_H

#include "ArcGISRestServicePort.h"
#include "GeoBoundingBox.h"
#include "GeoBase/CV/GB_Image.h"

#include <cstddef>
#include <string>
#include <vector>

class GB_Point2d;

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

// GeoImage
// - 带地理坐标范围的内存影像对象。
// - image：实际像素数据。
// - boundingBox：影像覆盖的坐标系 WKT 与该坐标系下的外接矩形范围。
struct ARCGIS_RESTSERVICE_PORT GeoImage
{
    GB_Image image;
    GeoBoundingBox boundingBox;

    GeoImage();
    GeoImage(const GB_Image& sourceImage, const GeoBoundingBox& sourceBoundingBox);
    GeoImage(GB_Image&& sourceImage, GeoBoundingBox&& sourceBoundingBox) noexcept;

    bool IsValid() const;
    void Reset();
    void Set(const GB_Image& image, const GeoBoundingBox& boundingBox);
    void Set(GB_Image&& image, GeoBoundingBox&& boundingBox) noexcept;
};

// GeoImageReprojectOptions
// - GeoImage 重投影选项。
// - outputWidth / outputHeight 为 0 时自动估算输出尺寸；两者均非 0 时使用外部指定尺寸。
// - targetPixelSizeX / targetPixelSizeY 大于 0 时优先使用指定目标像元大小。
// - maxOutputWidth / maxOutputHeight / maxOutputPixelCount 为 0 表示不限制。
// - clampToCrsValidArea 为 true 时，源图像有效参与范围与输出目标范围都会被限制在对应 CRS 的自身有效范围内。
struct ARCGIS_RESTSERVICE_PORT GeoImageReprojectOptions
{
    GB_ImageInterpolation interpolation = GB_ImageInterpolation::Linear;
    int sampleGridCount = 21;
    size_t outputWidth = 0;
    size_t outputHeight = 0;
    double targetPixelSizeX = 0.0;
    double targetPixelSizeY = 0.0;
    size_t maxOutputWidth = 0;
    size_t maxOutputHeight = 0;
    size_t maxOutputPixelCount = 0;
    bool enableOpenMP = false;
    bool clampToCrsValidArea = true;

    // 扫描线自适应近似变换误差阈值，单位为“源图像像素”。
    // - >0：启用近似变换。算法会参考 gdalwarp 的线性近似思路，
    //       用少量精确控制点约束插值误差，显著减少 PROJ/OGR 调用次数。
    // - <=0：完全逐像素精确变换，行为更接近旧实现，但速度较慢。
    // 默认 0.125 与 gdalwarp 常用默认误差阈值一致，适合交互式地图显示。
    double approxTransformErrorInSourcePixels = 0.125;

    // 单个近似线段允许覆盖的最大输出像素跨度。
    // 值越小，控制点越密、结果越保守；值越大，速度越快但更依赖误差判定。
    // 0 表示不限制线段长度，仅由误差阈值决定是否继续拆分。
    size_t maxApproxTransformSegmentLength = 64;
};

// GeoCrsTransform
// - 静态坐标系转换工具类（线程安全设计）：
//   1) 内部为每个线程维护一份 OGRCoordinateTransformation 缓存（避免跨线程共享 CT 对象）；
//   2) 全程采用传统 GIS 轴顺序（X=经度/Easting, Y=纬度/Northing），以与 GB_Point2d/GB_Rectangle 的语义一致；
//   3) 对 GeoBoundingBox 会先与源 CRS 的“自身有效范围”求交，再计算目标空间的 AABB：
//      - GDAL >= 3.4 优先使用 TransformBounds(densify_pts=sampleGridCount) 以更好地覆盖非线性投影边界；
//      - 其它版本/失败情况则退化为网格采样；
//      - 若目标为经纬度坐标系且跨越反经线（日期变更线），由于单个矩形无法表达两段经度，这里会保守返回 [-180,180]。
//   4) 对 GeoImage 会用目标像素中心反算回源 CRS 后重采样；源 CRS 有效范围外的源像素不参与输出，目标无源像素处保持全透明。
//   5) 变换失败时返回 false，并尽量保持输出/原地数据不被破坏。
class ARCGIS_RESTSERVICE_PORT GeoCrsTransform
{
public:
    // （1）把单个 GB_Point2d 从一个 WKT 转到另一个 WKT（输出到 outPoint）。
    static bool TransformPoint(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, const GB_Point2d& sourcePoint, GB_Point2d& outPoint);

    // （1）把单个 GB_Point2d 从一个 WKT 转到另一个 WKT（原地修改）。
    static bool TransformPoint(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, GB_Point2d& inOutPoint);

    // （2）将多个 GB_Point2d 从一个 WKT 转到另一个 WKT（输出到 outPoints）。
    // - enableOpenMp=true 时，若编译器支持 OpenMP，则并行处理。
    // - 返回值：所有点均成功变换返回 true；任一失败返回 false（但成功点仍会写入 outPoints）。
    static bool TransformPoints(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, const std::vector<GB_Point2d>& sourcePoints, std::vector<GB_Point2d>& outPoints, bool enableOpenMP = false);

    // （2）将多个 GB_Point2d 从一个 WKT 转到另一个 WKT（原地修改）。
    // - 返回值语义同上。
    static bool TransformPoints(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, std::vector<GB_Point2d>& inOutPoints, bool enableOpenMP = false);

    // （3）传入 x、y 坐标从一个 WKT 转到另一个 WKT。
    static bool TransformXY(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, double x, double y, double& outX, double& outY);

    // （3）传入 x、y、z 坐标从一个 WKT 转到另一个 WKT。
    static bool TransformXYZ(const std::string& sourceWktUtf8, const std::string& targetWktUtf8, double x, double y, double z, double& outX, double& outY, double& outZ);

    // （4）把单个 GeoBoundingBox 从当前 wkt 转到另一个 wkt（输出到 outBox）。
    static bool TransformBoundingBox(const GeoBoundingBox& sourceBox, const std::string& targetWktUtf8, GeoBoundingBox& outBox, int sampleGridCount = 11);

    // （4）把单个 GeoBoundingBox 从当前 wkt 转到另一个 wkt（原地修改）。
    static bool TransformBoundingBox(GeoBoundingBox& inOutBox, const std::string& targetWktUtf8, int sampleGridCount = 11);

    // （5）把多个 GeoBoundingBox 从各自的 wkt 转到另一个 wkt（输出到 outBoxes）。
    // - 返回值：所有 bbox 均成功变换返回 true；任一失败返回 false（失败项会被写成 Invalid）。
    static bool TransformBoundingBoxes(const std::vector<GeoBoundingBox>& sourceBoxes, const std::string& targetWktUtf8, std::vector<GeoBoundingBox>& outBoxes, bool enableOpenMP = false, int sampleGridCount = 11);

    // （5）把多个 GeoBoundingBox 从各自的 wkt 转到另一个 wkt（原地修改）。
    static bool TryTransformBoundingBoxes(std::vector<GeoBoundingBox>& inOutBoxes, const std::string& targetWktUtf8, bool enableOpenMP = false, int sampleGridCount = 11);

    // （6）重投影单个 GeoImage。
    // - 输出图像统一为 4 通道 BGRA，以保证无源像素区域可以被可靠表达为全透明。
    // - 若源 GeoImage 的 boundingBox 超出源 CRS 有效范围，默认仅使用有效范围内的部分参与输出。
    static bool ReprojectGeoImage(const GeoImage& sourceImage, const std::string& targetWktUtf8, GeoImage& outImage, const GeoImageReprojectOptions& options = GeoImageReprojectOptions());

    // （7）批量重投影 GeoImage。
    // - 输出 vector 与输入 vector 一一对应；失败项会被写成空 GeoImage。
    // - 返回值：全部成功返回 true；任一失败返回 false。
    static bool ReprojectGeoImages(const std::vector<GeoImage>& sourceImages, const std::string& targetWktUtf8, std::vector<GeoImage>& outImages, const GeoImageReprojectOptions& options = GeoImageReprojectOptions());

    // （8）批量重投影 GeoImage（原地修改）。
    static bool TryReprojectGeoImages(std::vector<GeoImage>& inOutImages, const std::string& targetWktUtf8, const GeoImageReprojectOptions& options = GeoImageReprojectOptions());

private:
    GeoCrsTransform() = delete;
    ~GeoCrsTransform() = delete;
    GeoCrsTransform(const GeoCrsTransform&) = delete;
    GeoCrsTransform& operator=(const GeoCrsTransform&) = delete;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
