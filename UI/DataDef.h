#pragma once

#include <string>
#include "GeoBase/CV/GB_Image.h"
#include "GeoBase/Geometry/GB_Rectangle.h"

class Drawable
{
public:
	std::string uid = "";

	// 绘制层号。-1 表示最顶层，GB_IntMax / 2 表示最底层；
	// 其它值越小越靠近顶层，越大越靠近底层。
	double layerNumber = 0;

	virtual ~Drawable() = default;
	virtual std::string CalculateUid() const = 0;
};


class MapTile : public Drawable
{
public:
	GB_Image image;
	GB_Rectangle extent;
	bool visible = true;

	virtual std::string CalculateUid() const override;
};




