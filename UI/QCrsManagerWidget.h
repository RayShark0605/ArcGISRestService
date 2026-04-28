#pragma once

#include <QDialog>
#include <cstdint>
#include <string>
#include <vector>

class QCloseEvent;
class QComboBox;
class QFrame;
class QLineEdit;
class QPoint;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTextEdit;
class QTimer;
class QToolButton;
class QMainCanvas;

class QCrsManagerWidget : public QDialog
{
	Q_OBJECT

public:
	struct CustomCrsDefinition
	{
		std::string idUtf8 = "";
		std::string nameUtf8 = "";
		std::string sourceUtf8 = u8"自定义";
		std::string codeUtf8 = "";
		std::string wktUtf8 = "";
	};

	explicit QCrsManagerWidget(QWidget* parent = nullptr);
	virtual ~QCrsManagerWidget() override;

	// 异步预加载系统坐标系列表。
	// 建议在进程启动后尽早调用；函数会立即返回，不阻塞调用线程。
	static void InitializeSystemCrsRecordsAsync();

	void BindMainCanvas(QMainCanvas* canvas);
	QMainCanvas* GetMainCanvas() const;

	void SetCustomCrsDefinitions(const std::vector<CustomCrsDefinition>& customDefinitions);
	std::vector<CustomCrsDefinition> GetCustomCrsDefinitions() const;

	void ReloadCoordinateSystems();

signals:
	void CustomCrsDefinitionsChanged();

protected:
	virtual void showEvent(QShowEvent* event) override;
	virtual void closeEvent(QCloseEvent* event) override;
	virtual void done(int result) override;

public:
	enum class CrsCategory
	{
		All = 0,
		Geographic,
		Projected,
		Custom
	};

	struct CrsRecord
	{
		std::string uniqueIdUtf8 = "";
		std::string nameUtf8 = "";
		std::string ellipsoidUtf8 = "";
		std::string sourceUtf8 = "";
		std::string codeUtf8 = "";
		std::string definitionUtf8 = "";
		std::string wktUtf8 = "";
		std::string areaNameUtf8 = "";
		std::string projectionMethodUtf8 = "";
		std::string bboxTextUtf8 = "";
		CrsCategory category = CrsCategory::Geographic;
		bool isCustom = false;
		bool isDeprecated = false;
	};

private:
	QMainCanvas* mainCanvas = nullptr;

	QComboBox* categoryComboBox = nullptr;
	QLineEdit* searchLineEdit = nullptr;
	QToolButton* addCustomCrsButton = nullptr;
	QToolButton* deleteCrsButton = nullptr;
	QTableWidget* crsTableWidget = nullptr;
	QTextEdit* crsDetailTextEdit = nullptr;
	QFrame* previewFrame = nullptr;
	QPushButton* applyToCanvasButton = nullptr;
	QTimer* systemCrsInitializationPollTimer = nullptr;

	std::vector<CrsRecord> systemCrsRecords;
	std::vector<CrsRecord> customCrsRecords;
	std::vector<CustomCrsDefinition> customDefinitions;

	bool isRefreshingTable = false;
	bool hasSelectedInitialCanvasCrs = false;
	bool isSystemCrsInitializationFinished = false;
	std::uint64_t loadedSystemCrsRecordsRevision = 0;
	std::string pendingCenteredCrsUniqueIdUtf8 = "";

	void InitializeUi();
	void InitializeConnections();
	bool LoadSystemCrsRecords();
	void RebuildCustomCrsRecords();
	void RefreshTable(bool preserveSelection);
	void RefreshTableAndKeepSelection();
	void PollSystemCrsRecordsInitialization();
	void UpdateInitializationUiState();
	void RestoreDialogSize();
	void SaveDialogSize() const;
	void SelectCanvasCrsIfAvailable();
	bool SelectCrsByUniqueId(const std::string& uniqueIdUtf8);
	bool SelectVisibleCrsRowByUniqueId(const std::string& uniqueIdUtf8, int* outRow = nullptr);
	void ScrollCrsRowToCenter(int row);
	void ScrollSelectedCrsToCenterIfAvailable();
	void ScheduleScrollSelectedCrsToCenterIfAvailable();
	void ClearTableSelection();
	void UpdateDetailsAndButtons();
	void UpdateCrsDetails();
	void UpdateActionButtons();
	void OnDeleteSelectedCrsClicked();
	void OnApplyToCanvasClicked();
	bool ApplySelectedCrsToCanvas(bool closeDialogAfterApply);
	void OnAddCustomCrsClicked();
	void ShowTableContextMenu(const QPoint& pos);
	void CloseOwningWindow();

	std::vector<const CrsRecord*> GetSelectedCrsRecords() const;
	const CrsRecord* FindCrsRecordByUniqueId(const std::string& uniqueIdUtf8) const;
	std::string ResolveWktForRecord(const CrsRecord& record) const;
	QString BuildDetailHtml(const CrsRecord& record) const;
	bool DoesRecordPassFilter(const CrsRecord& record, CrsCategory category, const QString& searchText) const;
};
