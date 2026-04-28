#include "QCrsManagerWidget.h"

#include "GeoCrs.h"
#include "GeoCrsManager.h"
#include "QMainCanvas.h"
#include "GeoBase/GB_Utf8String.h"
#include "GeoBase/GB_FileSystem.h"
#include "GeoBase/GB_IO.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QCloseEvent>
#include <QComboBox>
#include <QCursor>
#include <QFont>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndexList>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPoint>
#include <QPixmap>
#include <QPushButton>
#include <QRectF>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizeF>
#include <QSizePolicy>
#include <QSplitter>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	constexpr int ColumnName = 0;
	constexpr int ColumnEllipsoid = 1;
	constexpr int ColumnSource = 2;
	constexpr int ColumnCode = 3;
	constexpr int ColumnType = 4;
	constexpr int ColumnCount = 5;

	constexpr int CrsUniqueIdRole = Qt::UserRole + 1;
	constexpr int CrsLogicalSortTextRole = Qt::UserRole + 2;

	constexpr const char* CrsCacheMagic = "MWCRSCACHE";
	constexpr std::uint32_t CrsCacheVersion = 1;
	constexpr std::uint64_t MaxCachedCrsRecordCount = 1000000ULL;
	constexpr std::uint32_t MaxCachedStringLength = 64U * 1024U * 1024U;

	constexpr double PreviewMinUserScale = 1.0;
	constexpr double PreviewMaxUserScale = 64.0;

	const QString BaseWindowTitle = QStringLiteral("坐标系管理");
	const QString LoadingWindowTitle = QStringLiteral("坐标系管理（正在加载全部坐标系......）");

	QString ToQString(const std::string& textUtf8)
	{
		return QString::fromUtf8(textUtf8.c_str());
	}

	std::string ToUtf8(const QString& text)
	{
		return std::string(text.toUtf8().constData());
	}

	bool IsSupportedMapCrsType(GeoCrsDatabaseType type)
	{
		return type == GeoCrsDatabaseType::Geographic || type == GeoCrsDatabaseType::Projected;
	}

	QCrsManagerWidget::CrsCategory ToCategory(GeoCrsDatabaseType type)
	{
		if (type == GeoCrsDatabaseType::Projected)
		{
			return QCrsManagerWidget::CrsCategory::Projected;
		}

		return QCrsManagerWidget::CrsCategory::Geographic;
	}

	QString ToCategoryText(QCrsManagerWidget::CrsCategory category, bool isCustom)
	{
		if (isCustom)
		{
			return QStringLiteral("自定义");
		}

		switch (category)
		{
		case QCrsManagerWidget::CrsCategory::Geographic:
			return QStringLiteral("地理坐标系");
		case QCrsManagerWidget::CrsCategory::Projected:
			return QStringLiteral("投影坐标系");
		case QCrsManagerWidget::CrsCategory::Custom:
			return QStringLiteral("自定义坐标系");
		case QCrsManagerWidget::CrsCategory::All:
		default:
			return QStringLiteral("坐标系");
		}
	}

	std::string ToCategoryTextUtf8(QCrsManagerWidget::CrsCategory category, bool isCustom)
	{
		return ToUtf8(ToCategoryText(category, isCustom));
	}

	std::string BuildBboxText(const GeoCrsDatabaseRecord& record)
	{
		if (!record.hasLonLatBbox)
		{
			return "";
		}

		std::ostringstream stream;
		stream.setf(std::ios::fixed, std::ios::floatfield);
		stream << std::setprecision(8)
			<< "[" << record.westLongitudeDeg << ", "
			<< record.southLatitudeDeg << ", "
			<< record.eastLongitudeDeg << ", "
			<< record.northLatitudeDeg << "]";
		return stream.str();
	}

	std::string ExtractReferenceEllipsoidNameUtf8(const std::string& definitionUtf8)
	{
		if (definitionUtf8.empty())
		{
			return "";
		}

		std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromDefinitionCached(definitionUtf8, false, false);
		if (!crs || !crs->IsValid())
		{
			return "";
		}

		return crs->GetReferenceEllipsoidNameUtf8();
	}

	std::uint64_t CalculateFnv1a64(const std::string& text)
	{
		std::uint64_t hashValue = 14695981039346656037ULL;
		for (const unsigned char ch : text)
		{
			hashValue ^= static_cast<std::uint64_t>(ch);
			hashValue *= 1099511628211ULL;
		}
		return hashValue;
	}

	std::string ToHexString(std::uint64_t value)
	{
		std::ostringstream stream;
		stream << std::hex << std::nouppercase << value;
		return stream.str();
	}

	std::string BuildCustomUniqueId(const QCrsManagerWidget::CustomCrsDefinition& definition)
	{
		if (!GB_Utf8Trim(definition.idUtf8).empty())
		{
			return "CUSTOM:" + GB_Utf8Trim(definition.idUtf8);
		}

		const std::string key = definition.nameUtf8 + "\n" + definition.sourceUtf8 + "\n" + definition.codeUtf8 + "\n" + definition.wktUtf8;
		return "CUSTOM:WKT_HASH:" + ToHexString(CalculateFnv1a64(key));
	}

	QStringList BuildSearchTerms(const QString& searchText)
	{
		const QString simplifiedText = searchText.simplified();
		if (simplifiedText.isEmpty())
		{
			return QStringList();
		}

		return simplifiedText.split(QLatin1Char(' '), QString::SkipEmptyParts);
	}

	QString EscapeHtml(const QString& text)
	{
		return text.toHtmlEscaped();
	}

	QString EscapeHtmlUtf8(const std::string& textUtf8)
	{
		return EscapeHtml(ToQString(textUtf8));
	}

	QString BuildDetailRowHtml(const QString& label, const QString& value)
	{
		return QStringLiteral("<div><b>%1：</b>%2</div>").arg(EscapeHtml(label), EscapeHtml(value));
	}

	QString BuildDetailRowHtmlUtf8(const QString& label, const std::string& valueUtf8)
	{
		return BuildDetailRowHtml(label, ToQString(valueUtf8));
	}

	QSize& GetProcessLastDialogSize()
	{
		static QSize lastSize;
		return lastSize;
	}

	std::string GetCrsCacheFilePathUtf8()
	{
		const std::string tempDirectory = GB_GetTempDirectory();
		if (tempDirectory.empty())
		{
			return "";
		}

		const std::string cacheDirectory = GB_JoinPath(tempDirectory, "MapWeaver");
		return GB_JoinPath(cacheDirectory, "CrsCache.bin");
	}

	void AppendUInt8(GB_ByteBuffer& buffer, std::uint8_t value)
	{
		buffer.push_back(value);
	}

	bool ReadUInt8(const GB_ByteBuffer& buffer, size_t& offset, std::uint8_t& value)
	{
		if (offset >= buffer.size())
		{
			return false;
		}

		value = static_cast<std::uint8_t>(buffer[offset]);
		offset++;
		return true;
	}

	void AppendAsciiBytes(GB_ByteBuffer& buffer, const char* text)
	{
		if (text == nullptr)
		{
			return;
		}

		while (*text != '\0')
		{
			buffer.push_back(static_cast<unsigned char>(*text));
			text++;
		}
	}

	bool ReadAndCheckAsciiBytes(const GB_ByteBuffer& buffer, size_t& offset, const char* expectedText)
	{
		if (expectedText == nullptr)
		{
			return false;
		}

		while (*expectedText != '\0')
		{
			if (offset >= buffer.size() || static_cast<unsigned char>(*expectedText) != static_cast<unsigned char>(buffer[offset]))
			{
				return false;
			}

			offset++;
			expectedText++;
		}

		return true;
	}

	bool AppendUtf8String(GB_ByteBuffer& buffer, const std::string& textUtf8)
	{
		if (textUtf8.size() > static_cast<size_t>(std::numeric_limits<std::uint32_t>::max()))
		{
			return false;
		}

		GB_ByteBufferIO::AppendUInt32LE(buffer, static_cast<std::uint32_t>(textUtf8.size()));
		buffer.insert(buffer.end(), textUtf8.begin(), textUtf8.end());
		return true;
	}

	bool ReadUtf8String(const GB_ByteBuffer& buffer, size_t& offset, std::string& textUtf8)
	{
		std::uint32_t stringLength = 0;
		if (!GB_ByteBufferIO::ReadUInt32LE(buffer, offset, stringLength))
		{
			return false;
		}

		if (stringLength > MaxCachedStringLength)
		{
			return false;
		}

		if (static_cast<size_t>(stringLength) > buffer.size() - offset)
		{
			return false;
		}

		if (stringLength == 0)
		{
			textUtf8.clear();
		}
		else
		{
			textUtf8.assign(reinterpret_cast<const char*>(buffer.data() + offset), static_cast<size_t>(stringLength));
		}

		offset += static_cast<size_t>(stringLength);
		return true;
	}

	bool AppendCrsRecord(GB_ByteBuffer& buffer, const QCrsManagerWidget::CrsRecord& record)
	{
		return AppendUtf8String(buffer, record.uniqueIdUtf8)
			&& AppendUtf8String(buffer, record.nameUtf8)
			&& AppendUtf8String(buffer, record.ellipsoidUtf8)
			&& AppendUtf8String(buffer, record.sourceUtf8)
			&& AppendUtf8String(buffer, record.codeUtf8)
			&& AppendUtf8String(buffer, record.definitionUtf8)
			&& AppendUtf8String(buffer, record.wktUtf8)
			&& AppendUtf8String(buffer, record.areaNameUtf8)
			&& AppendUtf8String(buffer, record.projectionMethodUtf8)
			&& AppendUtf8String(buffer, record.bboxTextUtf8);
	}

	bool ReadCrsRecord(const GB_ByteBuffer& buffer, size_t& offset, QCrsManagerWidget::CrsRecord& record)
	{
		std::uint32_t categoryValue = 0;
		std::uint8_t isCustomValue = 0;
		std::uint8_t isDeprecatedValue = 0;

		if (!ReadUtf8String(buffer, offset, record.uniqueIdUtf8)
			|| !ReadUtf8String(buffer, offset, record.nameUtf8)
			|| !ReadUtf8String(buffer, offset, record.ellipsoidUtf8)
			|| !ReadUtf8String(buffer, offset, record.sourceUtf8)
			|| !ReadUtf8String(buffer, offset, record.codeUtf8)
			|| !ReadUtf8String(buffer, offset, record.definitionUtf8)
			|| !ReadUtf8String(buffer, offset, record.wktUtf8)
			|| !ReadUtf8String(buffer, offset, record.areaNameUtf8)
			|| !ReadUtf8String(buffer, offset, record.projectionMethodUtf8)
			|| !ReadUtf8String(buffer, offset, record.bboxTextUtf8)
			|| !GB_ByteBufferIO::ReadUInt32LE(buffer, offset, categoryValue)
			|| !ReadUInt8(buffer, offset, isCustomValue)
			|| !ReadUInt8(buffer, offset, isDeprecatedValue))
		{
			return false;
		}

		if (categoryValue > static_cast<std::uint32_t>(QCrsManagerWidget::CrsCategory::Custom))
		{
			return false;
		}

		record.category = static_cast<QCrsManagerWidget::CrsCategory>(categoryValue);
		record.isCustom = isCustomValue != 0;
		record.isDeprecated = isDeprecatedValue != 0;
		return !record.uniqueIdUtf8.empty() && !record.nameUtf8.empty();
	}

	bool BuildSystemCrsRecordsCacheData(const std::vector<QCrsManagerWidget::CrsRecord>& records, GB_ByteBuffer& outData)
	{
		outData.clear();

		AppendAsciiBytes(outData, CrsCacheMagic);
		GB_ByteBufferIO::AppendUInt32LE(outData, CrsCacheVersion);
		GB_ByteBufferIO::AppendUInt64LE(outData, static_cast<std::uint64_t>(records.size()));

		for (const QCrsManagerWidget::CrsRecord& record : records)
		{
			if (!AppendCrsRecord(outData, record))
			{
				outData.clear();
				return false;
			}

			GB_ByteBufferIO::AppendUInt32LE(outData, static_cast<std::uint32_t>(record.category));
			AppendUInt8(outData, record.isCustom ? 1 : 0);
			AppendUInt8(outData, record.isDeprecated ? 1 : 0);
		}

		return true;
	}

	bool TryReadSystemCrsRecordsCache(std::vector<QCrsManagerWidget::CrsRecord>& outRecords)
	{
		outRecords.clear();

		const std::string cacheFilePathUtf8 = GetCrsCacheFilePathUtf8();
		if (cacheFilePathUtf8.empty() || !GB_IsFileExists(cacheFilePathUtf8))
		{
			return false;
		}

		const GB_ByteBuffer cacheData = GB_ReadBinaryFromFile(cacheFilePathUtf8);
		if (cacheData.empty())
		{
			return false;
		}

		size_t offset = 0;
		std::uint32_t version = 0;
		std::uint64_t recordCount = 0;
		if (!ReadAndCheckAsciiBytes(cacheData, offset, CrsCacheMagic)
			|| !GB_ByteBufferIO::ReadUInt32LE(cacheData, offset, version)
			|| version != CrsCacheVersion
			|| !GB_ByteBufferIO::ReadUInt64LE(cacheData, offset, recordCount)
			|| recordCount > MaxCachedCrsRecordCount)
		{
			return false;
		}

		std::vector<QCrsManagerWidget::CrsRecord> records;
		records.reserve(static_cast<size_t>(recordCount));
		for (std::uint64_t recordIndex = 0; recordIndex < recordCount; recordIndex++)
		{
			QCrsManagerWidget::CrsRecord record;
			if (!ReadCrsRecord(cacheData, offset, record))
			{
				return false;
			}

			records.push_back(std::move(record));
		}

		if (offset != cacheData.size() || records.empty())
		{
			return false;
		}

		outRecords = std::move(records);
		return true;
	}

	bool TryWriteSystemCrsRecordsCache(const std::vector<QCrsManagerWidget::CrsRecord>& records)
	{
		if (records.empty())
		{
			return false;
		}

		const std::string cacheFilePathUtf8 = GetCrsCacheFilePathUtf8();
		if (cacheFilePathUtf8.empty())
		{
			return false;
		}

		GB_ByteBuffer cacheData;
		if (!BuildSystemCrsRecordsCacheData(records, cacheData))
		{
			return false;
		}

		return GB_WriteBinaryToFile(cacheData, cacheFilePathUtf8);
	}

	class CrsTableWidgetItem : public QTableWidgetItem
	{
	public:
		explicit CrsTableWidgetItem(const QString& text) : QTableWidgetItem(text)
		{
		}

		virtual QTableWidgetItem* clone() const override
		{
			return new CrsTableWidgetItem(*this);
		}

		virtual bool operator<(const QTableWidgetItem& other) const override
		{
			if (column() == ColumnCode)
			{
				const std::string leftText = ToUtf8(data(CrsLogicalSortTextRole).toString());
				const std::string rightText = ToUtf8(other.data(CrsLogicalSortTextRole).toString());
				const int compareResult = GB_Utf8CompareLogical(leftText, rightText);
				if (compareResult != 0)
				{
					return compareResult < 0;
				}
			}

			const int compareResult = QString::localeAwareCompare(text(), other.text());
			if (compareResult != 0)
			{
				return compareResult < 0;
			}

			return QTableWidgetItem::operator<(other);
		}
	};

	std::unique_ptr<QTableWidgetItem> MakeTableItem(const QString& text, const QCrsManagerWidget::CrsRecord& record)
	{
		std::unique_ptr<QTableWidgetItem> item(new CrsTableWidgetItem(text));
		item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
		item->setData(CrsUniqueIdRole, ToQString(record.uniqueIdUtf8));
		item->setData(CrsLogicalSortTextRole, text);
		if (record.isCustom)
		{
			item->setForeground(QBrush(Qt::red));
		}
		return item;
	}

	void SetTableItem(QTableWidget* tableWidget, int row, int column, const QString& text, const QCrsManagerWidget::CrsRecord& record)
	{
		std::unique_ptr<QTableWidgetItem> item = MakeTableItem(text, record);
		tableWidget->setItem(row, column, item.release());
	}

	QCrsManagerWidget::CrsRecord BuildCrsRecordFromDatabaseRecord(const GeoCrsDatabaseRecord& databaseRecord, bool& outValid)
	{
		outValid = false;

		if (!IsSupportedMapCrsType(databaseRecord.type))
		{
			return QCrsManagerWidget::CrsRecord();
		}

		QCrsManagerWidget::CrsRecord record;
		record.sourceUtf8 = databaseRecord.sourceUtf8;
		record.codeUtf8 = databaseRecord.codeUtf8;
		record.nameUtf8 = databaseRecord.nameUtf8;
		record.category = ToCategory(databaseRecord.type);
		record.isCustom = false;
		record.isDeprecated = databaseRecord.isDeprecated;
		record.areaNameUtf8 = databaseRecord.areaNameUtf8;
		record.projectionMethodUtf8 = databaseRecord.projectionMethodUtf8;
		record.bboxTextUtf8 = BuildBboxText(databaseRecord);

		if (record.sourceUtf8.empty() || record.codeUtf8.empty() || record.nameUtf8.empty())
		{
			return QCrsManagerWidget::CrsRecord();
		}

		record.uniqueIdUtf8 = databaseRecord.GetAuthorityCodeUtf8();
		record.definitionUtf8 = record.uniqueIdUtf8;
		record.ellipsoidUtf8 = ExtractReferenceEllipsoidNameUtf8(record.definitionUtf8);

		outValid = true;
		return record;
	}

	typedef std::function<void(const std::vector<QCrsManagerWidget::CrsRecord>&)> CrsRecordsProgressCallback;

	void AppendAuthorityCrsRecords(const std::string& authorityNameUtf8, std::vector<QCrsManagerWidget::CrsRecord>& records, const CrsRecordsProgressCallback& progressCallback)
	{
		const std::vector<GeoCrsDatabaseRecord> databaseRecords = GeoCrsManager::ListDatabaseCrsRecords(authorityNameUtf8);
		if (databaseRecords.empty())
		{
			return;
		}

		records.reserve(records.size() + databaseRecords.size());

		std::vector<QCrsManagerWidget::CrsRecord> localRecords(databaseRecords.size());
		std::vector<unsigned char> validFlags(databaseRecords.size(), 0);

#if defined(_OPENMP)
#pragma omp parallel for schedule(dynamic, 64)
#endif
		for (long long recordIndex = 0; recordIndex < static_cast<long long>(databaseRecords.size()); recordIndex++)
		{
			bool isValid = false;
			QCrsManagerWidget::CrsRecord record = BuildCrsRecordFromDatabaseRecord(databaseRecords[static_cast<size_t>(recordIndex)], isValid);
			if (isValid)
			{
				localRecords[static_cast<size_t>(recordIndex)] = std::move(record);
				validFlags[static_cast<size_t>(recordIndex)] = 1;
			}
		}

		const size_t oldRecordCount = records.size();
		for (size_t recordIndex = 0; recordIndex < localRecords.size(); recordIndex++)
		{
			if (validFlags[recordIndex] != 0)
			{
				records.push_back(std::move(localRecords[recordIndex]));
			}
		}

		if (progressCallback && records.size() != oldRecordCount)
		{
			progressCallback(records);
		}
	}

	std::vector<QCrsManagerWidget::CrsRecord> BuildSystemCrsRecordsInternal(const CrsRecordsProgressCallback& progressCallback = CrsRecordsProgressCallback())
	{
		std::vector<QCrsManagerWidget::CrsRecord> records;

		GeoCrsManager::GetProjDbDirectoryUtf8();
		AppendAuthorityCrsRecords("EPSG", records, progressCallback);
		AppendAuthorityCrsRecords("ESRI", records, progressCallback);

		std::sort(records.begin(), records.end(), [](const QCrsManagerWidget::CrsRecord& left, const QCrsManagerWidget::CrsRecord& right) -> bool
			{
				const int sourceCompare = GB_Utf8CompareLogical(left.sourceUtf8, right.sourceUtf8);
				if (sourceCompare != 0)
				{
					return sourceCompare < 0;
				}

				return GB_Utf8CompareLogical(left.codeUtf8, right.codeUtf8) < 0;
			});

		if (progressCallback)
		{
			progressCallback(records);
		}

		return records;
	}

	struct SystemCrsRecordsInitializationState
	{
		std::mutex mutex;
		std::condition_variable condition;
		bool isStarted = false;
		bool isFinished = false;
		bool isSucceeded = false;
		std::uint64_t dataRevision = 0;
		std::vector<QCrsManagerWidget::CrsRecord> records;
		std::string errorMessageUtf8 = "";
	};

	SystemCrsRecordsInitializationState& GetSystemCrsRecordsInitializationState()
	{
		static SystemCrsRecordsInitializationState* state = new SystemCrsRecordsInitializationState();
		return *state;
	}

	void PublishSystemCrsRecordsSnapshot(const std::vector<QCrsManagerWidget::CrsRecord>& records, bool isFinished, bool isSucceeded, const std::string& errorMessageUtf8)
	{
		SystemCrsRecordsInitializationState& state = GetSystemCrsRecordsInitializationState();
		{
			std::lock_guard<std::mutex> lock(state.mutex);
			state.records = records;
			state.errorMessageUtf8 = errorMessageUtf8;
			state.isSucceeded = isSucceeded;
			state.isFinished = isFinished;
			state.dataRevision++;
		}
		state.condition.notify_all();
	}

	bool CopySystemCrsRecordsSnapshotIfChanged(std::uint64_t knownRevision,
		std::vector<QCrsManagerWidget::CrsRecord>& outRecords,
		bool& outIsFinished,
		bool& outIsSucceeded,
		std::string& outErrorMessageUtf8,
		std::uint64_t& outRevision)
	{
		SystemCrsRecordsInitializationState& state = GetSystemCrsRecordsInitializationState();
		std::lock_guard<std::mutex> lock(state.mutex);

		outIsFinished = state.isFinished;
		outIsSucceeded = state.isSucceeded;
		outErrorMessageUtf8 = state.errorMessageUtf8;
		outRevision = state.dataRevision;

		if (state.dataRevision == knownRevision)
		{
			return false;
		}

		outRecords = state.records;
		return true;
	}

	void StartWriteSystemCrsRecordsCacheAsync(const std::vector<QCrsManagerWidget::CrsRecord>& records)
	{
		if (records.empty())
		{
			return;
		}

		try
		{
			std::vector<QCrsManagerWidget::CrsRecord> recordsCopy = records;
			std::thread([recordsCopy = std::move(recordsCopy)]() mutable
				{
					TryWriteSystemCrsRecordsCache(recordsCopy);
				}).detach();
		}
		catch (...)
		{
			// 缓存只是性能优化，写入失败不能影响主流程。
		}
	}

	void RunSystemCrsRecordsInitialization()
	{
		std::vector<QCrsManagerWidget::CrsRecord> records;
		std::string errorMessageUtf8;
		bool isSucceeded = false;
		bool shouldWriteCache = false;

		try
		{
			if (TryReadSystemCrsRecordsCache(records))
			{
				isSucceeded = true;
				PublishSystemCrsRecordsSnapshot(records, true, true, "");
				return;
			}

			const CrsRecordsProgressCallback progressCallback = [](const std::vector<QCrsManagerWidget::CrsRecord>& partialRecords)
				{
					PublishSystemCrsRecordsSnapshot(partialRecords, false, false, "");
				};

			records = BuildSystemCrsRecordsInternal(progressCallback);
			isSucceeded = true;
			shouldWriteCache = !records.empty();
		}
		catch (const std::exception& exception)
		{
			errorMessageUtf8 = exception.what();
		}
		catch (...)
		{
			errorMessageUtf8 = "Unknown exception while initializing CRS records.";
		}

		PublishSystemCrsRecordsSnapshot(records, true, isSucceeded, errorMessageUtf8);

		if (isSucceeded && shouldWriteCache)
		{
			StartWriteSystemCrsRecordsCacheAsync(records);
		}
	}

	class OverrideCursorGuard
	{
	public:
		explicit OverrideCursorGuard(Qt::CursorShape cursorShape)
		{
			QApplication::setOverrideCursor(QCursor(cursorShape));
		}

		~OverrideCursorGuard()
		{
			QApplication::restoreOverrideCursor();
		}

		OverrideCursorGuard(const OverrideCursorGuard&) = delete;
		OverrideCursorGuard& operator=(const OverrideCursorGuard&) = delete;
	};


	bool IsSameAuthorityCode(const std::string& left, const std::string& right)
	{
		return GB_Utf8Equals(left, right, false);
	}
}

class QCrsAreaPreviewFrame : public QFrame
{
public:
	explicit QCrsAreaPreviewFrame(QWidget* parent = nullptr) : QFrame(parent)
	{
		setMouseTracking(false);
		setFocusPolicy(Qt::StrongFocus);
		setCursor(Qt::OpenHandCursor);
		globalMapPixmap = QPixmap(QStringLiteral(":/resources/Resources/GlobalMap.jpg"));
	}

	void SetValidAreaSegments(const std::vector<GeoCrs::LonLatAreaSegment>& segments)
	{
		validAreaSegments = segments;
		hasPreview = true;
		statusText = validAreaSegments.empty() ? QStringLiteral("当前坐标系没有可用的经纬度有效范围。") : QString();
		ResetView();
		update();
	}

	void ClearPreview()
	{
		validAreaSegments.clear();
		hasPreview = false;
		statusText.clear();
		ResetView();
		update();
	}

protected:
	virtual void paintEvent(QPaintEvent* event) override
	{
		QFrame::paintEvent(event);

		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

		const QRectF contentRect = GetContentRect();
		if (contentRect.width() <= 1.0 || contentRect.height() <= 1.0)
		{
			return;
		}

		if (!hasPreview)
		{
			DrawCenteredText(painter, contentRect, QStringLiteral("未选择单个坐标系。"));
			return;
		}

		if (globalMapPixmap.isNull())
		{
			DrawCenteredText(painter, contentRect, QStringLiteral("无法加载预览地图资源：:/resources/Resources/GlobalMap.png"));
			return;
		}

		const QRectF imageTargetRect = GetImageTargetRect(contentRect);
		painter.save();
		painter.setClipRect(contentRect);
		painter.drawPixmap(imageTargetRect, globalMapPixmap, QRectF(globalMapPixmap.rect()));
		DrawValidAreaSegments(painter, imageTargetRect);
		painter.restore();

		if (!statusText.isEmpty())
		{
			DrawCornerText(painter, contentRect, statusText);
		}
	}

	virtual void wheelEvent(QWheelEvent* event) override
	{
		if (!CanInteract())
		{
			QFrame::wheelEvent(event);
			return;
		}

		const int wheelDelta = event->angleDelta().y();
		if (wheelDelta == 0)
		{
			event->ignore();
			return;
		}

		const QRectF contentRect = GetContentRect();
		if (contentRect.width() <= 1.0 || contentRect.height() <= 1.0)
		{
			event->ignore();
			return;
		}

		const QPointF cursorPos = event->pos();
		const QRectF oldImageTargetRect = GetImageTargetRect(contentRect);
		if (oldImageTargetRect.width() <= 0.0 || oldImageTargetRect.height() <= 0.0)
		{
			event->ignore();
			return;
		}

		const QPointF imagePoint((cursorPos.x() - oldImageTargetRect.left()) / oldImageTargetRect.width() * static_cast<double>(globalMapPixmap.width()),
			(cursorPos.y() - oldImageTargetRect.top()) / oldImageTargetRect.height() * static_cast<double>(globalMapPixmap.height()));

		const double factor = std::pow(1.0015, static_cast<double>(wheelDelta));
		userScale = ClampDouble(userScale * factor, PreviewMinUserScale, PreviewMaxUserScale);

		const double baseScale = GetBaseScale(contentRect);
		const double displayScale = baseScale * userScale;
		const QSizeF imageSize(static_cast<double>(globalMapPixmap.width()) * displayScale,
			static_cast<double>(globalMapPixmap.height()) * displayScale);
		const QPointF baseTopLeft(contentRect.center().x() - imageSize.width() * 0.5,
			contentRect.center().y() - imageSize.height() * 0.5);
		const QPointF desiredTopLeft(cursorPos.x() - imagePoint.x() * displayScale,
			cursorPos.y() - imagePoint.y() * displayScale);

		userPanPixels = desiredTopLeft - baseTopLeft;
		ClampPanPixels();
		update();
		event->accept();
	}

	virtual void mousePressEvent(QMouseEvent* event) override
	{
		if (event->button() == Qt::LeftButton && CanInteract())
		{
			isPanning = true;
			lastPanPos = event->pos();
			setCursor(Qt::ClosedHandCursor);
			event->accept();
			return;
		}

		QFrame::mousePressEvent(event);
	}

	virtual void mouseMoveEvent(QMouseEvent* event) override
	{
		if (isPanning)
		{
			const QPoint currentPos = event->pos();
			const QPoint delta = currentPos - lastPanPos;
			lastPanPos = currentPos;
			userPanPixels += QPointF(delta);
			ClampPanPixels();
			update();
			event->accept();
			return;
		}

		QFrame::mouseMoveEvent(event);
	}

	virtual void mouseReleaseEvent(QMouseEvent* event) override
	{
		if (event->button() == Qt::LeftButton && isPanning)
		{
			isPanning = false;
			setCursor(Qt::OpenHandCursor);
			event->accept();
			return;
		}

		QFrame::mouseReleaseEvent(event);
	}

	virtual void mouseDoubleClickEvent(QMouseEvent* event) override
	{
		if (event->button() == Qt::LeftButton && CanInteract())
		{
			ResetView();
			update();
			event->accept();
			return;
		}

		QFrame::mouseDoubleClickEvent(event);
	}

	virtual void resizeEvent(QResizeEvent* event) override
	{
		QFrame::resizeEvent(event);
		ClampPanPixels();
		update();
	}

private:
	QPixmap globalMapPixmap;
	std::vector<GeoCrs::LonLatAreaSegment> validAreaSegments;
	bool hasPreview = false;
	QString statusText;
	double userScale = 1.0;
	QPointF userPanPixels;
	bool isPanning = false;
	QPoint lastPanPos;

	static double ClampDouble(double value, double minValue, double maxValue)
	{
		if (value < minValue)
		{
			return minValue;
		}
		if (value > maxValue)
		{
			return maxValue;
		}
		return value;
	}

	static bool IsFinite(double value)
	{
		return std::isfinite(value);
	}

	static bool NormalizeSegment(const GeoCrs::LonLatAreaSegment& segment, double& west, double& south, double& east, double& north)
	{
		if (!IsFinite(segment.west) || !IsFinite(segment.south) || !IsFinite(segment.east) || !IsFinite(segment.north))
		{
			return false;
		}

		west = ClampDouble(segment.west, -180.0, 180.0);
		east = ClampDouble(segment.east, -180.0, 180.0);
		south = ClampDouble(segment.south, -90.0, 90.0);
		north = ClampDouble(segment.north, -90.0, 90.0);

		if (east < west || north < south)
		{
			return false;
		}

		return (east - west) > 0.0 && (north - south) > 0.0;
	}

	void ResetView()
	{
		userScale = 1.0;
		userPanPixels = QPointF(0.0, 0.0);
		isPanning = false;
		setCursor(Qt::OpenHandCursor);
	}

	bool CanInteract() const
	{
		return hasPreview && !globalMapPixmap.isNull();
	}

	QRectF GetContentRect() const
	{
		return QRectF(contentsRect()).adjusted(6.0, 6.0, -6.0, -6.0);
	}

	double GetBaseScale(const QRectF& contentRect) const
	{
		if (globalMapPixmap.isNull() || contentRect.width() <= 0.0 || contentRect.height() <= 0.0)
		{
			return 1.0;
		}

		const double scaleX = contentRect.width() / static_cast<double>(globalMapPixmap.width());
		const double scaleY = contentRect.height() / static_cast<double>(globalMapPixmap.height());
		return std::max(0.000001, std::min(scaleX, scaleY));
	}

	QRectF GetImageTargetRect(const QRectF& contentRect) const
	{
		if (globalMapPixmap.isNull())
		{
			return QRectF();
		}

		const double displayScale = GetBaseScale(contentRect) * userScale;
		const QSizeF imageSize(static_cast<double>(globalMapPixmap.width()) * displayScale,
			static_cast<double>(globalMapPixmap.height()) * displayScale);
		const QPointF center = contentRect.center() + userPanPixels;
		return QRectF(center.x() - imageSize.width() * 0.5,
			center.y() - imageSize.height() * 0.5,
			imageSize.width(),
			imageSize.height());
	}

	void ClampPanPixels()
	{
		if (globalMapPixmap.isNull())
		{
			userPanPixels = QPointF(0.0, 0.0);
			return;
		}

		const QRectF contentRect = GetContentRect();
		const double displayScale = GetBaseScale(contentRect) * userScale;
		const QSizeF imageSize(static_cast<double>(globalMapPixmap.width()) * displayScale,
			static_cast<double>(globalMapPixmap.height()) * displayScale);

		const double maxPanX = std::max(0.0, (imageSize.width() - contentRect.width()) * 0.5);
		const double maxPanY = std::max(0.0, (imageSize.height() - contentRect.height()) * 0.5);
		userPanPixels.setX(ClampDouble(userPanPixels.x(), -maxPanX, maxPanX));
		userPanPixels.setY(ClampDouble(userPanPixels.y(), -maxPanY, maxPanY));
	}

	void DrawCenteredText(QPainter& painter, const QRectF& rect, const QString& text) const
	{
		painter.save();
		painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
		painter.drawText(rect, Qt::AlignCenter | Qt::TextWordWrap, text);
		painter.restore();
	}

	void DrawCornerText(QPainter& painter, const QRectF& rect, const QString& text) const
	{
		painter.save();
		QFont textFont = font();
		textFont.setPointSize(std::max(8, textFont.pointSize()));
		painter.setFont(textFont);

		const QRectF textRect = painter.boundingRect(rect.adjusted(8.0, 8.0, -8.0, -8.0), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
		const QRectF backgroundRect = textRect.adjusted(-6.0, -4.0, 6.0, 4.0);
		painter.fillRect(backgroundRect, QColor(255, 255, 255, 210));
		painter.setPen(QColor(80, 80, 80));
		painter.drawText(textRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
		painter.restore();
	}

	void DrawValidAreaSegments(QPainter& painter, const QRectF& imageTargetRect) const
	{
		if (globalMapPixmap.isNull() || imageTargetRect.width() <= 0.0 || imageTargetRect.height() <= 0.0)
		{
			return;
		}

		painter.save();
		painter.setPen(QPen(QColor(255, 0, 0, 160), 1.0));
		painter.setBrush(QBrush(QColor(255, 0, 0, 80)));

		for (const GeoCrs::LonLatAreaSegment& segment : validAreaSegments)
		{
			double west = 0.0;
			double south = 0.0;
			double east = 0.0;
			double north = 0.0;
			if (!NormalizeSegment(segment, west, south, east, north))
			{
				continue;
			}

			const double leftRatio = (west + 180.0) / 360.0;
			const double rightRatio = (east + 180.0) / 360.0;
			const double topRatio = (90.0 - north) / 180.0;
			const double bottomRatio = (90.0 - south) / 180.0;

			const QRectF areaRect(imageTargetRect.left() + leftRatio * imageTargetRect.width(),
				imageTargetRect.top() + topRatio * imageTargetRect.height(),
				(rightRatio - leftRatio) * imageTargetRect.width(),
				(bottomRatio - topRatio) * imageTargetRect.height());
			painter.drawRect(areaRect);
		}

		painter.restore();
	}
};

QCrsManagerWidget::QCrsManagerWidget(QWidget* parent) : QDialog(parent)
{
	InitializeUi();
	InitializeConnections();
	ReloadCoordinateSystems();
}

QCrsManagerWidget::~QCrsManagerWidget()
{
	SaveDialogSize();
}

void QCrsManagerWidget::InitializeSystemCrsRecordsAsync()
{
	SystemCrsRecordsInitializationState& state = GetSystemCrsRecordsInitializationState();

	{
		std::lock_guard<std::mutex> lock(state.mutex);
		if (state.isStarted)
		{
			return;
		}

		state.isStarted = true;
		state.isFinished = false;
		state.isSucceeded = false;
		state.errorMessageUtf8.clear();
	}

	try
	{
		std::thread(RunSystemCrsRecordsInitialization).detach();
	}
	catch (const std::exception& exception)
	{
		{
			std::lock_guard<std::mutex> lock(state.mutex);
			state.isStarted = false;
			state.isFinished = true;
			state.isSucceeded = false;
			state.errorMessageUtf8 = exception.what();
			state.dataRevision++;
		}
		state.condition.notify_all();
	}
	catch (...)
	{
		{
			std::lock_guard<std::mutex> lock(state.mutex);
			state.isStarted = false;
			state.isFinished = true;
			state.isSucceeded = false;
			state.errorMessageUtf8 = "Unknown exception while starting CRS initialization thread.";
			state.dataRevision++;
		}
		state.condition.notify_all();
	}
}

void QCrsManagerWidget::BindMainCanvas(QMainCanvas* canvas)
{
	mainCanvas = canvas;
	hasSelectedInitialCanvasCrs = false;
	SelectCanvasCrsIfAvailable();
	UpdateActionButtons();
}

QMainCanvas* QCrsManagerWidget::GetMainCanvas() const
{
	return mainCanvas;
}

void QCrsManagerWidget::SetCustomCrsDefinitions(const std::vector<CustomCrsDefinition>& customDefinitionsValue)
{
	customDefinitions = customDefinitionsValue;
	RebuildCustomCrsRecords();
	RefreshTable(true);
	emit CustomCrsDefinitionsChanged();
}

std::vector<QCrsManagerWidget::CustomCrsDefinition> QCrsManagerWidget::GetCustomCrsDefinitions() const
{
	return customDefinitions;
}

void QCrsManagerWidget::ReloadCoordinateSystems()
{
	LoadSystemCrsRecords();
	RebuildCustomCrsRecords();
	RefreshTable(false);
	hasSelectedInitialCanvasCrs = false;
	SelectCanvasCrsIfAvailable();
}

void QCrsManagerWidget::showEvent(QShowEvent* event)
{
	QDialog::showEvent(event);
	LoadSystemCrsRecords();

	// 每次窗口从隐藏状态重新显示时，都重新按当前画布坐标系执行一次初始定位。
	// 这样可以避免在对话框尚未显示前已完成选中，但视口滚动位置未生效的问题。
	hasSelectedInitialCanvasCrs = false;
	SelectCanvasCrsIfAvailable();
	ScheduleScrollSelectedCrsToCenterIfAvailable();
}

void QCrsManagerWidget::closeEvent(QCloseEvent* event)
{
	SaveDialogSize();
	QDialog::closeEvent(event);
}

void QCrsManagerWidget::done(int result)
{
	SaveDialogSize();
	QDialog::done(result);
}

void QCrsManagerWidget::InitializeUi()
{
	setObjectName(QStringLiteral("QCrsManagerWidget"));
	setWindowTitle(BaseWindowTitle);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	resize(900, 600);

	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(6, 6, 6, 6);
	mainLayout->setSpacing(6);

	QHBoxLayout* topLayout = new QHBoxLayout();
	topLayout->setContentsMargins(0, 0, 0, 0);
	topLayout->setSpacing(6);

	categoryComboBox = new QComboBox(this);
	categoryComboBox->setObjectName(QStringLiteral("categoryComboBox"));
	categoryComboBox->addItem(QStringLiteral("全部"), static_cast<int>(CrsCategory::All));
	categoryComboBox->addItem(QStringLiteral("地理坐标系"), static_cast<int>(CrsCategory::Geographic));
	categoryComboBox->addItem(QStringLiteral("投影坐标系"), static_cast<int>(CrsCategory::Projected));
	categoryComboBox->addItem(QStringLiteral("自定义坐标系"), static_cast<int>(CrsCategory::Custom));
	categoryComboBox->setCurrentIndex(0);
	categoryComboBox->setMinimumWidth(140);
	topLayout->addWidget(categoryComboBox, 0);

	searchLineEdit = new QLineEdit(this);
	searchLineEdit->setObjectName(QStringLiteral("searchLineEdit"));
	searchLineEdit->setPlaceholderText(QStringLiteral("搜索坐标系名称、椭球、来源或代码"));
	searchLineEdit->setClearButtonEnabled(true);
	topLayout->addWidget(searchLineEdit, 1);

	addCustomCrsButton = new QToolButton(this);
	addCustomCrsButton->setObjectName(QStringLiteral("addCustomCrsButton"));
	addCustomCrsButton->setToolTip(QStringLiteral("添加自定义坐标系"));
	addCustomCrsButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
	addCustomCrsButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
	addCustomCrsButton->setAutoRaise(false);
	topLayout->addWidget(addCustomCrsButton, 0);

	deleteCrsButton = new QToolButton(this);
	deleteCrsButton->setObjectName(QStringLiteral("deleteCrsButton"));
	deleteCrsButton->setToolTip(QStringLiteral("删除选中坐标系"));
	deleteCrsButton->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
	deleteCrsButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
	deleteCrsButton->setAutoRaise(false);
	deleteCrsButton->setEnabled(false);
	topLayout->addWidget(deleteCrsButton, 0);

	mainLayout->addLayout(topLayout);

	QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
	splitter->setObjectName(QStringLiteral("mainSplitter"));
	mainLayout->addWidget(splitter, 1);

	QWidget* leftWidget = new QWidget(splitter);
	leftWidget->setObjectName(QStringLiteral("leftWidget"));
	QVBoxLayout* leftLayout = new QVBoxLayout(leftWidget);
	leftLayout->setContentsMargins(0, 0, 0, 0);
	leftLayout->setSpacing(6);

	crsTableWidget = new QTableWidget(leftWidget);
	crsTableWidget->setObjectName(QStringLiteral("crsTableWidget"));
	crsTableWidget->setColumnCount(ColumnCount);
	crsTableWidget->setHorizontalHeaderLabels(QStringList()
		<< QStringLiteral("坐标系名称")
		<< QStringLiteral("参考椭球")
		<< QStringLiteral("来源")
		<< QStringLiteral("代码")
		<< QStringLiteral("类型"));
	crsTableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
	crsTableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
	crsTableWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
	crsTableWidget->setContextMenuPolicy(Qt::CustomContextMenu);
	crsTableWidget->setAlternatingRowColors(true);
	crsTableWidget->setSortingEnabled(true);

	QPalette tablePalette = crsTableWidget->palette();
	tablePalette.setColor(QPalette::Highlight, QColor(47, 129, 247));
	tablePalette.setColor(QPalette::HighlightedText, Qt::white);
	tablePalette.setColor(QPalette::Inactive, QPalette::Highlight, QColor(47, 129, 247));
	tablePalette.setColor(QPalette::Inactive, QPalette::HighlightedText, Qt::white);
	crsTableWidget->setPalette(tablePalette);
	crsTableWidget->setStyleSheet(QStringLiteral(
		"QTableWidget#crsTableWidget::item:selected {"
		"  background: #2f81f7;"
		"  color: white;"
		"}"
		"QTableWidget#crsTableWidget::item:selected:!active {"
		"  background: #2f81f7;"
		"  color: white;"
		"}"));

	crsTableWidget->verticalHeader()->setVisible(false);
	crsTableWidget->verticalHeader()->setDefaultSectionSize(24);
	QHeaderView* horizontalHeader = crsTableWidget->horizontalHeader();
	horizontalHeader->setStretchLastSection(false);
	horizontalHeader->setSectionsMovable(false);
	horizontalHeader->setMinimumSectionSize(60);
	horizontalHeader->setDefaultSectionSize(120);
	horizontalHeader->setSectionResizeMode(QHeaderView::Interactive);
	crsTableWidget->setColumnWidth(ColumnName, 260);
	crsTableWidget->setColumnWidth(ColumnEllipsoid, 150);
	crsTableWidget->setColumnWidth(ColumnSource, 90);
	crsTableWidget->setColumnWidth(ColumnCode, 90);
	crsTableWidget->setColumnWidth(ColumnType, 110);
	leftLayout->addWidget(crsTableWidget, 1);

	applyToCanvasButton = new QPushButton(QStringLiteral("应用到画布"), leftWidget);
	applyToCanvasButton->setObjectName(QStringLiteral("applyToCanvasButton"));
	applyToCanvasButton->setToolTip(QStringLiteral("将当前选中的一个坐标系应用到主画布"));
	applyToCanvasButton->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
	applyToCanvasButton->setMinimumHeight(36);
	applyToCanvasButton->setDefault(true);
	applyToCanvasButton->setAutoDefault(true);
	QFont applyButtonFont = applyToCanvasButton->font();
	applyButtonFont.setBold(true);
	applyToCanvasButton->setFont(applyButtonFont);
	applyToCanvasButton->setStyleSheet(QStringLiteral(
		"QPushButton#applyToCanvasButton {"
		"  font-weight: 600;"
		"  padding: 6px 14px;"
		"  border-radius: 4px;"
		"  border: 1px solid #1f6feb;"
		"  background: #2f81f7;"
		"  color: white;"
		"}"
		"QPushButton#applyToCanvasButton:hover {"
		"  background: #1f6feb;"
		"}"
		"QPushButton#applyToCanvasButton:disabled {"
		"  border: 1px solid #a0a0a0;"
		"  background: #d0d0d0;"
		"  color: #707070;"
		"}"));
	applyToCanvasButton->setEnabled(false);
	leftLayout->addWidget(applyToCanvasButton, 0);

	splitter->addWidget(leftWidget);

	QWidget* rightWidget = new QWidget(splitter);
	rightWidget->setObjectName(QStringLiteral("rightWidget"));
	QVBoxLayout* rightLayout = new QVBoxLayout(rightWidget);
	rightLayout->setContentsMargins(0, 0, 0, 0);
	rightLayout->setSpacing(6);

	crsDetailTextEdit = new QTextEdit(rightWidget);
	crsDetailTextEdit->setObjectName(QStringLiteral("crsDetailTextEdit"));
	crsDetailTextEdit->setReadOnly(true);
	crsDetailTextEdit->setLineWrapMode(QTextEdit::NoWrap);
	crsDetailTextEdit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
	rightLayout->addWidget(crsDetailTextEdit, 2);

	previewFrame = new QCrsAreaPreviewFrame(rightWidget);
	previewFrame->setObjectName(QStringLiteral("previewFrame"));
	previewFrame->setFrameShape(QFrame::StyledPanel);
	previewFrame->setFrameShadow(QFrame::Sunken);
	previewFrame->setMinimumHeight(120);
	previewFrame->setToolTip(QStringLiteral("鼠标滚轮缩放，按住鼠标左键拖动平移，左键双击恢复初始视图"));
	previewFrame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	rightLayout->addWidget(previewFrame, 1);

	splitter->addWidget(rightWidget);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);
	splitter->setSizes(QList<int>() << 540 << 340);

	systemCrsInitializationPollTimer = new QTimer(this);
	systemCrsInitializationPollTimer->setInterval(250);

	RestoreDialogSize();
	UpdateInitializationUiState();
}

void QCrsManagerWidget::InitializeConnections()
{
	connect(categoryComboBox, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [this](int)
		{
			RefreshTable(true);
		});

	connect(searchLineEdit, &QLineEdit::textChanged, this, [this](const QString&)
		{
			RefreshTable(true);
		});

	connect(addCustomCrsButton, &QToolButton::clicked, this, [this]()
		{
			OnAddCustomCrsClicked();
		});

	connect(deleteCrsButton, &QToolButton::clicked, this, [this]()
		{
			OnDeleteSelectedCrsClicked();
		});

	connect(applyToCanvasButton, &QPushButton::clicked, this, [this]()
		{
			OnApplyToCanvasClicked();
		});

	connect(crsTableWidget->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]()
		{
			if (!isRefreshingTable)
			{
				UpdateDetailsAndButtons();
			}
		});

	connect(crsTableWidget, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos)
		{
			ShowTableContextMenu(pos);
		});

	connect(systemCrsInitializationPollTimer, &QTimer::timeout, this, [this]()
		{
			PollSystemCrsRecordsInitialization();
		});
}

bool QCrsManagerWidget::LoadSystemCrsRecords()
{
	InitializeSystemCrsRecordsAsync();

	std::vector<CrsRecord> recordsSnapshot;
	bool isFinished = false;
	bool isSucceeded = false;
	std::string errorMessageUtf8;
	std::uint64_t revision = loadedSystemCrsRecordsRevision;

	const bool hasNewSnapshot = CopySystemCrsRecordsSnapshotIfChanged(loadedSystemCrsRecordsRevision,
		recordsSnapshot,
		isFinished,
		isSucceeded,
		errorMessageUtf8,
		revision);

	isSystemCrsInitializationFinished = isFinished;

	if (hasNewSnapshot)
	{
		systemCrsRecords = std::move(recordsSnapshot);
		loadedSystemCrsRecordsRevision = revision;
	}

	if (!isSystemCrsInitializationFinished)
	{
		systemCrsInitializationPollTimer->start();
	}
	else
	{
		systemCrsInitializationPollTimer->stop();
	}

	UpdateInitializationUiState();
	return hasNewSnapshot;
}

void QCrsManagerWidget::PollSystemCrsRecordsInitialization()
{
	const bool hasNewSnapshot = LoadSystemCrsRecords();
	if (hasNewSnapshot)
	{
		RefreshTable(true);
		SelectCanvasCrsIfAvailable();
	}
}

void QCrsManagerWidget::UpdateInitializationUiState()
{
	setWindowTitle(isSystemCrsInitializationFinished ? BaseWindowTitle : LoadingWindowTitle);
}

void QCrsManagerWidget::RestoreDialogSize()
{
	QSize restoredSize = GetProcessLastDialogSize();
	if (!restoredSize.isValid())
	{
		QSettings settings;
		restoredSize = settings.value(QStringLiteral("QCrsManagerWidget/size")).toSize();
	}

	if (restoredSize.isValid() && restoredSize.width() >= 640 && restoredSize.height() >= 360)
	{
		resize(restoredSize);
	}
}

void QCrsManagerWidget::SaveDialogSize() const
{
	if (size().width() <= 0 || size().height() <= 0)
	{
		return;
	}

	GetProcessLastDialogSize() = size();
	QSettings settings;
	settings.setValue(QStringLiteral("QCrsManagerWidget/size"), size());
}

void QCrsManagerWidget::RebuildCustomCrsRecords()
{
	customCrsRecords.clear();
	customCrsRecords.reserve(customDefinitions.size());

	for (const CustomCrsDefinition& definition : customDefinitions)
	{
		const std::string wktUtf8 = GB_Utf8Trim(definition.wktUtf8);
		if (wktUtf8.empty())
		{
			continue;
		}

		std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromWktCached(wktUtf8);
		if (!crs || !crs->IsValid())
		{
			continue;
		}

		CrsRecord record;
		record.isCustom = true;
		record.uniqueIdUtf8 = BuildCustomUniqueId(definition);
		record.wktUtf8 = wktUtf8;
		record.definitionUtf8 = wktUtf8;
		record.nameUtf8 = GB_Utf8Trim(definition.nameUtf8);
		if (record.nameUtf8.empty())
		{
			record.nameUtf8 = crs->GetNameUtf8();
		}
		if (record.nameUtf8.empty())
		{
			record.nameUtf8 = "自定义坐标系";
		}

		record.sourceUtf8 = GB_Utf8Trim(definition.sourceUtf8);
		if (record.sourceUtf8.empty())
		{
			record.sourceUtf8 = "自定义";
		}
		record.codeUtf8 = GB_Utf8Trim(definition.codeUtf8);
		record.ellipsoidUtf8 = crs->GetReferenceEllipsoidNameUtf8();
		record.category = crs->IsProjected() ? CrsCategory::Projected : CrsCategory::Geographic;

		customCrsRecords.push_back(std::move(record));
	}
}

void QCrsManagerWidget::RefreshTableAndKeepSelection()
{
	RefreshTable(true);
}

void QCrsManagerWidget::RefreshTable(bool preserveSelection)
{
	std::set<std::string> selectedUniqueIds;
	if (preserveSelection)
	{
		const std::vector<const CrsRecord*> selectedRecords = GetSelectedCrsRecords();
		for (const CrsRecord* record : selectedRecords)
		{
			if (record)
			{
				selectedUniqueIds.insert(record->uniqueIdUtf8);
			}
		}
	}

	const int sortColumn = crsTableWidget->horizontalHeader()->sortIndicatorSection() >= 0 ? crsTableWidget->horizontalHeader()->sortIndicatorSection() : ColumnCode;
	const Qt::SortOrder sortOrder = crsTableWidget->horizontalHeader()->sortIndicatorOrder();

	isRefreshingTable = true;
	QSignalBlocker tableBlocker(crsTableWidget);
	crsTableWidget->setSortingEnabled(false);
	crsTableWidget->clearContents();
	crsTableWidget->setRowCount(0);

	const CrsCategory category = static_cast<CrsCategory>(categoryComboBox->currentData().toInt());
	const QString searchText = searchLineEdit->text();

	std::vector<const CrsRecord*> visibleRecords;
	visibleRecords.reserve(systemCrsRecords.size() + customCrsRecords.size());

	const auto AppendVisibleRecord = [this, category, searchText, &visibleRecords](const CrsRecord& record)
		{
			if (DoesRecordPassFilter(record, category, searchText))
			{
				visibleRecords.push_back(&record);
			}
		};

	for (const CrsRecord& record : systemCrsRecords)
	{
		AppendVisibleRecord(record);
	}
	for (const CrsRecord& record : customCrsRecords)
	{
		AppendVisibleRecord(record);
	}

	crsTableWidget->setRowCount(static_cast<int>(visibleRecords.size()));
	for (int row = 0; row < static_cast<int>(visibleRecords.size()); row++)
	{
		const CrsRecord& record = *visibleRecords[static_cast<size_t>(row)];
		SetTableItem(crsTableWidget, row, ColumnName, ToQString(record.nameUtf8), record);
		SetTableItem(crsTableWidget, row, ColumnEllipsoid, ToQString(record.ellipsoidUtf8), record);
		SetTableItem(crsTableWidget, row, ColumnSource, ToQString(record.sourceUtf8), record);
		SetTableItem(crsTableWidget, row, ColumnCode, ToQString(record.codeUtf8), record);
		SetTableItem(crsTableWidget, row, ColumnType, ToCategoryText(record.category, record.isCustom), record);
	}

	crsTableWidget->setSortingEnabled(true);
	crsTableWidget->sortItems(sortColumn, sortOrder);
	if (preserveSelection && !selectedUniqueIds.empty())
	{
		QItemSelectionModel* selectionModel = crsTableWidget->selectionModel();
		selectionModel->clearSelection();
		for (int row = 0; row < crsTableWidget->rowCount(); row++)
		{
			QTableWidgetItem* item = crsTableWidget->item(row, ColumnName);
			if (!item)
			{
				continue;
			}

			const std::string uniqueId = ToUtf8(item->data(CrsUniqueIdRole).toString());
			if (selectedUniqueIds.find(uniqueId) != selectedUniqueIds.end())
			{
				selectionModel->select(crsTableWidget->model()->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
			}
		}
	}

	isRefreshingTable = false;
	UpdateDetailsAndButtons();
}

void QCrsManagerWidget::SelectCanvasCrsIfAvailable()
{
	if (hasSelectedInitialCanvasCrs)
	{
		return;
	}

	if (!mainCanvas)
	{
		hasSelectedInitialCanvasCrs = true;
		ClearTableSelection();
		return;
	}

	const std::string& canvasWkt = mainCanvas->GetCrsWkt();
	if (canvasWkt.empty())
	{
		hasSelectedInitialCanvasCrs = true;
		ClearTableSelection();
		return;
	}

	const std::string authorityCode = GeoCrsManager::WktToEpsgCodeUtf8(canvasWkt);
	if (authorityCode.empty())
	{
		hasSelectedInitialCanvasCrs = true;
		ClearTableSelection();
		return;
	}

	if (SelectCrsByUniqueId(authorityCode))
	{
		hasSelectedInitialCanvasCrs = true;
		return;
	}

	if (isSystemCrsInitializationFinished)
	{
		hasSelectedInitialCanvasCrs = true;
	}
}

bool QCrsManagerWidget::SelectCrsByUniqueId(const std::string& uniqueIdUtf8)
{
	if (uniqueIdUtf8.empty())
	{
		pendingCenteredCrsUniqueIdUtf8.clear();
		ClearTableSelection();
		return false;
	}

	if (!crsTableWidget || !crsTableWidget->selectionModel())
	{
		pendingCenteredCrsUniqueIdUtf8.clear();
		return false;
	}

	crsTableWidget->selectionModel()->clearSelection();

	int targetRow = -1;
	if (SelectVisibleCrsRowByUniqueId(uniqueIdUtf8, &targetRow))
	{
		pendingCenteredCrsUniqueIdUtf8 = uniqueIdUtf8;
		ScrollCrsRowToCenter(targetRow);
		ScheduleScrollSelectedCrsToCenterIfAvailable();
		UpdateDetailsAndButtons();
		return true;
	}

	pendingCenteredCrsUniqueIdUtf8.clear();
	UpdateDetailsAndButtons();
	return false;
}

bool QCrsManagerWidget::SelectVisibleCrsRowByUniqueId(const std::string& uniqueIdUtf8, int* outRow)
{
	if (outRow)
	{
		*outRow = -1;
	}

	if (uniqueIdUtf8.empty() || !crsTableWidget || !crsTableWidget->selectionModel() || !crsTableWidget->model())
	{
		return false;
	}

	QItemSelectionModel* selectionModel = crsTableWidget->selectionModel();
	for (int row = 0; row < crsTableWidget->rowCount(); row++)
	{
		QTableWidgetItem* item = crsTableWidget->item(row, ColumnName);
		if (!item)
		{
			continue;
		}

		const std::string rowUniqueId = ToUtf8(item->data(CrsUniqueIdRole).toString());
		if (!IsSameAuthorityCode(rowUniqueId, uniqueIdUtf8))
		{
			continue;
		}

		const QModelIndex targetIndex = crsTableWidget->model()->index(row, ColumnName);
		selectionModel->setCurrentIndex(targetIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

		// setCurrentIndex(... | Rows) 理论上已经会选择整行；这里再显式调用表格层接口，
		// 是为了抵抗 showEvent 后焦点/布局初始化过程中的 selection 状态被刷新掉的情况。
		crsTableWidget->setCurrentCell(row, ColumnName, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
		crsTableWidget->selectRow(row);

		if (outRow)
		{
			*outRow = row;
		}
		return true;
	}

	return false;
}

void QCrsManagerWidget::ScrollCrsRowToCenter(int row)
{
	if (!crsTableWidget || row < 0 || row >= crsTableWidget->rowCount())
	{
		return;
	}

	QTableWidgetItem* item = crsTableWidget->item(row, ColumnName);
	if (!item)
	{
		return;
	}

	crsTableWidget->scrollToItem(item, QAbstractItemView::PositionAtCenter);
}

void QCrsManagerWidget::ScrollSelectedCrsToCenterIfAvailable()
{
	if (!crsTableWidget || !crsTableWidget->selectionModel())
	{
		return;
	}

	QItemSelectionModel* selectionModel = crsTableWidget->selectionModel();
	int targetRow = -1;

	const QModelIndex currentIndex = crsTableWidget->currentIndex();
	if (currentIndex.isValid() && selectionModel->isRowSelected(currentIndex.row(), QModelIndex()))
	{
		targetRow = currentIndex.row();
	}
	else
	{
		const QModelIndexList selectedRows = selectionModel->selectedRows(ColumnName);
		if (!selectedRows.empty())
		{
			targetRow = selectedRows.first().row();
		}
	}

	if (targetRow < 0 || targetRow >= crsTableWidget->rowCount())
	{
		return;
	}

	ScrollCrsRowToCenter(targetRow);
}

void QCrsManagerWidget::ScheduleScrollSelectedCrsToCenterIfAvailable()
{
	if (!crsTableWidget || !isVisible())
	{
		return;
	}

	const std::string targetUniqueIdUtf8 = pendingCenteredCrsUniqueIdUtf8;
	QTimer::singleShot(0, this, [this, targetUniqueIdUtf8]()
		{
			if (!crsTableWidget || !crsTableWidget->selectionModel())
			{
				return;
			}

			int targetRow = -1;
			if (!targetUniqueIdUtf8.empty() && SelectVisibleCrsRowByUniqueId(targetUniqueIdUtf8, &targetRow))
			{
				ScrollCrsRowToCenter(targetRow);
				crsTableWidget->setFocus(Qt::OtherFocusReason);
				UpdateDetailsAndButtons();
				return;
			}

			ScrollSelectedCrsToCenterIfAvailable();
		});
}

void QCrsManagerWidget::ClearTableSelection()
{
	pendingCenteredCrsUniqueIdUtf8.clear();
	if (crsTableWidget && crsTableWidget->selectionModel())
	{
		crsTableWidget->selectionModel()->clearSelection();
	}
	UpdateDetailsAndButtons();
}

void QCrsManagerWidget::UpdateDetailsAndButtons()
{
	UpdateCrsDetails();
	UpdateCrsPreview();
	UpdateActionButtons();
}

void QCrsManagerWidget::UpdateCrsDetails()
{
	const std::vector<const CrsRecord*> selectedRecords = GetSelectedCrsRecords();
	if (selectedRecords.size() != 1 || selectedRecords[0] == nullptr)
	{
		crsDetailTextEdit->clear();
		return;
	}

	crsDetailTextEdit->setHtml(BuildDetailHtml(*selectedRecords[0]));
}


void QCrsManagerWidget::UpdateCrsPreview()
{
	if (!previewFrame)
	{
		return;
	}

	const std::vector<const CrsRecord*> selectedRecords = GetSelectedCrsRecords();
	if (selectedRecords.size() != 1 || selectedRecords[0] == nullptr)
	{
		previewFrame->ClearPreview();
		return;
	}

	const CrsRecord& record = *selectedRecords[0];
	std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromDefinitionCached(record.definitionUtf8, false, false);
	if (!crs || !crs->IsValid())
	{
		const std::string wktUtf8 = ResolveWktForRecord(record);
		if (!wktUtf8.empty())
		{
			crs = GeoCrsManager::GetFromDefinitionCached(wktUtf8, false, false);
		}
	}

	if (!crs || !crs->IsValid())
	{
		previewFrame->ClearPreview();
		return;
	}

	previewFrame->SetValidAreaSegments(crs->GetValidAreaLonLatSegments());
}

void QCrsManagerWidget::UpdateActionButtons()
{
	const std::vector<const CrsRecord*> selectedRecords = GetSelectedCrsRecords();
	applyToCanvasButton->setEnabled(mainCanvas != nullptr && selectedRecords.size() == 1 && selectedRecords[0] != nullptr);

	bool canDelete = !selectedRecords.empty();
	for (const CrsRecord* record : selectedRecords)
	{
		if (record == nullptr || !record->isCustom)
		{
			canDelete = false;
			break;
		}
	}
	deleteCrsButton->setEnabled(canDelete);
}

void QCrsManagerWidget::OnDeleteSelectedCrsClicked()
{
	const std::vector<const CrsRecord*> selectedRecords = GetSelectedCrsRecords();
	if (selectedRecords.empty())
	{
		return;
	}

	for (const CrsRecord* record : selectedRecords)
	{
		if (record == nullptr || !record->isCustom)
		{
			return;
		}
	}

	const int answer = QMessageBox::question(this,
		QStringLiteral("确认删除"),
		QStringLiteral("确定要删除当前选中的 %1 个自定义坐标系吗？").arg(static_cast<int>(selectedRecords.size())),
		QMessageBox::Yes | QMessageBox::No,
		QMessageBox::No);
	if (answer != QMessageBox::Yes)
	{
		return;
	}

	std::set<std::string> idsToDelete;
	for (const CrsRecord* record : selectedRecords)
	{
		idsToDelete.insert(record->uniqueIdUtf8);
	}

	std::vector<CustomCrsDefinition> remainingDefinitions;
	remainingDefinitions.reserve(customDefinitions.size());
	for (const CustomCrsDefinition& definition : customDefinitions)
	{
		const std::string id = BuildCustomUniqueId(definition);
		if (idsToDelete.find(id) == idsToDelete.end())
		{
			remainingDefinitions.push_back(definition);
		}
	}

	customDefinitions.swap(remainingDefinitions);
	RebuildCustomCrsRecords();
	RefreshTable(false);
	emit CustomCrsDefinitionsChanged();
}

void QCrsManagerWidget::OnApplyToCanvasClicked()
{
	ApplySelectedCrsToCanvas(true);
}

bool QCrsManagerWidget::ApplySelectedCrsToCanvas(bool closeDialogAfterApply)
{
	const std::vector<const CrsRecord*> selectedRecords = GetSelectedCrsRecords();
	if (selectedRecords.size() != 1 || selectedRecords[0] == nullptr || mainCanvas == nullptr)
	{
		return false;
	}

	const std::string wktUtf8 = ResolveWktForRecord(*selectedRecords[0]);
	if (wktUtf8.empty())
	{
		QMessageBox::warning(this, QStringLiteral("设置坐标系失败"), QStringLiteral("未能生成当前坐标系的 WKT。"));
		return false;
	}

	mainCanvas->SetCrsWkt(wktUtf8);
	if (closeDialogAfterApply)
	{
		CloseOwningWindow();
	}

	return true;
}

void QCrsManagerWidget::OnAddCustomCrsClicked()
{
	// TODO: 后续在这里接入“添加自定义坐标系”的编辑对话框。
}

void QCrsManagerWidget::ShowTableContextMenu(const QPoint& pos)
{
	const QModelIndex index = crsTableWidget->indexAt(pos);
	if (!index.isValid())
	{
		return;
	}

	QItemSelectionModel* selectionModel = crsTableWidget->selectionModel();
	if (!selectionModel->isRowSelected(index.row(), QModelIndex()))
	{
		selectionModel->clearSelection();
		selectionModel->select(crsTableWidget->model()->index(index.row(), 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
	}

	const std::vector<const CrsRecord*> selectedRecords = GetSelectedCrsRecords();
	if (selectedRecords.size() != 1 || selectedRecords[0] == nullptr)
	{
		return;
	}

	QMenu menu(this);
	QAction* applyAction = menu.addAction(style()->standardIcon(QStyle::SP_DialogApplyButton), QStringLiteral("应用到画布"));
	applyAction->setEnabled(mainCanvas != nullptr);
	connect(applyAction, &QAction::triggered, this, [this]()
		{
			ApplySelectedCrsToCanvas(false);
		});

	menu.exec(crsTableWidget->viewport()->mapToGlobal(pos));
}

void QCrsManagerWidget::CloseOwningWindow()
{
	accept();
}

std::vector<const QCrsManagerWidget::CrsRecord*> QCrsManagerWidget::GetSelectedCrsRecords() const
{
	std::vector<const CrsRecord*> selectedRecords;
	if (!crsTableWidget || !crsTableWidget->selectionModel())
	{
		return selectedRecords;
	}

	const QModelIndexList selectedRows = crsTableWidget->selectionModel()->selectedRows();
	selectedRecords.reserve(static_cast<size_t>(selectedRows.size()));

	std::set<std::string> addedIds;
	for (const QModelIndex& index : selectedRows)
	{
		QTableWidgetItem* item = crsTableWidget->item(index.row(), ColumnName);
		if (!item)
		{
			continue;
		}

		const std::string uniqueId = ToUtf8(item->data(CrsUniqueIdRole).toString());
		if (uniqueId.empty() || addedIds.find(uniqueId) != addedIds.end())
		{
			continue;
		}

		const CrsRecord* record = FindCrsRecordByUniqueId(uniqueId);
		if (record)
		{
			selectedRecords.push_back(record);
			addedIds.insert(uniqueId);
		}
	}

	return selectedRecords;
}

const QCrsManagerWidget::CrsRecord* QCrsManagerWidget::FindCrsRecordByUniqueId(const std::string& uniqueIdUtf8) const
{
	for (const CrsRecord& record : systemCrsRecords)
	{
		if (IsSameAuthorityCode(record.uniqueIdUtf8, uniqueIdUtf8))
		{
			return &record;
		}
	}

	for (const CrsRecord& record : customCrsRecords)
	{
		if (IsSameAuthorityCode(record.uniqueIdUtf8, uniqueIdUtf8))
		{
			return &record;
		}
	}

	return nullptr;
}

std::string QCrsManagerWidget::ResolveWktForRecord(const CrsRecord& record) const
{
	if (!record.wktUtf8.empty())
	{
		return record.wktUtf8;
	}

	std::shared_ptr<const GeoCrs> crs = GeoCrsManager::GetFromDefinitionCached(record.definitionUtf8, false, false);
	if (!crs || !crs->IsValid())
	{
		return "";
	}

	return crs->ExportToWktUtf8(GeoCrs::WktFormat::Wkt2_2018, true);
}

QString QCrsManagerWidget::BuildDetailHtml(const CrsRecord& record) const
{
	QString html;
	html += QStringLiteral("<html><body style=\"font-family: '%1'; font-size: %2pt;\">")
		.arg(font().family())
		.arg(font().pointSize() > 0 ? font().pointSize() : 9);

	html += BuildDetailRowHtmlUtf8(QStringLiteral("名称"), record.nameUtf8);
	html += BuildDetailRowHtml(QStringLiteral("类型"), ToCategoryText(record.category, record.isCustom));
	html += BuildDetailRowHtmlUtf8(QStringLiteral("来源"), record.sourceUtf8);
	html += BuildDetailRowHtmlUtf8(QStringLiteral("代码"), record.codeUtf8);
	html += BuildDetailRowHtmlUtf8(QStringLiteral("参考椭球"), record.ellipsoidUtf8);

	if (!record.projectionMethodUtf8.empty())
	{
		html += BuildDetailRowHtmlUtf8(QStringLiteral("投影方法"), record.projectionMethodUtf8);
	}

	if (!record.areaNameUtf8.empty())
	{
		html += BuildDetailRowHtmlUtf8(QStringLiteral("适用区域"), record.areaNameUtf8);
	}

	if (!record.bboxTextUtf8.empty())
	{
		html += BuildDetailRowHtmlUtf8(QStringLiteral("经纬度范围"), record.bboxTextUtf8);
	}

	html += BuildDetailRowHtml(QStringLiteral("是否废弃"), record.isDeprecated ? QStringLiteral("是") : QStringLiteral("否"));
	html += BuildDetailRowHtmlUtf8(QStringLiteral("唯一标识"), record.uniqueIdUtf8);

	const QString wktHtml = EscapeHtmlUtf8(ResolveWktForRecord(record));
	html += QStringLiteral("<div style=\"margin-top: 10px;\"><b>WKT：</b></div>");
	html += QStringLiteral("<pre style=\"margin-top: 4px; white-space: pre; font-family: Consolas, 'Courier New', monospace;\">%1</pre>").arg(wktHtml);
	html += QStringLiteral("</body></html>");
	return html;
}

bool QCrsManagerWidget::DoesRecordPassFilter(const CrsRecord& record, CrsCategory category, const QString& searchText) const
{
	if (category == CrsCategory::Custom)
	{
		if (!record.isCustom)
		{
			return false;
		}
	}
	else if (category == CrsCategory::Geographic)
	{
		if (record.isCustom || record.category != CrsCategory::Geographic)
		{
			return false;
		}
	}
	else if (category == CrsCategory::Projected)
	{
		if (record.isCustom || record.category != CrsCategory::Projected)
		{
			return false;
		}
	}

	const QStringList searchTerms = BuildSearchTerms(searchText);
	if (searchTerms.empty())
	{
		return true;
	}

	const QString searchableText = ToQString(record.nameUtf8 + " " + record.ellipsoidUtf8 + " " + record.sourceUtf8 + " " + record.codeUtf8 + " " +
		record.definitionUtf8 + " " + record.areaNameUtf8 + " " + record.projectionMethodUtf8 + " " + ToCategoryTextUtf8(record.category, record.isCustom));
	for (const QString& term : searchTerms)
	{
		if (!searchableText.contains(term, Qt::CaseInsensitive))
		{
			return false;
		}
	}

	return true;
}
