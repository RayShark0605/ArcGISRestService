#include "QLayerManagerPanel.h"

#include <QAbstractItemView>
#include <QColor>
#include <QHash>
#include <QHBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace
{
	QString ToQString(const std::string& textUtf8)
	{
		return QString::fromUtf8(textUtf8.c_str());
	}

	void DrawLayerStack(QPainter& painter, const QRectF& bounds)
	{
		const qreal layerHeight = bounds.height() / 5.0;
		const qreal gap = layerHeight * 0.55;
		for (int i = 0; i < 3; i++)
		{
			const qreal top = bounds.top() + i * (layerHeight + gap);
			painter.drawRoundedRect(QRectF(bounds.left(), top, bounds.width(), layerHeight), 1.0, 1.0);
		}
	}

	void DrawEye(QPainter& painter, const QRectF& bounds)
	{
		QPainterPath eyePath;
		eyePath.moveTo(bounds.left(), bounds.center().y());
		eyePath.cubicTo(bounds.left() + bounds.width() * 0.25, bounds.top(), bounds.left() + bounds.width() * 0.75, bounds.top(), bounds.right(), bounds.center().y());
		eyePath.cubicTo(bounds.left() + bounds.width() * 0.75, bounds.bottom(), bounds.left() + bounds.width() * 0.25, bounds.bottom(), bounds.left(), bounds.center().y());
		painter.drawPath(eyePath);
		painter.drawEllipse(QRectF(bounds.center().x() - 2.0, bounds.center().y() - 2.0, 4.0, 4.0));
	}

	void DrawMagnifier(QPainter& painter, const QRectF& circleBounds)
	{
		painter.drawEllipse(circleBounds);
		painter.drawLine(circleBounds.right() - 1.0, circleBounds.bottom() - 1.0, circleBounds.right() + 7.0, circleBounds.bottom() + 7.0);
	}

	void DrawCross(QPainter& painter, const QRectF& bounds)
	{
		painter.drawLine(bounds.topLeft(), bounds.bottomRight());
		painter.drawLine(bounds.bottomLeft(), bounds.topRight());
	}

	QPixmap CreateLayerManagerActionPixmap(LayerManagerActionType actionType, const QColor& color)
	{
		QPixmap pixmap(32, 32);
		pixmap.fill(Qt::transparent);

		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setPen(QPen(color, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		painter.setBrush(Qt::NoBrush);

		switch (actionType)
		{
		case LayerManagerActionType::ShowAllLayers:
			DrawLayerStack(painter, QRectF(4, 7, 14, 18));
			DrawEye(painter, QRectF(17, 10, 12, 9));
			break;
		case LayerManagerActionType::HideAllLayers:
			DrawLayerStack(painter, QRectF(4, 7, 14, 18));
			DrawEye(painter, QRectF(17, 10, 12, 9));
			painter.drawLine(QPointF(19, 24), QPointF(29, 6));
			break;
		case LayerManagerActionType::ZoomToSelectedLayers:
			DrawMagnifier(painter, QRectF(5, 5, 15, 15));
			painter.drawRoundedRect(QRectF(9, 9, 7, 7), 1.0, 1.0);
			break;
		case LayerManagerActionType::ZoomToAllLayers:
			DrawMagnifier(painter, QRectF(5, 5, 15, 15));
			DrawLayerStack(painter, QRectF(10, 9, 7, 9));
			break;
		case LayerManagerActionType::RemoveSelectedLayers:
			painter.drawRoundedRect(QRectF(5, 6, 14, 14), 1.2, 1.2);
			DrawCross(painter, QRectF(20, 9, 8, 8));
			break;
		case LayerManagerActionType::RemoveAllLayers:
			DrawLayerStack(painter, QRectF(4, 7, 14, 18));
			DrawCross(painter, QRectF(20, 9, 8, 8));
			break;
		default:
			painter.drawEllipse(QRectF(8, 8, 16, 16));
			break;
		}

		return pixmap;
	}

	QIcon CreateLayerManagerActionIcon(LayerManagerActionType actionType)
	{
		QIcon icon;
		icon.addPixmap(CreateLayerManagerActionPixmap(actionType, QColor(45, 45, 45)), QIcon::Normal, QIcon::Off);
		icon.addPixmap(CreateLayerManagerActionPixmap(actionType, QColor(145, 145, 145)), QIcon::Disabled, QIcon::Off);
		return icon;
	}
}

QLayerManagerPanel::QLayerManagerPanel(QWidget* parent) : QDockWidget(QStringLiteral("图层管理"), parent)
{
	qRegisterMetaType<LayerImportRequestInfo>("LayerImportRequestInfo");
	qRegisterMetaType<LayerImportRequestInfo>("ArcGISRestLayerImportRequest");
	qRegisterMetaType<LayerManagerActionType>("LayerManagerActionType");
	qRegisterMetaType<LayerManagerLayerInfo>("LayerManagerLayerInfo");
	qRegisterMetaType<std::vector<LayerManagerLayerInfo>>("std::vector<LayerManagerLayerInfo>");
	qRegisterMetaType<LayerManagerChangeInfo>("LayerManagerChangeInfo");

	setObjectName(QStringLiteral("QLayerManagerPanel"));
	setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
	setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

	QWidget* contentWidget = new QWidget(this);
	QVBoxLayout* mainLayout = new QVBoxLayout(contentWidget);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(0);

	QWidget* buttonWidget = new QWidget(contentWidget);
	QHBoxLayout* buttonLayout = new QHBoxLayout(buttonWidget);
	buttonLayout->setContentsMargins(4, 4, 4, 4);
	buttonLayout->setSpacing(2);

	showAllButton = CreateButton(CreatePanelActionIcon(LayerManagerActionType::ShowAllLayers), QStringLiteral("全部显示"), QStringLiteral("将所有图层设置为显示状态。"), buttonWidget);
	hideAllButton = CreateButton(CreatePanelActionIcon(LayerManagerActionType::HideAllLayers), QStringLiteral("全部隐藏"), QStringLiteral("将所有图层设置为隐藏状态。"), buttonWidget);
	zoomSelectedButton = CreateButton(CreatePanelActionIcon(LayerManagerActionType::ZoomToSelectedLayers), QStringLiteral("缩放至选中图层"), QStringLiteral("请求缩放至当前选中的图层。"), buttonWidget);
	zoomAllButton = CreateButton(CreatePanelActionIcon(LayerManagerActionType::ZoomToAllLayers), QStringLiteral("缩放至全部图层"), QStringLiteral("请求缩放至全部图层。"), buttonWidget);
	removeSelectedButton = CreateButton(CreatePanelActionIcon(LayerManagerActionType::RemoveSelectedLayers), QStringLiteral("移除选中图层"), QStringLiteral("移除当前选中的图层。"), buttonWidget);
	removeAllButton = CreateButton(CreatePanelActionIcon(LayerManagerActionType::RemoveAllLayers), QStringLiteral("移除全部图层"), QStringLiteral("移除当前面板中的全部图层。"), buttonWidget);

	buttonLayout->addWidget(showAllButton);
	buttonLayout->addWidget(hideAllButton);
	buttonLayout->addWidget(zoomSelectedButton);
	buttonLayout->addWidget(zoomAllButton);
	buttonLayout->addWidget(removeSelectedButton);
	buttonLayout->addWidget(removeAllButton);
	buttonLayout->addStretch(1);
	mainLayout->addWidget(buttonWidget);

	listWidget = new QListWidget(contentWidget);
	listWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
	listWidget->setAlternatingRowColors(true);
	listWidget->setIconSize(QSize(20, 20));
	listWidget->setUniformItemSizes(true);
	listWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	listWidget->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	mainLayout->addWidget(listWidget, 1);

	setWidget(contentWidget);

	zoomSelectedButton->setEnabled(false);
	removeSelectedButton->setEnabled(false);

	connect(showAllButton, &QToolButton::clicked, this, &QLayerManagerPanel::OnShowAllLayersClicked);
	connect(hideAllButton, &QToolButton::clicked, this, &QLayerManagerPanel::OnHideAllLayersClicked);
	connect(zoomSelectedButton, &QToolButton::clicked, this, &QLayerManagerPanel::OnZoomToSelectedLayersClicked);
	connect(zoomAllButton, &QToolButton::clicked, this, &QLayerManagerPanel::OnZoomToAllLayersClicked);
	connect(removeSelectedButton, &QToolButton::clicked, this, &QLayerManagerPanel::OnRemoveSelectedLayersClicked);
	connect(removeAllButton, &QToolButton::clicked, this, &QLayerManagerPanel::OnRemoveAllLayersClicked);
	connect(listWidget, &QListWidget::itemChanged, this, &QLayerManagerPanel::OnItemChanged);
	connect(listWidget, &QListWidget::itemSelectionChanged, this, &QLayerManagerPanel::OnItemSelectionChanged);

	UpdateSelectionDependentButtons();
}

QLayerManagerPanel::~QLayerManagerPanel()
{
	layers.clear();
	layerIndexByUid.clear();
}

Qt::DockWidgetArea QLayerManagerPanel::GetDefaultDockWidgetArea()
{
	return Qt::RightDockWidgetArea;
}

QListWidget* QLayerManagerPanel::GetListWidget() const
{
	return listWidget;
}

bool QLayerManagerPanel::BindServiceBrowserPanel(QServiceBrowserPanel* serviceBrowserPanel)
{
	if (!serviceBrowserPanel)
	{
		return false;
	}

	const QMetaObject::Connection connection = connect(serviceBrowserPanel,
		&QServiceBrowserPanel::LayerImportRequested,
		this,
		&QLayerManagerPanel::HandleLayerImportRequested,
		Qt::UniqueConnection);
	return static_cast<bool>(connection);
}

int QLayerManagerPanel::GetLayerCount() const
{
	return static_cast<int>(layers.size());
}

std::vector<LayerManagerLayerInfo> QLayerManagerPanel::GetLayers() const
{
	return BuildLayerSnapshot();
}

std::vector<LayerManagerLayerInfo> QLayerManagerPanel::GetSelectedLayers() const
{
	return BuildSelectedLayerSnapshot();
}

bool QLayerManagerPanel::AddLayer(const LayerImportRequestInfo& request)
{
	if (!request.IsValid() || !listWidget)
	{
		return false;
	}

	LayerManagerLayerInfo layerInfo = CreateLayerInfoFromRequest(request);
	if (layerInfo.layerUid.isEmpty() || layerInfo.displayName.trimmed().isEmpty())
	{
		return false;
	}

	layerInfo.row = static_cast<int>(layers.size());

	QListWidgetItem* item = CreateItemFromLayerInfo(layerInfo);
	if (!item)
	{
		return false;
	}

	isUpdatingItems = true;
	listWidget->addItem(item);
	isUpdatingItems = false;

	layers.push_back(layerInfo);
	nextLayerSerial++;
	RefreshItemRows();
	UpdateSelectionDependentButtons();

	std::vector<LayerManagerLayerInfo> affectedLayers;
	affectedLayers.push_back(layerInfo);
	std::vector<int> affectedRows;
	affectedRows.push_back(layerInfo.row);
	EmitChange(LayerManagerActionType::LayerImported, affectedLayers, affectedRows, true);
	return true;
}

bool QLayerManagerPanel::RemoveLayerByUid(const QString& layerUid)
{
	if (layerUid.isEmpty())
	{
		return false;
	}

	const int layerIndex = FindLayerIndexByUid(layerUid);
	if (layerIndex < 0)
	{
		return false;
	}

	std::vector<int> rowsToRemove;
	rowsToRemove.push_back(layerIndex);
	RemoveRows(rowsToRemove, LayerManagerActionType::RemoveSelectedLayers);
	return true;
}

bool QLayerManagerPanel::RemoveLayersByUid(const std::vector<QString>& layerUids)
{
	if (layerUids.empty())
	{
		return false;
	}

	std::vector<int> rowsToRemove;
	rowsToRemove.reserve(layerUids.size());
	for (const QString& layerUid : layerUids)
	{
		const int layerIndex = FindLayerIndexByUid(layerUid);
		if (layerIndex >= 0)
		{
			rowsToRemove.push_back(layerIndex);
		}
	}

	if (rowsToRemove.empty())
	{
		return false;
	}

	RemoveRows(rowsToRemove, LayerManagerActionType::RemoveSelectedLayers);
	return true;
}

void QLayerManagerPanel::ClearLayers()
{
	if (!listWidget && layers.empty())
	{
		return;
	}

	const std::vector<LayerManagerLayerInfo> affectedLayers = BuildLayerSnapshot();
	std::vector<int> affectedRows;
	affectedRows.reserve(affectedLayers.size());
	for (const LayerManagerLayerInfo& layerInfo : affectedLayers)
	{
		affectedRows.push_back(layerInfo.row);
	}

	isUpdatingItems = true;
	if (listWidget)
	{
		listWidget->clear();
	}
	layers.clear();
	layerIndexByUid.clear();
	isUpdatingItems = false;

	UpdateSelectionDependentButtons();
	EmitChange(LayerManagerActionType::RemoveAllLayers, affectedLayers, affectedRows, true);
}

bool QLayerManagerPanel::SetLayerVisible(const QString& layerUid, bool visible)
{
	const int layerIndex = FindLayerIndexByUid(layerUid);
	if (layerIndex < 0)
	{
		return false;
	}

	if (layers[layerIndex].visible == visible)
	{
		return true;
	}

	layers[layerIndex].visible = visible;
	QListWidgetItem* item = ItemAtRow(layerIndex);
	if (item)
	{
		isUpdatingItems = true;
		item->setCheckState(visible ? Qt::Checked : Qt::Unchecked);
		isUpdatingItems = false;
	}

	std::vector<LayerManagerLayerInfo> affectedLayers;
	affectedLayers.push_back(layers[layerIndex]);
	std::vector<int> affectedRows;
	affectedRows.push_back(layerIndex);
	EmitChange(LayerManagerActionType::LayerVisibilityChanged, affectedLayers, affectedRows, visible);
	return true;
}

void QLayerManagerPanel::HandleLayerImportRequested(const LayerImportRequestInfo& request)
{
	AddLayer(request);
}

void QLayerManagerPanel::OnItemChanged(QListWidgetItem* item)
{
	if (isUpdatingItems || !item)
	{
		return;
	}

	const QString layerUid = item->data(RoleLayerUid).toString();
	const int layerIndex = FindLayerIndexByUid(layerUid);
	if (layerIndex < 0)
	{
		return;
	}

	const bool visible = (item->checkState() == Qt::Checked);
	if (layers[layerIndex].visible == visible)
	{
		return;
	}

	layers[layerIndex].visible = visible;

	std::vector<LayerManagerLayerInfo> affectedLayers;
	affectedLayers.push_back(layers[layerIndex]);
	std::vector<int> affectedRows;
	affectedRows.push_back(layerIndex);
	EmitChange(LayerManagerActionType::LayerVisibilityChanged, affectedLayers, affectedRows, visible);
}

void QLayerManagerPanel::OnItemSelectionChanged()
{
	UpdateSelectionDependentButtons();
}

void QLayerManagerPanel::OnShowAllLayersClicked()
{
	SetAllLayersVisible(true);
}

void QLayerManagerPanel::OnHideAllLayersClicked()
{
	SetAllLayersVisible(false);
}

void QLayerManagerPanel::OnZoomToSelectedLayersClicked()
{
	const std::vector<LayerManagerLayerInfo> selectedLayers = BuildSelectedLayerSnapshot();
	std::vector<int> selectedRows;
	selectedRows.reserve(selectedLayers.size());
	for (const LayerManagerLayerInfo& layerInfo : selectedLayers)
	{
		selectedRows.push_back(layerInfo.row);
	}
	EmitChange(LayerManagerActionType::ZoomToSelectedLayers, selectedLayers, selectedRows, true);
}

void QLayerManagerPanel::OnZoomToAllLayersClicked()
{
	const std::vector<LayerManagerLayerInfo> allLayers = BuildLayerSnapshot();
	std::vector<int> affectedRows;
	affectedRows.reserve(allLayers.size());
	for (const LayerManagerLayerInfo& layerInfo : allLayers)
	{
		affectedRows.push_back(layerInfo.row);
	}
	EmitChange(LayerManagerActionType::ZoomToAllLayers, allLayers, affectedRows, true);
}

void QLayerManagerPanel::OnRemoveSelectedLayersClicked()
{
	if (!listWidget)
	{
		return;
	}

	const QList<QListWidgetItem*> selectedItems = listWidget->selectedItems();
	std::vector<int> rowsToRemove;
	rowsToRemove.reserve(selectedItems.size());
	for (QListWidgetItem* item : selectedItems)
	{
		const int row = listWidget->row(item);
		if (row >= 0)
		{
			rowsToRemove.push_back(row);
		}
	}

	RemoveRows(rowsToRemove, LayerManagerActionType::RemoveSelectedLayers);
}

void QLayerManagerPanel::OnRemoveAllLayersClicked()
{
	ClearLayers();
}

QToolButton* QLayerManagerPanel::CreateButton(const QIcon& icon, const QString& accessibleText, const QString& toolTip, QWidget* parent) const
{
	QToolButton* button = new QToolButton(parent);
	button->setText(QString());
	button->setIcon(icon);
	button->setIconSize(QSize(20, 20));
	button->setToolTip(toolTip);
	button->setStatusTip(accessibleText);
	button->setAccessibleName(accessibleText);
	button->setAutoRaise(true);
	button->setToolButtonStyle(Qt::ToolButtonIconOnly);
	button->setMinimumSize(QSize(26, 26));
	button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	return button;
}

QIcon QLayerManagerPanel::CreatePanelActionIcon(LayerManagerActionType actionType) const
{
	return CreateLayerManagerActionIcon(actionType);
}

QListWidgetItem* QLayerManagerPanel::CreateItemFromLayerInfo(const LayerManagerLayerInfo& layerInfo) const
{
	QListWidgetItem* item = new QListWidgetItem(IconForNodeType(layerInfo.importRequest.nodeType), layerInfo.displayName);
	item->setData(RoleLayerUid, layerInfo.layerUid);
	item->setToolTip(layerInfo.displayName);
	item->setCheckState(layerInfo.visible ? Qt::Checked : Qt::Unchecked);
	item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
	return item;
}

LayerManagerLayerInfo QLayerManagerPanel::CreateLayerInfoFromRequest(const LayerImportRequestInfo& request)
{
	LayerManagerLayerInfo layerInfo;
	layerInfo.layerUid = CreateUniqueLayerUid(request);
	layerInfo.displayName = CreateDisplayNameFromRequest(request);
	layerInfo.row = -1;
	layerInfo.visible = true;
	layerInfo.importRequest = request;
	return layerInfo;
}

QString QLayerManagerPanel::CreateUniqueLayerUid(const LayerImportRequestInfo& request) const
{
	QString baseUid = ToQString(request.nodeUid).trimmed();
	if (baseUid.isEmpty())
	{
		baseUid = ToQString(request.serviceUrl).trimmed();
	}
	if (baseUid.isEmpty())
	{
		baseUid = QStringLiteral("layer");
	}

	QString candidate = baseUid;
	if (FindLayerIndexByUid(candidate) < 0)
	{
		return candidate;
	}

	int serial = nextLayerSerial;
	while (serial < 1000000000)
	{
		candidate = QStringLiteral("%1_%2").arg(baseUid).arg(serial);
		if (FindLayerIndexByUid(candidate) < 0)
		{
			return candidate;
		}
		serial++;
	}

	return QStringLiteral("%1_%2").arg(baseUid).arg(layers.size() + 1);
}

QString QLayerManagerPanel::CreateDisplayNameFromRequest(const LayerImportRequestInfo& request) const
{
	QString displayName = ToQString(request.nodeText).trimmed();
	if (!displayName.isEmpty())
	{
		return displayName;
	}

	displayName = ToQString(request.layerId).trimmed();
	if (!displayName.isEmpty())
	{
		return QStringLiteral("图层 %1").arg(displayName);
	}

	displayName = ToQString(request.nodeUrl).trimmed();
	if (!displayName.isEmpty())
	{
		return displayName;
	}

	displayName = ToQString(request.serviceUrl).trimmed();
	if (!displayName.isEmpty())
	{
		return displayName;
	}

	return QStringLiteral("未命名图层");
}

QIcon QLayerManagerPanel::IconForNodeType(ArcGISRestServiceTreeNode::NodeType nodeType) const
{
	switch (nodeType)
	{
	case ArcGISRestServiceTreeNode::NodeType::Root:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Root_32.ico");
	case ArcGISRestServiceTreeNode::NodeType::Folder:
		return style()->standardIcon(QStyle::SP_DirClosedIcon);
	case ArcGISRestServiceTreeNode::NodeType::MapService:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_MapService_32.ico");
	case ArcGISRestServiceTreeNode::NodeType::ImageService:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_ImageService_32.ico");
	case ArcGISRestServiceTreeNode::NodeType::FeatureService:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_FeatureService_32.ico");
	case ArcGISRestServiceTreeNode::NodeType::Table:
		return style()->standardIcon(QStyle::SP_FileDialogDetailedView);
	case ArcGISRestServiceTreeNode::NodeType::AllLayers:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_AllLayers_64.ico");
	case ArcGISRestServiceTreeNode::NodeType::UnknownVectorLayer:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Unknown_Layer_64.ico");
	case ArcGISRestServiceTreeNode::NodeType::PointVectorLayer:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Point_Layer_64.ico");
	case ArcGISRestServiceTreeNode::NodeType::LineVectorLayer:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Line_Layer_64.ico");
	case ArcGISRestServiceTreeNode::NodeType::PolygonVectorLayer:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Polygon_Layer_64.ico");
	case ArcGISRestServiceTreeNode::NodeType::RasterLayer:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Raster_Layer_64.ico");
	case ArcGISRestServiceTreeNode::NodeType::Unknown:
	default:
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Unknown_Layer_64.ico");
	}
}

int QLayerManagerPanel::FindLayerIndexByUid(const QString& layerUid) const
{
	if (layerUid.isEmpty())
	{
		return -1;
	}

	const auto iter = layerIndexByUid.constFind(layerUid);
	if (iter == layerIndexByUid.constEnd())
	{
		return -1;
	}

	return iter.value();
}

QListWidgetItem* QLayerManagerPanel::FindItemByLayerUid(const QString& layerUid) const
{
	if (!listWidget || layerUid.isEmpty())
	{
		return nullptr;
	}

	for (int row = 0; row < listWidget->count(); row++)
	{
		QListWidgetItem* item = listWidget->item(row);
		if (item && item->data(RoleLayerUid).toString() == layerUid)
		{
			return item;
		}
	}
	return nullptr;
}

QListWidgetItem* QLayerManagerPanel::ItemAtRow(int row) const
{
	if (!listWidget || row < 0 || row >= listWidget->count())
	{
		return nullptr;
	}
	return listWidget->item(row);
}

void QLayerManagerPanel::RebuildLayerUidIndex()
{
	layerIndexByUid.clear();
	for (size_t layerIndex = 0; layerIndex < layers.size(); layerIndex++)
	{
		layers[layerIndex].row = static_cast<int>(layerIndex);
		layerIndexByUid.insert(layers[layerIndex].layerUid, static_cast<int>(layerIndex));
	}
}

void QLayerManagerPanel::RefreshItemRows()
{
	RebuildLayerUidIndex();
	if (!listWidget)
	{
		return;
	}

	const int count = std::min(listWidget->count(), static_cast<int>(layers.size()));
	for (int row = 0; row < count; row++)
	{
		QListWidgetItem* item = listWidget->item(row);
		if (!item)
		{
			continue;
		}

		item->setData(RoleLayerUid, layers[row].layerUid);
		item->setText(layers[row].displayName);
		item->setToolTip(layers[row].displayName);
		item->setIcon(IconForNodeType(layers[row].importRequest.nodeType));
		item->setCheckState(layers[row].visible ? Qt::Checked : Qt::Unchecked);
	}
}

std::vector<LayerManagerLayerInfo> QLayerManagerPanel::BuildLayerSnapshot() const
{
	std::vector<LayerManagerLayerInfo> snapshot = layers;
	for (size_t layerIndex = 0; layerIndex < snapshot.size(); layerIndex++)
	{
		snapshot[layerIndex].row = static_cast<int>(layerIndex);
	}
	return snapshot;
}

std::vector<LayerManagerLayerInfo> QLayerManagerPanel::BuildSelectedLayerSnapshot() const
{
	std::vector<LayerManagerLayerInfo> selectedLayers;
	if (!listWidget)
	{
		return selectedLayers;
	}

	const QList<QListWidgetItem*> selectedItems = listWidget->selectedItems();
	selectedLayers.reserve(selectedItems.size());
	for (QListWidgetItem* item : selectedItems)
	{
		if (!item)
		{
			continue;
		}

		const QString layerUid = item->data(RoleLayerUid).toString();
		const int layerIndex = FindLayerIndexByUid(layerUid);
		if (layerIndex >= 0)
		{
			LayerManagerLayerInfo layerInfo = layers[layerIndex];
			layerInfo.row = layerIndex;
			selectedLayers.push_back(layerInfo);
		}
	}

	std::sort(selectedLayers.begin(), selectedLayers.end(), [](const LayerManagerLayerInfo& first, const LayerManagerLayerInfo& second) {
		return first.row < second.row;
		});
	return selectedLayers;
}

LayerManagerChangeInfo QLayerManagerPanel::CreateChangeInfo(LayerManagerActionType actionType,
	const std::vector<LayerManagerLayerInfo>& affectedLayers,
	const std::vector<int>& affectedRows,
	bool targetVisible) const
{
	LayerManagerChangeInfo changeInfo;
	changeInfo.actionType = actionType;
	changeInfo.allLayers = BuildLayerSnapshot();
	changeInfo.affectedLayers = affectedLayers;
	changeInfo.affectedRows = affectedRows;
	changeInfo.targetVisible = targetVisible;
	return changeInfo;
}

void QLayerManagerPanel::EmitChange(LayerManagerActionType actionType,
	const std::vector<LayerManagerLayerInfo>& affectedLayers,
	const std::vector<int>& affectedRows,
	bool targetVisible)
{
	const LayerManagerChangeInfo changeInfo = CreateChangeInfo(actionType, affectedLayers, affectedRows, targetVisible);
	emit LayersChanged(changeInfo);
}

void QLayerManagerPanel::SetAllLayersVisible(bool visible)
{
	std::vector<LayerManagerLayerInfo> affectedLayers;
	std::vector<int> affectedRows;
	affectedLayers.reserve(layers.size());
	affectedRows.reserve(layers.size());

	isUpdatingItems = true;
	for (size_t layerIndex = 0; layerIndex < layers.size(); layerIndex++)
	{
		layers[layerIndex].visible = visible;
		layers[layerIndex].row = static_cast<int>(layerIndex);
		affectedLayers.push_back(layers[layerIndex]);
		affectedRows.push_back(static_cast<int>(layerIndex));

		QListWidgetItem* item = ItemAtRow(static_cast<int>(layerIndex));
		if (item)
		{
			item->setCheckState(visible ? Qt::Checked : Qt::Unchecked);
		}
	}
	isUpdatingItems = false;

	EmitChange(visible ? LayerManagerActionType::ShowAllLayers : LayerManagerActionType::HideAllLayers, affectedLayers, affectedRows, visible);
}

void QLayerManagerPanel::RemoveRows(const std::vector<int>& rowsToRemove, LayerManagerActionType actionType)
{
	if (!listWidget || rowsToRemove.empty())
	{
		return;
	}

	std::vector<int> uniqueRows = rowsToRemove;
	std::sort(uniqueRows.begin(), uniqueRows.end());
	uniqueRows.erase(std::unique(uniqueRows.begin(), uniqueRows.end()), uniqueRows.end());

	std::vector<LayerManagerLayerInfo> affectedLayers;
	std::vector<int> affectedRows;
	affectedLayers.reserve(uniqueRows.size());
	affectedRows.reserve(uniqueRows.size());
	for (int row : uniqueRows)
	{
		if (row < 0 || row >= static_cast<int>(layers.size()))
		{
			continue;
		}

		LayerManagerLayerInfo layerInfo = layers[static_cast<size_t>(row)];
		layerInfo.row = row;
		affectedLayers.push_back(layerInfo);
		affectedRows.push_back(row);
	}

	if (affectedLayers.empty())
	{
		return;
	}

	isUpdatingItems = true;
	for (std::vector<int>::reverse_iterator rowIter = uniqueRows.rbegin(); rowIter != uniqueRows.rend(); ++rowIter)
	{
		const int row = *rowIter;
		if (row < 0 || row >= static_cast<int>(layers.size()))
		{
			continue;
		}

		delete listWidget->takeItem(row);
		layers.erase(layers.begin() + row);
	}
	isUpdatingItems = false;

	RefreshItemRows();
	UpdateSelectionDependentButtons();
	EmitChange(actionType, affectedLayers, affectedRows, true);
}

void QLayerManagerPanel::UpdateSelectionDependentButtons()
{
	const bool hasSelection = listWidget && !listWidget->selectedItems().isEmpty();
	if (zoomSelectedButton)
	{
		zoomSelectedButton->setEnabled(hasSelection);
	}
	if (removeSelectedButton)
	{
		removeSelectedButton->setEnabled(hasSelection);
	}
}
