#include <QApplication>
#include "QArcGISRestMainWindow.h"
#include "GeoCrsManager.h"
#include <thread>
#include "GeoBase/GB_Math.h"
#include "DataDef.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
	uint8_t CreateRandomByte(int minValue, int maxValue)
	{
		const int clampedMinValue = GB_Clamp(minValue, 0, 255);
		const int clampedMaxValue = GB_Clamp(maxValue, clampedMinValue, 255);
		return static_cast<uint8_t>(GB_RandomInt(clampedMinValue, clampedMaxValue));
	}

	GB_ColorRGBA CreateRandomColor(int minAlpha, int maxAlpha)
	{
		return GB_ColorRGBA(
			CreateRandomByte(0, 255),
			CreateRandomByte(0, 255),
			CreateRandomByte(0, 255),
			CreateRandomByte(minAlpha, maxAlpha));
	}

	GB_Rectangle MakeSafeGenerationExtent(const GB_Rectangle& generationExtent)
	{
		if (generationExtent.IsValid() && generationExtent.Width() > GB_Epsilon && generationExtent.Height() > GB_Epsilon)
		{
			return generationExtent;
		}

		return GB_Rectangle(-1000.0, -1000.0, 1000.0, 1000.0);
	}
}

std::vector<PolygonDrawable> CreateRandomPolygonDrawables(
	size_t polygonCount,
	const GB_Rectangle& generationExtent,
	int minVertexCount = 3,
	int maxVertexCount = 12,
	double minRadiusRatio = 0.003,
	double maxRadiusRatio = 0.03,
	double layerNumber = 0.0)
{
	std::vector<PolygonDrawable> polygons;
	polygons.reserve(polygonCount);

	if (polygonCount == 0)
	{
		return polygons;
	}

	const GB_Rectangle safeExtent = MakeSafeGenerationExtent(generationExtent);
	const double extentWidth = safeExtent.Width();
	const double extentHeight = safeExtent.Height();
	const double minExtentSize = std::min(extentWidth, extentHeight);

	const int safeMinVertexCount = std::max(3, minVertexCount);
	const int safeMaxVertexCount = std::max(safeMinVertexCount, maxVertexCount);

	const double safeMinRadiusRatio = std::max(1e-6, minRadiusRatio);
	const double safeMaxRadiusRatio = std::max(safeMinRadiusRatio, maxRadiusRatio);

	const double minRadius = std::max(minExtentSize * safeMinRadiusRatio, minExtentSize * 1e-6);
	const double maxRadius = std::max(minRadius, minExtentSize * std::min(safeMaxRadiusRatio, 0.45));

	for (size_t polygonIndex = 0; polygonIndex < polygonCount; polygonIndex++)
	{
		const int vertexCount = GB_RandomInt(safeMinVertexCount, safeMaxVertexCount);
		const double polygonRadius = GB_RandomDouble(minRadius, maxRadius);

		const double centerMinX = safeExtent.minX + polygonRadius;
		const double centerMaxX = safeExtent.maxX - polygonRadius;
		const double centerMinY = safeExtent.minY + polygonRadius;
		const double centerMaxY = safeExtent.maxY - polygonRadius;

		const double centerX = centerMinX < centerMaxX ? GB_RandomDouble(centerMinX, centerMaxX) : GB_RandomDouble(safeExtent.minX, safeExtent.maxX);
		const double centerY = centerMinY < centerMaxY ? GB_RandomDouble(centerMinY, centerMaxY) : GB_RandomDouble(safeExtent.minY, safeExtent.maxY);

		PolygonDrawable polygon;
		polygon.uid = polygon.CalculateUid();
		polygon.layerNumber = layerNumber;
		polygon.visible = true;
		polygon.fillColor = CreateRandomColor(50, 180);
		polygon.borderColor = CreateRandomColor(180, 255);
		polygon.borderWidth = GB_RandomInt(1, 5);
		polygon.vertices.reserve(static_cast<size_t>(vertexCount));

		const double angleOffset = GB_RandomDouble(0.0, GB_2Pi);
		const double angleStep = GB_2Pi / static_cast<double>(vertexCount);

		for (int vertexIndex = 0; vertexIndex < vertexCount; vertexIndex++)
		{
			const double angleJitter = angleStep * GB_RandomDouble(-0.35, 0.35);
			const double angle = angleOffset + angleStep * static_cast<double>(vertexIndex) + angleJitter;
			const double radius = polygonRadius * GB_RandomDouble(0.65, 1.0);

			const double x = centerX + std::cos(angle) * radius;
			const double y = centerY + std::sin(angle) * radius;

			polygon.vertices.push_back(GB_Point2d(x, y));
		}

		polygons.push_back(std::move(polygon));
	}

	return polygons;
}

//int main(int argc, char* argv[])
//{
//	QApplication app(argc, argv);
//	QArcGISRestMainWindow mainWindow;
//	mainWindow.show();
//	return app.exec();
//}

#include "QMainCanvas.h"
static QMainCanvas* g_mainCanvas = nullptr;
std::vector<PolygonDrawable> polygons;
void ThreadFunc()
{
	std::this_thread::sleep_for(std::chrono::seconds(5));
	if (g_mainCanvas)
	{
		g_mainCanvas->AddPolygonDrawables(polygons);
	}
}

int main(int argc, char* argv[])
{
	const GB_Rectangle testExtent(-10000000.0, -10000000.0, 10000000.0, 10000000.0);
	polygons = CreateRandomPolygonDrawables(20000, testExtent);


	QApplication app(argc, argv);
	QArcGISRestMainWindow mainWindow;
	mainWindow.show();
	g_mainCanvas = mainWindow.GetCanvas();

	std::thread workerThread(ThreadFunc);
	workerThread.detach();
	return app.exec();
}