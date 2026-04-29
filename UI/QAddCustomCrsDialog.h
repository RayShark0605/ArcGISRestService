#pragma once

#include <QDialog>
#include <functional>
#include <string>

class QPushButton;
class QTextEdit;

class QAddCustomCrsDialog : public QDialog
{
	Q_OBJECT

public:
	explicit QAddCustomCrsDialog(QWidget* parent = nullptr);
	virtual ~QAddCustomCrsDialog() override;

	void SetWktUtf8(const std::string& wktUtf8);
	std::string GetWktUtf8() const;

	void SetAcceptValidator(const std::function<bool(const std::string& wktUtf8, std::string& outErrorMessageUtf8)>& validator);

public slots:
	virtual void accept() override;

private:
	QTextEdit* wktTextEdit = nullptr;
	QPushButton* okButton = nullptr;
	std::function<bool(const std::string& wktUtf8, std::string& outErrorMessageUtf8)> acceptValidator;

	void InitializeUi();
};
