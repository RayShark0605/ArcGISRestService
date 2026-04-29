#include "QAddCustomCrsDialog.h"

#include <QFont>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

namespace
{
	QString ToQString(const std::string& textUtf8)
	{
		return QString::fromUtf8(textUtf8.c_str());
	}

	std::string ToUtf8(const QString& text)
	{
		return std::string(text.toUtf8().constData());
	}

	QString FromUtf8Literal(const char* textUtf8)
	{
		return QString::fromUtf8(textUtf8);
	}
}

QAddCustomCrsDialog::QAddCustomCrsDialog(QWidget* parent) : QDialog(parent)
{
	InitializeUi();
}

QAddCustomCrsDialog::~QAddCustomCrsDialog()
{
}

void QAddCustomCrsDialog::SetWktUtf8(const std::string& wktUtf8)
{
	if (wktTextEdit)
	{
		wktTextEdit->setPlainText(ToQString(wktUtf8));
	}
}

std::string QAddCustomCrsDialog::GetWktUtf8() const
{
	if (!wktTextEdit)
	{
		return "";
	}

	return ToUtf8(wktTextEdit->toPlainText());
}

void QAddCustomCrsDialog::SetAcceptValidator(const std::function<bool(const std::string& wktUtf8, std::string& outErrorMessageUtf8)>& validator)
{
	acceptValidator = validator;
}

void QAddCustomCrsDialog::accept()
{
	if (acceptValidator)
	{
		std::string errorMessageUtf8;
		if (!acceptValidator(GetWktUtf8(), errorMessageUtf8))
		{
			if (errorMessageUtf8.empty())
			{
				errorMessageUtf8 = u8"输入的自定义坐标系 WKT 无效。";
			}

			QMessageBox::warning(this, FromUtf8Literal(u8"添加自定义坐标系失败"), ToQString(errorMessageUtf8));
			return;
		}
	}

	QDialog::accept();
}

void QAddCustomCrsDialog::InitializeUi()
{
	setObjectName(QStringLiteral("QAddCustomCrsDialog"));
	setWindowTitle(QStringLiteral("添加自定义坐标系"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	resize(760, 520);
	setMinimumSize(520, 360);

	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(8, 8, 8, 8);
	mainLayout->setSpacing(8);

	wktTextEdit = new QTextEdit(this);
	wktTextEdit->setObjectName(QStringLiteral("wktTextEdit"));
	wktTextEdit->setAcceptRichText(false);
	wktTextEdit->setLineWrapMode(QTextEdit::NoWrap);
	wktTextEdit->setPlaceholderText(QStringLiteral("请在这里输入或粘贴自定义坐标系的 WKT。"));

	QFont textFont(QStringLiteral("Consolas"));
	textFont.setStyleHint(QFont::Monospace);
	wktTextEdit->setFont(textFont);
	mainLayout->addWidget(wktTextEdit, 1);

	QHBoxLayout* buttonLayout = new QHBoxLayout();
	buttonLayout->setContentsMargins(0, 0, 0, 0);
	buttonLayout->addStretch(1);

	okButton = new QPushButton(QStringLiteral("确定"), this);
	okButton->setObjectName(QStringLiteral("okButton"));
	okButton->setDefault(true);
	okButton->setAutoDefault(true);
	buttonLayout->addWidget(okButton, 0);
	mainLayout->addLayout(buttonLayout);

	connect(okButton, &QPushButton::clicked, this, &QAddCustomCrsDialog::accept);
}
