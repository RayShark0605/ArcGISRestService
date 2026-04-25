#ifndef QARCGISRESTCONNECTIONDIALOG_H
#define QARCGISRESTCONNECTIONDIALOG_H

#include <QDialog>

#include <functional>

#include "ArcGISRestCapabilities.h"

class QAction;
class QDialogButtonBox;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QToolButton;
class QWidget;

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

class QArcGISRestConnectionDialog : public QDialog
{
    Q_OBJECT

public:
    typedef std::function<bool(const QString& connectionName)> ConnectionNameExistsChecker;

    explicit QArcGISRestConnectionDialog(QWidget* parent = nullptr);
    ~QArcGISRestConnectionDialog() override;

    void SetSettings(const ArcGISRestConnectionSettings& settings);
    ArcGISRestConnectionSettings GetSettings() const;

    void SetConnectionNameExistsChecker(const ConnectionNameExistsChecker& checker);

    void Clear();

    bool ValidateInput(QString* errorMessage = nullptr) const;

    static bool EditSettings(QWidget* parent, ArcGISRestConnectionSettings& settings);

signals:
    void HelpRequested();

protected:
    void accept() override;

private slots:
    void OnTextChanged();
    void OnAddHeaderButtonClicked();
    void OnRemoveHeaderButtonClicked();
    void OnHeaderSelectionChanged();
    void OnAdvancedToggled(bool checked);
    void OnPasswordVisibleActionTriggered();

private:
    void InitializeUi();
    QWidget* CreateConnectionDetailsWidget();
    QWidget* CreatePortalDetailsWidget();
    QWidget* CreateAuthenticationWidget();
    QWidget* CreateHttpHeadersWidget();

    void AddHeaderRow(const QString& key, const QString& value, bool startEditing);
    void UpdateOkButtonState();
    void UpdateRemoveHeaderButtonState();
    void SetPasswordVisible(bool visible);

    static QString StdStringToQString(const std::string& textUtf8);
    static std::string QStringToStdString(const QString& text);
    static bool IsValidHttpUrl(const QString& text, bool allowEmpty);
    static bool ContainsLineBreak(const QString& text);

private:
    QLineEdit* mNameLineEdit = nullptr;
    QLineEdit* mUrlLineEdit = nullptr;
    QLineEdit* mPrefixLineEdit = nullptr;

    QLineEdit* mPortalCommunityLineEdit = nullptr;
    QLineEdit* mPortalContentLineEdit = nullptr;

    QLineEdit* mUsernameLineEdit = nullptr;
    QLineEdit* mPasswordLineEdit = nullptr;
    QAction* mPasswordVisibleAction = nullptr;
    bool mPasswordVisible = false;

    QLineEdit* mHttpRefererLineEdit = nullptr;
    QToolButton* mAdvancedButton = nullptr;
    QWidget* mAdvancedContainer = nullptr;
    QTableWidget* mHeaderTableWidget = nullptr;
    QPushButton* mAddHeaderButton = nullptr;
    QPushButton* mRemoveHeaderButton = nullptr;

    QDialogButtonBox* mButtonBox = nullptr;
    QPushButton* mOkButton = nullptr;
    ConnectionNameExistsChecker mConnectionNameExistsChecker;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
