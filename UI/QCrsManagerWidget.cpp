#include "QCrsManagerWidget.h"

#include "GeoCrs.h"
#include "GeoCrsManager.h"
#include "QMainCanvas.h"
#include "GeoBase/GB_Utf8String.h"

#include <QApplication>
#include <QBrush>
#include <QComboBox>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QModelIndexList>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <string>
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

	void AppendAuthorityCrsRecords(const std::string& authorityNameUtf8, std::vector<QCrsManagerWidget::CrsRecord>& records)
	{
		const std::vector<GeoCrsDatabaseRecord> databaseRecords = GeoCrsManager::ListDatabaseCrsRecords(authorityNameUtf8);
		for (const GeoCrsDatabaseRecord& databaseRecord : databaseRecords)
		{
			if (!IsSupportedMapCrsType(databaseRecord.type))
			{
				continue;
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
				continue;
			}

			record.uniqueIdUtf8 = databaseRecord.GetAuthorityCodeUtf8();
			record.definitionUtf8 = record.uniqueIdUtf8;
			record.ellipsoidUtf8 = ExtractReferenceEllipsoidNameUtf8(record.definitionUtf8);

			records.push_back(std::move(record));
		}
	}

	bool IsSameAuthorityCode(const std::string& left, const std::string& right)
	{
		return GB_Utf8Equals(left, right, false);
	}
}

QCrsManagerWidget::QCrsManagerWidget(QWidget* parent) : QWidget(parent)
{
	InitializeUi();
	InitializeConnections();
	ReloadCoordinateSystems();
}

QCrsManagerWidget::~QCrsManagerWidget()
{
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
	QWidget::showEvent(event);
	SelectCanvasCrsIfAvailable();
}

void QCrsManagerWidget::InitializeUi()
{
	setObjectName(QStringLiteral("QCrsManagerWidget"));
	setWindowTitle(QStringLiteral("坐标系管理"));
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
	crsTableWidget->setAlternatingRowColors(true);
	crsTableWidget->setSortingEnabled(true);
	crsTableWidget->verticalHeader()->setVisible(false);
	crsTableWidget->verticalHeader()->setDefaultSectionSize(24);
	crsTableWidget->horizontalHeader()->setStretchLastSection(false);
	crsTableWidget->horizontalHeader()->setSectionResizeMode(ColumnName, QHeaderView::Stretch);
	crsTableWidget->horizontalHeader()->setSectionResizeMode(ColumnEllipsoid, QHeaderView::ResizeToContents);
	crsTableWidget->horizontalHeader()->setSectionResizeMode(ColumnSource, QHeaderView::ResizeToContents);
	crsTableWidget->horizontalHeader()->setSectionResizeMode(ColumnCode, QHeaderView::ResizeToContents);
	crsTableWidget->horizontalHeader()->setSectionResizeMode(ColumnType, QHeaderView::ResizeToContents);
	leftLayout->addWidget(crsTableWidget, 1);

	applyToCanvasButton = new QPushButton(QStringLiteral("应用到画布"), leftWidget);
	applyToCanvasButton->setObjectName(QStringLiteral("applyToCanvasButton"));
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

	previewFrame = new QFrame(rightWidget);
	previewFrame->setObjectName(QStringLiteral("previewFrame"));
	previewFrame->setFrameShape(QFrame::StyledPanel);
	previewFrame->setFrameShadow(QFrame::Sunken);
	previewFrame->setMinimumHeight(120);
	previewFrame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	rightLayout->addWidget(previewFrame, 1);

	splitter->addWidget(rightWidget);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);
	splitter->setSizes(QList<int>() << 540 << 340);
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
}

void QCrsManagerWidget::LoadSystemCrsRecords()
{
	systemCrsRecords.clear();

	QApplication::setOverrideCursor(Qt::WaitCursor);

	GeoCrsManager::GetProjDbDirectoryUtf8();
	AppendAuthorityCrsRecords("EPSG", systemCrsRecords);
	AppendAuthorityCrsRecords("ESRI", systemCrsRecords);

	std::sort(systemCrsRecords.begin(), systemCrsRecords.end(), [](const CrsRecord& left, const CrsRecord& right) -> bool
		{
			const int sourceCompare = GB_Utf8CompareLogical(left.sourceUtf8, right.sourceUtf8);
			if (sourceCompare != 0)
			{
				return sourceCompare < 0;
			}

			return GB_Utf8CompareLogical(left.codeUtf8, right.codeUtf8) < 0;
		});

	QApplication::restoreOverrideCursor();
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

	const auto AppendRecordToTable = [this, category, searchText](const CrsRecord& record)
		{
			if (!DoesRecordPassFilter(record, category, searchText))
			{
				return;
			}

			const int row = crsTableWidget->rowCount();
			crsTableWidget->insertRow(row);
			SetTableItem(crsTableWidget, row, ColumnName, ToQString(record.nameUtf8), record);
			SetTableItem(crsTableWidget, row, ColumnEllipsoid, ToQString(record.ellipsoidUtf8), record);
			SetTableItem(crsTableWidget, row, ColumnSource, ToQString(record.sourceUtf8), record);
			SetTableItem(crsTableWidget, row, ColumnCode, ToQString(record.codeUtf8), record);
			SetTableItem(crsTableWidget, row, ColumnType, ToCategoryText(record.category, record.isCustom), record);
		};

	for (const CrsRecord& record : systemCrsRecords)
	{
		AppendRecordToTable(record);
	}
	for (const CrsRecord& record : customCrsRecords)
	{
		AppendRecordToTable(record);
	}

	crsTableWidget->setSortingEnabled(true);
	crsTableWidget->sortItems(sortColumn, sortOrder);
	crsTableWidget->resizeColumnToContents(ColumnSource);
	crsTableWidget->resizeColumnToContents(ColumnCode);
	crsTableWidget->resizeColumnToContents(ColumnType);

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

	hasSelectedInitialCanvasCrs = true;
	if (!mainCanvas)
	{
		ClearTableSelection();
		return;
	}

	const std::string& canvasWkt = mainCanvas->GetCrsWkt();
	if (canvasWkt.empty())
	{
		ClearTableSelection();
		return;
	}

	const std::string authorityCode = GeoCrsManager::WktToEpsgCodeUtf8(canvasWkt);
	if (authorityCode.empty())
	{
		ClearTableSelection();
		return;
	}

	SelectCrsByUniqueId(authorityCode);
}

void QCrsManagerWidget::SelectCrsByUniqueId(const std::string& uniqueIdUtf8)
{
	if (uniqueIdUtf8.empty())
	{
		ClearTableSelection();
		return;
	}

	QItemSelectionModel* selectionModel = crsTableWidget->selectionModel();
	selectionModel->clearSelection();

	for (int row = 0; row < crsTableWidget->rowCount(); row++)
	{
		QTableWidgetItem* item = crsTableWidget->item(row, ColumnName);
		if (!item)
		{
			continue;
		}

		const std::string rowUniqueId = ToUtf8(item->data(CrsUniqueIdRole).toString());
		if (IsSameAuthorityCode(rowUniqueId, uniqueIdUtf8))
		{
			selectionModel->select(crsTableWidget->model()->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
			crsTableWidget->scrollToItem(item, QAbstractItemView::PositionAtCenter);
			UpdateDetailsAndButtons();
			return;
		}
	}

	UpdateDetailsAndButtons();
}

void QCrsManagerWidget::ClearTableSelection()
{
	crsTableWidget->selectionModel()->clearSelection();
	UpdateDetailsAndButtons();
}

void QCrsManagerWidget::UpdateDetailsAndButtons()
{
	UpdateCrsDetails();
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

	crsDetailTextEdit->setPlainText(BuildDetailText(*selectedRecords[0]));
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
	const std::vector<const CrsRecord*> selectedRecords = GetSelectedCrsRecords();
	if (selectedRecords.size() != 1 || selectedRecords[0] == nullptr || mainCanvas == nullptr)
	{
		return;
	}

	const std::string wktUtf8 = ResolveWktForRecord(*selectedRecords[0]);
	if (wktUtf8.empty())
	{
		QMessageBox::warning(this, QStringLiteral("设置坐标系失败"), QStringLiteral("未能生成当前坐标系的 WKT。"));
		return;
	}

	mainCanvas->SetCrsWkt(wktUtf8);
	CloseOwningWindow();
}

void QCrsManagerWidget::OnAddCustomCrsClicked()
{
	// TODO: 后续在这里接入“添加自定义坐标系”的编辑对话框。
}

void QCrsManagerWidget::CloseOwningWindow()
{
	QWidget* ownerWindow = window();
	if (ownerWindow && ownerWindow != this)
	{
		ownerWindow->close();
		return;
	}

	close();
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

QString QCrsManagerWidget::BuildDetailText(const CrsRecord& record) const
{
	QString text;
	text += QStringLiteral("名称: ") + ToQString(record.nameUtf8) + QLatin1Char('\n');
	text += QStringLiteral("类型: ") + ToCategoryText(record.category, record.isCustom) + QLatin1Char('\n');
	text += QStringLiteral("来源: ") + ToQString(record.sourceUtf8) + QLatin1Char('\n');
	text += QStringLiteral("代码: ") + ToQString(record.codeUtf8) + QLatin1Char('\n');
	text += QStringLiteral("参考椭球: ") + ToQString(record.ellipsoidUtf8) + QLatin1Char('\n');
	if (!record.projectionMethodUtf8.empty())
	{
		text += QStringLiteral("投影方法: ") + ToQString(record.projectionMethodUtf8) + QLatin1Char('\n');
	}
	if (!record.areaNameUtf8.empty())
	{
		text += QStringLiteral("适用区域: ") + ToQString(record.areaNameUtf8) + QLatin1Char('\n');
	}
	if (!record.bboxTextUtf8.empty())
	{
		text += QStringLiteral("经纬度范围: ") + ToQString(record.bboxTextUtf8) + QLatin1Char('\n');
	}
	text += QStringLiteral("是否废弃: ") + (record.isDeprecated ? QStringLiteral("是") : QStringLiteral("否")) + QLatin1Char('\n');
	text += QStringLiteral("唯一标识: ") + ToQString(record.uniqueIdUtf8) + QLatin1Char('\n');
	text += QLatin1Char('\n');
	text += QStringLiteral("WKT:") + QLatin1Char('\n');
	text += ToQString(ResolveWktForRecord(record));
	return text;
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
