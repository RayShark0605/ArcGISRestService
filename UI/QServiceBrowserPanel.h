#pragma once

#include <QDockWidget>
#include <QHash>
#include <QIcon>
#include <QPoint>
#include <QString>

#include <functional>
#include <vector>

#include "ArcGISRestCapabilities.h"

class QMenu;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

struct ServiceBrowserNodeInfo
{
	QString uid = "";
	QString text = "";
	QString url = "";
	QString detailText = "";
	int nodeType = 0;
	bool canDrag = false;
	bool canShowContextMenu = true;
	bool isArcGISRestNode = false;
	bool isCategoryNode = false;
	bool hasLazyChildren = false;
};

enum class ArcGISRestConnectionLoadMode
{
	AddNew,
	RefreshExisting,
	EditExisting
};

class QServiceBrowserPanel : public QDockWidget
{
	Q_OBJECT

private:
	class ArcGISRestNodeExpandThread;
	class ArcGISRestConnectionLoadThread;

public:
	typedef std::function<void(QMenu* menu, const ServiceBrowserNodeInfo& nodeInfo)> ContextMenuBuilder;

	explicit QServiceBrowserPanel(QWidget* parent = nullptr);
	virtual ~QServiceBrowserPanel() override;

	static QString GetArcGISRestCategoryUid();
	static Qt::DockWidgetArea GetDefaultDockWidgetArea();

	QTreeWidget* GetTreeWidget() const;
	QTextEdit* GetDetailsTextEdit() const;

	bool HasArcGISRestConnectionDisplayName(const QString& displayName, const QString& ignoredUid = QString()) const;

	void ClearArcGISRestServices();
	void SetArcGISRestRootChildren(const std::vector<ArcGISRestServiceTreeNode>& nodes);
	bool AddArcGISRestServiceNode(const ArcGISRestServiceTreeNode& node, bool recursive = true);
	bool UpdateArcGISRestServiceNode(const ArcGISRestServiceTreeNode& node, bool recursive = true);
	bool RemoveNode(const QString& uid);

	bool SetNodeDraggable(const QString& uid, bool enabled);
	bool SetNodeContextMenuEnabled(const QString& uid, bool enabled);
	bool SetNodeDetailText(const QString& uid, const QString& detailText);
	bool SelectNode(const QString& uid);
	bool ExpandNodeByUid(const QString& uid);

	bool GetSelectedNodeInfo(ServiceBrowserNodeInfo& outNodeInfo) const;

	void SetDefaultContextMenuEnabled(bool enabled);
	bool IsDefaultContextMenuEnabled() const;

	void SetContextMenuBuilder(const ContextMenuBuilder& builder);

	void SetShowLazyExpandableIndicators(bool enabled);
	bool IsShowLazyExpandableIndicatorsEnabled() const;

signals:
	void CurrentNodeChanged(const QString& nodeUid, const QString& url, const QString& text, int nodeType);
	void NodeActivated(const QString& nodeUid, const QString& url, const QString& text, int nodeType);
	void NodeDoubleClicked(const QString& nodeUid, const QString& url, const QString& text, int nodeType);
	void ArcGISRestNodeExpandRequested(const QString& nodeUid, const QString& url, const QString& text, int nodeType);
	void NodeRefreshRequested(const QString& nodeUid, const QString& url, const QString& text, int nodeType);

private slots:
	void OnCurrentItemChanged(QTreeWidgetItem* currentItem, QTreeWidgetItem* previousItem);
	void OnItemActivated(QTreeWidgetItem* item, int column);
	void OnItemDoubleClicked(QTreeWidgetItem* item, int column);
	void OnItemExpanded(QTreeWidgetItem* item);
	void OnCustomContextMenuRequested(const QPoint& position);
	void OnNewArcGISRestConnectionRequested();
	void OnLoadingAnimationTimerTimeout();

private:
	QTreeWidgetItem* CreateArcGISRestCategoryItem() const;
	QTreeWidgetItem* CreateTreeItemFromArcGISRestNode(const ArcGISRestServiceTreeNode& node, bool recursive);
	QTreeWidgetItem* CreateLazyPlaceholderItem() const;
	QTreeWidgetItem* CreateLoadingPlaceholderItem() const;

	bool AddArcGISRestServiceNodeInternal(const ArcGISRestServiceTreeNode& node, QTreeWidgetItem* explicitParentItem, bool recursive);
	bool UpdateArcGISRestServiceNodeInternal(const ArcGISRestServiceTreeNode& node, bool recursive);

	QTreeWidgetItem* FindItemByUid(const QString& uid) const;
	QString GetCurrentSelectableNodeUid() const;
	void RestoreCurrentSelectableNodeByUid(const QString& uid, const QString& fallbackUid = QString());
	void RegisterItemRecursively(QTreeWidgetItem* item);
	void UnregisterItemRecursively(QTreeWidgetItem* item);
	void RemoveChildrenAndUnregister(QTreeWidgetItem* item);

	void SetItemLoadingState(QTreeWidgetItem* item, bool enabled);
	void RestoreArcGISRestNodeLazyExpansionState(const ArcGISRestServiceTreeNode& node, const QString& statusText);
	bool HasLoadingItems() const;
	bool HasLoadingItemsRecursively(const QTreeWidgetItem* item) const;
	void UpdateLoadingAnimationTimerState();
	void UpdateLoadingAnimationFrame();
	void UpdateLoadingAnimationFrameRecursively(QTreeWidgetItem* item, const QIcon& loadingIcon);
	QIcon CreateLoadingIcon(int frameIndex) const;

	ServiceBrowserNodeInfo GetNodeInfo(QTreeWidgetItem* item) const;
	void UpdateDetailsForItem(QTreeWidgetItem* item);
	void EmitNodeSignal(void (QServiceBrowserPanel::* signalFunc)(const QString&, const QString&, const QString&, int), QTreeWidgetItem* item);

	void ApplyDragFlag(QTreeWidgetItem* item, bool enabled) const;
	void ApplyStandardItemFlags(QTreeWidgetItem* item, bool canDrag) const;
	void AddDefaultContextMenuActions(QMenu* menu, const ServiceBrowserNodeInfo& nodeInfo, QTreeWidgetItem* item);
	void AddArcGISRestConnectionContextMenuActions(QMenu* menu, const ServiceBrowserNodeInfo& nodeInfo, QTreeWidgetItem* item);
	void AddArcGISRestChildNodeContextMenuActions(QMenu* menu, const ServiceBrowserNodeInfo& nodeInfo, QTreeWidgetItem* item);
	bool IsArcGISRestCategoryNode(const ServiceBrowserNodeInfo& nodeInfo) const;
	bool IsArcGISRestConnectionRootItem(const QTreeWidgetItem* item) const;
	void EnsureArcGISRestCategoryItem();

	const QTreeWidgetItem* FindArcGISRestConnectionRootItem(const QTreeWidgetItem* item) const;
	bool GetArcGISRestConnectionSettingsForItem(const QTreeWidgetItem* item, ArcGISRestConnectionSettings& outSettings) const;
	bool GetArcGISRestConnectionSettingsForNodeItem(const QTreeWidgetItem* item, ArcGISRestConnectionSettings& outSettings) const;
	void SetArcGISRestConnectionSettings(const QString& uid, const ArcGISRestConnectionSettings& settings);
	void RemoveArcGISRestConnectionSettings(const QString& uid);
	void RemoveArcGISRestConnectionSettingsRecursively(const QTreeWidgetItem* item);
	void MoveArcGISRestConnectionSettings(const QString& oldUid, const QString& newUid);
	quint64 BeginArcGISRestConnectionLoad(const QString& uid);
	bool IsArcGISRestConnectionLoadCurrent(const QString& uid, quint64 loadToken) const;
	void EndArcGISRestConnectionLoad(const QString& uid, quint64 loadToken);
	void CancelArcGISRestConnectionLoadsRecursively(const QTreeWidgetItem* item);

	void RefreshArcGISRestConnection(QTreeWidgetItem* item);
	void RefreshArcGISRestChildNode(QTreeWidgetItem* item);
	void ImportArcGISRestNode(QTreeWidgetItem* item);
	void EditArcGISRestConnection(QTreeWidgetItem* item);
	void DeleteArcGISRestConnection(QTreeWidgetItem* item);
	void StartArcGISRestConnectionLoad(const QString& sourceUid, const ArcGISRestConnectionSettings& settings, const ArcGISRestServiceTreeNode& connectionNode, ArcGISRestConnectionLoadMode mode);
	bool UpdateArcGISRestConnectionDisplayNameOnly(QTreeWidgetItem* item, const ArcGISRestConnectionSettings& settings);

	QString NormalizeUid(const ArcGISRestServiceTreeNode& node) const;
	QString CreateDetailTextForArcGISRestNode(const ArcGISRestServiceTreeNode& node, bool canDrag, bool hasLazyChildren) const;
	QString CreateDetailTextFromNodeInfo(const ServiceBrowserNodeInfo& nodeInfo) const;

	bool CanDragArcGISRestNodeByDefault(ArcGISRestServiceTreeNode::NodeType nodeType) const;
	QString NodeTypeToDisplayText(int nodeType) const;
	QIcon IconForArcGISRestNodeType(ArcGISRestServiceTreeNode::NodeType nodeType) const;

private:
	QTreeWidget* treeWidget = nullptr;
	QTextEdit* detailsTextEdit = nullptr;
	QTreeWidgetItem* arcGISRestCategoryItem = nullptr;
	QHash<QString, QTreeWidgetItem*> itemByUid;
	QHash<QString, ArcGISRestConnectionSettings> arcGISRestConnectionSettingsByUid;
	QHash<QString, quint64> arcGISRestConnectionLoadTokenByUid;
	quint64 nextArcGISRestConnectionLoadToken = 1;
	ContextMenuBuilder contextMenuBuilder;
	bool defaultContextMenuEnabled = true;
	bool showLazyExpandableIndicators = true;
	QTimer* loadingAnimationTimer = nullptr;
	int loadingAnimationFrame = 0;
};
