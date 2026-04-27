#ifndef TILE_IMAGE_CACHE_H
#define TILE_IMAGE_CACHE_H

#include "ArcGISRestCapabilities.h"
#include "GeoBase/CV/GB_Image.h"
#include "GeoBase/Geometry/GB_Rectangle.h"

#include <cstddef>
#include <string>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief 瓦片/影像缓存索引信息。
 *
 * 设计说明：
 * - requestUrl 应传入最终用于请求影像的 URL。若外部还存在 urlPrefix、代理转发前缀等逻辑，
 *   建议把拼接后的实际下载 URL 写入 requestUrl，避免不同入口错误复用同一个缓存。
 * - imageExtent 通常已经被编码在 requestUrl 的 bbox / tile 行列号中，但仍纳入 key，
 *   用于增强索引语义，也便于动态导出影像场景下规避异常 URL 复用。
 * - sourceWktUtf8 / targetWktUtf8 可按需填写：
 *   1) 若缓存的是服务端原始返回影像，通常只需要填写 sourceWktUtf8 或留空 targetWktUtf8；
 *   2) 若缓存的是已经重投影后的显示影像，则必须把目标坐标系 targetWktUtf8 纳入 key。
 * - extraKeyUtf8 用于外部扩展，例如鉴权用户、样式版本、透明度、服务端 token 之外的业务条件等。
 */
struct TileImageCacheKey
{
	std::string serviceUrl = "";
	std::string layerId = "";
	std::string imageFormat = "";
	std::string requestUrl = "";
	GB_Rectangle imageExtent;
	std::string sourceWktUtf8 = "";
	std::string targetWktUtf8 = "";
	std::string extraKeyUtf8 = "";

	TileImageCacheKey();
	explicit TileImageCacheKey(const ImageRequestItem& requestItem);
	TileImageCacheKey(const ImageRequestItem& requestItem, const std::string& sourceWktUtf8, const std::string& targetWktUtf8 = "", const std::string& extraKeyUtf8 = "");

	bool IsValid() const;
	void SetFromImageRequestItem(const ImageRequestItem& requestItem);
	std::string SerializeToKeyText() const;
};

/**
 * @brief 瓦片/影像磁盘缓存管理器。
 *
 * 设计目标：
 * - 使用全局静态接口，内部由一个进程级单例状态承载，避免调用方管理生命周期；
 * - 缓存目录可配置，不配置时默认使用当前用户临时目录下的 MapWeaver/TileCache；
 * - 文件索引采用稳定哈希，目录按哈希前缀分桶，避免单目录文件数过多；
 * - 每个缓存文件都配套一个 .meta 元数据文件，记录最近访问时间、格式、大小等信息；
 * - 读取时会真实解码影像，确保“存在”不仅代表文件存在，也代表影像可被 GB_Image 正确读取；
 * - 写入后会按 LRU 策略清理最久未访问缓存，使总占用不超过最大容量；
 * - 所有公开接口均是线程安全的。
 *
 * 性能说明：
 * - 缓存目录扫描只在首次使用、切换目录或主动 RebuildIndex() 时执行；
 * - 查询/读取命中时会更新 LRU 元数据；
 * - 为避免清理线程删除正在读取的文件，读取阶段会对缓存 UID 做短期 pin 保护；
 *   图像解码本身不长时间持有全局索引锁。
 */
class TileImageCache
{
public:
	static size_t GetDefaultMaxCacheSizeBytes();

	// 设置缓存目录（UTF-8）。支持 '\\' 和 '/' 两种路径分隔符。目录不存在时会递归创建。
	static bool SetCacheDirectoryUtf8(const std::string& cacheDirectoryUtf8);

	// 获取当前缓存目录（UTF-8），统一使用 '/'，且保证以 '/' 结尾。
	static std::string GetCacheDirectoryUtf8();

	// 设置最大缓存容量（字节）。设置为 0 表示禁用缓存并清空现有缓存。
	static bool SetMaxCacheSizeBytes(size_t maxCacheSizeBytes);
	static size_t GetMaxCacheSizeBytes();

	static bool SetMaxCacheSizeMB(size_t maxCacheSizeMB);
	static double GetMaxCacheSizeMB();

	// 获取当前缓存占用大小（字节，包含影像文件和元数据文件）。
	static size_t GetCurrentCacheSizeBytes();

	// 根据 key 构建稳定缓存 UID。
	static std::string BuildCacheUid(const TileImageCacheKey& cacheKey);

	// 获取 key 对应的缓存文件路径；若缓存尚不存在或 key 无效，则返回空字符串。
	static std::string GetCacheFilePathUtf8(const TileImageCacheKey& cacheKey);

	// 判断缓存中是否存在可读取的有效影像。命中时会刷新 LRU 访问时间。
	static bool ContainsValidImage(const TileImageCacheKey& cacheKey);

	// 从缓存中读取影像。若缓存不存在或影像不可读取，则返回 false，outImage 会被清空。
	static bool TryReadImage(const TileImageCacheKey& cacheKey, GB_Image& outImage, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());

	// 写入已编码的影像二进制数据。写入前会先验证该数据能被 GB_Image 解码。
	static bool PutEncodedImage(const TileImageCacheKey& cacheKey, const GB_ByteBuffer& encodedBytes, const std::string& preferredFileExtUtf8 = "");
	static bool PutEncodedImage(const TileImageCacheKey& cacheKey, const std::string& encodedBytes, const std::string& preferredFileExtUtf8 = "");

	// 写入内存影像。若 fileExtUtf8 为空，则优先根据 cacheKey.imageFormat 判断，最终默认写为 PNG。
	static bool PutImage(const TileImageCacheKey& cacheKey, const GB_Image& image, const std::string& fileExtUtf8 = "", const GB_ImageSaveOptions& saveOptions = GB_ImageSaveOptions());

	// 删除指定 key 对应的缓存。
	static bool Remove(const TileImageCacheKey& cacheKey);

	// 清空全部缓存文件。
	static bool Clear();

	// 重新扫描缓存目录，重建内存索引。
	static bool RebuildIndex();

	// 主动按当前容量上限执行 LRU 清理，返回实际删除的字节数。
	static size_t PruneToCapacity();

private:
	TileImageCache() = delete;
	~TileImageCache() = delete;
	TileImageCache(const TileImageCache&) = delete;
	TileImageCache& operator=(const TileImageCache&) = delete;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
