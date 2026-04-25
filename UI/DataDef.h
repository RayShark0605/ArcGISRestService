#pragma once

#include <string>
#include "GeoBase/CV/GB_Image.h"
#include "GeoBase/Geometry/GB_Rectangle.h"

class Drawable
{
public:
	std::string uid = "";

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




