#pragma once

#include <QDockWidget>
#include <QIcon>
#include <QHash>
#include <QMetaType>
#include <QPointer>
#include <QString>

#include <vector>

#include "QServiceBrowserPanel.h"

class QEvent;
class QListWidget;
class QListWidgetItem;
class QModelIndex;
class QPoint;
class QToolButton;
class QServiceBrowserPanel;
class QMainCanvas;
class GB_Rectangle;
class GeoBoundingBox;

/**
 * @brief 图层管理面板中的动作类型。
 */
enum class LayerManagerActionType
{
	None = 0,
	LayerImported,
	LayerVisibilityChanged,
	ShowAllLayers,
	HideAllLayers,
	ZoomToSelectedLayers,
	ZoomToAllLayers,
	RemoveSelectedLayers,
	RemoveAllLayers,
	LayerRenamed,
	LayerCloned,
	LayerOrderChanged
};

/**
 * @brief 图层管理面板中一行图层的快照信息。
 */
struct LayerManagerLayerInfo
{
	QString layerUid = "";      // 图层管理面板内部使用的唯一 ID。
	QString displayName = "";   // 当前列表中显示的图层名称。
	int row = -1;                // 当前行号。注意：row 始终是列表中的可视行号，0 表示最顶层。
	bool visible = true;         // 当前显示状态。

	LayerImportRequestInfo importRequest; // 服务浏览面板发出的原始导入请求。
};

/**
 * @brief 图层管理面板状态变化或动作触发时发出的完整快照。
 *
 * 顺序约定：
 * - 面板列表从上到下表示从顶层到底层，row=0 为最顶层；
 * - allLayers / affectedLayers 等图层数组统一按列表语义组织：开头是最顶层，最后是最底层。
 */
struct LayerManagerChangeInfo
{
	LayerManagerActionType actionType = LayerManagerActionType::None;
	std::vector<LayerManagerLayerInfo> allLayers;      // 变化后的全部图层状态，顺序为顶层 -> 底层。
	std::vector<LayerManagerLayerInfo> affectedLayers; // 本次动作直接影响的图层，顺序为顶层 -> 底层。
	std::vector<int> affectedRows;                     // 本次动作直接影响的行号；移除动作中为移除前行号，顺序与 affectedLayers 对齐。
	bool targetVisible = true;                         // Show/Hide/勾选变化的目标可见状态。
};

Q_DECLARE_METATYPE(LayerManagerActionType)
Q_DECLARE_METATYPE(LayerManagerLayerInfo)
Q_DECLARE_METATYPE(std::vector<LayerManagerLayerInfo>)
Q_DECLARE_METATYPE(LayerManagerChangeInfo)

/**
 * @brief 图层管理 Dock 面板。
 *
 * 该面板负责展示当前已导入图层的列表，并维护每个图层的显示状态。
 * 显示、隐藏、导入、移除、重命名、克隆、排序等状态变化仍通过 LayersChanged() 向外部业务层发送快照；
 * 缩放至单个图层 / 选中图层 / 全部图层属于纯视图定位操作，会直接计算图层范围并设置已绑定主画布的当前视口，
 * 不再额外发出 LayersChanged() 信号。
 */
	class QLayerManagerPanel : public QDockWidget
{
	Q_OBJECT

public:
	explicit QLayerManagerPanel(QWidget* parent = nullptr);
	virtual ~QLayerManagerPanel() override;

	static Qt::DockWidgetArea GetDefaultDockWidgetArea();

	QListWidget* GetListWidget() const;

	bool BindServiceBrowserPanel(QServiceBrowserPanel* serviceBrowserPanel);
	bool BindMainCanvas(QMainCanvas* canvas);
	QMainCanvas* GetMainCanvas() const;

	int GetLayerCount() const;
	std::vector<LayerManagerLayerInfo> GetLayers() const;
	std::vector<LayerManagerLayerInfo> GetSelectedLayers() const;

	bool AddLayer(const LayerImportRequestInfo& request);
	bool RemoveLayerByUid(const QString& layerUid);
	bool RemoveLayersByUid(const std::vector<QString>& layerUids);
	void ClearLayers();

	bool SetLayerVisible(const QString& layerUid, bool visible);

public slots:
	void HandleLayerImportRequested(const LayerImportRequestInfo& request);

signals:
	void LayersChanged(const LayerManagerChangeInfo& changeInfo);

private slots:
	void OnItemChanged(QListWidgetItem* item);
	void OnItemSelectionChanged();
	void OnShowAllLayersClicked();
	void OnHideAllLayersClicked();
	void OnZoomToSelectedLayersClicked();
	void OnZoomToAllLayersClicked();
	void OnRemoveSelectedLayersClicked();
	void OnRemoveAllLayersClicked();
	void OnListContextMenuRequested(const QPoint& position);
	void OnLayerRowsMoved(const QModelIndex& sourceParent, int sourceStart, int sourceEnd, const QModelIndex& destinationParent, int destinationRow);
	void OnLayerOrderPossiblyChanged();

private:
	enum LayerListItemRole
	{
		RoleLayerUid = Qt::UserRole + 1
	};

	virtual bool eventFilter(QObject* watched, QEvent* event) override;

	QToolButton* CreateButton(const QIcon& icon, const QString& accessibleText, const QString& toolTip, QWidget* parent) const;
	QIcon CreatePanelActionIcon(LayerManagerActionType actionType) const;
	QListWidgetItem* CreateItemFromLayerInfo(const LayerManagerLayerInfo& layerInfo) const;

	LayerManagerLayerInfo CreateLayerInfoFromRequest(const LayerImportRequestInfo& request);
	QString CreateUniqueLayerUid(const LayerImportRequestInfo& request) const;
	QString CreateUniqueLayerUidFromBase(const QString& baseUid) const;
	QString CreateDisplayNameFromRequest(const LayerImportRequestInfo& request) const;
	QIcon IconForNodeType(ArcGISRestServiceTreeNode::NodeType nodeType) const;

	int FindLayerIndexByUid(const QString& layerUid) const;
	QListWidgetItem* FindItemByLayerUid(const QString& layerUid) const;
	QListWidgetItem* ItemAtRow(int row) const;
	void RebuildLayerUidIndex();
	void RefreshItemRows();
	bool SynchronizeLayersFromListWidget();

	std::vector<LayerManagerLayerInfo> BuildLayerSnapshot() const;
	std::vector<LayerManagerLayerInfo> BuildSelectedLayerSnapshot() const;
	std::vector<int> BuildAllVisualRowsTopToBottom() const;
	LayerManagerChangeInfo CreateChangeInfo(LayerManagerActionType actionType,
		const std::vector<LayerManagerLayerInfo>& affectedLayers,
		const std::vector<int>& affectedRows,
		bool targetVisible) const;
	void EmitChange(LayerManagerActionType actionType,
		const std::vector<LayerManagerLayerInfo>& affectedLayers = std::vector<LayerManagerLayerInfo>(),
		const std::vector<int>& affectedRows = std::vector<int>(),
		bool targetVisible = true);

	bool ZoomToLayers(const std::vector<LayerManagerLayerInfo>& targetLayers);
	bool TryCalculateLayersExtentInCanvasCrs(const std::vector<LayerManagerLayerInfo>& targetLayers, GB_Rectangle& outExtent) const;
	bool TryCalculateLayerExtentInCanvasCrs(const LayerManagerLayerInfo& layerInfo, GB_Rectangle& outExtent) const;
	bool TryGetLayerBoundingBox(const LayerManagerLayerInfo& layerInfo, GeoBoundingBox& outBox) const;
	bool TryTransformBoundingBoxToCanvasCrs(const GeoBoundingBox& sourceBox, GB_Rectangle& outExtent) const;

	void SetAllLayersVisible(bool visible);
	bool RemoveRows(const std::vector<int>& rowsToRemove, LayerManagerActionType actionType);
	bool ConfirmRemoveLayers(int layerCount, LayerManagerActionType actionType) const;
	bool RenameLayerAtRow(int row);
	bool CloneLayerAtRow(int row);
	bool ZoomToLayerAtRow(int row);
	bool RemoveLayerAtRow(int row);
	void SelectOnlyRow(int row);
	void UpdateSelectionDependentButtons();

	QListWidget* listWidget = nullptr;
	QToolButton* showAllButton = nullptr;
	QToolButton* hideAllButton = nullptr;
	QToolButton* zoomSelectedButton = nullptr;
	QToolButton* zoomAllButton = nullptr;
	QToolButton* removeSelectedButton = nullptr;
	QToolButton* removeAllButton = nullptr;
	QPointer<QMainCanvas> mainCanvas;

	// 内部顺序始终与列表可视顺序一致：index 0 为最顶层，最后一个为最底层。
	std::vector<LayerManagerLayerInfo> layers;
	QHash<QString, int> layerIndexByUid;
	bool isUpdatingItems = false;
	int nextLayerSerial = 1;
};
