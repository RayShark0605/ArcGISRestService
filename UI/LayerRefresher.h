#pragma once

#include <QObject>
#include <QPointer>

#include <memory>
#include <string>

#include "GeoBase/Geometry/GB_Rectangle.h"
#include "QServiceBrowserPanel.h"
#include "QLayerManagerPanel.h"

class QMainCanvas;

/**
 * @brief ArcGIS REST 图层刷新控制器。
 *
 * 该类负责把图层管理面板中的图层快照与主画布视口状态合并，形成当前应显示的
 * ArcGIS REST 影像瓦片/动态影像请求集合，以及 FeatureServer / MapServer 矢量要素查询请求集合。
 * 影像任务会在后台线程执行“计算请求范围 -> 影像缓存命中读取/缓存未命中下载 -> 解码 -> 必要时重投影 -> 显示”；
 * 矢量任务会在后台线程执行“计算视口空间过滤范围 -> 矢量缓存命中读取/缓存未命中 query 请求 -> JSON 解析 -> 必要时坐标转换 -> Drawable 显示”。
 *
 * 线程模型：
 * - ViewStateChanged / LayersChanged 在 UI 线程进入本对象；
 * - 缓存读取、网络下载、图片解码、ArcGIS REST 矢量 JSON 解析在内部 worker 线程执行；
 * - 画布增删瓦片/矢量 Drawable 只通过 Qt::QueuedConnection 回到本对象所属线程后执行。
 *
 * 刷新模型：
 * - 每次视口或图层快照变化都会开启一个新的刷新代际；
 * - 新代际会清空尚未执行的旧任务；
 * - 已经在执行的旧任务无法强杀网络请求，但完成后会因代际失效而被丢弃；
 * - 已显示但不再属于新代际目标集合的瓦片会临时降到画布底层，作为新帧加载过程中的过渡底图；
 * - 新代际需要下载的瓦片全部完成（成功或失败）后，过渡底图会被清理，避免长期显示过期内容；
 * - 已显示且仍属于目标集合的瓦片或矢量 Drawable 会被复用；若图层顺序变化，则直接调整层号，避免重复解码、解析或重建显示对象。
 */
class LayerRefresher : public QObject
{
	Q_OBJECT

public:
	static LayerRefresher* GetInstance();

	// 兼容旧调用：没有 QLayerManagerPanel 时，退化为直接响应 QServiceBrowserPanel::LayerImportRequested。
	void SetCanvasAndPanel(QMainCanvas* canvas, QServiceBrowserPanel* panel);

	// 推荐调用：LayerRefresher 以 QLayerManagerPanel 的完整图层快照作为刷新依据。
	void SetCanvasAndPanel(QMainCanvas* canvas, QLayerManagerPanel* layerManagerPanel);
	void SetCanvasAndPanels(QMainCanvas* canvas, QServiceBrowserPanel* serviceBrowserPanel, QLayerManagerPanel* layerManagerPanel);

	void StopRefresh();

private slots:
	void OnViewStateChanged(const GB_Rectangle& extent, double approximateMetersPerPixel);
	void OnLayersChanged(const LayerManagerChangeInfo& changeInfo);
	void OnArcGISRestLayerImportRequested(const LayerImportRequestInfo& request);

private:
	class Impl;

	LayerRefresher();
	virtual ~LayerRefresher() override;

	LayerRefresher(const LayerRefresher&) = delete;
	LayerRefresher& operator=(const LayerRefresher&) = delete;

	std::unique_ptr<Impl> impl;
};
