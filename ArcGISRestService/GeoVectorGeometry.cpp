#include "GeoVectorGeometry.h"

#include <algorithm>
#include <utility>

namespace
{
    char ToLowerAscii(const char character)
    {
        if (character >= 'A' && character <= 'Z')
        {
            return static_cast<char>(character - 'A' + 'a');
        }

        return character;
    }

    std::string ToLowerAsciiString(const std::string& text)
    {
        std::string lowerText = text;
        for (size_t i = 0; i < lowerText.size(); i++)
        {
            lowerText[i] = ToLowerAscii(lowerText[i]);
        }
        return lowerText;
    }

    const GeoVectorGeometry::PointDataType& GetEmptyPointData()
    {
        static const GeoVectorGeometry::PointDataType emptyData;
        return emptyData;
    }

    const GeoVectorGeometry::PolylineDataType& GetEmptyPolylineData()
    {
        static const GeoVectorGeometry::PolylineDataType emptyData;
        return emptyData;
    }

    bool IsPointDataValid(const GeoVectorGeometry::PointDataType& points)
    {
        for (size_t i = 0; i < points.size(); i++)
        {
            if (!points[i].IsValid())
            {
                return false;
            }
        }
        return true;
    }

    bool IsPolylineDataValid(const GeoVectorGeometry::PolylineDataType& polylines)
    {
        for (size_t i = 0; i < polylines.size(); i++)
        {
            if (!polylines[i].IsValid())
            {
                return false;
            }
        }
        return true;
    }

    bool TryNormalizePolygonPart(const GB_Polyline& polygon, GB_Polyline& outPolygon, const bool closePolygonPart)
    {
        if (polygon.IsEmpty())
        {
            return false;
        }

        std::vector<GB_Point2d> vertices = polygon.GetVertices();
        if (vertices.size() < 3)
        {
            return false;
        }

        for (size_t i = 0; i < vertices.size(); i++)
        {
            if (!vertices[i].IsValid())
            {
                return false;
            }
        }

        if (!vertices.front().IsNearEqual(vertices.back(), GB_Epsilon))
        {
            if (!closePolygonPart)
            {
                return false;
            }

            vertices.push_back(vertices.front());
        }
        else
        {
            vertices.back() = vertices.front();
        }

        if (vertices.size() < 4)
        {
            return false;
        }

        GB_Polyline normalizedPolygon(std::move(vertices));
        if (!normalizedPolygon.IsValid() || !normalizedPolygon.IsClosed())
        {
            return false;
        }

        outPolygon = std::move(normalizedPolygon);
        return true;
    }

    bool TryNormalizePolygonData(const GeoVectorGeometry::PolygonDataType& polygons, GeoVectorGeometry::PolygonDataType& outPolygons, const bool closePolygonParts)
    {
        GeoVectorGeometry::PolygonDataType normalizedPolygons;
        normalizedPolygons.reserve(polygons.size());

        for (size_t i = 0; i < polygons.size(); i++)
        {
            GB_Polyline normalizedPolygon;
            if (!TryNormalizePolygonPart(polygons[i], normalizedPolygon, closePolygonParts))
            {
                return false;
            }

            normalizedPolygons.push_back(std::move(normalizedPolygon));
        }

        outPolygons = std::move(normalizedPolygons);
        return true;
    }

    bool TryNormalizePolygonData(GeoVectorGeometry::PolygonDataType&& polygons, GeoVectorGeometry::PolygonDataType& outPolygons, const bool closePolygonParts)
    {
        GeoVectorGeometry::PolygonDataType normalizedPolygons;
        normalizedPolygons.reserve(polygons.size());

        for (size_t i = 0; i < polygons.size(); i++)
        {
            GB_Polyline normalizedPolygon;
            if (!TryNormalizePolygonPart(polygons[i], normalizedPolygon, closePolygonParts))
            {
                return false;
            }

            normalizedPolygons.push_back(std::move(normalizedPolygon));
        }

        outPolygons = std::move(normalizedPolygons);
        return true;
    }

    bool IsPolygonDataValid(const GeoVectorGeometry::PolygonDataType& polygons)
    {
        for (size_t i = 0; i < polygons.size(); i++)
        {
            if (!polygons[i].IsValid() || !polygons[i].IsClosed() || polygons[i].GetNumVertices() < 4)
            {
                return false;
            }
        }

        return true;
    }

    bool IsPolylineVectorNearEqual(const GeoVectorGeometry::PolylineDataType& leftPolylines, const GeoVectorGeometry::PolylineDataType& rightPolylines, const double tolerance)
    {
        if (leftPolylines.size() != rightPolylines.size())
        {
            return false;
        }

        for (size_t i = 0; i < leftPolylines.size(); i++)
        {
            if (!leftPolylines[i].IsNearEqual(rightPolylines[i], tolerance))
            {
                return false;
            }
        }

        return true;
    }

    bool IsPointVectorNearEqual(const GeoVectorGeometry::PointDataType& leftPoints, const GeoVectorGeometry::PointDataType& rightPoints, const double tolerance)
    {
        if (leftPoints.size() != rightPoints.size())
        {
            return false;
        }

        for (size_t i = 0; i < leftPoints.size(); i++)
        {
            if (!leftPoints[i].IsNearEqual(rightPoints[i], tolerance))
            {
                return false;
            }
        }

        return true;
    }
}

GeoVectorGeometry::GeoVectorGeometry()
{
    Reset();
}

GeoVectorGeometry::GeoVectorGeometry(const PointDataType& points)
{
    Reset();
    SetPoints(points);
}

GeoVectorGeometry::GeoVectorGeometry(PointDataType&& points)
{
    Reset();
    SetPoints(std::move(points));
}

GeoVectorGeometry::GeoVectorGeometry(const GeoVectorGeometryType geometryType, const PolylineDataType& parts, const bool closePolygonParts)
{
    Reset();
    if (geometryType == GeoVectorGeometryType::Polyline)
    {
        SetPolylines(parts);
    }
    else if (geometryType == GeoVectorGeometryType::Polygon)
    {
        SetPolygons(parts, closePolygonParts);
    }
}

GeoVectorGeometry::GeoVectorGeometry(const GeoVectorGeometryType geometryType, PolylineDataType&& parts, const bool closePolygonParts)
{
    Reset();
    if (geometryType == GeoVectorGeometryType::Polyline)
    {
        SetPolylines(std::move(parts));
    }
    else if (geometryType == GeoVectorGeometryType::Polygon)
    {
        SetPolygons(std::move(parts), closePolygonParts);
    }
}

GeoVectorGeometry GeoVectorGeometry::CreatePointGeometry(const PointDataType& points)
{
    GeoVectorGeometry geometry;
    geometry.SetPoints(points);
    return geometry;
}

GeoVectorGeometry GeoVectorGeometry::CreatePointGeometry(PointDataType&& points)
{
    GeoVectorGeometry geometry;
    geometry.SetPoints(std::move(points));
    return geometry;
}

GeoVectorGeometry GeoVectorGeometry::CreatePolylineGeometry(const PolylineDataType& polylines)
{
    GeoVectorGeometry geometry;
    geometry.SetPolylines(polylines);
    return geometry;
}

GeoVectorGeometry GeoVectorGeometry::CreatePolylineGeometry(PolylineDataType&& polylines)
{
    GeoVectorGeometry geometry;
    geometry.SetPolylines(std::move(polylines));
    return geometry;
}

GeoVectorGeometry GeoVectorGeometry::CreatePolygonGeometry(const PolygonDataType& polygons, const bool closePolygonParts)
{
    GeoVectorGeometry geometry;
    geometry.SetPolygons(polygons, closePolygonParts);
    return geometry;
}

GeoVectorGeometry GeoVectorGeometry::CreatePolygonGeometry(PolygonDataType&& polygons, const bool closePolygonParts)
{
    GeoVectorGeometry geometry;
    geometry.SetPolygons(std::move(polygons), closePolygonParts);
    return geometry;
}

std::string GeoVectorGeometry::GeometryTypeToString(const GeoVectorGeometryType geometryType)
{
    switch (geometryType)
    {
    case GeoVectorGeometryType::Point:
        return "Point";
    case GeoVectorGeometryType::Polyline:
        return "Polyline";
    case GeoVectorGeometryType::Polygon:
        return "Polygon";
    case GeoVectorGeometryType::Unknown:
    default:
        return "Unknown";
    }
}

GeoVectorGeometryType GeoVectorGeometry::GeometryTypeFromArcGISString(const std::string& geometryTypeText)
{
    const std::string lowerText = ToLowerAsciiString(geometryTypeText);

    if (lowerText == "esrigeometrypoint" || lowerText == "point" || lowerText == "esrigeometrymultipoint" || lowerText == "multipoint")
    {
        return GeoVectorGeometryType::Point;
    }

    if (lowerText == "esrigeometrypolyline" || lowerText == "polyline" || lowerText == "linestring" || lowerText == "multilinestring")
    {
        return GeoVectorGeometryType::Polyline;
    }

    if (lowerText == "esrigeometrypolygon" || lowerText == "polygon" || lowerText == "multipolygon")
    {
        return GeoVectorGeometryType::Polygon;
    }

    return GeoVectorGeometryType::Unknown;
}

std::string GeoVectorGeometry::GeometryTypeToArcGISString(const GeoVectorGeometryType geometryType)
{
    switch (geometryType)
    {
    case GeoVectorGeometryType::Point:
        return "esriGeometryPoint";
    case GeoVectorGeometryType::Polyline:
        return "esriGeometryPolyline";
    case GeoVectorGeometryType::Polygon:
        return "esriGeometryPolygon";
    case GeoVectorGeometryType::Unknown:
    default:
        return "";
    }
}

void GeoVectorGeometry::Reset()
{
    geometryType = GeoVectorGeometryType::Unknown;
    geometryData.Reset();
}

bool GeoVectorGeometry::SetEmptyGeometry(const GeoVectorGeometryType newGeometryType)
{
    switch (newGeometryType)
    {
    case GeoVectorGeometryType::Unknown:
        Reset();
        return true;
    case GeoVectorGeometryType::Point:
        geometryType = GeoVectorGeometryType::Point;
        geometryData = PointDataType();
        return true;
    case GeoVectorGeometryType::Polyline:
        geometryType = GeoVectorGeometryType::Polyline;
        geometryData = PolylineDataType();
        return true;
    case GeoVectorGeometryType::Polygon:
        geometryType = GeoVectorGeometryType::Polygon;
        geometryData = PolygonDataType();
        return true;
    default:
        return false;
    }
}

GeoVectorGeometryType GeoVectorGeometry::GetGeometryType() const
{
    return geometryType;
}

std::string GeoVectorGeometry::GetGeometryTypeString() const
{
    return GeometryTypeToString(geometryType);
}

bool GeoVectorGeometry::IsPoint() const
{
    return geometryType == GeoVectorGeometryType::Point;
}

bool GeoVectorGeometry::IsPolyline() const
{
    return geometryType == GeoVectorGeometryType::Polyline;
}

bool GeoVectorGeometry::IsPolygon() const
{
    return geometryType == GeoVectorGeometryType::Polygon;
}

bool GeoVectorGeometry::IsEmpty() const
{
    switch (geometryType)
    {
    case GeoVectorGeometryType::Point:
    {
        const PointDataType* points = geometryData.AnyCast<PointDataType>();
        return points == nullptr || points->empty();
    }
    case GeoVectorGeometryType::Polyline:
    {
        const PolylineDataType* polylines = geometryData.AnyCast<PolylineDataType>();
        return polylines == nullptr || polylines->empty();
    }
    case GeoVectorGeometryType::Polygon:
    {
        const PolygonDataType* polygons = geometryData.AnyCast<PolygonDataType>();
        return polygons == nullptr || polygons->empty();
    }
    case GeoVectorGeometryType::Unknown:
    default:
        return true;
    }
}

bool GeoVectorGeometry::IsValid() const
{
    switch (geometryType)
    {
    case GeoVectorGeometryType::Point:
    {
        const PointDataType* points = geometryData.AnyCast<PointDataType>();
        return points != nullptr && IsPointDataValid(*points);
    }
    case GeoVectorGeometryType::Polyline:
    {
        const PolylineDataType* polylines = geometryData.AnyCast<PolylineDataType>();
        return polylines != nullptr && IsPolylineDataValid(*polylines);
    }
    case GeoVectorGeometryType::Polygon:
    {
        const PolygonDataType* polygons = geometryData.AnyCast<PolygonDataType>();
        return polygons != nullptr && IsPolygonDataValid(*polygons);
    }
    case GeoVectorGeometryType::Unknown:
    default:
        return false;
    }
}

size_t GeoVectorGeometry::GetPartCount() const
{
    switch (geometryType)
    {
    case GeoVectorGeometryType::Point:
    {
        const PointDataType* points = geometryData.AnyCast<PointDataType>();
        return points == nullptr ? 0 : points->size();
    }
    case GeoVectorGeometryType::Polyline:
    {
        const PolylineDataType* polylines = geometryData.AnyCast<PolylineDataType>();
        return polylines == nullptr ? 0 : polylines->size();
    }
    case GeoVectorGeometryType::Polygon:
    {
        const PolygonDataType* polygons = geometryData.AnyCast<PolygonDataType>();
        return polygons == nullptr ? 0 : polygons->size();
    }
    case GeoVectorGeometryType::Unknown:
    default:
        return 0;
    }
}

size_t GeoVectorGeometry::GetVertexCount() const
{
    switch (geometryType)
    {
    case GeoVectorGeometryType::Point:
    {
        const PointDataType* points = geometryData.AnyCast<PointDataType>();
        return points == nullptr ? 0 : points->size();
    }
    case GeoVectorGeometryType::Polyline:
    {
        const PolylineDataType* polylines = geometryData.AnyCast<PolylineDataType>();
        if (polylines == nullptr)
        {
            return 0;
        }

        size_t vertexCount = 0;
        for (size_t i = 0; i < polylines->size(); i++)
        {
            vertexCount += (*polylines)[i].GetNumVertices();
        }
        return vertexCount;
    }
    case GeoVectorGeometryType::Polygon:
    {
        const PolygonDataType* polygons = geometryData.AnyCast<PolygonDataType>();
        if (polygons == nullptr)
        {
            return 0;
        }

        size_t vertexCount = 0;
        for (size_t i = 0; i < polygons->size(); i++)
        {
            vertexCount += (*polygons)[i].GetNumVertices();
        }
        return vertexCount;
    }
    case GeoVectorGeometryType::Unknown:
    default:
        return 0;
    }
}

GB_Rectangle GeoVectorGeometry::BoundingRectangle() const
{
    GB_Rectangle boundingRectangle;

    switch (geometryType)
    {
    case GeoVectorGeometryType::Point:
    {
        const PointDataType* points = geometryData.AnyCast<PointDataType>();
        if (points == nullptr)
        {
            return boundingRectangle;
        }

        for (size_t i = 0; i < points->size(); i++)
        {
            if ((*points)[i].IsValid())
            {
                boundingRectangle.Expand((*points)[i]);
            }
        }
        return boundingRectangle;
    }
    case GeoVectorGeometryType::Polyline:
    {
        const PolylineDataType* polylines = geometryData.AnyCast<PolylineDataType>();
        if (polylines == nullptr)
        {
            return boundingRectangle;
        }

        for (size_t i = 0; i < polylines->size(); i++)
        {
            const GB_Rectangle partRectangle = (*polylines)[i].BoundingRectangle();
            if (partRectangle.IsValid())
            {
                boundingRectangle.Expand(partRectangle);
            }
        }
        return boundingRectangle;
    }
    case GeoVectorGeometryType::Polygon:
    {
        const PolygonDataType* polygons = geometryData.AnyCast<PolygonDataType>();
        if (polygons == nullptr)
        {
            return boundingRectangle;
        }

        for (size_t i = 0; i < polygons->size(); i++)
        {
            const GB_Rectangle partRectangle = (*polygons)[i].BoundingRectangle();
            if (partRectangle.IsValid())
            {
                boundingRectangle.Expand(partRectangle);
            }
        }
        return boundingRectangle;
    }
    case GeoVectorGeometryType::Unknown:
    default:
        return boundingRectangle;
    }
}

bool GeoVectorGeometry::SetPoints(const PointDataType& points)
{
    if (!IsPointDataValid(points))
    {
        return false;
    }

    geometryType = GeoVectorGeometryType::Point;
    geometryData = points;
    return true;
}

bool GeoVectorGeometry::SetPoints(PointDataType&& points)
{
    if (!IsPointDataValid(points))
    {
        return false;
    }

    geometryType = GeoVectorGeometryType::Point;
    geometryData = std::move(points);
    return true;
}

bool GeoVectorGeometry::SetPolylines(const PolylineDataType& polylines)
{
    if (!IsPolylineDataValid(polylines))
    {
        return false;
    }

    geometryType = GeoVectorGeometryType::Polyline;
    geometryData = polylines;
    return true;
}

bool GeoVectorGeometry::SetPolylines(PolylineDataType&& polylines)
{
    if (!IsPolylineDataValid(polylines))
    {
        return false;
    }

    geometryType = GeoVectorGeometryType::Polyline;
    geometryData = std::move(polylines);
    return true;
}

bool GeoVectorGeometry::SetPolygons(const PolygonDataType& polygons, const bool closePolygonParts)
{
    PolygonDataType normalizedPolygons;
    if (!TryNormalizePolygonData(polygons, normalizedPolygons, closePolygonParts))
    {
        return false;
    }

    geometryType = GeoVectorGeometryType::Polygon;
    geometryData = std::move(normalizedPolygons);
    return true;
}

bool GeoVectorGeometry::SetPolygons(PolygonDataType&& polygons, const bool closePolygonParts)
{
    PolygonDataType normalizedPolygons;
    if (!TryNormalizePolygonData(std::move(polygons), normalizedPolygons, closePolygonParts))
    {
        return false;
    }

    geometryType = GeoVectorGeometryType::Polygon;
    geometryData = std::move(normalizedPolygons);
    return true;
}

bool GeoVectorGeometry::SetGeometryData(const GeoVectorGeometryType newGeometryType, const GB_Variant& data, const bool closePolygonParts)
{
    switch (newGeometryType)
    {
    case GeoVectorGeometryType::Unknown:
        if (!data.IsEmpty())
        {
            return false;
        }
        return SetEmptyGeometry(GeoVectorGeometryType::Unknown);
    case GeoVectorGeometryType::Point:
    {
        const PointDataType* points = data.AnyCast<PointDataType>();
        if (points == nullptr)
        {
            return false;
        }
        return SetPoints(*points);
    }
    case GeoVectorGeometryType::Polyline:
    {
        const PolylineDataType* polylines = data.AnyCast<PolylineDataType>();
        if (polylines == nullptr)
        {
            return false;
        }
        return SetPolylines(*polylines);
    }
    case GeoVectorGeometryType::Polygon:
    {
        const PolygonDataType* polygons = data.AnyCast<PolygonDataType>();
        if (polygons == nullptr)
        {
            return false;
        }
        return SetPolygons(*polygons, closePolygonParts);
    }
    default:
        return false;
    }
}

bool GeoVectorGeometry::SetGeometryData(const GeoVectorGeometryType newGeometryType, GB_Variant&& data, const bool closePolygonParts)
{
    switch (newGeometryType)
    {
    case GeoVectorGeometryType::Unknown:
        if (!data.IsEmpty())
        {
            return false;
        }
        return SetEmptyGeometry(GeoVectorGeometryType::Unknown);
    case GeoVectorGeometryType::Point:
    {
        PointDataType* points = data.AnyCast<PointDataType>();
        if (points == nullptr)
        {
            return false;
        }
        PointDataType movedPoints = std::move(*points);
        return SetPoints(std::move(movedPoints));
    }
    case GeoVectorGeometryType::Polyline:
    {
        PolylineDataType* polylines = data.AnyCast<PolylineDataType>();
        if (polylines == nullptr)
        {
            return false;
        }
        PolylineDataType movedPolylines = std::move(*polylines);
        return SetPolylines(std::move(movedPolylines));
    }
    case GeoVectorGeometryType::Polygon:
    {
        PolygonDataType* polygons = data.AnyCast<PolygonDataType>();
        if (polygons == nullptr)
        {
            return false;
        }
        PolygonDataType movedPolygons = std::move(*polygons);
        return SetPolygons(std::move(movedPolygons), closePolygonParts);
    }
    default:
        return false;
    }
}

bool GeoVectorGeometry::AddPoint(const GB_Point2d& point)
{
    if (!point.IsValid())
    {
        return false;
    }

    if (geometryType == GeoVectorGeometryType::Unknown)
    {
        SetEmptyGeometry(GeoVectorGeometryType::Point);
    }

    PointDataType* points = GetMutablePoints();
    if (points == nullptr)
    {
        return false;
    }

    points->push_back(point);
    return true;
}

bool GeoVectorGeometry::AddPolyline(const GB_Polyline& polyline)
{
    if (!polyline.IsValid())
    {
        return false;
    }

    if (geometryType == GeoVectorGeometryType::Unknown)
    {
        SetEmptyGeometry(GeoVectorGeometryType::Polyline);
    }

    PolylineDataType* polylines = GetMutablePolylines();
    if (polylines == nullptr)
    {
        return false;
    }

    polylines->push_back(polyline);
    return true;
}

bool GeoVectorGeometry::AddPolygon(const GB_Polyline& polygon, const bool closePolygonPart)
{
    GB_Polyline normalizedPolygon;
    if (!TryNormalizePolygonPart(polygon, normalizedPolygon, closePolygonPart))
    {
        return false;
    }

    if (geometryType == GeoVectorGeometryType::Unknown)
    {
        SetEmptyGeometry(GeoVectorGeometryType::Polygon);
    }

    PolygonDataType* polygons = GetMutablePolygons();
    if (polygons == nullptr)
    {
        return false;
    }

    polygons->push_back(std::move(normalizedPolygon));
    return true;
}

const GeoVectorGeometry::PointDataType& GeoVectorGeometry::GetPoints() const
{
    const PointDataType* points = GetPointsPtr();
    return points == nullptr ? GetEmptyPointData() : *points;
}

GeoVectorGeometry::PointDataType* GeoVectorGeometry::GetMutablePoints()
{
    if (geometryType != GeoVectorGeometryType::Point)
    {
        return nullptr;
    }

    return geometryData.AnyCast<PointDataType>();
}

const GeoVectorGeometry::PointDataType* GeoVectorGeometry::GetPointsPtr() const
{
    if (geometryType != GeoVectorGeometryType::Point)
    {
        return nullptr;
    }

    return geometryData.AnyCast<PointDataType>();
}

const GeoVectorGeometry::PolylineDataType& GeoVectorGeometry::GetPolylines() const
{
    const PolylineDataType* polylines = GetPolylinesPtr();
    return polylines == nullptr ? GetEmptyPolylineData() : *polylines;
}

GeoVectorGeometry::PolylineDataType* GeoVectorGeometry::GetMutablePolylines()
{
    if (geometryType != GeoVectorGeometryType::Polyline)
    {
        return nullptr;
    }

    return geometryData.AnyCast<PolylineDataType>();
}

const GeoVectorGeometry::PolylineDataType* GeoVectorGeometry::GetPolylinesPtr() const
{
    if (geometryType != GeoVectorGeometryType::Polyline)
    {
        return nullptr;
    }

    return geometryData.AnyCast<PolylineDataType>();
}

const GeoVectorGeometry::PolygonDataType& GeoVectorGeometry::GetPolygons() const
{
    const PolygonDataType* polygons = GetPolygonsPtr();
    return polygons == nullptr ? GetEmptyPolylineData() : *polygons;
}

GeoVectorGeometry::PolygonDataType* GeoVectorGeometry::GetMutablePolygons()
{
    if (geometryType != GeoVectorGeometryType::Polygon)
    {
        return nullptr;
    }

    return geometryData.AnyCast<PolygonDataType>();
}

const GeoVectorGeometry::PolygonDataType* GeoVectorGeometry::GetPolygonsPtr() const
{
    if (geometryType != GeoVectorGeometryType::Polygon)
    {
        return nullptr;
    }

    return geometryData.AnyCast<PolygonDataType>();
}

const GB_Variant& GeoVectorGeometry::GetGeometryData() const
{
    return geometryData;
}

GB_Point2d GeoVectorGeometry::GetPoint(const size_t index) const
{
    GB_Point2d point;
    TryGetPoint(index, point);
    return point;
}

bool GeoVectorGeometry::TryGetPoint(const size_t index, GB_Point2d& outPoint) const
{
    const PointDataType* points = GetPointsPtr();
    if (points == nullptr || index >= points->size())
    {
        outPoint = GB_Point2d();
        return false;
    }

    outPoint = (*points)[index];
    return true;
}

GB_Polyline GeoVectorGeometry::GetPolyline(const size_t index) const
{
    GB_Polyline polyline;
    if (!TryGetPolyline(index, polyline))
    {
        return GB_Polyline::Invalid;
    }

    return polyline;
}

bool GeoVectorGeometry::TryGetPolyline(const size_t index, GB_Polyline& outPolyline) const
{
    const PolylineDataType* polylines = GetPolylinesPtr();
    if (polylines == nullptr || index >= polylines->size())
    {
        outPolyline = GB_Polyline::Invalid;
        return false;
    }

    outPolyline = (*polylines)[index];
    return true;
}

GB_Polyline GeoVectorGeometry::GetPolygon(const size_t index) const
{
    GB_Polyline polygon;
    if (!TryGetPolygon(index, polygon))
    {
        return GB_Polyline::Invalid;
    }

    return polygon;
}

bool GeoVectorGeometry::TryGetPolygon(const size_t index, GB_Polyline& outPolygon) const
{
    const PolygonDataType* polygons = GetPolygonsPtr();
    if (polygons == nullptr || index >= polygons->size())
    {
        outPolygon = GB_Polyline::Invalid;
        return false;
    }

    outPolygon = (*polygons)[index];
    return true;
}

bool GeoVectorGeometry::operator==(const GeoVectorGeometry& other) const
{
    if (geometryType != other.geometryType)
    {
        return false;
    }

    switch (geometryType)
    {
    case GeoVectorGeometryType::Unknown:
        return true;
    case GeoVectorGeometryType::Point:
    {
        const PointDataType* points = GetPointsPtr();
        const PointDataType* otherPoints = other.GetPointsPtr();
        return points != nullptr && otherPoints != nullptr && *points == *otherPoints;
    }
    case GeoVectorGeometryType::Polyline:
    {
        const PolylineDataType* polylines = GetPolylinesPtr();
        const PolylineDataType* otherPolylines = other.GetPolylinesPtr();
        return polylines != nullptr && otherPolylines != nullptr && *polylines == *otherPolylines;
    }
    case GeoVectorGeometryType::Polygon:
    {
        const PolygonDataType* polygons = GetPolygonsPtr();
        const PolygonDataType* otherPolygons = other.GetPolygonsPtr();
        return polygons != nullptr && otherPolygons != nullptr && *polygons == *otherPolygons;
    }
    default:
        return false;
    }
}

bool GeoVectorGeometry::operator!=(const GeoVectorGeometry& other) const
{
    return !(*this == other);
}

bool GeoVectorGeometry::IsNearEqual(const GeoVectorGeometry& other, const double tolerance) const
{
    if (geometryType != other.geometryType)
    {
        return false;
    }

    switch (geometryType)
    {
    case GeoVectorGeometryType::Unknown:
        return true;
    case GeoVectorGeometryType::Point:
    {
        const PointDataType* points = GetPointsPtr();
        const PointDataType* otherPoints = other.GetPointsPtr();
        return points != nullptr && otherPoints != nullptr && IsPointVectorNearEqual(*points, *otherPoints, tolerance);
    }
    case GeoVectorGeometryType::Polyline:
    {
        const PolylineDataType* polylines = GetPolylinesPtr();
        const PolylineDataType* otherPolylines = other.GetPolylinesPtr();
        return polylines != nullptr && otherPolylines != nullptr && IsPolylineVectorNearEqual(*polylines, *otherPolylines, tolerance);
    }
    case GeoVectorGeometryType::Polygon:
    {
        const PolygonDataType* polygons = GetPolygonsPtr();
        const PolygonDataType* otherPolygons = other.GetPolygonsPtr();
        return polygons != nullptr && otherPolygons != nullptr && IsPolylineVectorNearEqual(*polygons, *otherPolygons, tolerance);
    }
    default:
        return false;
    }
}
