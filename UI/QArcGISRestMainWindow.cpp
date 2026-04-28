#include "QArcGISRestMainWindow.h"
#include "QMainCanvas.h"
#include "QServiceBrowserPanel.h"
#include "LayerRefresher.h"
#include "QLayerManagerPanel.h"
#include "QCrsManagerWidget.h"

#include "GeoBase/Geometry/GB_Point2d.h"
#include "GeoBase/Geometry/GB_Rectangle.h"
#include "GeoBoundingBox.h"
#include "GeoCrs.h"
#include "GeoCrsManager.h"
#include "GeoCrsTransform.h"

#include <QCheckBox>
#include <QColor>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStyle>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace
{
	QString FormatStatusCoordinate(double value, int decimalDigits = 4)
	{
		if (!std::isfinite(value))
		{
			return QStringLiteral("-");
		}

		const double zeroTolerance = std::pow(10.0, -static_cast<double>(std::max(1, decimalDigits))) * 0.5;
		if (std::abs(value) < zeroTolerance)
		{
			value = 0.0;
		}

		return QString::number(value, 'f', decimalDigits);
	}

	QString FormatMousePositionStatusText(const GB_Point2d& position, bool hasPosition)
	{
		if (!hasPosition || !position.IsValid())
		{
			return QStringLiteral("鼠标: (-, -)");
		}

		return QStringLiteral("鼠标: (%1, %2)")
			.arg(FormatStatusCoordinate(position.x))
			.arg(FormatStatusCoordinate(position.y));
	}

	QString FormatLongitudeLatitudeMousePositionStatusText(const GB_Point2d& position, bool hasPosition)
	{
		if (!hasPosition || !position.IsValid())
		{
			return QStringLiteral("鼠标(经纬度): (-, -)");
		}

		return QStringLiteral("鼠标(经纬度): (%1, %2)")
			.arg(FormatStatusCoordinate(position.x, 6))
			.arg(FormatStatusCoordinate(position.y, 6));
	}

	QString FormatViewExtentStatusText(const GB_Rectangle& extent)
	{
		if (!extent.IsValid())
		{
			return QStringLiteral("范围: 无效");
		}

		return QStringLiteral("范围: [%1, %2, %3, %4]")
			.arg(FormatStatusCoordinate(extent.minX))
			.arg(FormatStatusCoordinate(extent.minY))
			.arg(FormatStatusCoordinate(extent.maxX))
			.arg(FormatStatusCoordinate(extent.maxY));
	}

	QString FormatCrsStatusText(const QString& crsDisplayText)
	{
		return QStringLiteral("坐标系: %1").arg(crsDisplayText);
	}

	std::string BuildWgs84WktUtf8()
	{
		const std::shared_ptr<const GeoCrs> wgs84 = GeoCrsManager::GetWgs84();
		if (wgs84 && wgs84->IsValid())
		{
			const std::string wktUtf8 = wgs84->ExportToWktUtf8(GeoCrs::WktFormat::Wkt2_2018, false);
			if (!wktUtf8.empty())
			{
				return wktUtf8;
			}
		}

		return GeoCrsManager::UserInputToWktUtf8("EPSG:4326");
	}

	const std::string& GetWgs84WktUtf8()
	{
		static std::string wgs84WktUtf8;
		if (wgs84WktUtf8.empty())
		{
			wgs84WktUtf8 = BuildWgs84WktUtf8();
		}

		return wgs84WktUtf8;
	}

	bool TryTransformPositionToLongitudeLatitude(const QMainCanvas* canvas, const GB_Point2d& position, GB_Point2d& outLongitudeLatitudePosition)
	{
		outLongitudeLatitudePosition = GB_Point2d();
		if (!canvas || !position.IsValid())
		{
			return false;
		}

		const std::string& sourceWktUtf8 = canvas->GetCrsWkt();
		if (sourceWktUtf8.empty())
		{
			return false;
		}

		const std::string& targetWktUtf8 = GetWgs84WktUtf8();
		if (targetWktUtf8.empty())
		{
			return false;
		}

		GeoBoundingBox lonLatValidArea;
		GeoBoundingBox selfValidArea;
		if (GeoCrsManager::TryGetValidAreasCached(sourceWktUtf8, lonLatValidArea, selfValidArea) && selfValidArea.rect.IsValid() && !selfValidArea.rect.IsContains(position))
		{
			return false;
		}

		try
		{
			return GeoCrsTransform::TransformPoint(sourceWktUtf8, targetWktUtf8, position, outLongitudeLatitudePosition) && outLongitudeLatitudePosition.IsValid();
		}
		catch (...)
		{
			outLongitudeLatitudePosition = GB_Point2d();
			return false;
		}
	}

	void InitializeStatusLabel(QLabel* label, Qt::Alignment alignment, QSizePolicy::Policy horizontalPolicy = QSizePolicy::Preferred)
	{
		if (!label)
		{
			return;
		}

		label->setAlignment(alignment | Qt::AlignVCenter);
		label->setTextInteractionFlags(Qt::TextSelectableByMouse);
		label->setSizePolicy(horizontalPolicy, QSizePolicy::Preferred);
	}

	void InitializeStatusCheckBox(QCheckBox* checkBox)
	{
		if (!checkBox)
		{
			return;
		}

		checkBox->setChecked(false);
		checkBox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	}

	void InitializeStatusButton(QPushButton* button)
	{
		if (!button)
		{
			return;
		}

		button->setFlat(true);
		button->setCursor(Qt::PointingHandCursor);
		button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	}

	void InitializeStatusToolButton(QToolButton* button, const QString& accessibleName)
	{
		if (!button)
		{
			return;
		}

		button->setAutoRaise(true);
		button->setCursor(Qt::PointingHandCursor);
		button->setToolButtonStyle(Qt::ToolButtonIconOnly);
		button->setIconSize(QSize(18, 18));
		button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
		button->setAccessibleName(accessibleName);
	}

	QIcon CreateZoomValidAreaIcon()
	{
		return QIcon(":/resources/Resources/ArcGIS_Rest_Service_Zoom_To_CRS_64.ico");
		/*QPixmap pixmap(18, 18);
		pixmap.fill(Qt::transparent);

		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing, true);

		const QColor extentColor(55, 110, 190);
		const QColor fillColor(55, 110, 190, 35);
		const QColor arrowColor(45, 45, 45);

		QPen extentPen(extentColor, 1.5);
		extentPen.setCapStyle(Qt::RoundCap);
		extentPen.setJoinStyle(Qt::RoundJoin);
		painter.setPen(extentPen);
		painter.setBrush(fillColor);
		painter.drawRoundedRect(QRectF(3.0, 3.0, 12.0, 10.0), 2.0, 2.0);

		QPen arrowPen(arrowColor, 1.5);
		arrowPen.setCapStyle(Qt::RoundCap);
		arrowPen.setJoinStyle(Qt::RoundJoin);
		painter.setPen(arrowPen);
		painter.setBrush(Qt::NoBrush);
		painter.drawLine(QPointF(10.0, 14.0), QPointF(15.0, 14.0));
		painter.drawLine(QPointF(14.0, 10.0), QPointF(14.0, 15.0));
		painter.drawLine(QPointF(11.0, 11.0), QPointF(14.0, 14.0));
		painter.drawLine(QPointF(14.0, 14.0), QPointF(11.5, 14.0));
		painter.drawLine(QPointF(14.0, 14.0), QPointF(14.0, 11.5));

		painter.end();
		return QIcon(pixmap);*/
	}

}

QArcGISRestMainWindow::QArcGISRestMainWindow(QWidget* parent) : QMainWindow(parent)
{
	InitializeUi();
	QCrsManagerWidget::InitializeSystemCrsRecordsAsync();

	LayerRefresher* refresher = LayerRefresher::GetInstance();
	if (refresher)
	{
		refresher->SetCanvasAndPanels(mainCanvas, serviceBrowserPanel, layerManagerPanel);
	}
}

void QArcGISRestMainWindow::InitializeUi()
{
	setObjectName(QStringLiteral("QArcGISRestMainWindow"));
	setWindowTitle(QStringLiteral("MapWeaver"));
	resize(1280, 800);

	setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);

	mainCanvas = new QMainCanvas(this);
	mainCanvas->setObjectName(QStringLiteral("mainCanvas"));
	setCentralWidget(mainCanvas);

	serviceBrowserPanel = new QServiceBrowserPanel(this);
	serviceBrowserPanel->setObjectName(QStringLiteral("serviceBrowserPanel"));
	serviceBrowserPanel->setMinimumWidth(260);
	serviceBrowserPanel->BindMainCanvas(mainCanvas);
	addDockWidget(QServiceBrowserPanel::GetDefaultDockWidgetArea(), serviceBrowserPanel);

	layerManagerPanel = new QLayerManagerPanel(this);
	layerManagerPanel->setObjectName(QStringLiteral("layerManagerPanel"));
	layerManagerPanel->setMinimumWidth(260);
	layerManagerPanel->BindServiceBrowserPanel(serviceBrowserPanel);
	layerManagerPanel->BindMainCanvas(mainCanvas);
	addDockWidget(QLayerManagerPanel::GetDefaultDockWidgetArea(), layerManagerPanel);

	InitializeStatusBar();

	const QIcon appIcon(":/resources/Resources/Main.ico");
	setWindowIcon(appIcon);
}

void QArcGISRestMainWindow::InitializeStatusBar()
{
	QStatusBar* mainStatusBar = statusBar();
	mainStatusBar->setObjectName(QStringLiteral("mainStatusBar"));
	mainStatusBar->setSizeGripEnabled(false);

	QWidget* statusContentWidget = new QWidget(mainStatusBar);
	statusContentWidget->setObjectName(QStringLiteral("statusContentWidget"));
	statusContentWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

	QGridLayout* statusLayout = new QGridLayout(statusContentWidget);
	statusLayout->setContentsMargins(6, 0, 6, 0);
	statusLayout->setHorizontalSpacing(12);
	statusLayout->setColumnStretch(0, 0);
	statusLayout->setColumnStretch(1, 1);
	statusLayout->setColumnStretch(2, 0);
	statusLayout->setColumnStretch(3, 1);
	statusLayout->setColumnStretch(4, 0);

	leftStatusWidget = new QWidget(statusContentWidget);
	leftStatusWidget->setObjectName(QStringLiteral("leftStatusWidget"));
	leftStatusWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	QHBoxLayout* leftStatusLayout = new QHBoxLayout(leftStatusWidget);
	leftStatusLayout->setContentsMargins(0, 0, 0, 0);
	leftStatusLayout->setSpacing(8);

	longitudeLatitudeStatusCheckBox = new QCheckBox(QStringLiteral("经纬度"), leftStatusWidget);
	longitudeLatitudeStatusCheckBox->setObjectName(QStringLiteral("longitudeLatitudeStatusCheckBox"));
	InitializeStatusCheckBox(longitudeLatitudeStatusCheckBox);
	leftStatusLayout->addWidget(longitudeLatitudeStatusCheckBox, 0, Qt::AlignLeft | Qt::AlignVCenter);

	mousePositionStatusLabel = new QLabel(leftStatusWidget);
	mousePositionStatusLabel->setObjectName(QStringLiteral("mousePositionStatusLabel"));
	InitializeStatusLabel(mousePositionStatusLabel, Qt::AlignLeft, QSizePolicy::Preferred);
	leftStatusLayout->addWidget(mousePositionStatusLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
	leftStatusLayout->addStretch(1);

	statusLayout->addWidget(leftStatusWidget, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);

	viewExtentStatusLabel = new QLabel(statusContentWidget);
	viewExtentStatusLabel->setObjectName(QStringLiteral("viewExtentStatusLabel"));
	InitializeStatusLabel(viewExtentStatusLabel, Qt::AlignCenter, QSizePolicy::Preferred);
	statusLayout->addWidget(viewExtentStatusLabel, 0, 2, Qt::AlignCenter);

	rightStatusWidget = new QWidget(statusContentWidget);
	rightStatusWidget->setObjectName(QStringLiteral("rightStatusWidget"));
	rightStatusWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	QHBoxLayout* rightStatusLayout = new QHBoxLayout(rightStatusWidget);
	rightStatusLayout->setContentsMargins(0, 0, 0, 0);
	rightStatusLayout->setSpacing(6);
	rightStatusLayout->addStretch(1);

	crsStatusButton = new QPushButton(rightStatusWidget);
	crsStatusButton->setObjectName(QStringLiteral("crsStatusButton"));
	InitializeStatusButton(crsStatusButton);
	rightStatusLayout->addWidget(crsStatusButton, 0, Qt::AlignRight | Qt::AlignVCenter);

	crsValidAreaVisibleButton = new QToolButton(rightStatusWidget);
	crsValidAreaVisibleButton->setObjectName(QStringLiteral("crsValidAreaVisibleButton"));
	crsValidAreaVisibleButton->setCheckable(true);
	crsValidAreaVisibleButton->setChecked(false);
	crsValidAreaVisibleButton->setAutoRaise(true);
	crsValidAreaVisibleButton->setCursor(Qt::PointingHandCursor);
	crsValidAreaVisibleButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
	crsValidAreaVisibleButton->setIcon(QIcon(":/resources/Resources/ArcGIS_Rest_Service_Show_Crs_BBOX_64.ico"));
	crsValidAreaVisibleButton->setIconSize(QSize(18, 18));
	crsValidAreaVisibleButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	crsValidAreaVisibleButton->setAccessibleName(QStringLiteral("显示坐标系适用区域"));
	rightStatusLayout->addWidget(crsValidAreaVisibleButton, 0, Qt::AlignRight | Qt::AlignVCenter);
	crsZoomValidAreaButton = new QToolButton(rightStatusWidget);
	crsZoomValidAreaButton->setObjectName(QStringLiteral("crsZoomValidAreaButton"));
	InitializeStatusToolButton(crsZoomValidAreaButton, QStringLiteral("缩放至坐标系适用范围"));
	crsZoomValidAreaButton->setIcon(CreateZoomValidAreaIcon());
	rightStatusLayout->addWidget(crsZoomValidAreaButton, 0, Qt::AlignRight | Qt::AlignVCenter);


	statusLayout->addWidget(rightStatusWidget, 0, 4, Qt::AlignRight | Qt::AlignVCenter);

	mainStatusBar->addWidget(statusContentWidget, 1);

	connect(longitudeLatitudeStatusCheckBox, &QCheckBox::stateChanged, this, &QArcGISRestMainWindow::OnLongitudeLatitudeCheckBoxStateChanged);
	connect(crsStatusButton, &QPushButton::clicked, this, [this]()
		{
			OnCrsStatusButtonClicked();
		});
	connect(crsValidAreaVisibleButton, &QToolButton::toggled, this, &QArcGISRestMainWindow::OnCrsValidAreaVisibleButtonToggled);
	connect(crsZoomValidAreaButton, &QToolButton::clicked, this, [this](bool)
		{
			OnCrsZoomValidAreaButtonClicked();
		});

	connect(mainCanvas, &QMainCanvas::MousePositionChanged, this, &QArcGISRestMainWindow::UpdateMousePositionStatus);
	connect(mainCanvas, &QMainCanvas::ViewExtentDisplayChanged, this, &QArcGISRestMainWindow::UpdateViewExtentStatus);
	connect(mainCanvas, &QMainCanvas::CrsDisplayTextChanged, this, &QArcGISRestMainWindow::UpdateCrsStatus);

	GB_Point2d mousePosition;
	UpdateMousePositionStatus(mousePosition, mainCanvas->TryGetCurrentMouseWorldPosition(mousePosition));
	UpdateViewExtentStatus(mainCanvas->GetCurrentViewExtent());
	UpdateCrsStatus(mainCanvas->GetCrsDisplayText());
	UpdateCrsValidAreaVisibleButtonTooltip();
	UpdateCrsZoomValidAreaButtonState();
	BalanceStatusSideWidgetWidths();
}

QArcGISRestMainWindow::~QArcGISRestMainWindow()
{
}

QMainCanvas* QArcGISRestMainWindow::GetCanvas() const
{
	return mainCanvas;
}

void QArcGISRestMainWindow::UpdateMousePositionStatus(const GB_Point2d& position, bool hasPosition)
{
	if (!mousePositionStatusLabel)
	{
		return;
	}

	if (longitudeLatitudeStatusCheckBox && longitudeLatitudeStatusCheckBox->isChecked())
	{
		GB_Point2d longitudeLatitudePosition;
		const bool hasLongitudeLatitudePosition = hasPosition && TryTransformPositionToLongitudeLatitude(mainCanvas, position, longitudeLatitudePosition);
		mousePositionStatusLabel->setText(FormatLongitudeLatitudeMousePositionStatusText(longitudeLatitudePosition, hasLongitudeLatitudePosition));

		if (!hasPosition || !position.IsValid())
		{
			mousePositionStatusLabel->setToolTip(QStringLiteral("当前没有可显示的鼠标位置。"));
		}
		else if (!mainCanvas || mainCanvas->GetCrsWkt().empty())
		{
			mousePositionStatusLabel->setToolTip(QStringLiteral("当前未设置坐标系，无法转换到经纬度。"));
		}
		else if (!hasLongitudeLatitudePosition)
		{
			mousePositionStatusLabel->setToolTip(QStringLiteral("当前鼠标位置无法转换到经纬度，可能超出了坐标系有效范围。"));
		}
		else
		{
			mousePositionStatusLabel->setToolTip(QStringLiteral("当前显示为经纬度坐标。"));
		}
	}
	else
	{
		mousePositionStatusLabel->setText(FormatMousePositionStatusText(position, hasPosition));
		mousePositionStatusLabel->setToolTip(QString());
	}

	BalanceStatusSideWidgetWidths();
}

void QArcGISRestMainWindow::UpdateViewExtentStatus(const GB_Rectangle& extent)
{
	if (!viewExtentStatusLabel)
	{
		return;
	}

	viewExtentStatusLabel->setText(FormatViewExtentStatusText(extent));
}

void QArcGISRestMainWindow::UpdateCrsStatus(const QString& crsDisplayText)
{
	if (!crsStatusButton)
	{
		return;
	}

	crsStatusButton->setText(FormatCrsStatusText(crsDisplayText));
	UpdateCrsZoomValidAreaButtonState();
	BalanceStatusSideWidgetWidths();
}

void QArcGISRestMainWindow::UpdateCrsValidAreaVisibleButtonTooltip()
{
	if (!crsValidAreaVisibleButton)
	{
		return;
	}

	if (crsValidAreaVisibleButton->isChecked())
	{
		crsValidAreaVisibleButton->setToolTip(QStringLiteral("当前正在显示坐标系适用区域。点击后隐藏坐标系适用区域。"));
	}
	else
	{
		crsValidAreaVisibleButton->setToolTip(QStringLiteral("当前未显示坐标系适用区域。点击后显示坐标系适用区域。"));
	}
}

void QArcGISRestMainWindow::UpdateCrsZoomValidAreaButtonState()
{
	if (!crsZoomValidAreaButton)
	{
		return;
	}

	const bool hasCrs = mainCanvas && !mainCanvas->GetCrsWkt().empty();
	crsZoomValidAreaButton->setEnabled(hasCrs);
	if (hasCrs)
	{
		crsZoomValidAreaButton->setToolTip(QStringLiteral("缩放至当前坐标系的最大适用范围。"));
	}
	else
	{
		crsZoomValidAreaButton->setToolTip(QStringLiteral("当前未设置坐标系，无法缩放至坐标系适用范围。"));
	}
}

void QArcGISRestMainWindow::BalanceStatusSideWidgetWidths()
{
	if (!leftStatusWidget || !rightStatusWidget)
	{
		return;
	}

	leftStatusWidget->setMinimumWidth(0);
	leftStatusWidget->setMaximumWidth(QWIDGETSIZE_MAX);
	rightStatusWidget->setMinimumWidth(0);
	rightStatusWidget->setMaximumWidth(QWIDGETSIZE_MAX);

	const int leftWidth = std::max(0, leftStatusWidget->sizeHint().width());
	const int rightWidth = std::max(0, rightStatusWidget->sizeHint().width());
	const int balancedWidth = std::max(leftWidth, rightWidth);
	if (balancedWidth <= 0)
	{
		return;
	}

	if (leftStatusWidget->minimumWidth() != balancedWidth || leftStatusWidget->maximumWidth() != balancedWidth)
	{
		leftStatusWidget->setFixedWidth(balancedWidth);
	}

	if (rightStatusWidget->minimumWidth() != balancedWidth || rightStatusWidget->maximumWidth() != balancedWidth)
	{
		rightStatusWidget->setFixedWidth(balancedWidth);
	}
}

void QArcGISRestMainWindow::OnLongitudeLatitudeCheckBoxStateChanged(int checkState)
{
	Q_UNUSED(checkState);

	if (!mainCanvas)
	{
		UpdateMousePositionStatus(GB_Point2d(), false);
		return;
	}

	GB_Point2d mousePosition;
	const bool hasPosition = mainCanvas->TryGetCurrentMouseWorldPosition(mousePosition);
	UpdateMousePositionStatus(mousePosition, hasPosition);
}

void QArcGISRestMainWindow::OnCrsStatusButtonClicked()
{
	QCrsManagerWidget crsManagerWidget(this);
	crsManagerWidget.BindMainCanvas(mainCanvas);
	crsManagerWidget.exec();
}

void QArcGISRestMainWindow::OnCrsValidAreaVisibleButtonToggled(bool checked)
{
	if (mainCanvas)
	{
		std::vector<std::string> drawableUids;
		drawableUids.push_back(QMainCanvas::GetCrsValidAreaDrawableUid());
		mainCanvas->SetDrawablesVisible(drawableUids, checked);
	}

	UpdateCrsValidAreaVisibleButtonTooltip();
}

void QArcGISRestMainWindow::OnCrsZoomValidAreaButtonClicked()
{
	if (!mainCanvas)
	{
		return;
	}

	if (!mainCanvas->ZoomToCrsValidArea(0.05))
	{
		statusBar()->showMessage(QStringLiteral("当前坐标系没有可用的适用范围。"), 3000);
	}
}
