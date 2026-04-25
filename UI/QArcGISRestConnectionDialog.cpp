#include "QArcGISRestConnectionDialog.h"

#if defined(_MSC_VER)
#  pragma execution_character_set("utf-8")
#endif

#include <QAbstractItemView>
#include <QByteArray>
#include <QColor>
#include <QAction>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QModelIndex>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QSize>
#include <QSizePolicy>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>

#include <algorithm>
#include <functional>

namespace
{
    class BoldTitleGroupBox : public QGroupBox
    {
    public:
        explicit BoldTitleGroupBox(const QString& title, QWidget* parent = nullptr)
            : QGroupBox(title, parent)
        {
            // 只对 QGroupBox 的标题子控件加粗，不修改 QGroupBox 本体字体，
            // 避免子控件继承粗体字体；同时给标题左右留出少量内边距，
            // 防止长标题在不同 Windows 字体缩放/DPI 下被裁切。
            setStyleSheet(QString::fromUtf8(
                "QGroupBox::title {"
                " font-weight: bold;"
                " padding-left: 2px;"
                " padding-right: 8px;"
                "}"
            ));
        }
    };

    QIcon CreatePlusIcon()
    {
        QPixmap pixmap(22, 22);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(50, 130, 50), 1.8));
        painter.setBrush(QColor(235, 250, 235));
        painter.drawRect(QRectF(3.5, 3.5, 15.0, 15.0));
        painter.drawLine(QPointF(11.0, 6.6), QPointF(11.0, 15.4));
        painter.drawLine(QPointF(6.6, 11.0), QPointF(15.4, 11.0));
        return QIcon(pixmap);
    }

    QIcon CreateMinusIcon()
    {
        QPixmap pixmap(22, 22);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(180, 60, 60), 1.8));
        painter.setBrush(QColor(252, 238, 238));
        painter.drawRect(QRectF(3.5, 3.5, 15.0, 15.0));
        painter.drawLine(QPointF(6.6, 11.0), QPointF(15.4, 11.0));
        return QIcon(pixmap);
    }

    QIcon CreateEyeIcon(bool visible)
    {
        QPixmap pixmap(18, 18);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(80, 90, 100), 1.4));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(3.0, 5.2, 12.0, 7.6));
        painter.setBrush(QColor(80, 90, 100));
        painter.drawEllipse(QRectF(7.0, 7.2, 4.0, 4.0));

        if (!visible)
        {
            painter.setPen(QPen(QColor(120, 70, 70), 1.7));
            painter.drawLine(QPointF(4.0, 14.0), QPointF(14.0, 4.0));
        }

        return QIcon(pixmap);
    }

    void SetFixedFieldHeight(QLineEdit* lineEdit)
    {
        if (!lineEdit)
        {
            return;
        }

        lineEdit->setMinimumHeight(23);
    }
}

QArcGISRestConnectionDialog::QArcGISRestConnectionDialog(QWidget* parent)
    : QDialog(parent)
{
    InitializeUi();
    Clear();
}

QArcGISRestConnectionDialog::~QArcGISRestConnectionDialog()
{
}

void QArcGISRestConnectionDialog::SetSettings(const ArcGISRestConnectionSettings& settings)
{
    if (!mNameLineEdit || !mHeaderTableWidget)
    {
        return;
    }

    mNameLineEdit->setText(StdStringToQString(settings.displayName));
    mUrlLineEdit->setText(StdStringToQString(settings.serviceUrl));
    mPrefixLineEdit->setText(StdStringToQString(settings.urlPrefix));
    mPortalCommunityLineEdit->setText(StdStringToQString(settings.portalCommunityEndpoint));
    mPortalContentLineEdit->setText(StdStringToQString(settings.portalContentEndpoint));
    mUsernameLineEdit->setText(StdStringToQString(settings.username));
    mPasswordLineEdit->setText(StdStringToQString(settings.password));
    mHttpRefererLineEdit->setText(StdStringToQString(settings.httpReferer));

    mHeaderTableWidget->blockSignals(true);
    mHeaderTableWidget->setRowCount(0);
    for (const std::pair<std::string, std::string>& header : settings.httpCustomHeaders)
    {
        AddHeaderRow(StdStringToQString(header.first), StdStringToQString(header.second), false);
    }

    if (mHeaderTableWidget->rowCount() == 0)
    {
        AddHeaderRow(QString(), QString(), false);
    }
    mHeaderTableWidget->blockSignals(false);

    UpdateOkButtonState();
    UpdateRemoveHeaderButtonState();
}

ArcGISRestConnectionSettings QArcGISRestConnectionDialog::GetSettings() const
{
    ArcGISRestConnectionSettings settings;
    settings.displayName = QStringToStdString(mNameLineEdit ? mNameLineEdit->text().trimmed() : QString());
    settings.serviceUrl = QStringToStdString(mUrlLineEdit ? mUrlLineEdit->text().trimmed() : QString());
    settings.urlPrefix = QStringToStdString(mPrefixLineEdit ? mPrefixLineEdit->text().trimmed() : QString());
    settings.portalCommunityEndpoint = QStringToStdString(mPortalCommunityLineEdit ? mPortalCommunityLineEdit->text().trimmed() : QString());
    settings.portalContentEndpoint = QStringToStdString(mPortalContentLineEdit ? mPortalContentLineEdit->text().trimmed() : QString());
    settings.username = QStringToStdString(mUsernameLineEdit ? mUsernameLineEdit->text() : QString());
    settings.password = QStringToStdString(mPasswordLineEdit ? mPasswordLineEdit->text() : QString());
    settings.httpReferer = QStringToStdString(mHttpRefererLineEdit ? mHttpRefererLineEdit->text().trimmed() : QString());

    if (mHeaderTableWidget)
    {
        const int numRows = mHeaderTableWidget->rowCount();
        for (int rowIndex = 0; rowIndex < numRows; rowIndex++)
        {
            const QTableWidgetItem* keyItem = mHeaderTableWidget->item(rowIndex, 0);
            const QTableWidgetItem* valueItem = mHeaderTableWidget->item(rowIndex, 1);
            const QString key = keyItem ? keyItem->text().trimmed() : QString();
            const QString value = valueItem ? valueItem->text() : QString();

            if (key.isEmpty() && value.trimmed().isEmpty())
            {
                continue;
            }

            settings.httpCustomHeaders.push_back(std::make_pair(QStringToStdString(key), QStringToStdString(value)));
        }
    }

    return settings;
}


void QArcGISRestConnectionDialog::SetConnectionNameExistsChecker(const ConnectionNameExistsChecker& checker)
{
    mConnectionNameExistsChecker = checker;
}

void QArcGISRestConnectionDialog::Clear()
{
    if (!mNameLineEdit || !mHeaderTableWidget)
    {
        return;
    }

    mNameLineEdit->clear();
    mUrlLineEdit->clear();
    mPrefixLineEdit->clear();
    mPortalCommunityLineEdit->clear();
    mPortalContentLineEdit->clear();
    mUsernameLineEdit->clear();
    mPasswordLineEdit->clear();
    mHttpRefererLineEdit->clear();

    SetPasswordVisible(false);

    mHeaderTableWidget->blockSignals(true);
    mHeaderTableWidget->setRowCount(0);
    AddHeaderRow(QString(), QString(), false);
    mHeaderTableWidget->blockSignals(false);

    OnAdvancedToggled(true);
    mAdvancedButton->setChecked(true);

    UpdateOkButtonState();
    UpdateRemoveHeaderButtonState();
}

bool QArcGISRestConnectionDialog::ValidateInput(QString* errorMessage) const
{
    if (errorMessage)
    {
        errorMessage->clear();
    }

    const QString name = mNameLineEdit ? mNameLineEdit->text().trimmed() : QString();
    if (name.isEmpty())
    {
        if (errorMessage)
        {
            *errorMessage = QString::fromUtf8("名称不能为空。");
        }
        return false;
    }

    const QString url = mUrlLineEdit ? mUrlLineEdit->text().trimmed() : QString();
    if (!IsValidHttpUrl(url, false))
    {
        if (errorMessage)
        {
            *errorMessage = QString::fromUtf8("URL 必须是有效的 http 或 https 地址。");
        }
        return false;
    }

    const QString prefix = mPrefixLineEdit ? mPrefixLineEdit->text().trimmed() : QString();
    if (!IsValidHttpUrl(prefix, true))
    {
        if (errorMessage)
        {
            *errorMessage = QString::fromUtf8("Prefix 必须为空，或为有效的 http/https 地址。");
        }
        return false;
    }

    const QString portalCommunityEndpoint = mPortalCommunityLineEdit ? mPortalCommunityLineEdit->text().trimmed() : QString();
    if (!IsValidHttpUrl(portalCommunityEndpoint, true))
    {
        if (errorMessage)
        {
            *errorMessage = QString::fromUtf8("共享终端URL 必须为空，或为有效的 http/https 地址。");
        }
        return false;
    }

    const QString portalContentEndpoint = mPortalContentLineEdit ? mPortalContentLineEdit->text().trimmed() : QString();
    if (!IsValidHttpUrl(portalContentEndpoint, true))
    {
        if (errorMessage)
        {
            *errorMessage = QString::fromUtf8("内容终端URL 必须为空，或为有效的 http/https 地址。");
        }
        return false;
    }

    const QString httpReferer = mHttpRefererLineEdit ? mHttpRefererLineEdit->text().trimmed() : QString();
    if (ContainsLineBreak(httpReferer))
    {
        if (errorMessage)
        {
            *errorMessage = QString::fromUtf8("HTTP 来源不能包含换行符。");
        }
        return false;
    }

    if (mHeaderTableWidget)
    {
        const int numRows = mHeaderTableWidget->rowCount();
        for (int rowIndex = 0; rowIndex < numRows; rowIndex++)
        {
            const QTableWidgetItem* keyItem = mHeaderTableWidget->item(rowIndex, 0);
            const QTableWidgetItem* valueItem = mHeaderTableWidget->item(rowIndex, 1);
            const QString key = keyItem ? keyItem->text().trimmed() : QString();
            const QString value = valueItem ? valueItem->text() : QString();

            if (key.isEmpty() && value.trimmed().isEmpty())
            {
                continue;
            }

            if (key.isEmpty())
            {
                if (errorMessage)
                {
                    *errorMessage = QString::fromUtf8("HTTP 头第 %1 行的键不能为空。").arg(rowIndex + 1);
                }
                return false;
            }

            if (key.contains(QLatin1Char(':')) || ContainsLineBreak(key))
            {
                if (errorMessage)
                {
                    *errorMessage = QString::fromUtf8("HTTP 头第 %1 行的键不能包含冒号或换行符。").arg(rowIndex + 1);
                }
                return false;
            }

            if (ContainsLineBreak(value))
            {
                if (errorMessage)
                {
                    *errorMessage = QString::fromUtf8("HTTP 头第 %1 行的值不能包含换行符。").arg(rowIndex + 1);
                }
                return false;
            }
        }
    }

    return true;
}

bool QArcGISRestConnectionDialog::EditSettings(QWidget* parent, ArcGISRestConnectionSettings& settings)
{
    QArcGISRestConnectionDialog dialog(parent);
    dialog.SetSettings(settings);
    if (dialog.exec() != QDialog::Accepted)
    {
        return false;
    }

    settings = dialog.GetSettings();
    return true;
}

void QArcGISRestConnectionDialog::accept()
{
    QString errorMessage;
    if (!ValidateInput(&errorMessage))
    {
        QMessageBox::warning(this, windowTitle(), errorMessage);
        return;
    }

    const QString connectionName = mNameLineEdit ? mNameLineEdit->text().trimmed() : QString();
    if (mConnectionNameExistsChecker && mConnectionNameExistsChecker(connectionName))
    {
        QMessageBox::warning(this, windowTitle(), QString::fromUtf8("链接名称已存在，请使用其它名称。"));

        if (mNameLineEdit)
        {
            mNameLineEdit->setFocus(Qt::OtherFocusReason);
            mNameLineEdit->selectAll();
        }
        return;
    }

    QDialog::accept();
}

void QArcGISRestConnectionDialog::OnTextChanged()
{
    UpdateOkButtonState();
}

void QArcGISRestConnectionDialog::OnAddHeaderButtonClicked()
{
    AddHeaderRow(QString(), QString(), true);
    UpdateRemoveHeaderButtonState();
}

void QArcGISRestConnectionDialog::OnRemoveHeaderButtonClicked()
{
    if (!mHeaderTableWidget)
    {
        return;
    }

    QSet<int> selectedRows;
    const QModelIndexList selectedIndexes = mHeaderTableWidget->selectionModel()->selectedIndexes();
    for (const QModelIndex& selectedIndex : selectedIndexes)
    {
        selectedRows.insert(selectedIndex.row());
    }

    QList<int> rows = selectedRows.values();
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int rowIndex : rows)
    {
        if (rowIndex >= 0 && rowIndex < mHeaderTableWidget->rowCount())
        {
            mHeaderTableWidget->removeRow(rowIndex);
        }
    }

    if (mHeaderTableWidget->rowCount() == 0)
    {
        AddHeaderRow(QString(), QString(), false);
    }

    UpdateRemoveHeaderButtonState();
}

void QArcGISRestConnectionDialog::OnHeaderSelectionChanged()
{
    UpdateRemoveHeaderButtonState();
}

void QArcGISRestConnectionDialog::OnAdvancedToggled(bool checked)
{
    if (mAdvancedContainer)
    {
        mAdvancedContainer->setVisible(checked);
    }

    if (mAdvancedButton)
    {
        mAdvancedButton->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
    }
}

void QArcGISRestConnectionDialog::OnPasswordVisibleActionTriggered()
{
    SetPasswordVisible(!mPasswordVisible);
}

void QArcGISRestConnectionDialog::InitializeUi()
{
    setWindowTitle(QString::fromUtf8("新建 ArcGIS REST Server 链接"));
    setModal(true);
    setSizeGripEnabled(true);
    resize(744, 836);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(13, 12, 13, 8);
    mainLayout->setSpacing(8);

    mainLayout->addWidget(CreateConnectionDetailsWidget());
    mainLayout->addWidget(CreatePortalDetailsWidget());
    mainLayout->addWidget(CreateAuthenticationWidget());
    mainLayout->addWidget(CreateHttpHeadersWidget(), 1);

    mButtonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mOkButton = mButtonBox->button(QDialogButtonBox::Ok);

    if (mOkButton)
    {
        mOkButton->setText(QString::fromUtf8("确定"));
    }
    if (mButtonBox->button(QDialogButtonBox::Cancel))
    {
        mButtonBox->button(QDialogButtonBox::Cancel)->setText(QString::fromUtf8("取消"));
    }

    connect(mButtonBox, &QDialogButtonBox::accepted, this, &QArcGISRestConnectionDialog::accept);
    connect(mButtonBox, &QDialogButtonBox::rejected, this, &QArcGISRestConnectionDialog::reject);

    mainLayout->addWidget(mButtonBox);

    setTabOrder(mNameLineEdit, mUrlLineEdit);
    setTabOrder(mUrlLineEdit, mPrefixLineEdit);
    setTabOrder(mPrefixLineEdit, mPortalCommunityLineEdit);
    setTabOrder(mPortalCommunityLineEdit, mPortalContentLineEdit);
    setTabOrder(mPortalContentLineEdit, mUsernameLineEdit);
    setTabOrder(mUsernameLineEdit, mPasswordLineEdit);
    setTabOrder(mPasswordLineEdit, mHttpRefererLineEdit);
    setTabOrder(mHttpRefererLineEdit, mHeaderTableWidget);
}

QWidget* QArcGISRestConnectionDialog::CreateConnectionDetailsWidget()
{
    QGroupBox* groupBox = new BoldTitleGroupBox(QString::fromUtf8("链接详情"), this);
    QFormLayout* formLayout = new QFormLayout(groupBox);
    formLayout->setContentsMargins(17, 16, 13, 13);
    formLayout->setHorizontalSpacing(10);
    formLayout->setVerticalSpacing(8);
    formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    mNameLineEdit = new QLineEdit(groupBox);
    mUrlLineEdit = new QLineEdit(groupBox);
    mPrefixLineEdit = new QLineEdit(groupBox);
    mPrefixLineEdit->setPlaceholderText(QString::fromUtf8("https://mysite.com/proxy.jsp?"));

    SetFixedFieldHeight(mNameLineEdit);
    SetFixedFieldHeight(mUrlLineEdit);
    SetFixedFieldHeight(mPrefixLineEdit);

    connect(mNameLineEdit, &QLineEdit::textChanged, this, &QArcGISRestConnectionDialog::OnTextChanged);
    connect(mUrlLineEdit, &QLineEdit::textChanged, this, &QArcGISRestConnectionDialog::OnTextChanged);
    connect(mPrefixLineEdit, &QLineEdit::textChanged, this, &QArcGISRestConnectionDialog::OnTextChanged);

    formLayout->addRow(QString::fromUtf8("名称"), mNameLineEdit);
    formLayout->addRow(QString::fromUtf8("URL"), mUrlLineEdit);
    formLayout->addRow(QString::fromUtf8("Prefix"), mPrefixLineEdit);

    return groupBox;
}

QWidget* QArcGISRestConnectionDialog::CreatePortalDetailsWidget()
{
    QGroupBox* groupBox = new BoldTitleGroupBox(QString::fromUtf8("ArcGIS Portal 详细信息"), this);
    QFormLayout* formLayout = new QFormLayout(groupBox);
    formLayout->setContentsMargins(17, 16, 13, 13);
    formLayout->setHorizontalSpacing(10);
    formLayout->setVerticalSpacing(8);
    formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    mPortalCommunityLineEdit = new QLineEdit(groupBox);
    mPortalContentLineEdit = new QLineEdit(groupBox);
    mPortalCommunityLineEdit->setPlaceholderText(QString::fromUtf8("https://mysite.com/portal/sharing/rest/community/"));
    mPortalContentLineEdit->setPlaceholderText(QString::fromUtf8("https://mysite.com/portal/sharing/rest/content/"));

    SetFixedFieldHeight(mPortalCommunityLineEdit);
    SetFixedFieldHeight(mPortalContentLineEdit);

    connect(mPortalCommunityLineEdit, &QLineEdit::textChanged, this, &QArcGISRestConnectionDialog::OnTextChanged);
    connect(mPortalContentLineEdit, &QLineEdit::textChanged, this, &QArcGISRestConnectionDialog::OnTextChanged);

    formLayout->addRow(QString::fromUtf8("共享终端URL"), mPortalCommunityLineEdit);
    formLayout->addRow(QString::fromUtf8("内容终端URL"), mPortalContentLineEdit);

    return groupBox;
}

QWidget* QArcGISRestConnectionDialog::CreateAuthenticationWidget()
{
    QGroupBox* groupBox = new BoldTitleGroupBox(QString::fromUtf8("认证"), this);
    QFormLayout* formLayout = new QFormLayout(groupBox);
    formLayout->setContentsMargins(17, 16, 13, 13);
    formLayout->setHorizontalSpacing(10);
    formLayout->setVerticalSpacing(8);
    formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    mUsernameLineEdit = new QLineEdit(groupBox);
    mPasswordLineEdit = new QLineEdit(groupBox);
    mPasswordLineEdit->setPlaceholderText(QString::fromUtf8("可选"));
    mPasswordLineEdit->setEchoMode(QLineEdit::Password);
    mPasswordVisibleAction = mPasswordLineEdit->addAction(CreateEyeIcon(false), QLineEdit::TrailingPosition);

    SetFixedFieldHeight(mUsernameLineEdit);
    SetFixedFieldHeight(mPasswordLineEdit);

    connect(mPasswordVisibleAction, &QAction::triggered, this, &QArcGISRestConnectionDialog::OnPasswordVisibleActionTriggered);

    formLayout->addRow(QString::fromUtf8("用户名(&U)"), mUsernameLineEdit);
    formLayout->addRow(QString::fromUtf8("密码(&D)"), mPasswordLineEdit);

    return groupBox;
}

QWidget* QArcGISRestConnectionDialog::CreateHttpHeadersWidget()
{
    QGroupBox* groupBox = new BoldTitleGroupBox(QString::fromUtf8("HTTP 头文件"), this);
    QVBoxLayout* groupLayout = new QVBoxLayout(groupBox);
    groupLayout->setContentsMargins(13, 16, 13, 13);
    groupLayout->setSpacing(8);

    QWidget* topWidget = new QWidget(groupBox);
    QVBoxLayout* topLayout = new QVBoxLayout(topWidget);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(6);

    QHBoxLayout* refererLayout = new QHBoxLayout();
    refererLayout->setContentsMargins(0, 0, 0, 0);
    refererLayout->setSpacing(10);

    QLabel* refererLabel = new QLabel(QString::fromUtf8("来源"), topWidget);
    refererLabel->setFixedWidth(30);
    mHttpRefererLineEdit = new QLineEdit(topWidget);
    SetFixedFieldHeight(mHttpRefererLineEdit);
    connect(mHttpRefererLineEdit, &QLineEdit::textChanged, this, &QArcGISRestConnectionDialog::OnTextChanged);

    refererLayout->addWidget(refererLabel);
    refererLayout->addWidget(mHttpRefererLineEdit, 1);
    topLayout->addLayout(refererLayout);

    mAdvancedButton = new QToolButton(topWidget);
    mAdvancedButton->setText(QString::fromUtf8("高级"));
    mAdvancedButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mAdvancedButton->setAutoRaise(true);
    mAdvancedButton->setCheckable(true);
    mAdvancedButton->setChecked(true);
    mAdvancedButton->setArrowType(Qt::DownArrow);
    mAdvancedButton->setMinimumHeight(26);
    mAdvancedButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(mAdvancedButton, &QToolButton::toggled, this, &QArcGISRestConnectionDialog::OnAdvancedToggled);
    topLayout->addWidget(mAdvancedButton, 0, Qt::AlignLeft);

    groupLayout->addWidget(topWidget, 0, Qt::AlignTop);

    mAdvancedContainer = new QWidget(groupBox);
    mAdvancedContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QHBoxLayout* advancedLayout = new QHBoxLayout(mAdvancedContainer);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    advancedLayout->setSpacing(5);

    mHeaderTableWidget = new QTableWidget(mAdvancedContainer);
    mHeaderTableWidget->setColumnCount(2);
    mHeaderTableWidget->setHorizontalHeaderLabels(QStringList() << QString::fromUtf8("密钥") << QString::fromUtf8("值 (raw)"));
    mHeaderTableWidget->horizontalHeader()->setStretchLastSection(true);
    mHeaderTableWidget->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    mHeaderTableWidget->horizontalHeader()->resizeSection(0, 120);
    mHeaderTableWidget->verticalHeader()->setVisible(false);
    mHeaderTableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    mHeaderTableWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mHeaderTableWidget->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    mHeaderTableWidget->setAlternatingRowColors(false);
    mHeaderTableWidget->setMinimumHeight(190);

    connect(mHeaderTableWidget, &QTableWidget::cellChanged, this, &QArcGISRestConnectionDialog::OnTextChanged);
    connect(mHeaderTableWidget->selectionModel(), &QItemSelectionModel::selectionChanged, this, &QArcGISRestConnectionDialog::OnHeaderSelectionChanged);

    QVBoxLayout* headerButtonLayout = new QVBoxLayout();
    headerButtonLayout->setContentsMargins(0, 0, 0, 0);
    headerButtonLayout->setSpacing(6);

    mAddHeaderButton = new QPushButton(mAdvancedContainer);
    mAddHeaderButton->setIcon(CreatePlusIcon());
    mAddHeaderButton->setIconSize(QSize(22, 22));
    mAddHeaderButton->setFixedSize(30, 30);
    mAddHeaderButton->setToolTip(QString::fromUtf8("添加 HTTP 头"));

    mRemoveHeaderButton = new QPushButton(mAdvancedContainer);
    mRemoveHeaderButton->setIcon(CreateMinusIcon());
    mRemoveHeaderButton->setIconSize(QSize(22, 22));
    mRemoveHeaderButton->setFixedSize(30, 30);
    mRemoveHeaderButton->setToolTip(QString::fromUtf8("删除选中的 HTTP 头"));

    connect(mAddHeaderButton, &QPushButton::clicked, this, &QArcGISRestConnectionDialog::OnAddHeaderButtonClicked);
    connect(mRemoveHeaderButton, &QPushButton::clicked, this, &QArcGISRestConnectionDialog::OnRemoveHeaderButtonClicked);

    headerButtonLayout->addWidget(mAddHeaderButton);
    headerButtonLayout->addWidget(mRemoveHeaderButton);
    headerButtonLayout->addStretch(1);

    advancedLayout->addWidget(mHeaderTableWidget, 1);
    advancedLayout->addLayout(headerButtonLayout);

    groupLayout->addWidget(mAdvancedContainer, 1);
    return groupBox;
}

void QArcGISRestConnectionDialog::AddHeaderRow(const QString& key, const QString& value, bool startEditing)
{
    if (!mHeaderTableWidget)
    {
        return;
    }

    const int rowIndex = mHeaderTableWidget->rowCount();
    mHeaderTableWidget->insertRow(rowIndex);

    QTableWidgetItem* keyItem = new QTableWidgetItem(key);
    QTableWidgetItem* valueItem = new QTableWidgetItem(value);
    keyItem->setFlags(keyItem->flags() | Qt::ItemIsEditable);
    valueItem->setFlags(valueItem->flags() | Qt::ItemIsEditable);

    mHeaderTableWidget->setItem(rowIndex, 0, keyItem);
    mHeaderTableWidget->setItem(rowIndex, 1, valueItem);
    if (startEditing)
    {
        mHeaderTableWidget->setCurrentCell(rowIndex, 0);
        mHeaderTableWidget->editItem(keyItem);
    }
}

void QArcGISRestConnectionDialog::UpdateOkButtonState()
{
    if (!mOkButton)
    {
        return;
    }

    const QString name = mNameLineEdit ? mNameLineEdit->text().trimmed() : QString();
    const QString url = mUrlLineEdit ? mUrlLineEdit->text().trimmed() : QString();
    mOkButton->setEnabled(!name.isEmpty() && IsValidHttpUrl(url, false));
}

void QArcGISRestConnectionDialog::UpdateRemoveHeaderButtonState()
{
    if (!mRemoveHeaderButton || !mHeaderTableWidget || !mHeaderTableWidget->selectionModel())
    {
        return;
    }

    mRemoveHeaderButton->setEnabled(mHeaderTableWidget->selectionModel()->hasSelection());
}

void QArcGISRestConnectionDialog::SetPasswordVisible(bool visible)
{
    mPasswordVisible = visible;
    if (mPasswordLineEdit)
    {
        mPasswordLineEdit->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
    }

    if (mPasswordVisibleAction)
    {
        mPasswordVisibleAction->setIcon(CreateEyeIcon(!visible));
        mPasswordVisibleAction->setToolTip(visible ? QString::fromUtf8("隐藏密码") : QString::fromUtf8("显示密码"));
    }
}

QString QArcGISRestConnectionDialog::StdStringToQString(const std::string& textUtf8)
{
    return QString::fromUtf8(textUtf8.c_str(), static_cast<int>(textUtf8.size()));
}

std::string QArcGISRestConnectionDialog::QStringToStdString(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
}

bool QArcGISRestConnectionDialog::IsValidHttpUrl(const QString& text, bool allowEmpty)
{
    const QString trimmedText = text.trimmed();
    if (trimmedText.isEmpty())
    {
        return allowEmpty;
    }

    const QUrl url(trimmedText, QUrl::StrictMode);
    if (!url.isValid())
    {
        return false;
    }

    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
    {
        return false;
    }

    return !url.host().isEmpty();
}

bool QArcGISRestConnectionDialog::ContainsLineBreak(const QString& text)
{
    return text.contains(QLatin1Char('\r')) || text.contains(QLatin1Char('\n'));
}
