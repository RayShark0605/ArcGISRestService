#ifndef GEO_IO_H
#define GEO_IO_H

#include "ArcGISRestServicePort.h"
#include "GeoBoundingBox.h"
#include "GeoVectorFeature.h"
#include "GeoVectorField.h"
#include "GeoVectorGeometry.h"
#include "GeoBase/GB_Variant.h"
#include "GeoBase/Geometry/GB_Rectangle.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 基于 GDAL/OGR 的 Shapefile 矢量数据读写工具。
 *
 * 设计目标：
 * - 以静态工具类形式提供 Shapefile 读取、写出能力，不持有全局状态；
 * - 输出统一的 GeoVectorField / GeoVectorFeature / GeoVectorGeometry 数据结构；
 * - 尽量保留 Shapefile 数据源的元数据、字段定义、编码信息、空间参考和诊断信息；
 * - 头文件不暴露 GDAL 类型，避免调用方直接依赖 GDAL 头文件；
 * - 所有文件路径均约定为 UTF-8 编码。
 *
 * 重要约定：
 * - 当前 GeoVectorGeometry 只表达二维 Point / Polyline / Polygon，因此读取 Z/M Shapefile 时会降为二维，
 *   并在 LayerGeometryInfo 中标记 droppedZ / droppedM；
 * - Shapefile 一个图层只能存储一种几何大类，写出时会根据 WriteOptions 或输入数据推断 POINT / ARC / POLYGON / MULTIPOINT；
 * - Shapefile/DBF 字段名、字段类型、字段宽度存在格式限制。写出时会尽量通过 GDAL 的近似创建能力完成兼容处理。
 */
class ARCGIS_RESTSERVICE_PORT GeoIO_Shp
{
public:
#pragma region SourceFile
    /**
     * @brief Shapefile 相关文件角色。
     *
     * 一个完整的 Shapefile 通常由 .shp / .shx / .dbf 组成，并可能附带 .prj / .cpg / .qix 等文件。
     */
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

    /**
     * @brief Shapefile 单个源文件信息。
     */
    struct SourceFile
    {
        /** @brief 文件角色。 */
        SourceFileRole role = SourceFileRole::Unknown;

        /** @brief 文件路径，UTF-8 编码。 */
        std::string filePathUtf8 = "";

        /** @brief 文件是否存在。 */
        bool exists = false;

        /** @brief 文件大小，单位字节。不存在时为 0。 */
        std::uint64_t fileSizeBytes = 0;
    };

    /**
     * @brief Shapefile 相关源文件集合。
     */
    struct SourceFileSet
    {
        std::vector<SourceFile> files;

        /** @brief 获取指定角色对应的第一个文件路径；不存在时返回空字符串。 */
        std::string GetFilePathUtf8(SourceFileRole role) const;

        /** @brief 判断指定角色的源文件是否存在。 */
        bool HasFile(SourceFileRole role) const;
    };
#pragma endregion

#pragma region Diagnostic
    /**
     * @brief 读取诊断级别。
     */
    enum class ReadDiagnosticLevel
    {
        Info = 0,
        Warning,
        Error
    };

    /**
     * @brief 读取诊断信息。
     *
     * featureFid / fieldIndex 用于定位具体要素或字段；当不适用时分别为 GeoInvalidFid / -1。
     */
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
    /**
     * @brief Shapefile/DBF 文本编码识别信息。
     */
    struct EncodingInfo
    {
        /** @brief 是否存在 .cpg 文件。 */
        bool hasCpgFile = false;

        /** @brief .cpg 文件中的原始编码标识文本。 */
        std::string cpgValueUtf8 = "";

        /** @brief DBF 头中是否存在非零 LDID/codepage 值。 */
        bool hasLdidValue = false;

        /** @brief DBF 头中的 LDID/codepage 数值。 */
        int ldidValue = 0;

        /** @brief GDAL 从 .cpg 推断出的编码名。 */
        std::string encodingFromCpgUtf8 = "";

        /** @brief GDAL 从 LDID 推断出的编码名。 */
        std::string encodingFromLdidUtf8 = "";

        /** @brief GDAL 识别到的数据源编码。 */
        std::string sourceEncodingUtf8 = "";

        /** @brief 本次读取实际使用的编码。 */
        std::string usedEncodingUtf8 = "";

        /** @brief 是否由用户通过 ReadOptions::overrideEncodingUtf8 覆盖编码。 */
        bool encodingOverriddenByUser = false;

        /** @brief 是否让 GDAL 把字符串重编码为 UTF-8。 */
        bool recodeToUtf8 = true;
    };

    /**
     * @brief GDAL 数据源或图层元数据项。
     */
    struct MetadataItem
    {
        std::string domainUtf8 = "";
        std::string keyUtf8 = "";
        std::string valueUtf8 = "";
    };

    /**
     * @brief 源数据类型。
     */
    enum class SourceType
    {
        Unknown = 0,
        EsriShapefile,
        StandaloneDbf
    };

    /**
     * @brief 图层来源信息。
     */
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
    /**
     * @brief Shapefile 原生几何大类。
     */
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

    /**
     * @brief 图层几何信息。
     */
    struct LayerGeometryInfo
    {
        /** @brief 项目内部统一几何类型。 */
        GeoVectorGeometryType geometryType = GeoVectorGeometryType::Unknown;

        /** @brief Shapefile 原生几何大类。 */
        ShapefileGeometryKind shapefileGeometryKind = ShapefileGeometryKind::Unknown;

        /** @brief OGRwkbGeometryType 的整数编码，只保存编码，不在头文件暴露 GDAL 类型。 */
        int sourceWkbGeometryTypeCode = 0;

        /** @brief OGR 几何类型名称。 */
        std::string sourceWkbGeometryTypeNameUtf8 = "";

        /** @brief 源几何类型是否带 Z。 */
        bool hasZ = false;

        /** @brief 源几何类型是否带 M。 */
        bool hasM = false;

        /** @brief 是否存在空几何或 Null Shape。 */
        bool hasNullGeometry = false;

        /** @brief 是否存在单部件与多部件混合。 */
        bool hasMixedSingleAndMultipartGeometry = false;

        /** @brief 是否在 Polygon 中发现内环。当前 GeoVectorGeometry 会以独立闭合环保存，写出时会尝试重组洞关系。 */
        bool hasInteriorRing = false;

        /** @brief 是否由于 GeoVectorGeometry 当前只支持二维而丢弃了 Z。 */
        bool droppedZ = false;

        /** @brief 是否由于 GeoVectorGeometry 当前只支持二维而丢弃了 M。 */
        bool droppedM = false;

        /** @brief 是否出现 GeoVectorGeometry 当前不支持的几何类型。 */
        bool hasUnsupportedGeometry = false;
    };
#pragma endregion

#pragma region ShpData
    /**
     * @brief 图层空间信息。
     */
    struct LayerSpatialInfo
    {
        bool hasCrs = false;
        std::string crsWktUtf8 = "";
        std::string authorityCodeUtf8 = "";
        bool hasExtent = false;
        GB_Rectangle extent;
        GeoBoundingBox boundingBox;
    };

    /**
     * @brief 图层统计信息。
     */
    struct LayerStatistics
    {
        /** @brief 数据源返回的要素总数。-1 表示未知。 */
        long long featureCount = -1;

        /** @brief 实际装载到 features 中的要素数量。 */
        std::size_t loadedFeatureCount = 0;

        /** @brief 字段数量。 */
        std::size_t fieldCount = 0;

        /** @brief 空几何或 Null Shape 要素数量。 */
        std::size_t nullGeometryFeatureCount = 0;

        /** @brief 不支持几何要素数量。 */
        std::size_t unsupportedGeometryFeatureCount = 0;
    };

    /**
     * @brief 完整的 Shapefile 图层数据。
     */
    struct ShpData
    {
        /** @brief 图层真实名称。 */
        std::string layerNameUtf8 = "";

        /** @brief 展示名称。为空时可退化使用 layerNameUtf8。 */
        std::string displayNameUtf8 = "";

        LayerSourceInfo sourceInfo;
        LayerGeometryInfo geometryInfo;
        LayerSpatialInfo spatialInfo;
        LayerStatistics statistics;
        GeoVectorFields fields;
        std::vector<GeoVectorFeature> features;
        std::vector<MetadataItem> metadataItems;
        std::vector<ReadDiagnostic> diagnostics;

        /** @brief 判断当前对象是否至少包含可识别的数据源和图层名称。 */
        bool IsValid() const;

        /** @brief 清空所有内容并恢复默认状态。 */
        void Reset();
    };
#pragma endregion

#pragma region Progress
    /**
     * @brief Shapefile 读写进度。
     *
     * @details
     * - currentProgress：当前正在处理的要素下标，按 0 开始计数；处理完成后会被置为 totalProgress。
     * - totalProgress：本次读写的总工作量；若设置 maxFeatureCount，则为本次实际计划读取数量。
     */
    struct ReadWriteProgress
    {
        std::atomic_size_t currentProgress{ 0 };
        std::atomic_size_t totalProgress{ 0 };
    };
#pragma endregion

#pragma region Options
    /**
     * @brief Shapefile 读取选项。
     */
    struct ReadOptions
    {
        ReadOptions();

        /** @brief 要读取的图层下标。普通 .shp 文件通常只有 0 号图层。 */
        int layerIndex;

        /** @brief 指定图层名。非空时优先按名称查找图层，找不到则失败。 */
        std::string layerNameUtf8;

        /** @brief 用户指定源 DBF 编码。为空时由 GDAL 根据 .cpg / LDID 自动判断。 */
        std::string overrideEncodingUtf8;

        /** @brief 是否让 GDAL 将字符串字段重编码为 UTF-8。false 时会尽量保留 DBF 原始字节解释。 */
        bool recodeTextToUtf8;

        /** @brief 是否启用 OGR_ORGANIZE_POLYGONS=DEFAULT 修复方向错误的面环关系。 */
        bool organizePolygons;

        /** @brief 是否读取字段定义。通常应保持 true。 */
        bool loadFields;

        /** @brief 是否读取要素。若只想读取图层结构和空间信息，可设为 false。 */
        bool loadFeatures;

        /** @brief 是否装载空几何要素。false 时空几何要素会被跳过。 */
        bool includeNullGeometryFeatures;

        /** @brief 是否跳过不支持几何类型的要素。false 时会以空几何保留该要素属性。 */
        bool skipUnsupportedGeometries;

        /** @brief 是否读取 GDAL 元数据。 */
        bool loadMetadata;

        /** @brief 是否收集 .shp/.dbf/.prj/.cpg 等源文件信息。 */
        bool collectSourceFiles;

        /** @brief 是否把 GDAL FID 复制到 GeoVectorFeature::fid。 */
        bool useGdalFeatureIdAsFid;

        /** @brief 最大读取要素数量。0 表示不限制。 */
        std::size_t maxFeatureCount;
    };

    /**
     * @brief Shapefile 写出选项。
     */
    struct WriteOptions
    {
        WriteOptions();

        /** @brief 目标文件存在时是否允许覆盖。 */
        bool overwrite;

        /** @brief 输出图层名。为空时使用 data.layerNameUtf8；仍为空时使用目标文件名。 */
        std::string layerNameUtf8;

        /** @brief 输出坐标系 WKT。为空时使用 data.spatialInfo.crsWktUtf8。 */
        std::string crsWktUtf8;

        /** @brief 输出几何类型。Unknown 时从 data.geometryInfo 或要素几何自动推断。 */
        GeoVectorGeometryType geometryType;

        /** @brief DBF 编码。为空时不向 GDAL 传递 ENCODING 图层创建选项。建议使用 "UTF-8"。 */
        std::string encodingUtf8;

        /** @brief 是否额外写出 .cpg 文件。GDAL 通常会处理编码信息；该选项用于增强互操作性。 */
        bool writeCpgFile;

        /** @brief 是否跳过 Shapefile/DBF 不支持的字段类型。false 时遇到不支持字段会失败。 */
        bool skipUnsupportedFields;

        /** @brief 是否允许 GDAL 近似创建字段，例如截断过长 DBF 字段名。 */
        bool approximateFieldDefinition;

        /** @brief 是否跳过空几何要素。false 时会写出空几何要素。 */
        bool skipEmptyGeometryFeatures;

        /** @brief 是否把 GeoVectorFeature::fid 写作 OGR FID。通常不建议开启，避免破坏 Shapefile 记录号语义。 */
        bool preserveFeatureId;

        /** @brief 是否在写出后创建 .qix 空间索引。 */
        bool createSpatialIndex;

        /** @brief 写出 Polygon 多环时是否尝试按环方向和包含关系重组外环/内环，以尽量保留 Shapefile 洞关系。 */
        bool organizePolygonRingsOnWrite;
    };
#pragma endregion

    /**
     * @brief 收集 Shapefile 同名配套文件信息。
     *
     * @param shpOrDbfFilePathUtf8 .shp 或 .dbf 文件路径，UTF-8 编码。
     * @return 源文件集合。即使部分文件不存在，也会返回对应角色项并标记 exists=false。
     */
    static SourceFileSet CollectSourceFileSet(const std::string& shpOrDbfFilePathUtf8);

    /**
     * @brief 判断给定路径是否看起来是 Shapefile 主文件或独立 DBF 文件。
     */
    static bool IsSupportedFilePath(const std::string& filePathUtf8);

    /**
     * @brief 从 Shapefile/DBF 读取图层数据。
     *
     * @param filePathUtf8 输入 .shp 或 .dbf 文件路径，UTF-8 编码。
     * @param outData 输出数据。成功时被替换为读取结果；失败时被重置。
     * @param options 读取选项。
     * @param errorMessageUtf8 可选错误信息。
     * @param progress 可选读写进度。非空且两个原子指针均非空时，会更新总工作量和当前读取位置。
     * @return true 表示读取成功；false 表示打开、解析或读取过程失败。
     */
    static bool Read(const std::string& filePathUtf8, ShpData& outData, const ReadOptions& options = ReadOptions(), std::string* errorMessageUtf8 = nullptr, ReadWriteProgress* progress = nullptr);

    /**
     * @brief 将 ShpData 写出为 Shapefile。
     *
     * @param filePathUtf8 输出 .shp 文件路径，UTF-8 编码。
     * @param data 待写出的图层数据。
     * @param options 写出选项。
     * @param errorMessageUtf8 可选错误信息。
     * @param progress 可选读写进度。非空且两个原子指针均非空时，会更新总工作量和当前写出位置。
     * @return true 表示写出成功；false 表示创建数据源、字段、几何或要素失败。
     */
    static bool Write(const std::string& filePathUtf8, const ShpData& data, const WriteOptions& options = WriteOptions(), std::string* errorMessageUtf8 = nullptr, ReadWriteProgress* progress = nullptr);
};

#endif
