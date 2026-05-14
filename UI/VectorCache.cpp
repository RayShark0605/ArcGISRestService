#include "VectorCache.h"

#include "GeoBase/GB_Crypto.h"
#include "GeoBase/GB_FileSystem.h"
#include "GeoBase/GB_FormatParser.h"
#include "GeoBase/GB_IO.h"
#include "GeoBase/GB_Utf8String.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	constexpr size_t DefaultMaxCacheSizeBytesValue = 500ULL * 1024ULL * 1024ULL;
	const char* const MetadataFileExt = ".meta";
	const char* const DefaultVectorFileExt = ".json";
	const char* const MetadataHeader = "MapWeaverVectorCacheMetaV1";

	struct CacheItem
	{
		std::string uid = "";
		std::string vectorFilePathUtf8 = "";
		std::string metadataFilePathUtf8 = "";
		std::string fileExtUtf8 = "";
		size_t vectorFileSizeBytes = 0;
		size_t metadataFileSizeBytes = 0;
		std::uint64_t lastAccessMs = 0;
	};

	struct CacheState
	{
		std::mutex mutex;
		std::string cacheDirectoryUtf8 = "";
		size_t maxCacheSizeBytes = DefaultMaxCacheSizeBytesValue;
		size_t currentCacheSizeBytes = 0;
		bool indexBuilt = false;
		std::unordered_map<std::string, CacheItem> itemsByUid;

		// 正在读取指定 uid 文件的线程数量。读矢量 JSON 时不长时间持有全局 mutex，
		// 但清理/删除逻辑仍不能删除正在被读取的文件。
		std::unordered_map<std::string, size_t> activeOperationsByUid;

		// 删除遇到正在读取的缓存项时，先从索引移除并扣减容量，文件实体延后删除。
		std::vector<CacheItem> deferredDeleteItems;
	};

	CacheState& GetState()
	{
		// 故意使用进程级泄漏单例，避免程序关闭时因静态析构顺序导致后台刷新线程
		// 访问已析构的 mutex/unordered_map。该状态在进程退出时由操作系统回收。
		static CacheState* state = new CacheState();
		return *state;
	}

	std::uint64_t GetCurrentTimeMs()
	{
		const auto now = std::chrono::system_clock::now();
		const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
		return static_cast<std::uint64_t>(duration.count());
	}

	std::string TrimAscii(const std::string& text)
	{
		size_t beginIndex = 0;
		while (beginIndex < text.size() && std::isspace(static_cast<unsigned char>(text[beginIndex])))
		{
			beginIndex++;
		}

		size_t endIndex = text.size();
		while (endIndex > beginIndex && std::isspace(static_cast<unsigned char>(text[endIndex - 1])))
		{
			endIndex--;
		}

		return text.substr(beginIndex, endIndex - beginIndex);
	}

	std::string ToLowerAscii(std::string text)
	{
		for (char& ch : text)
		{
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		}
		return text;
	}

	bool EndsWithAsciiNoCase(const std::string& text, const std::string& suffix)
	{
		if (text.size() < suffix.size())
		{
			return false;
		}

		const size_t offset = text.size() - suffix.size();
		for (size_t charIndex = 0; charIndex < suffix.size(); charIndex++)
		{
			const char left = static_cast<char>(std::tolower(static_cast<unsigned char>(text[offset + charIndex])));
			const char right = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[charIndex])));
			if (left != right)
			{
				return false;
			}
		}
		return true;
	}

	std::string NormalizeDirectoryUtf8(const std::string& directoryUtf8)
	{
		std::string normalized = TrimAscii(directoryUtf8);
		for (char& ch : normalized)
		{
			if (ch == '\\')
			{
				ch = '/';
			}
		}

		if (normalized.empty())
		{
			return std::string();
		}

		if (normalized.back() != '/')
		{
			normalized.push_back('/');
		}

		return GB_JoinPath("", normalized);
	}

	std::string GetDefaultCacheDirectoryUtf8()
	{
		std::string tempDirectory = GB_GetTempDirectory();
		if (!tempDirectory.empty())
		{
			return NormalizeDirectoryUtf8(GB_JoinPath(tempDirectory, "MapWeaver/VectorCache/"));
		}

		std::string homeDirectory = GB_GetHomeDirectory();
		if (!homeDirectory.empty())
		{
#ifdef _WIN32
			return NormalizeDirectoryUtf8(GB_JoinPath(homeDirectory, "AppData/Local/Temp/MapWeaver/VectorCache/"));
#else
			return NormalizeDirectoryUtf8(GB_JoinPath(homeDirectory, ".cache/MapWeaver/VectorCache/"));
#endif
		}

		return NormalizeDirectoryUtf8("./MapWeaver/VectorCache/");
	}

	std::string NormalizeFileExtUtf8(const std::string& fileExtUtf8)
	{
		std::string ext = ToLowerAscii(TrimAscii(fileExtUtf8));
		if (ext.empty())
		{
			return std::string();
		}

		if (ext[0] != '.')
		{
			ext.insert(ext.begin(), '.');
		}

		bool allSafe = ext.size() >= 2 && ext.size() <= 16;
		for (size_t charIndex = 1; charIndex < ext.size(); charIndex++)
		{
			const unsigned char ch = static_cast<unsigned char>(ext[charIndex]);
			if (!std::isalnum(ch))
			{
				allSafe = false;
				break;
			}
		}

		return allSafe ? ext : std::string();
	}

	std::string ChooseFileExtUtf8(const std::string& preferredFileExtUtf8)
	{
		std::string ext = NormalizeFileExtUtf8(preferredFileExtUtf8);
		if (!ext.empty())
		{
			return ext;
		}
		return DefaultVectorFileExt;
	}

	std::string FormatDoubleForKey(double value)
	{
		std::ostringstream stream;
		stream.precision(17);
		stream << value;
		return stream.str();
	}

	void AppendKeyValueLine(std::string& text, const std::string& key, const std::string& value)
	{
		text += key;
		text += "=";
		text += value;
		text += "\n";
	}

	std::string BuildUidFromKeyText(const std::string& keyText)
	{
		std::string hash = GB_ShaHash(keyText, GB_ShaMethod::Sha256);
		if (hash.empty())
		{
			hash = GB_Md5Hash(keyText);
		}
		return ToLowerAscii(hash);
	}

	bool IsSafeUid(const std::string& uid)
	{
		if (uid.size() < 16 || uid.size() > 128)
		{
			return false;
		}

		for (char ch : uid)
		{
			if (!std::isxdigit(static_cast<unsigned char>(ch)))
			{
				return false;
			}
		}
		return true;
	}

	std::string GetCacheBucketDirectoryNoLock(const CacheState& state, const std::string& uid)
	{
		if (state.cacheDirectoryUtf8.empty() || uid.size() < 4)
		{
			return std::string();
		}

		const std::string firstLevel = uid.substr(0, 2);
		const std::string secondLevel = uid.substr(2, 2);
		return GB_JoinPath(GB_JoinPath(state.cacheDirectoryUtf8, firstLevel + "/"), secondLevel + "/");
	}

	std::string GetMetadataPathNoLock(const CacheState& state, const std::string& uid)
	{
		const std::string bucketDirectory = GetCacheBucketDirectoryNoLock(state, uid);
		if (bucketDirectory.empty())
		{
			return std::string();
		}
		return GB_JoinPath(bucketDirectory, uid + MetadataFileExt);
	}

	std::string GetVectorPathNoLock(const CacheState& state, const std::string& uid, const std::string& fileExtUtf8)
	{
		const std::string bucketDirectory = GetCacheBucketDirectoryNoLock(state, uid);
		if (bucketDirectory.empty())
		{
			return std::string();
		}
		return GB_JoinPath(bucketDirectory, uid + NormalizeFileExtUtf8(fileExtUtf8));
	}

	bool TryParseUInt64(const std::string& text, std::uint64_t& outValue)
	{
		outValue = 0;
		const std::string trimmed = TrimAscii(text);
		if (trimmed.empty())
		{
			return false;
		}

		for (char ch : trimmed)
		{
			if (!std::isdigit(static_cast<unsigned char>(ch)))
			{
				return false;
			}
		}

		char* endPtr = nullptr;
		const unsigned long long parsed = std::strtoull(trimmed.c_str(), &endPtr, 10);
		if (endPtr == nullptr || *endPtr != '\0')
		{
			return false;
		}

		outValue = static_cast<std::uint64_t>(parsed);
		return true;
	}

	std::unordered_map<std::string, std::string> ParseMetadataText(const std::string& text)
	{
		std::unordered_map<std::string, std::string> values;
		std::istringstream stream(text);
		std::string line;
		bool isFirstLine = true;
		while (std::getline(stream, line))
		{
			if (!line.empty() && line.back() == '\r')
			{
				line.pop_back();
			}

			if (isFirstLine)
			{
				isFirstLine = false;
				if (line == MetadataHeader)
				{
					continue;
				}
			}

			const size_t equalPos = line.find('=');
			if (equalPos == std::string::npos)
			{
				continue;
			}

			const std::string key = line.substr(0, equalPos);
			const std::string value = line.substr(equalPos + 1);
			values[key] = value;
		}
		return values;
	}

	bool TryReadMetadata(const std::string& metadataFilePathUtf8, CacheItem& outItem)
	{
		outItem = CacheItem();

		if (!GB_IsFileExists(metadataFilePathUtf8))
		{
			return false;
		}

		const std::string metadataText = GB_ReadFromFile(metadataFilePathUtf8);
		if (metadataText.empty())
		{
			return false;
		}

		const std::unordered_map<std::string, std::string> values = ParseMetadataText(metadataText);
		const auto uidIter = values.find("uid");
		const auto extIter = values.find("fileExt");
		if (uidIter == values.end() || extIter == values.end())
		{
			return false;
		}

		const std::string uid = ToLowerAscii(TrimAscii(uidIter->second));
		const std::string fileExt = NormalizeFileExtUtf8(extIter->second);
		if (!IsSafeUid(uid) || fileExt.empty())
		{
			return false;
		}

		std::uint64_t lastAccessMs = 0;
		const auto accessIter = values.find("lastAccessMs");
		if (accessIter != values.end())
		{
			TryParseUInt64(accessIter->second, lastAccessMs);
		}
		if (lastAccessMs == 0)
		{
			lastAccessMs = GetCurrentTimeMs();
		}

		const std::string directoryPath = GB_GetDirectoryPath(metadataFilePathUtf8);
		const std::string vectorFilePath = GB_JoinPath(directoryPath, uid + fileExt);
		if (!GB_IsFileExists(vectorFilePath))
		{
			return false;
		}

		const size_t vectorFileSize = GB_GetFileSizeByte(vectorFilePath);
		if (vectorFileSize == 0)
		{
			return false;
		}

		outItem.uid = uid;
		outItem.vectorFilePathUtf8 = vectorFilePath;
		outItem.metadataFilePathUtf8 = metadataFilePathUtf8;
		outItem.fileExtUtf8 = fileExt;
		outItem.vectorFileSizeBytes = vectorFileSize;
		outItem.metadataFileSizeBytes = GB_GetFileSizeByte(metadataFilePathUtf8);
		outItem.lastAccessMs = lastAccessMs;
		return true;
	}

	std::string BuildMetadataText(const CacheItem& item)
	{
		std::string text;
		text.reserve(256);
		text += MetadataHeader;
		text += "\n";
		AppendKeyValueLine(text, "uid", item.uid);
		AppendKeyValueLine(text, "fileExt", item.fileExtUtf8);
		AppendKeyValueLine(text, "vectorFileSizeBytes", std::to_string(static_cast<unsigned long long>(item.vectorFileSizeBytes)));
		AppendKeyValueLine(text, "lastAccessMs", std::to_string(static_cast<unsigned long long>(item.lastAccessMs)));
		return text;
	}

	bool WriteMetadataNoLock(CacheState& state, CacheItem& item)
	{
		const size_t oldMetadataSize = item.metadataFileSizeBytes;
		const std::string metadataText = BuildMetadataText(item);
		if (!GB_WriteUtf8ToFile(item.metadataFilePathUtf8, metadataText, false, false))
		{
			return false;
		}

		item.metadataFileSizeBytes = GB_GetFileSizeByte(item.metadataFilePathUtf8);
		if (state.currentCacheSizeBytes >= oldMetadataSize)
		{
			state.currentCacheSizeBytes -= oldMetadataSize;
		}
		else
		{
			state.currentCacheSizeBytes = 0;
		}
		state.currentCacheSizeBytes += item.metadataFileSizeBytes;
		return true;
	}

	void DeleteCacheItemFilesNoLock(const CacheItem& item)
	{
		if (!item.vectorFilePathUtf8.empty() && GB_IsFileExists(item.vectorFilePathUtf8))
		{
			GB_DeleteFile(item.vectorFilePathUtf8);
		}
		if (!item.metadataFilePathUtf8.empty() && GB_IsFileExists(item.metadataFilePathUtf8))
		{
			GB_DeleteFile(item.metadataFilePathUtf8);
		}
	}

	bool IsUidActiveNoLock(const CacheState& state, const std::string& uid)
	{
		const auto iter = state.activeOperationsByUid.find(uid);
		return iter != state.activeOperationsByUid.end() && iter->second > 0;
	}

	void BeginActiveOperationNoLock(CacheState& state, const std::string& uid)
	{
		state.activeOperationsByUid[uid]++;
	}

	size_t GetItemTotalSize(const CacheItem& item);
	void SubtractCacheSizeNoLock(CacheState& state, size_t itemSize);

	std::vector<CacheItem> TakeDeferredDeleteItemsForUidNoLock(CacheState& state, const std::string& uid)
	{
		std::vector<CacheItem> itemsToDelete;
		if (state.deferredDeleteItems.empty())
		{
			return itemsToDelete;
		}

		std::vector<CacheItem> remainingItems;
		remainingItems.reserve(state.deferredDeleteItems.size());
		for (CacheItem& item : state.deferredDeleteItems)
		{
			if (item.uid == uid)
			{
				itemsToDelete.push_back(std::move(item));
			}
			else
			{
				remainingItems.push_back(std::move(item));
			}
		}

		state.deferredDeleteItems.swap(remainingItems);
		return itemsToDelete;
	}

	bool IsUidDeferredDeleteNoLock(const CacheState& state, const std::string& uid)
	{
		for (const CacheItem& item : state.deferredDeleteItems)
		{
			if (item.uid == uid)
			{
				return true;
			}
		}
		return false;
	}

	void EndActiveOperation(CacheState& state, const std::string& uid)
	{
		std::vector<CacheItem> itemsToDelete;
		{
			std::lock_guard<std::mutex> lock(state.mutex);

			auto iter = state.activeOperationsByUid.find(uid);
			if (iter == state.activeOperationsByUid.end())
			{
				return;
			}

			if (iter->second > 1)
			{
				iter->second--;
				return;
			}

			state.activeOperationsByUid.erase(iter);
			itemsToDelete = TakeDeferredDeleteItemsForUidNoLock(state, uid);
		}

		for (const CacheItem& item : itemsToDelete)
		{
			DeleteCacheItemFilesNoLock(item);
		}
	}

	class ActiveCacheOperationGuard
	{
	public:
		ActiveCacheOperationGuard() = default;

		ActiveCacheOperationGuard(CacheState& state, const std::string& uid)
			: state_(&state), uid_(uid), active_(true)
		{
		}

		~ActiveCacheOperationGuard()
		{
			Release();
		}

		ActiveCacheOperationGuard(const ActiveCacheOperationGuard&) = delete;
		ActiveCacheOperationGuard& operator=(const ActiveCacheOperationGuard&) = delete;

		ActiveCacheOperationGuard(ActiveCacheOperationGuard&& other) noexcept
			: state_(other.state_), uid_(std::move(other.uid_)), active_(other.active_)
		{
			other.state_ = nullptr;
			other.active_ = false;
		}

		ActiveCacheOperationGuard& operator=(ActiveCacheOperationGuard&& other) noexcept
		{
			if (this == &other)
			{
				return *this;
			}

			Release();
			state_ = other.state_;
			uid_ = std::move(other.uid_);
			active_ = other.active_;
			other.state_ = nullptr;
			other.active_ = false;
			return *this;
		}

		void Release()
		{
			if (!active_ || state_ == nullptr)
			{
				return;
			}

			CacheState* state = state_;
			const std::string uid = uid_;
			state_ = nullptr;
			uid_.clear();
			active_ = false;
			EndActiveOperation(*state, uid);
		}

	private:
		CacheState* state_ = nullptr;
		std::string uid_ = "";
		bool active_ = false;
	};

	size_t GetItemTotalSize(const CacheItem& item)
	{
		return item.vectorFileSizeBytes + item.metadataFileSizeBytes;
	}

	void SubtractCacheSizeNoLock(CacheState& state, size_t itemSize)
	{
		if (state.currentCacheSizeBytes >= itemSize)
		{
			state.currentCacheSizeBytes -= itemSize;
		}
		else
		{
			state.currentCacheSizeBytes = 0;
		}
	}

	void RemoveItemFromIndexNoLock(CacheState& state, const std::string& uid, bool deleteFiles, size_t* removedBytes = nullptr)
	{
		auto iter = state.itemsByUid.find(uid);
		if (iter == state.itemsByUid.end())
		{
			return;
		}

		const size_t itemSize = GetItemTotalSize(iter->second);
		if (deleteFiles)
		{
			if (IsUidActiveNoLock(state, uid))
			{
				state.deferredDeleteItems.push_back(iter->second);
			}
			else
			{
				DeleteCacheItemFilesNoLock(iter->second);
			}
		}

		SubtractCacheSizeNoLock(state, itemSize);
		if (removedBytes != nullptr)
		{
			*removedBytes += itemSize;
		}

		state.itemsByUid.erase(iter);
	}

	bool EnsureCacheDirectoryNoLock(CacheState& state)
	{
		if (state.cacheDirectoryUtf8.empty())
		{
			state.cacheDirectoryUtf8 = GetDefaultCacheDirectoryUtf8();
		}

		if (state.cacheDirectoryUtf8.empty())
		{
			return false;
		}

		if (!GB_CreateDirectory(state.cacheDirectoryUtf8))
		{
			return false;
		}

		return GB_IsDirectoryExists(state.cacheDirectoryUtf8);
	}

	bool RebuildIndexNoLock(CacheState& state)
	{
		state.itemsByUid.clear();
		state.currentCacheSizeBytes = 0;
		state.indexBuilt = false;

		if (!EnsureCacheDirectoryNoLock(state))
		{
			return false;
		}

		const std::vector<std::string> filePaths = GB_GetFilesList(state.cacheDirectoryUtf8, true);
		for (const std::string& filePath : filePaths)
		{
			if (!EndsWithAsciiNoCase(filePath, MetadataFileExt))
			{
				continue;
			}

			CacheItem item;
			if (!TryReadMetadata(filePath, item))
			{
				continue;
			}

			if (IsUidDeferredDeleteNoLock(state, item.uid))
			{
				continue;
			}

			const size_t itemSize = GetItemTotalSize(item);
			state.currentCacheSizeBytes += itemSize;
			state.itemsByUid[item.uid] = std::move(item);
		}

		state.indexBuilt = true;
		return true;
	}

	bool EnsureIndexNoLock(CacheState& state)
	{
		if (!EnsureCacheDirectoryNoLock(state))
		{
			return false;
		}

		if (state.indexBuilt)
		{
			return true;
		}

		return RebuildIndexNoLock(state);
	}

	size_t PruneToCapacityNoLock(CacheState& state, const std::string& protectedUid = "")
	{
		size_t removedBytes = 0;
		if (state.maxCacheSizeBytes == 0)
		{
			std::vector<std::string> uids;
			uids.reserve(state.itemsByUid.size());
			for (const auto& itemPair : state.itemsByUid)
			{
				uids.push_back(itemPair.first);
			}
			for (const std::string& uid : uids)
			{
				RemoveItemFromIndexNoLock(state, uid, true, &removedBytes);
			}
			return removedBytes;
		}

		while (state.currentCacheSizeBytes > state.maxCacheSizeBytes)
		{
			auto oldestIter = state.itemsByUid.end();
			for (auto iter = state.itemsByUid.begin(); iter != state.itemsByUid.end(); iter++)
			{
				if (!protectedUid.empty() && iter->first == protectedUid)
				{
					continue;
				}

				if (oldestIter == state.itemsByUid.end() || iter->second.lastAccessMs < oldestIter->second.lastAccessMs)
				{
					oldestIter = iter;
				}
			}

			if (oldestIter == state.itemsByUid.end())
			{
				break;
			}

			const std::string uid = oldestIter->first;
			RemoveItemFromIndexNoLock(state, uid, true, &removedBytes);
		}

		return removedBytes;
	}

	bool UpdateAccessTimeNoLock(CacheState& state, CacheItem& item)
	{
		item.lastAccessMs = GetCurrentTimeMs();
		return WriteMetadataNoLock(state, item);
	}

	bool IsValidArcGISRestVectorJson(const std::string& data)
	{
		if (TrimAscii(data).empty())
		{
			return false;
		}

		GB_VariantMap rootMap;
		std::string errorMessage;
		if (!GB_JsonParser::ParseToVariantMap(data, rootMap, &errorMessage))
		{
			return false;
		}

		// ArcGIS REST 错误响应通常为 {"error":{...}}。这种内容不能进入缓存，
		// 否则后续刷新会稳定命中错误结果而不再尝试网络请求。
		return rootMap.find("error") == rootMap.end();
	}

	void RemoveCorruptCacheItem(CacheState& state, const std::string& uid)
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		RemoveItemFromIndexNoLock(state, uid, true);
	}

	bool ValidateVectorFile(CacheState& state, const std::string& uid, CacheItem& item, std::string* outData)
	{
		if (!GB_IsFileExists(item.vectorFilePathUtf8) || GB_GetFileSizeByte(item.vectorFilePathUtf8) == 0)
		{
			RemoveCorruptCacheItem(state, uid);
			if (outData != nullptr)
			{
				outData->clear();
			}
			return false;
		}

		const std::string data = GB_ReadFromFile(item.vectorFilePathUtf8);
		if (!IsValidArcGISRestVectorJson(data))
		{
			RemoveCorruptCacheItem(state, uid);
			if (outData != nullptr)
			{
				outData->clear();
			}
			return false;
		}

		{
			std::lock_guard<std::mutex> lock(state.mutex);
			UpdateAccessTimeNoLock(state, item);
		}
		if (outData != nullptr)
		{
			*outData = data;
		}
		return true;
	}

	bool PutDataNoLock(CacheState& state, const VectorCacheKey& cacheKey, const std::string& data, const std::string& preferredFileExtUtf8)
	{
		if (!cacheKey.IsValid() || data.empty() || state.maxCacheSizeBytes == 0)
		{
			return false;
		}

		if (!EnsureIndexNoLock(state))
		{
			return false;
		}

		const std::string uid = BuildUidFromKeyText(cacheKey.SerializeToKeyText());
		if (!IsSafeUid(uid))
		{
			return false;
		}

		// 若同一 uid 正在被其它线程读取，则本次不写入缓存，避免覆盖正在读取的文件。
		// 调用方已经拿到了网络返回数据，本次跳过缓存不会影响当前刷新，只会等待下一次刷新再缓存。
		if (IsUidActiveNoLock(state, uid))
		{
			return false;
		}

		const std::string fileExt = ChooseFileExtUtf8(preferredFileExtUtf8);
		const std::string bucketDirectory = GetCacheBucketDirectoryNoLock(state, uid);
		if (bucketDirectory.empty() || !GB_CreateDirectory(bucketDirectory))
		{
			return false;
		}

		const std::string vectorFilePath = GetVectorPathNoLock(state, uid, fileExt);
		const std::string metadataFilePath = GetMetadataPathNoLock(state, uid);
		if (vectorFilePath.empty() || metadataFilePath.empty())
		{
			return false;
		}

		const size_t metadataEstimateBytes = 512;
		if (data.size() + metadataEstimateBytes > state.maxCacheSizeBytes)
		{
			return false;
		}

		RemoveItemFromIndexNoLock(state, uid, true);
		if (!GB_WriteBinaryToFile(data, vectorFilePath))
		{
			return false;
		}

		CacheItem item;
		item.uid = uid;
		item.vectorFilePathUtf8 = vectorFilePath;
		item.metadataFilePathUtf8 = metadataFilePath;
		item.fileExtUtf8 = fileExt;
		item.vectorFileSizeBytes = GB_GetFileSizeByte(vectorFilePath);
		item.metadataFileSizeBytes = 0;
		item.lastAccessMs = GetCurrentTimeMs();

		if (item.vectorFileSizeBytes == 0)
		{
			DeleteCacheItemFilesNoLock(item);
			return false;
		}

		state.currentCacheSizeBytes += item.vectorFileSizeBytes;
		if (!WriteMetadataNoLock(state, item))
		{
			DeleteCacheItemFilesNoLock(item);
			if (state.currentCacheSizeBytes >= item.vectorFileSizeBytes)
			{
				state.currentCacheSizeBytes -= item.vectorFileSizeBytes;
			}
			else
			{
				state.currentCacheSizeBytes = 0;
			}
			return false;
		}

		state.itemsByUid[uid] = item;
		PruneToCapacityNoLock(state, uid);
		if (state.currentCacheSizeBytes > state.maxCacheSizeBytes)
		{
			RemoveItemFromIndexNoLock(state, uid, true);
			return false;
		}

		return state.itemsByUid.find(uid) != state.itemsByUid.end();
	}
}

VectorCacheKey::VectorCacheKey() = default;

bool VectorCacheKey::IsValid() const
{
	return !TrimAscii(requestUrl).empty();
}

std::string VectorCacheKey::SerializeToKeyText() const
{
	std::string text;
	text.reserve(serviceUrl.size() + layerId.size() + requestUrl.size() + geometryTypeText.size() + sourceWktUtf8.size() + targetWktUtf8.size() + extraKeyUtf8.size() + 256);
	AppendKeyValueLine(text, "version", "1");
	AppendKeyValueLine(text, "serviceUrl", serviceUrl);
	AppendKeyValueLine(text, "layerId", layerId);
	AppendKeyValueLine(text, "requestUrl", requestUrl);
	AppendKeyValueLine(text, "geometryType", geometryTypeText);
	if (queryExtent.IsValid())
	{
		AppendKeyValueLine(text, "extentMinX", FormatDoubleForKey(queryExtent.minX));
		AppendKeyValueLine(text, "extentMinY", FormatDoubleForKey(queryExtent.minY));
		AppendKeyValueLine(text, "extentMaxX", FormatDoubleForKey(queryExtent.maxX));
		AppendKeyValueLine(text, "extentMaxY", FormatDoubleForKey(queryExtent.maxY));
	}
	else
	{
		AppendKeyValueLine(text, "extent", "invalid");
	}
	AppendKeyValueLine(text, "sourceWkt", sourceWktUtf8);
	AppendKeyValueLine(text, "targetWkt", targetWktUtf8);
	AppendKeyValueLine(text, "extraKey", extraKeyUtf8);
	return text;
}

size_t VectorCache::GetDefaultMaxCacheSizeBytes()
{
	return DefaultMaxCacheSizeBytesValue;
}

bool VectorCache::SetCacheDirectoryUtf8(const std::string& cacheDirectoryUtf8)
{
	CacheState& state = GetState();
	std::lock_guard<std::mutex> lock(state.mutex);

	std::string normalizedDirectory = NormalizeDirectoryUtf8(cacheDirectoryUtf8);
	if (normalizedDirectory.empty())
	{
		normalizedDirectory = GetDefaultCacheDirectoryUtf8();
	}

	if (normalizedDirectory.empty() || !GB_CreateDirectory(normalizedDirectory) || !GB_IsDirectoryExists(normalizedDirectory))
	{
		return false;
	}

	state.cacheDirectoryUtf8 = normalizedDirectory;
	state.itemsByUid.clear();
	state.currentCacheSizeBytes = 0;
	state.indexBuilt = false;
	return RebuildIndexNoLock(state);
}

std::string VectorCache::GetCacheDirectoryUtf8()
{
	CacheState& state = GetState();
	std::lock_guard<std::mutex> lock(state.mutex);

	if (!EnsureCacheDirectoryNoLock(state))
	{
		return std::string();
	}
	return state.cacheDirectoryUtf8;
}

bool VectorCache::SetMaxCacheSizeBytes(size_t maxCacheSizeBytes)
{
	CacheState& state = GetState();
	std::lock_guard<std::mutex> lock(state.mutex);

	state.maxCacheSizeBytes = maxCacheSizeBytes;
	if (!EnsureIndexNoLock(state))
	{
		return false;
	}

	PruneToCapacityNoLock(state);
	return state.currentCacheSizeBytes <= state.maxCacheSizeBytes;
}

size_t VectorCache::GetMaxCacheSizeBytes()
{
	CacheState& state = GetState();
	std::lock_guard<std::mutex> lock(state.mutex);
	return state.maxCacheSizeBytes;
}

bool VectorCache::SetMaxCacheSizeMB(size_t maxCacheSizeMB)
{
	if (maxCacheSizeMB > std::numeric_limits<size_t>::max() / (1024ULL * 1024ULL))
	{
		return false;
	}
	return SetMaxCacheSizeBytes(maxCacheSizeMB * 1024ULL * 1024ULL);
}

double VectorCache::GetMaxCacheSizeMB()
{
	return static_cast<double>(GetMaxCacheSizeBytes()) / (1024.0 * 1024.0);
}

size_t VectorCache::GetCurrentCacheSizeBytes()
{
	CacheState& state = GetState();
	std::lock_guard<std::mutex> lock(state.mutex);

	if (!EnsureIndexNoLock(state))
	{
		return 0;
	}
	return state.currentCacheSizeBytes;
}

std::string VectorCache::BuildCacheUid(const VectorCacheKey& cacheKey)
{
	if (!cacheKey.IsValid())
	{
		return std::string();
	}
	return BuildUidFromKeyText(cacheKey.SerializeToKeyText());
}

std::string VectorCache::GetCacheFilePathUtf8(const VectorCacheKey& cacheKey)
{
	CacheState& state = GetState();
	std::lock_guard<std::mutex> lock(state.mutex);

	if (!EnsureIndexNoLock(state))
	{
		return std::string();
	}

	const std::string uid = BuildCacheUid(cacheKey);
	if (uid.empty())
	{
		return std::string();
	}

	const auto iter = state.itemsByUid.find(uid);
	if (iter == state.itemsByUid.end() || !GB_IsFileExists(iter->second.vectorFilePathUtf8))
	{
		return std::string();
	}
	return iter->second.vectorFilePathUtf8;
}

bool VectorCache::ContainsValidData(const VectorCacheKey& cacheKey)
{
	std::string data;
	return TryReadData(cacheKey, data);
}

bool VectorCache::TryReadData(const VectorCacheKey& cacheKey, std::string& outData)
{
	outData.clear();

	const std::string uid = BuildCacheUid(cacheKey);
	if (uid.empty())
	{
		return false;
	}

	CacheState& state = GetState();
	CacheItem itemSnapshot;
	ActiveCacheOperationGuard activeGuard;
	{
		std::lock_guard<std::mutex> lock(state.mutex);

		if (!EnsureIndexNoLock(state))
		{
			return false;
		}

		auto iter = state.itemsByUid.find(uid);
		if (iter == state.itemsByUid.end())
		{
			return false;
		}

		itemSnapshot = iter->second;
		BeginActiveOperationNoLock(state, uid);
		activeGuard = ActiveCacheOperationGuard(state, uid);
	}

	std::string data;
	if (!ValidateVectorFile(state, uid, itemSnapshot, &data))
	{
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(state.mutex);
		auto iter = state.itemsByUid.find(uid);
		if (iter != state.itemsByUid.end())
		{
			iter->second.lastAccessMs = itemSnapshot.lastAccessMs;
			iter->second.metadataFileSizeBytes = itemSnapshot.metadataFileSizeBytes;
		}
	}

	outData = std::move(data);
	return true;
}

bool VectorCache::TryReadData(const VectorCacheKey& cacheKey, GB_ByteBuffer& outData)
{
	outData.clear();
	std::string data;
	if (!TryReadData(cacheKey, data))
	{
		return false;
	}
	outData = GB_StringToByteBuffer(data);
	return true;
}

bool VectorCache::PutData(const VectorCacheKey& cacheKey, const std::string& data, const std::string& preferredFileExtUtf8)
{
	if (!cacheKey.IsValid() || data.empty())
	{
		return false;
	}

	if (!IsValidArcGISRestVectorJson(data))
	{
		return false;
	}

	CacheState& state = GetState();
	std::lock_guard<std::mutex> lock(state.mutex);
	return PutDataNoLock(state, cacheKey, data, preferredFileExtUtf8);
}

bool VectorCache::PutData(const VectorCacheKey& cacheKey, const GB_ByteBuffer& data, const std::string& preferredFileExtUtf8)
{
	const std::string dataText(data.empty() ? std::string() : std::string(reinterpret_cast<const char*>(&data[0]), data.size()));
	return PutData(cacheKey, dataText, preferredFileExtUtf8);
}

bool VectorCache::Remove(const VectorCacheKey& cacheKey)
{
	CacheState& state = GetState();
	std::lock_guard<std::mutex> lock(state.mutex);

	if (!EnsureIndexNoLock(state))
	{
		return false;
	}

	const std::string uid = BuildCacheUid(cacheKey);
	if (uid.empty())
	{
		return false;
	}

	const bool existed = state.itemsByUid.find(uid) != state.itemsByUid.end();
	RemoveItemFromIndexNoLock(state, uid, true);
	return existed;
}

bool VectorCache::Clear()
{
	CacheState& state = GetState();
	std::lock_guard<std::mutex> lock(state.mutex);

	if (!EnsureIndexNoLock(state))
	{
		return false;
	}

	std::vector<std::string> uids;
	uids.reserve(state.itemsByUid.size());
	for (const auto& itemPair : state.itemsByUid)
	{
		uids.push_back(itemPair.first);
	}

	for (const std::string& uid : uids)
	{
		RemoveItemFromIndexNoLock(state, uid, true);
	}

	return true;
}

bool VectorCache::RebuildIndex()
{
	CacheState& state = GetState();
	std::lock_guard<std::mutex> lock(state.mutex);
	return RebuildIndexNoLock(state);
}

size_t VectorCache::PruneToCapacity()
{
	CacheState& state = GetState();
	std::lock_guard<std::mutex> lock(state.mutex);

	if (!EnsureIndexNoLock(state))
	{
		return 0;
	}

	return PruneToCapacityNoLock(state);
}
