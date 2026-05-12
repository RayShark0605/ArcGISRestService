#ifndef VECTOR_CACHE_H
#define VECTOR_CACHE_H

#include "GeoBase/GB_BaseTypes.h"
#include "GeoBase/Geometry/GB_Rectangle.h"

#include <cstddef>
#include <string>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief ArcGIS REST 矢量查询缓存索引信息。
 *
 * 设计说明：
 * - requestUrl 应传入最终用于请求矢量 JSON 的 URL。若存在代理转发前缀、token、分页参数等，
 *   建议把拼接后的实际请求 URL 写入 requestUrl，避免不同入口错误复用同一缓存。
 * - queryExtent 通常已经被编码在 requestUrl 的 geometry 参数中，但仍纳入 key，
 *   用于增强索引语义，也便于后续扩展为非 URL 请求体缓存。
 * - sourceWktUtf8 表示服务端返回几何所在坐标系；targetWktUtf8 仅在缓存“已投影后的显示矢量”时使用。
 *   当前 LayerRefresher 缓存的是服务端原始 JSON，因此通常只填写 sourceWktUtf8。
 * - extraKeyUtf8 用于外部扩展，例如鉴权用户、请求头、样式版本、业务过滤条件等。
 */
struct VectorCacheKey
{
	std::string serviceUrl = "";
	std::string layerId = "";
	std::string requestUrl = "";
	GB_Rectangle queryExtent;
	std::string geometryTypeText = "";
	std::string sourceWktUtf8 = "";
	std::string targetWktUtf8 = "";
	std::string extraKeyUtf8 = "";

	VectorCacheKey();

	bool IsValid() const;
	std::string SerializeToKeyText() const;
};

/**
 * @brief ArcGIS REST 矢量 JSON 磁盘缓存管理器。
 *
 * 设计目标：
 * - 使用全局静态接口，内部由一个进程级单例状态承载，避免调用方管理生命周期；
 * - 缓存目录可配置，不配置时默认使用当前用户临时目录下的 MapWeaver/VectorCache；
 * - 文件索引采用稳定哈希，目录按哈希前缀分桶，避免单目录文件数过多；
 * - 每个缓存文件都配套一个 .meta 元数据文件，记录最近访问时间、格式、大小等信息；
 * - 读取和写入时都会校验 JSON 基本有效性，避免缓存网络错误页、HTML 错误页或 ArcGIS REST error 响应；
 * - 写入后会按 LRU 策略清理最久未访问缓存，使总占用不超过最大容量；
 * - 所有公开接口均是线程安全的。
 *
 * 性能说明：
 * - 缓存目录扫描只在首次使用、切换目录或主动 RebuildIndex() 时执行；
 * - 查询/读取命中时会更新 LRU 元数据；
 * - 为避免清理线程删除正在读取的文件，读取阶段会对缓存 UID 做短期 pin 保护；
 *   JSON 读取本身不长时间持有全局索引锁。
 */
class VectorCache
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

	// 获取当前缓存占用大小（字节，包含矢量 JSON 文件和元数据文件）。
	static size_t GetCurrentCacheSizeBytes();

	// 根据 key 构建稳定缓存 UID。
	static std::string BuildCacheUid(const VectorCacheKey& cacheKey);

	// 获取 key 对应的缓存文件路径；若缓存尚不存在或 key 无效，则返回空字符串。
	static std::string GetCacheFilePathUtf8(const VectorCacheKey& cacheKey);

	// 判断缓存中是否存在可读取的有效矢量 JSON。命中时会刷新 LRU 访问时间。
	static bool ContainsValidData(const VectorCacheKey& cacheKey);

	// 从缓存中读取矢量 JSON。若缓存不存在或内容不可用，则返回 false，outData 会被清空。
	static bool TryReadData(const VectorCacheKey& cacheKey, std::string& outData);
	static bool TryReadData(const VectorCacheKey& cacheKey, GB_ByteBuffer& outData);

	// 写入矢量 JSON。写入前会验证该数据可被解析为 ArcGIS REST JSON 对象，且不是 error 响应。
	static bool PutData(const VectorCacheKey& cacheKey, const std::string& data, const std::string& preferredFileExtUtf8 = "");
	static bool PutData(const VectorCacheKey& cacheKey, const GB_ByteBuffer& data, const std::string& preferredFileExtUtf8 = "");

	// 删除指定 key 对应的缓存。
	static bool Remove(const VectorCacheKey& cacheKey);

	// 清空全部缓存文件。
	static bool Clear();

	// 重新扫描缓存目录，重建内存索引。
	static bool RebuildIndex();

	// 主动按当前容量上限执行 LRU 清理，返回实际删除的字节数。
	static size_t PruneToCapacity();

private:
	VectorCache() = delete;
	~VectorCache() = delete;
	VectorCache(const VectorCache&) = delete;
	VectorCache& operator=(const VectorCache&) = delete;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
