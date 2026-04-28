#pragma once

#include <QWidget>

#include <string>
#include <vector>

class QComboBox;
class QFrame;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTextEdit;
class QToolButton;
class QMainCanvas;

class QCrsManagerWidget : public QWidget
{
	Q_OBJECT

public:
	struct CustomCrsDefinition
	{
		std::string idUtf8 = "";
		std::string nameUtf8 = "";
		std::string sourceUtf8 = "自定义";
		std::string codeUtf8 = "";
		std::string wktUtf8 = "";
	};

	explicit QCrsManagerWidget(QWidget* parent = nullptr);
	virtual ~QCrsManagerWidget() override;

	void BindMainCanvas(QMainCanvas* canvas);
	QMainCanvas* GetMainCanvas() const;

	void SetCustomCrsDefinitions(const std::vector<CustomCrsDefinition>& customDefinitions);
	std::vector<CustomCrsDefinition> GetCustomCrsDefinitions() const;

	void ReloadCoordinateSystems();

signals:
	void CustomCrsDefinitionsChanged();

protected:
	virtual void showEvent(QShowEvent* event) override;

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

	std::vector<CrsRecord> systemCrsRecords;
	std::vector<CrsRecord> customCrsRecords;
	std::vector<CustomCrsDefinition> customDefinitions;

	bool isRefreshingTable = false;
	bool hasSelectedInitialCanvasCrs = false;

	void InitializeUi();
	void InitializeConnections();
	void LoadSystemCrsRecords();
	void RebuildCustomCrsRecords();
	void RefreshTable(bool preserveSelection);
	void RefreshTableAndKeepSelection();
	void SelectCanvasCrsIfAvailable();
	void SelectCrsByUniqueId(const std::string& uniqueIdUtf8);
	void ClearTableSelection();
	void UpdateDetailsAndButtons();
	void UpdateCrsDetails();
	void UpdateActionButtons();
	void OnDeleteSelectedCrsClicked();
	void OnApplyToCanvasClicked();
	void OnAddCustomCrsClicked();
	void CloseOwningWindow();

	std::vector<const CrsRecord*> GetSelectedCrsRecords() const;
	const CrsRecord* FindCrsRecordByUniqueId(const std::string& uniqueIdUtf8) const;
	std::string ResolveWktForRecord(const CrsRecord& record) const;
	QString BuildDetailText(const CrsRecord& record) const;
	bool DoesRecordPassFilter(const CrsRecord& record, CrsCategory category, const QString& searchText) const;
};
