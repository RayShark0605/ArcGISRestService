#pragma once

#include <string>
#include "GeoBase/CV/GB_Image.h"
#include "GeoBase/Geometry/GB_Rectangle.h"

class IDrawable
{
public:
	std::string uid = "";

	// 绘制层号。-1 表示最顶层，GB_IntMax / 2 表示最底层；
	// 其它值越小越靠近顶层，越大越靠近底层。
	double layerNumber = 0;
	bool visible = true;

	virtual ~IDrawable() = default;
	virtual std::string CalculateUid() const;
};

class MapTileDrawable : public IDrawable
{
public:
	GB_Image image;
	GB_Rectangle extent;
};

class PointDrawable : public IDrawable
{
public:
	GB_Point2d position;
	enum class SymbolShape
	{
		Circle,				// 圆形
		Square,				// 正方形
		Triangle,			// 等边三角形，顶点朝上
		Cross,				// 十字形
		X,					// X 形
		Star,				// 星形
		FivePointStar,		// 五角星
		Diamond				// 菱形
	};
	SymbolShape symbolShape = SymbolShape::Circle;
	bool symbolFilled = true;
	GB_ColorRGBA borderColor = GB_ColorRGBA(0, 0, 0);
	GB_ColorRGBA fillColor = GB_ColorRGBA(255, 0, 0);
	int borderWidth = 2;
	int symbolSize = 10; // 符号外接正方形的边长（显示像素）
};

class PolylineDrawable : public IDrawable
{
public:
	std::vector<GB_Point2d> vertices;
	GB_ColorRGBA lineColor = GB_ColorRGBA(255, 0, 0);
	int lineWidth = 2;
};

class PolygonDrawable : public IDrawable
{
public:
	std::vector<GB_Point2d> vertices;
	GB_ColorRGBA fillColor = GB_ColorRGBA(255, 0, 0);
	GB_ColorRGBA borderColor = GB_ColorRGBA(0, 0, 0);
	int borderWidth = 2;
};
