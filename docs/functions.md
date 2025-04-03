# DuckDB Spatial Function Reference

## Function Index 
**[Scalar Functions](#scalar-functions)**

| Function                                                      | Summary                                                                                                                                                                                                          |
|---------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| [`ST_Area`](#st_area)                                         | Compute the area of a geometry.                                                                                                                                                                                  |
| [`ST_Area_Spheroid`](#st_area_spheroid)                       | Returns the area of a geometry in meters, using an ellipsoidal model of the earth                                                                                                                                |
| [`ST_AsGeoJSON`](#st_asgeojson)                               | Returns the geometry as a GeoJSON fragment                                                                                                                                                                       |
| [`ST_AsHEXWKB`](#st_ashexwkb)                                 | Returns the geometry as a HEXWKB string                                                                                                                                                                          |
| [`ST_AsSVG`](#st_assvg)                                       | Convert the geometry into a SVG fragment or path                                                                                                                                                                 |
| [`ST_AsText`](#st_astext)                                     | Returns the geometry as a WKT string                                                                                                                                                                             |
| [`ST_AsWKB`](#st_aswkb)                                       | Returns the geometry as a WKB blob                                                                                                                                                                               |
| [`ST_Boundary`](#st_boundary)                                 | Returns the "boundary" of a geometry                                                                                                                                                                             |
| [`ST_Buffer`](#st_buffer)                                     | Returns a buffer around the input geometry at the target distance                                                                                                                                                |
| [`ST_Centroid`](#st_centroid)                                 | Calculates the centroid of a geometry                                                                                                                                                                            |
| [`ST_Collect`](#st_collect)                                   | Collects a list of geometries into a collection geometry.                                                                                                                                                        |
| [`ST_CollectionExtract`](#st_collectionextract)               | Extracts geometries from a GeometryCollection into a typed multi geometry.                                                                                                                                       |
| [`ST_Contains`](#st_contains)                                 | Returns true if geom1 contains geom2.                                                                                                                                                                            |
| [`ST_ContainsProperly`](#st_containsproperly)                 | Returns true if geom1 "properly contains" geom2                                                                                                                                                                  |
| [`ST_ConcaveHull`](#st_concavehull)                           | Returns the concave hull of the input geometry, containing all of the source input's points.                                                                                                                     |
| [`ST_ConvexHull`](#st_convexhull)                             | Returns the convex hull enclosing the geometry                                                                                                                                                                   |
| [`ST_CoveredBy`](#st_coveredby)                               | Returns true if geom1 is "covered" by geom2                                                                                                                                                                      |
| [`ST_Covers`](#st_covers)                                     | Returns if geom1 "covers" geom2                                                                                                                                                                                  |
| [`ST_Crosses`](#st_crosses)                                   | Returns true if geom1 "crosses" geom2                                                                                                                                                                            |
| [`ST_DWithin`](#st_dwithin)                                   | Returns if two geometries are within a target distance of each-other                                                                                                                                             |
| [`ST_DWithin_Spheroid`](#st_dwithin_spheroid)                 | Returns if two POINT_2D's are within a target distance in meters, using an ellipsoidal model of the earths surface                                                                                               |
| [`ST_Difference`](#st_difference)                             | Returns the "difference" between two geometries                                                                                                                                                                  |
| [`ST_Dimension`](#st_dimension)                               | Returns the dimension of a geometry.                                                                                                                                                                             |
| [`ST_Disjoint`](#st_disjoint)                                 | Returns if two geometries are disjoint                                                                                                                                                                           |
| [`ST_Distance`](#st_distance)                                 | Returns the distance between two geometries.                                                                                                                                                                     |
| [`ST_Distance_Sphere`](#st_distance_sphere)                   | Returns the haversine distance between two geometries.                                                                                                                                                           |
| [`ST_Distance_Spheroid`](#st_distance_spheroid)               | Returns the distance between two geometries in meters using a ellipsoidal model of the earths surface                                                                                                            |
| [`ST_Dump`](#st_dump)                                         | Dumps a geometry into a list of sub-geometries and their "path" in the original geometry.                                                                                                                        |
| [`ST_EndPoint`](#st_endpoint)                                 | Returns the last point of a line.                                                                                                                                                                                |
| [`ST_Envelope`](#st_envelope)                                 | Returns the minimum bounding box for the input geometry as a polygon geometry.                                                                                                                                   |
| [`ST_Equals`](#st_equals)                                     | Compares two geometries for equality                                                                                                                                                                             |
| [`ST_Extent`](#st_extent)                                     | Returns the minimal bounding box enclosing the input geometry                                                                                                                                                    |
| [`ST_ExteriorRing`](#st_exteriorring)                         | Returns the exterior ring (shell) of a polygon geometry.                                                                                                                                                         |
| [`ST_FlipCoordinates`](#st_flipcoordinates)                   | Returns a new geometry with the coordinates of the input geometry "flipped" so that x = y and y = x.                                                                                                             |
| [`ST_Force2D`](#st_force2d)                                   | Forces the vertices of a geometry to have X and Y components                                                                                                                                                     |
| [`ST_Force3DM`](#st_force3dm)                                 | Forces the vertices of a geometry to have X, Y and M components                                                                                                                                                  |
| [`ST_Force3DZ`](#st_force3dz)                                 | Forces the vertices of a geometry to have X, Y and Z components                                                                                                                                                  |
| [`ST_Force4D`](#st_force4d)                                   | Forces the vertices of a geometry to have X, Y, Z and M components                                                                                                                                               |
| [`ST_GeomFromGeoJSON`](#st_geomfromgeojson)                   | Deserializes a GEOMETRY from a GeoJSON fragment.                                                                                                                                                                 |
| [`ST_GeomFromHEXEWKB`](#st_geomfromhexewkb)                   | Deserialize a GEOMETRY from a HEXEWKB encoded string                                                                                                                                                             |
| [`ST_GeomFromHEXWKB`](#st_geomfromhexwkb)                     | Creates a GEOMETRY from a HEXWKB string                                                                                                                                                                          |
| [`ST_GeomFromText`](#st_geomfromtext)                         | Deserializes a GEOMETRY from a WKT string, optionally ignoring invalid geometries                                                                                                                                |
| [`ST_GeomFromWKB`](#st_geomfromwkb)                           | Deserializes a GEOMETRY from a WKB encoded blob                                                                                                                                                                  |
| [`ST_GeometryType`](#st_geometrytype)                         | Returns a 'GEOMETRY_TYPE' enum identifying the input geometry type. Possible enum return types are: `POINT`, `LINESTRING`, `POLYGON`, `MULTIPOINT`, `MULTILINESTRING`, `MULTIPOLYGON`, and `GEOMETRYCOLLECTION`. |
| [`ST_HasM`](#st_hasm)                                         | Check if the input geometry has M values.                                                                                                                                                                        |
| [`ST_HasZ`](#st_hasz)                                         | Check if the input geometry has Z values.                                                                                                                                                                        |
| [`ST_Hilbert`](#st_hilbert)                                   | Encodes the X and Y values as the hilbert curve index for a curve covering the given bounding box.                                                                                                               |
| [`ST_Intersection`](#st_intersection)                         | Returns the "intersection" of geom1 and geom2                                                                                                                                                                    |
| [`ST_Intersects`](#st_intersects)                             | Returns true if two geometries intersects                                                                                                                                                                        |
| [`ST_Intersects_Extent`](#st_intersects_extent)               | Returns true if the extent of two geometries intersects                                                                                                                                                          |
| [`ST_IsClosed`](#st_isclosed)                                 | Returns true if a geometry is "closed"                                                                                                                                                                           |
| [`ST_IsEmpty`](#st_isempty)                                   | Returns true if the geometry is "empty"                                                                                                                                                                          |
| [`ST_IsRing`](#st_isring)                                     | Returns true if the input line geometry is a ring (both ST_IsClosed and ST_IsSimple).                                                                                                                            |
| [`ST_IsSimple`](#st_issimple)                                 | Returns true if the input geometry is "simple"                                                                                                                                                                   |
| [`ST_IsValid`](#st_isvalid)                                   | Returns true if the geometry is topologically "valid"                                                                                                                                                            |
| [`ST_Length`](#st_length)                                     | Returns the length of the input line geometry                                                                                                                                                                    |
| [`ST_Length_Spheroid`](#st_length_spheroid)                   | Returns the length of the input geometry in meters, using a ellipsoidal model of the earth                                                                                                                       |
| [`ST_LineMerge`](#st_linemerge)                               | "Merges" the input line geometry, optionally taking direction into account.                                                                                                                                      |
| [`ST_M`](#st_m)                                               | Returns the M value of a point geometry, or NULL if not a point or empty                                                                                                                                         |
| [`ST_MMax`](#st_mmax)                                         | Returns the maximum M value of a geometry                                                                                                                                                                        |
| [`ST_MMin`](#st_mmin)                                         | Returns the minimum M value of a geometry                                                                                                                                                                        |
| [`ST_MakeEnvelope`](#st_makeenvelope)                         | Returns a minimal bounding box polygon enclosing the input geometry                                                                                                                                              |
| [`ST_MakeLine`](#st_makeline)                                 | Creates a LINESTRING geometry from a pair or list of input points                                                                                                                                                |
| [`ST_MakePolygon`](#st_makepolygon)                           | Creates a polygon from a shell geometry and an optional set of holes                                                                                                                                             |
| [`ST_MakeValid`](#st_makevalid)                               | Attempts to make an invalid geometry valid without removing any vertices                                                                                                                                         |
| [`ST_MinimumRotatedRectangle`](#st_minimumrotatedrectangle)   | Returns the minimum rotated rectangle that bounds the input geometry                                                                                                                                         |
| [`ST_Multi`](#st_multi)                                       | Turns a single geometry into a multi geometry.                                                                                                                                                                   |
| [`ST_NGeometries`](#st_ngeometries)                           | Returns the number of component geometries in a collection geometry.                                                                                                                                             |
| [`ST_NInteriorRings`](#st_ninteriorrings)                     | Returns the number if interior rings of a polygon                                                                                                                                                                |
| [`ST_NPoints`](#st_npoints)                                   | Returns the number of vertices within a geometry                                                                                                                                                                 |
| [`ST_Normalize`](#st_normalize)                               | Returns a "normalized" version of the input geometry.                                                                                                                                                            |
| [`ST_NumGeometries`](#st_numgeometries)                       | Returns the number of component geometries in a collection geometry.                                                                                                                                             |
| [`ST_NumInteriorRings`](#st_numinteriorrings)                 | Returns the number if interior rings of a polygon                                                                                                                                                                |
| [`ST_NumPoints`](#st_numpoints)                               | Returns the number of vertices within a geometry                                                                                                                                                                 |
| [`ST_Overlaps`](#st_overlaps)                                 | Returns true if geom1 "overlaps" geom2                                                                                                                                                                           |
| [`ST_Perimeter`](#st_perimeter)                               | Returns the length of the perimeter of the geometry                                                                                                                                                              |
| [`ST_Perimeter_Spheroid`](#st_perimeter_spheroid)             | Returns the length of the perimeter in meters using an ellipsoidal model of the earths surface                                                                                                                   |
| [`ST_Point`](#st_point)                                       | Creates a GEOMETRY point                                                                                                                                                                                         |
| [`ST_Point2D`](#st_point2d)                                   | Creates a POINT_2D                                                                                                                                                                                               |
| [`ST_Point3D`](#st_point3d)                                   | Creates a POINT_3D                                                                                                                                                                                               |
| [`ST_Point4D`](#st_point4d)                                   | Creates a POINT_4D                                                                                                                                                                                               |
| [`ST_PointN`](#st_pointn)                                     | Returns the n'th vertex from the input geometry as a point geometry                                                                                                                                              |
| [`ST_PointOnSurface`](#st_pointonsurface)                     | Returns a point that is guaranteed to be on the surface of the input geometry. Sometimes a useful alternative to ST_Centroid.                                                                                    |
| [`ST_Points`](#st_points)                                     | Collects all the vertices in the geometry into a multipoint                                                                                                                                                      |
| [`ST_QuadKey`](#st_quadkey)                                   | Compute the [quadkey](https://learn.microsoft.com/en-us/bingmaps/articles/bing-maps-tile-system) for a given lon/lat point at a given level.                                                                     |
| [`ST_ReducePrecision`](#st_reduceprecision)                   | Returns the geometry with all vertices reduced to the target precision                                                                                                                                           |
| [`ST_RemoveRepeatedPoints`](#st_removerepeatedpoints)         | Returns a new geometry with repeated points removed, optionally within a target distance of eachother.                                                                                                           |
| [`ST_Reverse`](#st_reverse)                                   | Returns a new version of the input geometry with the order of its vertices reversed                                                                                                                              |
| [`ST_ShortestLine`](#st_shortestline)                         | Returns the line between the two closest points between geom1 and geom2                                                                                                                                          |
| [`ST_Simplify`](#st_simplify)                                 | Simplifies the input geometry by collapsing edges smaller than 'distance'                                                                                                                                        |
| [`ST_SimplifyPreserveTopology`](#st_simplifypreservetopology) | Returns a simplified geometry but avoids creating invalid topologies                                                                                                                                             |
| [`ST_StartPoint`](#st_startpoint)                             | Returns the first point of a line geometry                                                                                                                                                                       |
| [`ST_Touches`](#st_touches)                                   | Returns true if geom1 "touches" geom2                                                                                                                                                                            |
| [`ST_Transform`](#st_transform)                               | Transforms a geometry between two coordinate systems                                                                                                                                                             |
| [`ST_Union`](#st_union)                                       | Returns the union of two geometries.                                                                                                                                                                             |
| [`ST_VoronoiDiagram`](#st_voronoidiagram)                     | Returns the Voronoi diagram of the supplied MultiPoint geometry                                                                                                                                                                          |
| [`ST_Within`](#st_within)                                     | Returns true if geom1 is "within" geom2                                                                                                                                                                          |
| [`ST_X`](#st_x)                                               | Returns the X value of a point geometry, or NULL if not a point or empty                                                                                                                                         |
| [`ST_XMax`](#st_xmax)                                         | Returns the maximum X value of a geometry                                                                                                                                                                        |
| [`ST_XMin`](#st_xmin)                                         | Returns the minimum X value of a geometry                                                                                                                                                                        |
| [`ST_Y`](#st_y)                                               | Returns the Y value of a point geometry, or NULL if not a point or empty                                                                                                                                         |
| [`ST_YMax`](#st_ymax)                                         | Returns the maximum Y value of a geometry                                                                                                                                                                        |
| [`ST_YMin`](#st_ymin)                                         | Returns the minimum Y value of a geometry                                                                                                                                                                        |
| [`ST_Z`](#st_z)                                               | Returns the Z value of a point geometry, or NULL if not a point or empty                                                                                                                                         |
| [`ST_ZMFlag`](#st_zmflag)                                     | Returns a flag indicating the presence of Z and M values in the input geometry.                                                                                                                                  |
| [`ST_ZMax`](#st_zmax)                                         | Returns the maximum Z value of a geometry                                                                                                                                                                        |
| [`ST_ZMin`](#st_zmin)                                         | Returns the minimum Z value of a geometry                                                                                                                                                                        |

**[Aggregate Functions](#aggregate-functions)**

| Function | Summary |
| --- | --- |
| [`ST_Envelope_Agg`](#st_envelope_agg) | Alias for [ST_Extent_Agg](#st_extent_agg). |
| [`ST_Extent_Agg`](#st_extent_agg) | Computes the minimal-bounding-box polygon containing the set of input geometries |
| [`ST_Intersection_Agg`](#st_intersection_agg) | Computes the intersection of a set of geometries |
| [`ST_Union_Agg`](#st_union_agg) | Computes the union of a set of input geometries |

**[Table Functions](#table-functions)**

| Function | Summary |
| --- | --- |
| [`ST_Drivers`](#st_drivers) | Returns the list of supported GDAL drivers and file formats |
| [`ST_Read`](#st_read) | Read and import a variety of geospatial file formats using the GDAL library. |
| [`ST_ReadOSM`](#st_readosm) | The `ST_ReadOsm()` table function enables reading compressed OpenStreetMap data directly from a `.osm.pbf file.` |
| [`ST_Read_Meta`](#st_read_meta) | Read the metadata from a variety of geospatial file formats using the GDAL library. |

----

## Scalar Functions

### ST_Area


#### Signatures

```sql
DOUBLE ST_Area (col0 POINT_2D)
DOUBLE ST_Area (col0 LINESTRING_2D)
DOUBLE ST_Area (col0 POLYGON_2D)
DOUBLE ST_Area (col0 GEOMETRY)
DOUBLE ST_Area (col0 BOX_2D)
```

#### Description

Compute the area of a geometry.

Returns `0.0` for any geometry that is not a `POLYGON`, `MULTIPOLYGON` or `GEOMETRYCOLLECTION` containing polygon geometries.
	The area is in the same units as the spatial reference system of the geometry.

The `POINT_2D` and `LINESTRING_2D` overloads of this function always return `0.0` but are included for completeness.

#### Example

```sql
select ST_Area('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::geometry);
	-- 1.0
```

----

### ST_Area_Spheroid


#### Signatures

```sql
DOUBLE ST_Area_Spheroid (col0 POLYGON_2D)
DOUBLE ST_Area_Spheroid (col0 GEOMETRY)
```

#### Description

Returns the area of a geometry in meters, using an ellipsoidal model of the earth

The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the area is returned in square meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library, calculating the area using an ellipsoidal model of the earth. This is a highly accurate method for calculating the area of a polygon taking the curvature of the earth into account, but is also the slowest.

Returns `0.0` for any geometry that is not a `POLYGON`, `MULTIPOLYGON` or `GEOMETRYCOLLECTION` containing polygon geometries.

----

### ST_AsGeoJSON


#### Signature

```sql
JSON ST_AsGeoJSON (col0 GEOMETRY)
```

#### Description

Returns the geometry as a GeoJSON fragment

This does not return a complete GeoJSON document, only the geometry fragment. To construct a complete GeoJSON document or feature, look into using the DuckDB JSON extension in conjunction with this function.
	This function supports geometries with Z values, but not M values.

#### Example

```sql
select ST_AsGeoJSON('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::geometry);
----
{"type":"Polygon","coordinates":[[[0.0,0.0],[0.0,1.0],[1.0,1.0],[1.0,0.0],[0.0,0.0]]]}

-- Convert a geometry into a full GeoJSON feature (requires the JSON extension to be loaded)
SELECT CAST({
	type: 'Feature', 
	geometry: ST_AsGeoJSON(ST_Point(1,2)), 
	properties: { 
		name: 'my_point' 
	} 
} AS JSON);
----
{"type":"Feature","geometry":{"type":"Point","coordinates":[1.0,2.0]},"properties":{"name":"my_point"}}
```

----

### ST_AsHEXWKB


#### Signature

```sql
VARCHAR ST_AsHEXWKB (col0 GEOMETRY)
```

#### Description

Returns the geometry as a HEXWKB string

#### Example

```sql
SELECT ST_AsHexWKB('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::geometry);
----
01030000000100000005000000000000000000000000000...
```

----

### ST_AsSVG


#### Signature

```sql
VARCHAR ST_AsSVG (col0 GEOMETRY, col1 BOOLEAN, col2 INTEGER)
```

#### Description

Convert the geometry into a SVG fragment or path

Convert the geometry into a SVG fragment or path
	The SVG fragment is returned as a string. The fragment is a path element that can be used in an SVG document.
	The second boolean argument specifies whether the path should be relative or absolute.
	The third argument specifies the maximum number of digits to use for the coordinates.

	Points are formatted as cx/cy using absolute coordinates or x/y using relative coordinates.

#### Example

```sql
SELECT ST_AsSVG('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::GEOMETRY, false, 15);
----
M 0 0 L 0 -1 1 -1 1 0 Z
```

----

### ST_AsText


#### Signatures

```sql
VARCHAR ST_AsText (col0 POINT_2D)
VARCHAR ST_AsText (col0 LINESTRING_2D)
VARCHAR ST_AsText (col0 POLYGON_2D)
VARCHAR ST_AsText (col0 BOX_2D)
VARCHAR ST_AsText (col0 GEOMETRY)
```

#### Description

Returns the geometry as a WKT string

#### Example

```sql
SELECT ST_AsText(ST_MakeEnvelope(0,0,1,1));
----
POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0))
```

----

### ST_AsWKB


#### Signature

```sql
WKB_BLOB ST_AsWKB (col0 GEOMETRY)
```

#### Description

Returns the geometry as a WKB blob

#### Example

```sql
SELECT ST_AsWKB('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::GEOMETRY)::BLOB;
----
\x01\x03\x00\x00\x00\x01\x00\x00\x00\x05...
```

----

### ST_Boundary


#### Signature

```sql
GEOMETRY ST_Boundary (col0 GEOMETRY)
```

#### Description

Returns the "boundary" of a geometry

----

### ST_Buffer


#### Signatures

```sql
GEOMETRY ST_Buffer (geom GEOMETRY, distance DOUBLE)
GEOMETRY ST_Buffer (geom GEOMETRY, distance DOUBLE, num_triangles INTEGER)
GEOMETRY ST_Buffer (geom GEOMETRY, distance DOUBLE, num_triangles INTEGER, cap_style VARCHAR, join_style VARCHAR, mitre_limit DOUBLE)
```

#### Description

Returns a buffer around the input geometry at the target distance

`geom` is the input geometry.

`distance` is the target distance for the buffer, using the same units as the input geometry.

`num_triangles` represents how many triangles that will be produced to approximate a quarter circle. The larger the number, the smoother the resulting geometry. The default value is 8.

`cap_style` must be one of "CAP_ROUND", "CAP_FLAT", "CAP_SQUARE". This parameter is case-insensitive.

`join_style` must be one of "JOIN_ROUND", "JOIN_MITRE", "JOIN_BEVEL". This parameter is case-insensitive.

`mitre_limit` only applies when `join_style` is "JOIN_MITRE". It is the ratio of the distance from the corner to the mitre point to the corner radius. The default value is 1.0.

This is a planar operation and will not take into account the curvature of the earth.

----

### ST_Centroid


#### Signatures

```sql
POINT_2D ST_Centroid (col0 POINT_2D)
POINT_2D ST_Centroid (col0 LINESTRING_2D)
POINT_2D ST_Centroid (col0 POLYGON_2D)
POINT_2D ST_Centroid (col0 BOX_2D)
POINT_2D ST_Centroid (col0 BOX_2DF)
GEOMETRY ST_Centroid (col0 GEOMETRY)
```

#### Description

Calculates the centroid of a geometry

#### Example

```sql
select st_centroid('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::geometry);
----
 POINT(0.5 0.5)
```

----

### ST_Collect


#### Signature

```sql
GEOMETRY ST_Collect (col0 GEOMETRY[])
```

#### Description

Collects a list of geometries into a collection geometry.
- If all geometries are `POINT`'s, a `MULTIPOINT` is returned.
- If all geometries are `LINESTRING`'s, a `MULTILINESTRING` is returned.
- If all geometries are `POLYGON`'s, a `MULTIPOLYGON` is returned.
- Otherwise if the input collection contains a mix of geometry types, a `GEOMETRYCOLLECTION` is returned.

Empty and `NULL` geometries are ignored. If all geometries are empty or `NULL`, a `GEOMETRYCOLLECTION EMPTY` is returned.

#### Example

```sql
-- With all POINT's, a MULTIPOINT is returned
SELECT ST_Collect([ST_Point(1, 2), ST_Point(3, 4)]);
----
MULTIPOINT (1 2, 3 4)

-- With mixed geometry types, a GEOMETRYCOLLECTION is returned
SELECT ST_Collect([ST_Point(1, 2), ST_GeomFromText('LINESTRING(3 4, 5 6)')]);
----
GEOMETRYCOLLECTION (POINT (1 2), LINESTRING (3 4, 5 6))

-- Note that the empty geometry is ignored, so the result is a MULTIPOINT
SELECT ST_Collect([ST_Point(1, 2), NULL, ST_GeomFromText('GEOMETRYCOLLECTION EMPTY')]);
----
MULTIPOINT (1 2)

-- If all geometries are empty or NULL, a GEOMETRYCOLLECTION EMPTY is returned
SELECT ST_Collect([NULL, ST_GeomFromText('GEOMETRYCOLLECTION EMPTY')]);
----
GEOMETRYCOLLECTION EMPTY

-- Tip: You can use the `ST_Collect` function together with the `list()` aggregate function to collect multiple rows of geometries into a single geometry collection:

CREATE TABLE points (geom GEOMETRY);

INSERT INTO points VALUES (ST_Point(1, 2)), (ST_Point(3, 4));

SELECT ST_Collect(list(geom)) FROM points;
----
MULTIPOINT (1 2, 3 4)
```

----

### ST_CollectionExtract


#### Signatures

```sql
GEOMETRY ST_CollectionExtract (geom GEOMETRY)
GEOMETRY ST_CollectionExtract (geom GEOMETRY, type INTEGER)
```

#### Description

Extracts geometries from a GeometryCollection into a typed multi geometry.

If the input geometry is a GeometryCollection, the function will return a multi geometry, determined by the `type` parameter.
- if `type` = 1, returns a MultiPoint containg all the Points in the collection
- if `type` = 2, returns a MultiLineString containg all the LineStrings in the collection
- if `type` = 3, returns a MultiPolygon containg all the Polygons in the collection

If no `type` parameters is provided, the function will return a multi geometry matching the highest "surface dimension"
of the contained geometries. E.g. if the collection contains only Points, a MultiPoint will be returned. But if the
collection contains both Points and LineStrings, a MultiLineString will be returned. Similarly, if the collection
contains Polygons, a MultiPolygon will be returned. Contained geometries of a lower surface dimension will be ignored.

If the input geometry contains nested GeometryCollections, their geometries will be extracted recursively and included
into the final multi geometry as well.

If the input geometry is not a GeometryCollection, the function will return the input geometry as is.

#### Example

```sql
select st_collectionextract('MULTIPOINT(1 2,3 4)'::geometry, 1);
-- MULTIPOINT (1 2, 3 4)
```

----

### ST_Contains


#### Signatures

```sql
BOOLEAN ST_Contains (geom1 POLYGON_2D, geom2 POINT_2D)
BOOLEAN ST_Contains (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if geom1 contains geom2.

#### Example

```sql
select st_contains('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::geometry, 'POINT(0.5 0.5)'::geometry);
----
true
```

----

### ST_ContainsProperly


#### Signature

```sql
BOOLEAN ST_ContainsProperly (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if geom1 "properly contains" geom2
----

### ST_ConcaveHull


#### Signature

```sql
GEOMETRY ST_ConcaveHull (col0 GEOMETRY, ratio DOUBLE, allowHoles BOOLEAN)
```

#### Description

Returns the concave hull of the input geometry, containing all of the source input's points, and which can be used to create polygons from points. The ratio parameter dictates the level of concavity; 1.0 returns the convex hull; and 0 indicates to return the most concave hull possible. Set allowHoles to a non-zero value to allow output containing holes.

----

### ST_ConvexHull


#### Signature

```sql
GEOMETRY ST_ConvexHull (col0 GEOMETRY)
```

#### Description

Returns the convex hull enclosing the geometry

----

### ST_CoveredBy


#### Signature

```sql
BOOLEAN ST_CoveredBy (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if geom1 is "covered" by geom2

----

### ST_Covers


#### Signature

```sql
BOOLEAN ST_Covers (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns if geom1 "covers" geom2

----

### ST_Crosses


#### Signature

```sql
BOOLEAN ST_Crosses (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if geom1 "crosses" geom2

----

### ST_DWithin


#### Signature

```sql
BOOLEAN ST_DWithin (col0 GEOMETRY, col1 GEOMETRY, col2 DOUBLE)
```

#### Description

Returns if two geometries are within a target distance of each-other

----

### ST_DWithin_Spheroid


#### Signature

```sql
BOOLEAN ST_DWithin_Spheroid (col0 POINT_2D, col1 POINT_2D, col2 DOUBLE)
```

#### Description

Returns if two POINT_2D's are within a target distance in meters, using an ellipsoidal model of the earths surface

	The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the distance is returned in meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library to solve the [inverse geodesic problem](https://en.wikipedia.org/wiki/Geodesics_on_an_ellipsoid#Solution_of_the_direct_and_inverse_problems), calculating the distance between two points using an ellipsoidal model of the earth. This is a highly accurate method for calculating the distance between two arbitrary points taking the curvature of the earths surface into account, but is also the slowest.

----

### ST_Difference


#### Signature

```sql
GEOMETRY ST_Difference (col0 GEOMETRY, col1 GEOMETRY)
```

#### Description

Returns the "difference" between two geometries

----

### ST_Dimension


#### Signature

```sql
INTEGER ST_Dimension (col0 GEOMETRY)
```

#### Description

Returns the dimension of a geometry.

#### Example

```sql
select st_dimension('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::geometry);
----
2
```

----

### ST_Disjoint


#### Signature

```sql
BOOLEAN ST_Disjoint (col0 GEOMETRY, col1 GEOMETRY)
```

#### Description

Returns if two geometries are disjoint

----

### ST_Distance


#### Signatures

```sql
DOUBLE ST_Distance (col0 POINT_2D, col1 POINT_2D)
DOUBLE ST_Distance (col0 POINT_2D, col1 LINESTRING_2D)
DOUBLE ST_Distance (col0 LINESTRING_2D, col1 POINT_2D)
DOUBLE ST_Distance (col0 GEOMETRY, col1 GEOMETRY)
```

#### Description

Returns the distance between two geometries.

#### Example

```sql
select st_distance('POINT(0 0)'::geometry, 'POINT(1 1)'::geometry);
----
1.4142135623731
```

----

### ST_Distance_Sphere


#### Signatures

```sql
DOUBLE ST_Distance_Sphere (col0 POINT_2D, col1 POINT_2D)
DOUBLE ST_Distance_Sphere (col0 GEOMETRY, col1 GEOMETRY)
```

#### Description

Returns the haversine distance between two geometries.

- Only supports POINT geometries.
- Returns the distance in meters.
- The input is expected to be in WGS84 (EPSG:4326) coordinates, using a [latitude, longitude] axis order.

----

### ST_Distance_Spheroid


#### Signature

```sql
DOUBLE ST_Distance_Spheroid (col0 POINT_2D, col1 POINT_2D)
```

#### Description

Returns the distance between two geometries in meters using a ellipsoidal model of the earths surface

The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the distance limit is expected to be in meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library to solve the [inverse geodesic problem](https://en.wikipedia.org/wiki/Geodesics_on_an_ellipsoid#Solution_of_the_direct_and_inverse_problems), calculating the distance between two points using an ellipsoidal model of the earth. This is a highly accurate method for calculating the distance between two arbitrary points taking the curvature of the earths surface into account, but is also the slowest.

#### Example

```sql
-- Note: the coordinates are in WGS84 and [latitude, longitude] axis order
-- Whats the distance between New York and Amsterdam (JFK and AMS airport)?
SELECT st_distance_spheroid(
st_point(40.6446, -73.7797),
st_point(52.3130, 4.7725)
);
----
5863418.7459356235
-- Roughly 5863km!
```

----

### ST_Dump


#### Signature

```sql
STRUCT(geom GEOMETRY, path INTEGER[])[] ST_Dump (col0 GEOMETRY)
```

#### Description

Dumps a geometry into a list of sub-geometries and their "path" in the original geometry.

#### Example

```sql
select st_dump('MULTIPOINT(1 2,3 4)'::geometry);
----
[{'geom': 'POINT(1 2)', 'path': [0]}, {'geom': 'POINT(3 4)', 'path': [1]}]
```

----

### ST_EndPoint


#### Signatures

```sql
GEOMETRY ST_EndPoint (col0 GEOMETRY)
POINT_2D ST_EndPoint (col0 LINESTRING_2D)
```

#### Description

Returns the last point of a line.

#### Example

```sql
select ST_EndPoint('LINESTRING(0 0, 1 1)'::geometry);
-- POINT(1 1)
```

----

### ST_Envelope


#### Signature

```sql
GEOMETRY ST_Envelope (col0 GEOMETRY)
```

#### Description

Returns the minimum bounding box for the input geometry as a polygon geometry.

----

### ST_Equals


#### Signature

```sql
BOOLEAN ST_Equals (col0 GEOMETRY, col1 GEOMETRY)
```

#### Description

Compares two geometries for equality

----

### ST_Extent


#### Signatures

```sql
BOX_2D ST_Extent (col0 GEOMETRY)
BOX_2D ST_Extent (col0 WKB_BLOB)
```

#### Description

Returns the minimal bounding box enclosing the input geometry

----

### ST_ExteriorRing


#### Signatures

```sql
LINESTRING_2D ST_ExteriorRing (col0 POLYGON_2D)
GEOMETRY ST_ExteriorRing (col0 GEOMETRY)
```

#### Description

Returns the exterior ring (shell) of a polygon geometry.

----

### ST_FlipCoordinates


#### Signatures

```sql
POINT_2D ST_FlipCoordinates (col0 POINT_2D)
LINESTRING_2D ST_FlipCoordinates (col0 LINESTRING_2D)
POLYGON_2D ST_FlipCoordinates (col0 POLYGON_2D)
BOX_2D ST_FlipCoordinates (col0 BOX_2D)
GEOMETRY ST_FlipCoordinates (col0 GEOMETRY)
```

#### Description

Returns a new geometry with the coordinates of the input geometry "flipped" so that x = y and y = x.

----

### ST_Force2D


#### Signature

```sql
GEOMETRY ST_Force2D (col0 GEOMETRY)
```

#### Description

Forces the vertices of a geometry to have X and Y components

This function will drop any Z and M values from the input geometry, if present. If the input geometry is already 2D, it will be returned as is.

----

### ST_Force3DM


#### Signature

```sql
GEOMETRY ST_Force3DM (col0 GEOMETRY, col1 DOUBLE)
```

#### Description

Forces the vertices of a geometry to have X, Y and M components

The following cases apply:
- If the input geometry has a Z component but no M component, the Z component will be replaced with the new M value.
- If the input geometry has a M component but no Z component, it will be returned as is.
- If the input geometry has both a Z component and a M component, the Z component will be removed.
- Otherwise, if the input geometry has neither a Z or M component, the new M value will be added to the vertices of the input geometry.

----

### ST_Force3DZ


#### Signature

```sql
GEOMETRY ST_Force3DZ (col0 GEOMETRY, col1 DOUBLE)
```

#### Description

Forces the vertices of a geometry to have X, Y and Z components

The following cases apply:
- If the input geometry has a M component but no Z component, the M component will be replaced with the new Z value.
- If the input geometry has a Z component but no M component, it will be returned as is.
- If the input geometry has both a Z component and a M component, the M component will be removed.
- Otherwise, if the input geometry has neither a Z or M component, the new Z value will be added to the vertices of the input geometry.

----

### ST_Force4D


#### Signature

```sql
GEOMETRY ST_Force4D (col0 GEOMETRY, col1 DOUBLE, col2 DOUBLE)
```

#### Description

Forces the vertices of a geometry to have X, Y, Z and M components

The following cases apply:
- If the input geometry has a Z component but no M component, the new M value will be added to the vertices of the input geometry.
- If the input geometry has a M component but no Z component, the new Z value will be added to the vertices of the input geometry.
- If the input geometry has both a Z component and a M component, the geometry will be returned as is.
- Otherwise, if the input geometry has neither a Z or M component, the new Z and M values will be added to the vertices of the input geometry.

----

### ST_GeomFromGeoJSON


#### Signatures

```sql
GEOMETRY ST_GeomFromGeoJSON (col0 VARCHAR)
GEOMETRY ST_GeomFromGeoJSON (col0 JSON)
```

#### Description

Deserializes a GEOMETRY from a GeoJSON fragment.

#### Example

```sql
SELECT ST_GeomFromGeoJSON('{"type":"Point","coordinates":[1.0,2.0]}');
----
POINT (1 2)
```

----

### ST_GeomFromHEXEWKB


#### Signature

```sql
GEOMETRY ST_GeomFromHEXEWKB (col0 VARCHAR)
```

#### Description

Deserialize a GEOMETRY from a HEXEWKB encoded string

----

### ST_GeomFromHEXWKB


#### Signature

```sql
GEOMETRY ST_GeomFromHEXWKB (col0 VARCHAR)
```

#### Description

Creates a GEOMETRY from a HEXWKB string

----

### ST_GeomFromText


#### Signatures

```sql
GEOMETRY ST_GeomFromText (col0 VARCHAR)
GEOMETRY ST_GeomFromText (col0 VARCHAR, col1 BOOLEAN)
```

#### Description

Deserializes a GEOMETRY from a WKT string, optionally ignoring invalid geometries

----

### ST_GeomFromWKB


#### Signatures

```sql
GEOMETRY ST_GeomFromWKB (col0 WKB_BLOB)
GEOMETRY ST_GeomFromWKB (col0 BLOB)
```

#### Description

Deserializes a GEOMETRY from a WKB encoded blob

----

### ST_GeometryType


#### Signatures

```sql
ANY ST_GeometryType (col0 POINT_2D)
ANY ST_GeometryType (col0 LINESTRING_2D)
ANY ST_GeometryType (col0 POLYGON_2D)
ANY ST_GeometryType (col0 GEOMETRY)
ANY ST_GeometryType (col0 WKB_BLOB)
```

#### Description

Returns a 'GEOMETRY_TYPE' enum identifying the input geometry type. Possible enum return types are: `POINT`, `LINESTRING`, `POLYGON`, `MULTIPOINT`, `MULTILINESTRING`, `MULTIPOLYGON`, and `GEOMETRYCOLLECTION`.

#### Example

```sql
SELECT DISTINCT ST_GeometryType(ST_GeomFromText('POINT(1 1)'));
----
POINT
```

----

### ST_HasM


#### Signatures

```sql
BOOLEAN ST_HasM (col0 GEOMETRY)
BOOLEAN ST_HasM (col0 WKB_BLOB)
```

#### Description

Check if the input geometry has M values.

#### Example

```sql
-- HasM for a 2D geometry
SELECT ST_HasM(ST_GeomFromText('POINT(1 1)'));
----
false

-- HasM for a 3DZ geometry
SELECT ST_HasM(ST_GeomFromText('POINT Z(1 1 1)'));
----
false

-- HasM for a 3DM geometry
SELECT ST_HasM(ST_GeomFromText('POINT M(1 1 1)'));
----
true

-- HasM for a 4D geometry
SELECT ST_HasM(ST_GeomFromText('POINT ZM(1 1 1 1)'));
----
true
```

----

### ST_HasZ


#### Signatures

```sql
BOOLEAN ST_HasZ (col0 GEOMETRY)
BOOLEAN ST_HasZ (col0 WKB_BLOB)
```

#### Description

Check if the input geometry has Z values.

#### Example

```sql
-- HasZ for a 2D geometry
SELECT ST_HasZ(ST_GeomFromText('POINT(1 1)'));
----
false

-- HasZ for a 3DZ geometry
SELECT ST_HasZ(ST_GeomFromText('POINT Z(1 1 1)'));
----
true

-- HasZ for a 3DM geometry
SELECT ST_HasZ(ST_GeomFromText('POINT M(1 1 1)'));
----
false

-- HasZ for a 4D geometry
SELECT ST_HasZ(ST_GeomFromText('POINT ZM(1 1 1 1)'));
----
true
```

----

### ST_Hilbert


#### Signatures

```sql
UINTEGER ST_Hilbert (col0 DOUBLE, col1 DOUBLE, col2 BOX_2D)
UINTEGER ST_Hilbert (col0 GEOMETRY, col1 BOX_2D)
UINTEGER ST_Hilbert (col0 BOX_2D, col1 BOX_2D)
UINTEGER ST_Hilbert (col0 BOX_2DF, col1 BOX_2DF)
UINTEGER ST_Hilbert (col0 GEOMETRY)
```

#### Description

Encodes the X and Y values as the hilbert curve index for a curve covering the given bounding box.
If a geometry is provided, the center of the approximate bounding box is used as the point to encode.
If no bounding box is provided, the hilbert curve index is mapped to the full range of a single-presicion float.
For the BOX_2D and BOX_2DF variants, the center of the box is used as the point to encode.

----

### ST_Intersection


#### Signature

```sql
GEOMETRY ST_Intersection (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns the "intersection" of geom1 and geom2

----

### ST_Intersects


#### Signatures

```sql
BOOLEAN ST_Intersects (col0 BOX_2D, col1 BOX_2D)
BOOLEAN ST_Intersects (col0 GEOMETRY, col1 GEOMETRY)
```

#### Description

Returns true if two geometries intersects

----

### ST_Intersects_Extent


#### Signature

```sql
BOOLEAN ST_Intersects_Extent (col0 GEOMETRY, col1 GEOMETRY)
```

#### Description

Returns true if the extent of two geometries intersects

----

### ST_IsClosed


#### Signature

```sql
BOOLEAN ST_IsClosed (col0 GEOMETRY)
```

#### Description

Returns true if a geometry is "closed"

----

### ST_IsEmpty


#### Signatures

```sql
BOOLEAN ST_IsEmpty (col0 LINESTRING_2D)
BOOLEAN ST_IsEmpty (col0 POLYGON_2D)
BOOLEAN ST_IsEmpty (col0 GEOMETRY)
```

#### Description

Returns true if the geometry is "empty"

----

### ST_IsRing


#### Signature

```sql
BOOLEAN ST_IsRing (col0 GEOMETRY)
```

#### Description

Returns true if the input line geometry is a ring (both ST_IsClosed and ST_IsSimple).

----

### ST_IsSimple


#### Signature

```sql
BOOLEAN ST_IsSimple (col0 GEOMETRY)
```

#### Description

Returns true if the input geometry is "simple"

----

### ST_IsValid


#### Signature

```sql
BOOLEAN ST_IsValid (col0 GEOMETRY)
```

#### Description

Returns true if the geometry is topologically "valid"

----

### ST_Length


#### Signatures

```sql
DOUBLE ST_Length (col0 LINESTRING_2D)
DOUBLE ST_Length (col0 GEOMETRY)
```

#### Description

Returns the length of the input line geometry

----

### ST_Length_Spheroid


#### Signatures

```sql
DOUBLE ST_Length_Spheroid (col0 LINESTRING_2D)
DOUBLE ST_Length_Spheroid (col0 GEOMETRY)
```

#### Description

Returns the length of the input geometry in meters, using a ellipsoidal model of the earth

The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the length is returned in square meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library, calculating the length using an ellipsoidal model of the earth. This is a highly accurate method for calculating the length of a line geometry taking the curvature of the earth into account, but is also the slowest.

Returns `0.0` for any geometry that is not a `LINESTRING`, `MULTILINESTRING` or `GEOMETRYCOLLECTION` containing line geometries.

----

### ST_LineMerge


#### Signatures

```sql
GEOMETRY ST_LineMerge (col0 GEOMETRY)
GEOMETRY ST_LineMerge (col0 GEOMETRY, col1 BOOLEAN)
```

#### Description

"Merges" the input line geometry, optionally taking direction into account.

----

### ST_M


#### Signature

```sql
DOUBLE ST_M (col0 GEOMETRY)
```

#### Description

Returns the M value of a point geometry, or NULL if not a point or empty

----

### ST_MMax


#### Signature

```sql
DOUBLE ST_MMax (col0 GEOMETRY)
```

#### Description

Returns the maximum M value of a geometry

----

### ST_MMin


#### Signature

```sql
DOUBLE ST_MMin (col0 GEOMETRY)
```

#### Description

Returns the minimum M value of a geometry

----

### ST_MinimumRotatedRectangle


#### Signature

```sql
GEOMETRY ST_MinimumRotatedRectangle (col0 GEOMETRY)
```

#### Description

Returns the minimum rotated rectangle that bounds the input geometry, finding the surrounding box that has the lowest area by using a rotated rectangle, rather than taking the lowest and highest coordinate values as per ST_Envelope().

----

### ST_MakeEnvelope


#### Signature

```sql
GEOMETRY ST_MakeEnvelope (min_x DOUBLE, min_y DOUBLE, max_x DOUBLE, max_y DOUBLE)
```

#### Description

Returns a minimal bounding box polygon enclosing the input geometry

----

### ST_MakeLine


#### Signatures

```sql
GEOMETRY ST_MakeLine (col0 GEOMETRY[])
GEOMETRY ST_MakeLine (col0 GEOMETRY, col1 GEOMETRY)
```

#### Description

Creates a LINESTRING geometry from a pair or list of input points

----

### ST_MakePolygon


#### Signatures

```sql
GEOMETRY ST_MakePolygon (col0 GEOMETRY, col1 GEOMETRY[])
GEOMETRY ST_MakePolygon (col0 GEOMETRY)
```

#### Description

Creates a polygon from a shell geometry and an optional set of holes

----

### ST_MakeValid


#### Signature

```sql
GEOMETRY ST_MakeValid (col0 GEOMETRY)
```

#### Description

Attempts to make an invalid geometry valid without removing any vertices

----

### ST_Multi


#### Signature

```sql
GEOMETRY ST_Multi (col0 GEOMETRY)
```

#### Description

Turns a single geometry into a multi geometry.

If the geometry is already a multi geometry, it is returned as is.

#### Example

```sql
SELECT ST_Multi(ST_GeomFromText('POINT(1 2)'));
-- MULTIPOINT (1 2)

SELECT ST_Multi(ST_GeomFromText('LINESTRING(1 1, 2 2)'));
-- MULTILINESTRING ((1 1, 2 2))

SELECT ST_Multi(ST_GeomFromText('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'));
-- MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)))
```

----

### ST_NGeometries


#### Signature

```sql
INTEGER ST_NGeometries (col0 GEOMETRY)
```

#### Description

Returns the number of component geometries in a collection geometry.
If the input geometry is not a collection, this function returns 0 or 1 depending on if the geometry is empty or not.

----

### ST_NInteriorRings


#### Signatures

```sql
INTEGER ST_NInteriorRings (col0 POLYGON_2D)
INTEGER ST_NInteriorRings (col0 GEOMETRY)
```

#### Description

Returns the number if interior rings of a polygon

----

### ST_NPoints


#### Signatures

```sql
UBIGINT ST_NPoints (col0 POINT_2D)
UBIGINT ST_NPoints (col0 LINESTRING_2D)
UBIGINT ST_NPoints (col0 POLYGON_2D)
UBIGINT ST_NPoints (col0 BOX_2D)
UINTEGER ST_NPoints (col0 GEOMETRY)
```

#### Description

Returns the number of vertices within a geometry

----

### ST_Normalize


#### Signature

```sql
GEOMETRY ST_Normalize (col0 GEOMETRY)
```

#### Description

Returns a "normalized" version of the input geometry.

----

### ST_NumGeometries


#### Signature

```sql
INTEGER ST_NumGeometries (col0 GEOMETRY)
```

#### Description

Returns the number of component geometries in a collection geometry.
If the input geometry is not a collection, this function returns 0 or 1 depending on if the geometry is empty or not.

----

### ST_NumInteriorRings


#### Signatures

```sql
INTEGER ST_NumInteriorRings (col0 POLYGON_2D)
INTEGER ST_NumInteriorRings (col0 GEOMETRY)
```

#### Description

Returns the number if interior rings of a polygon

----

### ST_NumPoints


#### Signatures

```sql
UBIGINT ST_NumPoints (col0 POINT_2D)
UBIGINT ST_NumPoints (col0 LINESTRING_2D)
UBIGINT ST_NumPoints (col0 POLYGON_2D)
UBIGINT ST_NumPoints (col0 BOX_2D)
UINTEGER ST_NumPoints (col0 GEOMETRY)
```

#### Description

Returns the number of vertices within a geometry

----

### ST_Overlaps


#### Signature

```sql
BOOLEAN ST_Overlaps (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if geom1 "overlaps" geom2

----

### ST_Perimeter


#### Signatures

```sql
DOUBLE ST_Perimeter (col0 BOX_2D)
DOUBLE ST_Perimeter (col0 POLYGON_2D)
DOUBLE ST_Perimeter (col0 GEOMETRY)
```

#### Description

Returns the length of the perimeter of the geometry

----

### ST_Perimeter_Spheroid


#### Signatures

```sql
DOUBLE ST_Perimeter_Spheroid (col0 POLYGON_2D)
DOUBLE ST_Perimeter_Spheroid (col0 GEOMETRY)
```

#### Description

Returns the length of the perimeter in meters using an ellipsoidal model of the earths surface

The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the length is returned in meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library, calculating the perimeter using an ellipsoidal model of the earth. This is a highly accurate method for calculating the perimeter of a polygon taking the curvature of the earth into account, but is also the slowest.

Returns `0.0` for any geometry that is not a `POLYGON`, `MULTIPOLYGON` or `GEOMETRYCOLLECTION` containing polygon geometries.

----

### ST_Point


#### Signature

```sql
GEOMETRY ST_Point (x DOUBLE, y DOUBLE)
```

#### Description

Creates a GEOMETRY point

----

### ST_Point2D


#### Signature

```sql
POINT_2D ST_Point2D (x DOUBLE, y DOUBLE)
```

#### Description

Creates a POINT_2D

----

### ST_Point3D


#### Signature

```sql
POINT_3D ST_Point3D (x DOUBLE, y DOUBLE, z DOUBLE)
```

#### Description

Creates a POINT_3D

----

### ST_Point4D


#### Signature

```sql
POINT_4D ST_Point4D (x DOUBLE, y DOUBLE, z DOUBLE, m DOUBLE)
```

#### Description

Creates a POINT_4D

----

### ST_PointN


#### Signatures

```sql
GEOMETRY ST_PointN (col0 GEOMETRY, col1 INTEGER)
POINT_2D ST_PointN (col0 LINESTRING_2D, col1 INTEGER)
```

#### Description

Returns the n'th vertex from the input geometry as a point geometry

----

### ST_PointOnSurface


#### Signature

```sql
GEOMETRY ST_PointOnSurface (col0 GEOMETRY)
```

#### Description

Returns a point that is guaranteed to be on the surface of the input geometry. Sometimes a useful alternative to ST_Centroid.

----

### ST_Points


#### Signature

```sql
GEOMETRY ST_Points (col0 GEOMETRY)
```

#### Description

Collects all the vertices in the geometry into a multipoint

#### Example

```sql
select st_points('LINESTRING(1 1, 2 2)'::geometry);
----
MULTIPOINT (1 1, 2 2)

select st_points('MULTIPOLYGON Z EMPTY'::geometry);
----
MULTIPOINT Z EMPTY
```

----

### ST_QuadKey


#### Signatures

```sql
VARCHAR ST_QuadKey (lon DOUBLE, lat DOUBLE, level INTEGER)
VARCHAR ST_QuadKey (point GEOMETRY, level INTEGER)
```

#### Description

Compute the [quadkey](https://learn.microsoft.com/en-us/bingmaps/articles/bing-maps-tile-system) for a given lon/lat point at a given level.
Note that the parameter order is __longitude__, __latitude__.

`level` has to be between 1 and 23, inclusive.

The input coordinates will be clamped to the lon/lat bounds of the earth (longitude between -180 and 180, latitude between -85.05112878 and 85.05112878).

The geometry overload throws an error if the input geometry is not a `POINT`

#### Example

```sql
SELECT ST_QuadKey(st_point(11.08, 49.45), 10);
----
1333203202
```

----

### ST_ReducePrecision


#### Signature

```sql
GEOMETRY ST_ReducePrecision (col0 GEOMETRY, col1 DOUBLE)
```

#### Description

Returns the geometry with all vertices reduced to the target precision

----

### ST_RemoveRepeatedPoints


#### Signatures

```sql
LINESTRING_2D ST_RemoveRepeatedPoints (col0 LINESTRING_2D)
LINESTRING_2D ST_RemoveRepeatedPoints (col0 LINESTRING_2D, col1 DOUBLE)
GEOMETRY ST_RemoveRepeatedPoints (col0 GEOMETRY)
GEOMETRY ST_RemoveRepeatedPoints (col0 GEOMETRY, col1 DOUBLE)
```

#### Description

Returns a new geometry with repeated points removed, optionally within a target distance of eachother.

----

### ST_Reverse


#### Signature

```sql
GEOMETRY ST_Reverse (col0 GEOMETRY)
```

#### Description

Returns a new version of the input geometry with the order of its vertices reversed

----

### ST_ShortestLine


#### Signature

```sql
GEOMETRY ST_ShortestLine (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns the line between the two closest points between geom1 and geom2

----

### ST_Simplify


#### Signature

```sql
GEOMETRY ST_Simplify (col0 GEOMETRY, col1 DOUBLE)
```

#### Description

Simplifies the input geometry by collapsing edges smaller than 'distance'

----

### ST_SimplifyPreserveTopology


#### Signature

```sql
GEOMETRY ST_SimplifyPreserveTopology (col0 GEOMETRY, col1 DOUBLE)
```

#### Description

Returns a simplified geometry but avoids creating invalid topologies

----

### ST_StartPoint


#### Signatures

```sql
GEOMETRY ST_StartPoint (col0 GEOMETRY)
POINT_2D ST_StartPoint (col0 LINESTRING_2D)
```

#### Description

Returns the first point of a line geometry

#### Example

```sql
select ST_StartPoint('LINESTRING(0 0, 1 1)'::geometry);
-- POINT(0 0)
```

----

### ST_Touches


#### Signature

```sql
BOOLEAN ST_Touches (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if geom1 "touches" geom2

----

### ST_Transform


#### Signatures

```sql
BOX_2D ST_Transform (geom BOX_2D, source_crs VARCHAR, target_crs VARCHAR)
BOX_2D ST_Transform (geom BOX_2D, source_crs VARCHAR, target_crs VARCHAR, always_xy BOOLEAN)
POINT_2D ST_Transform (geom POINT_2D, source_crs VARCHAR, target_crs VARCHAR)
POINT_2D ST_Transform (geom POINT_2D, source_crs VARCHAR, target_crs VARCHAR, always_xy BOOLEAN)
GEOMETRY ST_Transform (geom GEOMETRY, source_crs VARCHAR, target_crs VARCHAR)
GEOMETRY ST_Transform (geom GEOMETRY, source_crs VARCHAR, target_crs VARCHAR, always_xy BOOLEAN)
```

#### Description

Transforms a geometry between two coordinate systems

The source and target coordinate systems can be specified using any format that the [PROJ library](https://proj.org) supports.

The third optional `always_xy` parameter can be used to force the input and output geometries to be interpreted as having a [easting, northing] coordinate axis order regardless of what the source and target coordinate system definition says. This is particularly useful when transforming to/from the [WGS84/EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (what most people think of when they hear "longitude"/"latitude" or "GPS coordinates"), which is defined as having a [latitude, longitude] axis order even though [longitude, latitude] is commonly used in practice (e.g. in [GeoJSON](https://tools.ietf.org/html/rfc7946)). More details available in the [PROJ documentation](https://proj.org/en/9.3/faq.html#why-is-the-axis-ordering-in-proj-not-consistent).

DuckDB spatial vendors its own static copy of the PROJ database of coordinate systems, so if you have your own installation of PROJ on your system the available coordinate systems may differ to what's available in other GIS software.

#### Example

```sql
-- Transform a geometry from EPSG:4326 to EPSG:3857 (WGS84 to WebMercator)
-- Note that since WGS84 is defined as having a [latitude, longitude] axis order
-- we follow the standard and provide the input geometry using that axis order,
-- but the output will be [easting, northing] because that is what's defined by
-- WebMercator.

SELECT ST_AsText(
    ST_Transform(
        st_point(52.373123, 4.892360),
        'EPSG:4326',
        'EPSG:3857'
    )
);
----
POINT (544615.0239773799 6867874.103539125)

-- Alternatively, let's say we got our input point from e.g. a GeoJSON file,
-- which uses WGS84 but with [longitude, latitude] axis order. We can use the
-- `always_xy` parameter to force the input geometry to be interpreted as having
-- a [northing, easting] axis order instead, even though the source coordinate
-- reference system definition (WGS84) says otherwise.

SELECT ST_AsText(
    ST_Transform(
        -- note the axis order is reversed here
        st_point(4.892360, 52.373123),
        'EPSG:4326',
        'EPSG:3857',
        always_xy := true
    )
);
----
POINT (544615.0239773799 6867874.103539125)
```

----

### ST_Union


#### Signature

```sql
GEOMETRY ST_Union (col0 GEOMETRY, col1 GEOMETRY)
```

#### Description

Returns the union of two geometries.

#### Example

```sql
SELECT ST_AsText(
    ST_Union(
        ST_GeomFromText('POINT(1 2)'),
        ST_GeomFromText('POINT(3 4)')
    )
);
----
MULTIPOINT (1 2, 3 4)
```

----

### ST_VoronoiDiagram


#### Signatures

```sql
GEOMETRY ST_VoronoiDiagram (geom0 MULTIPOINT)
```

#### Description

Returns the Voronoi diagram of the supplied MultiPoint geometry

----

### ST_Within


#### Signatures

```sql
BOOLEAN ST_Within (geom1 POINT_2D, geom2 POLYGON_2D)
BOOLEAN ST_Within (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if geom1 is "within" geom2

----

### ST_X


#### Signatures

```sql
DOUBLE ST_X (col0 POINT_2D)
DOUBLE ST_X (col0 GEOMETRY)
```

#### Description

Returns the X value of a point geometry, or NULL if not a point or empty

----

### ST_XMax


#### Signatures

```sql
DOUBLE ST_XMax (col0 BOX_2D)
FLOAT ST_XMax (col0 BOX_2DF)
DOUBLE ST_XMax (col0 POINT_2D)
DOUBLE ST_XMax (col0 LINESTRING_2D)
DOUBLE ST_XMax (col0 POLYGON_2D)
DOUBLE ST_XMax (col0 GEOMETRY)
```

#### Description

Returns the maximum X value of a geometry

----

### ST_XMin


#### Signatures

```sql
DOUBLE ST_XMin (col0 BOX_2D)
FLOAT ST_XMin (col0 BOX_2DF)
DOUBLE ST_XMin (col0 POINT_2D)
DOUBLE ST_XMin (col0 LINESTRING_2D)
DOUBLE ST_XMin (col0 POLYGON_2D)
DOUBLE ST_XMin (col0 GEOMETRY)
```

#### Description

Returns the minimum X value of a geometry

----

### ST_Y


#### Signatures

```sql
DOUBLE ST_Y (col0 POINT_2D)
DOUBLE ST_Y (col0 GEOMETRY)
```

#### Description

Returns the Y value of a point geometry, or NULL if not a point or empty

----

### ST_YMax


#### Signatures

```sql
DOUBLE ST_YMax (col0 BOX_2D)
FLOAT ST_YMax (col0 BOX_2DF)
DOUBLE ST_YMax (col0 POINT_2D)
DOUBLE ST_YMax (col0 LINESTRING_2D)
DOUBLE ST_YMax (col0 POLYGON_2D)
DOUBLE ST_YMax (col0 GEOMETRY)
```

#### Description

Returns the maximum Y value of a geometry

----

### ST_YMin


#### Signatures

```sql
DOUBLE ST_YMin (col0 BOX_2D)
FLOAT ST_YMin (col0 BOX_2DF)
DOUBLE ST_YMin (col0 POINT_2D)
DOUBLE ST_YMin (col0 LINESTRING_2D)
DOUBLE ST_YMin (col0 POLYGON_2D)
DOUBLE ST_YMin (col0 GEOMETRY)
```

#### Description

Returns the minimum Y value of a geometry

----

### ST_Z


#### Signature

```sql
DOUBLE ST_Z (col0 GEOMETRY)
```

#### Description

Returns the Z value of a point geometry, or NULL if not a point or empty

----

### ST_ZMFlag


#### Signatures

```sql
UTINYINT ST_ZMFlag (col0 GEOMETRY)
UTINYINT ST_ZMFlag (col0 WKB_BLOB)
```

#### Description

Returns a flag indicating the presence of Z and M values in the input geometry.
0 = No Z or M values
1 = M values only
2 = Z values only
3 = Z and M values

#### Example

```sql
-- ZMFlag for a 2D geometry
SELECT ST_ZMFlag(ST_GeomFromText('POINT(1 1)'));
----
0

-- ZMFlag for a 3DZ geometry
SELECT ST_ZMFlag(ST_GeomFromText('POINT Z(1 1 1)'));
----
2

-- ZMFlag for a 3DM geometry
SELECT ST_ZMFlag(ST_GeomFromText('POINT M(1 1 1)'));
----
1

-- ZMFlag for a 4D geometry
SELECT ST_ZMFlag(ST_GeomFromText('POINT ZM(1 1 1 1)'));
----
3
```

----

### ST_ZMax


#### Signature

```sql
DOUBLE ST_ZMax (col0 GEOMETRY)
```

#### Description

Returns the maximum Z value of a geometry

----

### ST_ZMin


#### Signature

```sql
DOUBLE ST_ZMin (col0 GEOMETRY)
```

#### Description

Returns the minimum Z value of a geometry

----

## Aggregate Functions

### ST_Envelope_Agg


#### Signature

```sql
GEOMETRY ST_Envelope_Agg (col0 GEOMETRY)
```

#### Description

Alias for [ST_Extent_Agg](#st_extent_agg).

Computes the minimal-bounding-box polygon containing the set of input geometries.

#### Example

```sql
SELECT ST_Extent_Agg(geom) FROM UNNEST([ST_Point(1,1), ST_Point(5,5)]) AS _(geom);
-- POLYGON ((1 1, 1 5, 5 5, 5 1, 1 1))
```

----

### ST_Extent_Agg


#### Signature

```sql
GEOMETRY ST_Extent_Agg (col0 GEOMETRY)
```

#### Description

Computes the minimal-bounding-box polygon containing the set of input geometries

#### Example

```sql
SELECT ST_Extent_Agg(geom) FROM UNNEST([ST_Point(1,1), ST_Point(5,5)]) AS _(geom);
-- POLYGON ((1 1, 1 5, 5 5, 5 1, 1 1))
```

----

### ST_Intersection_Agg


#### Signature

```sql
GEOMETRY ST_Intersection_Agg (col0 GEOMETRY)
```

#### Description

Computes the intersection of a set of geometries

----

### ST_Union_Agg


#### Signature

```sql
GEOMETRY ST_Union_Agg (col0 GEOMETRY)
```

#### Description

Computes the union of a set of input geometries

----

## Table Functions

### ST_Drivers

#### Signature

```sql
ST_Drivers ()
```

#### Description

Returns the list of supported GDAL drivers and file formats

Note that far from all of these drivers have been tested properly, and some may require additional options to be passed to work as expected. If you run into any issues please first consult the [consult the GDAL docs](https://gdal.org/drivers/vector/index.html).

#### Example

```sql
SELECT * FROM ST_Drivers();
```

----

### ST_Read

#### Signature

```sql
ST_Read (col0 VARCHAR, keep_wkb BOOLEAN, max_batch_size INTEGER, sequential_layer_scan BOOLEAN, layer VARCHAR, sibling_files VARCHAR[], spatial_filter WKB_BLOB, spatial_filter_box BOX_2D, allowed_drivers VARCHAR[], open_options VARCHAR[])
```

#### Description

Read and import a variety of geospatial file formats using the GDAL library.

The `ST_Read` table function is based on the [GDAL](https://gdal.org/index.html) translator library and enables reading spatial data from a variety of geospatial vector file formats as if they were DuckDB tables.

> See [ST_Drivers](#st_drivers) for a list of supported file formats and drivers.

Except for the `path` parameter, all parameters are optional.

| Parameter | Type | Description |
| --------- | -----| ----------- |
| `path` | VARCHAR | The path to the file to read. Mandatory |
| `sequential_layer_scan` | BOOLEAN | If set to true, the table function will scan through all layers sequentially and return the first layer that matches the given layer name. This is required for some drivers to work properly, e.g., the OSM driver. |
| `spatial_filter` | WKB_BLOB | If set to a WKB blob, the table function will only return rows that intersect with the given WKB geometry. Some drivers may support efficient spatial filtering natively, in which case it will be pushed down. Otherwise the filtering is done by GDAL which may be much slower. |
| `open_options` | VARCHAR[] | A list of key-value pairs that are passed to the GDAL driver to control the opening of the file. E.g., the GeoJSON driver supports a FLATTEN_NESTED_ATTRIBUTES=YES option to flatten nested attributes. |
| `layer` | VARCHAR | The name of the layer to read from the file. If NULL, the first layer is returned. Can also be a layer index (starting at 0). |
| `allowed_drivers` | VARCHAR[] | A list of GDAL driver names that are allowed to be used to open the file. If empty, all drivers are allowed. |
| `sibling_files` | VARCHAR[] | A list of sibling files that are required to open the file. E.g., the ESRI Shapefile driver requires a .shx file to be present. Although most of the time these can be discovered automatically. |
| `spatial_filter_box` | BOX_2D | If set to a BOX_2D, the table function will only return rows that intersect with the given bounding box. Similar to spatial_filter. |
| `keep_wkb` | BOOLEAN | If set, the table function will return geometries in a wkb_geometry column with the type WKB_BLOB (which can be cast to BLOB) instead of GEOMETRY. This is useful if you want to use DuckDB with more exotic geometry subtypes that DuckDB spatial doesnt support representing in the GEOMETRY type yet. |

Note that GDAL is single-threaded, so this table function will not be able to make full use of parallelism.

By using `ST_Read`, the spatial extension also provides “replacement scans” for common geospatial file formats, allowing you to query files of these formats as if they were tables directly.

```sql
SELECT * FROM './path/to/some/shapefile/dataset.shp';
```

In practice this is just syntax-sugar for calling ST_Read, so there is no difference in performance. If you want to pass additional options, you should use the ST_Read table function directly.

The following formats are currently recognized by their file extension:

| Format | Extension |
| ------ | --------- |
| ESRI ShapeFile | .shp |
| GeoPackage | .gpkg |
| FlatGeoBuf | .fgb |

#### Example

```sql
-- Read a Shapefile
SELECT * FROM ST_Read('some/file/path/filename.shp');

-- Read a GeoJSON file
CREATE TABLE my_geojson_table AS SELECT * FROM ST_Read('some/file/path/filename.json');
```

----

### ST_ReadOSM

#### Signature

```sql
ST_ReadOSM (col0 VARCHAR)
```

#### Description

The `ST_ReadOsm()` table function enables reading compressed OpenStreetMap data directly from a `.osm.pbf file.`

This function uses multithreading and zero-copy protobuf parsing which makes it a lot faster than using the `ST_Read()` OSM driver, however it only outputs the raw OSM data (Nodes, Ways, Relations), without constructing any geometries. For simple node entities (like PoI's) you can trivially construct POINT geometries, but it is also possible to construct LINESTRING and POLYGON geometries by manually joining refs and nodes together in SQL, although with available memory usually being a limiting factor.
The `ST_ReadOSM()` function also provides a "replacement scan" to enable reading from a file directly as if it were a table. This is just syntax sugar for calling `ST_ReadOSM()` though. Example:

```sql
SELECT * FROM 'tmp/data/germany.osm.pbf' LIMIT 5;
```

#### Example

```sql
SELECT *
FROM ST_ReadOSM('tmp/data/germany.osm.pbf')
WHERE tags['highway'] != []
LIMIT 5;
----
┌──────────────────────┬────────┬──────────────────────┬─────────┬────────────────────┬────────────┬───────────┬────────────────────────┐
│         kind         │   id   │         tags         │  refs   │        lat         │    lon     │ ref_roles │       ref_types        │
│ enum('node', 'way'…  │ int64  │ map(varchar, varch…  │ int64[] │       double       │   double   │ varchar[] │ enum('node', 'way', …  │
├──────────────────────┼────────┼──────────────────────┼─────────┼────────────────────┼────────────┼───────────┼────────────────────────┤
│ node                 │ 122351 │ {bicycle=yes, butt…  │         │         53.5492951 │   9.977553 │           │                        │
│ node                 │ 122397 │ {crossing=no, high…  │         │ 53.520990100000006 │ 10.0156924 │           │                        │
│ node                 │ 122493 │ {TMC:cid_58:tabcd_…  │         │ 53.129614600000004 │  8.1970173 │           │                        │
│ node                 │ 123566 │ {highway=traffic_s…  │         │ 54.617268200000005 │  8.9718171 │           │                        │
│ node                 │ 125801 │ {TMC:cid_58:tabcd_…  │         │ 53.070685000000005 │  8.7819939 │           │                        │
└──────────────────────┴────────┴──────────────────────┴─────────┴────────────────────┴────────────┴───────────┴────────────────────────┘
```

----

### ST_Read_Meta

#### Signature

```sql
ST_Read_Meta (col0 VARCHAR)
ST_Read_Meta (col0 VARCHAR[])
```

#### Description

Read the metadata from a variety of geospatial file formats using the GDAL library.

The `ST_Read_Meta` table function accompanies the `ST_Read` table function, but instead of reading the contents of a file, this function scans the metadata instead.
Since the data model of the underlying GDAL library is quite flexible, most of the interesting metadata is within the returned `layers` column, which is a somewhat complex nested structure of DuckDB `STRUCT` and `LIST` types.

#### Example

```sql
-- Find the coordinate reference system authority name and code for the first layers first geometry column in the file
SELECT
    layers[1].geometry_fields[1].crs.auth_name as name,
    layers[1].geometry_fields[1].crs.auth_code as code
FROM st_read_meta('../../tmp/data/amsterdam_roads.fgb');
```

----

