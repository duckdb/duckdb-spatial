# DuckDB Spatial Function Reference

## Function Index 
**[Scalar Functions](#scalar-functions)**

| Function | Summary |
| --- | --- |
| [`DuckDB_PROJ_Compiled_Version`](#duckdb_proj_compiled_version) | Returns a text description of the PROJ library version that that this instance of DuckDB was compiled against. |
| [`DuckDB_Proj_Version`](#duckdb_proj_version) | Returns a text description of the PROJ library version that is being used by this instance of DuckDB. |
| [`ST_Affine`](#st_affine) | Applies an affine transformation to a geometry. |
| [`ST_Area`](#st_area) | Compute the area of a geometry. |
| [`ST_Area_Spheroid`](#st_area_spheroid) | Returns the area of a geometry in meters, using an ellipsoidal model of the earth |
| [`ST_AsGeoJSON`](#st_asgeojson) | Returns the geometry as a GeoJSON fragment |
| [`ST_AsHEXWKB`](#st_ashexwkb) | Returns the geometry as a HEXWKB string |
| [`ST_AsMVTGeom`](#st_asmvtgeom) | Transform and clip geometry to a tile boundary |
| [`ST_AsSVG`](#st_assvg) | Convert the geometry into a SVG fragment or path |
| [`ST_AsText`](#st_astext) | Returns the geometry as a WKT string |
| [`ST_AsWKB`](#st_aswkb) | Returns the geometry as a WKB (Well-Known-Binary) blob |
| [`ST_Azimuth`](#st_azimuth) | Returns the azimuth (a clockwise angle measured from north) of two points in radian. |
| [`ST_Boundary`](#st_boundary) | Returns the "boundary" of a geometry |
| [`ST_Buffer`](#st_buffer) | Returns a buffer around the input geometry at the target distance |
| [`ST_BuildArea`](#st_buildarea) | Creates a polygonal geometry by attemtping to "fill in" the input geometry. |
| [`ST_Centroid`](#st_centroid) | Returns the centroid of a geometry |
| [`ST_Collect`](#st_collect) | Collects a list of geometries into a collection geometry. |
| [`ST_CollectionExtract`](#st_collectionextract) | Extracts geometries from a GeometryCollection into a typed multi geometry. |
| [`ST_ConcaveHull`](#st_concavehull) | Returns the 'concave' hull of the input geometry, containing all of the source input's points, and which can be used to create polygons from points. The ratio parameter dictates the level of concavity; 1.0 returns the convex hull; and 0 indicates to return the most concave hull possible. Set allowHoles to a non-zero value to allow output containing holes. |
| [`ST_Contains`](#st_contains) | Returns true if the first geometry contains the second geometry |
| [`ST_ContainsProperly`](#st_containsproperly) | Returns true if the first geometry \"properly\" contains the second geometry |
| [`ST_ConvexHull`](#st_convexhull) | Returns the convex hull enclosing the geometry |
| [`ST_CoverageInvalidEdges`](#st_coverageinvalidedges) | Returns the invalid edges in a polygonal coverage, which are edges that are not shared by two polygons. |
| [`ST_CoverageSimplify`](#st_coveragesimplify) | Simplify the edges in a polygonal coverage, preserving the coverange by ensuring that the there are no seams between the resulting simplified polygons. |
| [`ST_CoverageUnion`](#st_coverageunion) | Union all geometries in a polygonal coverage into a single geometry. |
| [`ST_CoveredBy`](#st_coveredby) | Returns true if geom1 is "covered by" geom2 |
| [`ST_Covers`](#st_covers) | Returns true if the geom1 "covers" geom2 |
| [`ST_Crosses`](#st_crosses) | Returns true if geom1 "crosses" geom2 |
| [`ST_DWithin`](#st_dwithin) | Returns if two geometries are within a target distance of each-other |
| [`ST_DWithin_GEOS`](#st_dwithin_geos) | Returns if two geometries are within a target distance of each-other |
| [`ST_DWithin_Spheroid`](#st_dwithin_spheroid) | Returns if two POINT_2D's are within a target distance in meters, using an ellipsoidal model of the earths surface |
| [`ST_Difference`](#st_difference) | Returns the "difference" between two geometries |
| [`ST_Dimension`](#st_dimension) | Returns the "topological dimension" of a geometry. |
| [`ST_Disjoint`](#st_disjoint) | Returns true if the geometries are disjoint |
| [`ST_Distance`](#st_distance) | Returns the planar distance between two geometries |
| [`ST_Distance_GEOS`](#st_distance_geos) | Returns the planar distance between two geometries |
| [`ST_Distance_Sphere`](#st_distance_sphere) | Returns the haversine (great circle) distance between two geometries. |
| [`ST_Distance_Spheroid`](#st_distance_spheroid) | Returns the distance between two geometries in meters using an ellipsoidal model of the earths surface |
| [`ST_Dump`](#st_dump) | Dumps a geometry into a list of sub-geometries and their "path" in the original geometry. |
| [`ST_EndPoint`](#st_endpoint) | Returns the end point of a LINESTRING. |
| [`ST_Envelope`](#st_envelope) | Returns the minimum bounding rectangle of a geometry as a polygon geometry |
| [`ST_Equals`](#st_equals) | Returns true if the geometries are "equal" |
| [`ST_Extent`](#st_extent) | Returns the minimal bounding box enclosing the input geometry |
| [`ST_Extent_Approx`](#st_extent_approx) | Returns the approximate bounding box of a geometry, if available. |
| [`ST_ExteriorRing`](#st_exteriorring) | Returns the exterior ring (shell) of a polygon geometry. |
| [`ST_FlipCoordinates`](#st_flipcoordinates) | Returns a new geometry with the coordinates of the input geometry "flipped" so that x = y and y = x |
| [`ST_Force2D`](#st_force2d) | Forces the vertices of a geometry to have X and Y components |
| [`ST_Force3DM`](#st_force3dm) | Forces the vertices of a geometry to have X, Y and M components |
| [`ST_Force3DZ`](#st_force3dz) | Forces the vertices of a geometry to have X, Y and Z components |
| [`ST_Force4D`](#st_force4d) | Forces the vertices of a geometry to have X, Y, Z and M components |
| [`ST_GeomFromGeoJSON`](#st_geomfromgeojson) | Deserializes a GEOMETRY from a GeoJSON fragment. |
| [`ST_GeomFromHEXEWKB`](#st_geomfromhexewkb) | Deserialize a GEOMETRY from a HEX(E)WKB encoded string |
| [`ST_GeomFromHEXWKB`](#st_geomfromhexwkb) | Deserialize a GEOMETRY from a HEX(E)WKB encoded string |
| [`ST_GeomFromText`](#st_geomfromtext) | Deserialize a GEOMETRY from a WKT encoded string |
| [`ST_GeomFromWKB`](#st_geomfromwkb) | Deserializes a GEOMETRY from a WKB encoded blob |
| [`ST_GeometryType`](#st_geometrytype) | Returns a 'GEOMETRY_TYPE' enum identifying the input geometry type. Possible enum return types are: `POINT`, `LINESTRING`, `POLYGON`, `MULTIPOINT`, `MULTILINESTRING`, `MULTIPOLYGON`, and `GEOMETRYCOLLECTION`. |
| [`ST_HasM`](#st_hasm) | Check if the input geometry has M values. |
| [`ST_HasZ`](#st_hasz) | Check if the input geometry has Z values. |
| [`ST_Hilbert`](#st_hilbert) | Encodes the X and Y values as the hilbert curve index for a curve covering the given bounding box. |
| [`ST_InterpolatePoint`](#st_interpolatepoint) | Computes the closest point on a LINESTRING to a given POINT and returns the interpolated M value of that point. |
| [`ST_Intersection`](#st_intersection) | Returns the intersection of two geometries |
| [`ST_Intersects`](#st_intersects) | Returns true if the geometries intersect |
| [`ST_Intersects_Extent`](#st_intersects_extent) | Returns true if the extent of two geometries intersects |
| [`ST_IsClosed`](#st_isclosed) | Check if a geometry is 'closed' |
| [`ST_IsEmpty`](#st_isempty) | Returns true if the geometry is "empty". |
| [`ST_IsRing`](#st_isring) | Returns true if the geometry is a ring (both ST_IsClosed and ST_IsSimple). |
| [`ST_IsSimple`](#st_issimple) | Returns true if the geometry is simple |
| [`ST_IsValid`](#st_isvalid) | Returns true if the geometry is valid |
| [`ST_Length`](#st_length) | Returns the length of the input line geometry |
| [`ST_Length_Spheroid`](#st_length_spheroid) | Returns the length of the input geometry in meters, using an ellipsoidal model of the earth |
| [`ST_LineInterpolatePoint`](#st_lineinterpolatepoint) | Returns a point interpolated along a line at a fraction of total 2D length. |
| [`ST_LineInterpolatePoints`](#st_lineinterpolatepoints) | Returns a multi-point interpolated along a line at a fraction of total 2D length. |
| [`ST_LineLocatePoint`](#st_linelocatepoint) | Returns the location on a line closest to a point as a fraction of the total 2D length of the line. |
| [`ST_LineMerge`](#st_linemerge) | "Merges" the input line geometry, optionally taking direction into account. |
| [`ST_LineString2DFromWKB`](#st_linestring2dfromwkb) | Deserialize a LINESTRING_2D from a WKB encoded blob |
| [`ST_LineSubstring`](#st_linesubstring) | Returns a substring of a line between two fractions of total 2D length. |
| [`ST_LocateAlong`](#st_locatealong) | Returns a point or multi-point, containing the point(s) at the geometry with the given measure |
| [`ST_LocateBetween`](#st_locatebetween) | Returns a geometry or geometry collection created by filtering and interpolating vertices within a range of "M" values |
| [`ST_M`](#st_m) | Returns the M coordinate of a point geometry |
| [`ST_MMax`](#st_mmax) | Returns the maximum M coordinate of a geometry |
| [`ST_MMin`](#st_mmin) | Returns the minimum M coordinate of a geometry |
| [`ST_MakeBox2D`](#st_makebox2d) | Create a BOX2D from two POINT geometries |
| [`ST_MakeEnvelope`](#st_makeenvelope) | Create a rectangular polygon from min/max coordinates |
| [`ST_MakeLine`](#st_makeline) | Create a LINESTRING from a list of POINT geometries |
| [`ST_MakePoint`](#st_makepoint) | Creates a GEOMETRY point from an pair of floating point numbers. |
| [`ST_MakePolygon`](#st_makepolygon) | Create a POLYGON from a LINESTRING shell |
| [`ST_MakeValid`](#st_makevalid) | Returns a valid representation of the geometry |
| [`ST_MaximumInscribedCircle`](#st_maximuminscribedcircle) | Returns the maximum inscribed circle of the input geometry, optionally with a tolerance. |
| [`ST_MinimumRotatedRectangle`](#st_minimumrotatedrectangle) | Returns the minimum rotated rectangle that bounds the input geometry, finding the surrounding box that has the lowest area by using a rotated rectangle, rather than taking the lowest and highest coordinate values as per ST_Envelope(). |
| [`ST_Multi`](#st_multi) | Turns a single geometry into a multi geometry. |
| [`ST_NGeometries`](#st_ngeometries) | Returns the number of component geometries in a collection geometry. |
| [`ST_NInteriorRings`](#st_ninteriorrings) | Returns the number of interior rings of a polygon |
| [`ST_NPoints`](#st_npoints) | Returns the number of vertices within a geometry |
| [`ST_Node`](#st_node) | Returns a "noded" MultiLinestring, produced by combining a collection of input linestrings and adding additional vertices where they intersect. |
| [`ST_Normalize`](#st_normalize) | Returns the "normalized" representation of the geometry |
| [`ST_NumGeometries`](#st_numgeometries) | Returns the number of component geometries in a collection geometry. |
| [`ST_NumInteriorRings`](#st_numinteriorrings) | Returns the number of interior rings of a polygon |
| [`ST_NumPoints`](#st_numpoints) | Returns the number of vertices within a geometry |
| [`ST_Overlaps`](#st_overlaps) | Returns true if the geometries overlap |
| [`ST_Perimeter`](#st_perimeter) | Returns the length of the perimeter of the geometry |
| [`ST_Perimeter_Spheroid`](#st_perimeter_spheroid) | Returns the length of the perimeter in meters using an ellipsoidal model of the earths surface |
| [`ST_Point`](#st_point) | Creates a GEOMETRY point |
| [`ST_Point2D`](#st_point2d) | Creates a POINT_2D |
| [`ST_Point2DFromWKB`](#st_point2dfromwkb) | Deserialize a POINT_2D from a WKB encoded blob |
| [`ST_Point3D`](#st_point3d) | Creates a POINT_3D |
| [`ST_Point4D`](#st_point4d) | Creates a POINT_4D |
| [`ST_PointN`](#st_pointn) | Returns the n'th vertex from the input geometry as a point geometry |
| [`ST_PointOnSurface`](#st_pointonsurface) | Returns a point guaranteed to lie on the surface of the geometry |
| [`ST_Points`](#st_points) | Collects all the vertices in the geometry into a MULTIPOINT |
| [`ST_Polygon2DFromWKB`](#st_polygon2dfromwkb) | Deserialize a POLYGON_2D from a WKB encoded blob |
| [`ST_Polygonize`](#st_polygonize) | Returns a polygonized representation of the input geometries |
| [`ST_QuadKey`](#st_quadkey) | Compute the [quadkey](https://learn.microsoft.com/en-us/bingmaps/articles/bing-maps-tile-system) for a given lon/lat point at a given level. |
| [`ST_ReducePrecision`](#st_reduceprecision) | Returns the geometry with all vertices reduced to the given precision |
| [`ST_RemoveRepeatedPoints`](#st_removerepeatedpoints) | Remove repeated points from a LINESTRING. |
| [`ST_Reverse`](#st_reverse) | Returns the geometry with the order of its vertices reversed |
| [`ST_ShortestLine`](#st_shortestline) | Returns the shortest line between two geometries |
| [`ST_Simplify`](#st_simplify) | Returns a simplified version of the geometry |
| [`ST_SimplifyPreserveTopology`](#st_simplifypreservetopology) | Returns a simplified version of the geometry that preserves topology |
| [`ST_Snap`](#st_snap) | Snaps the vertices and segments of a geometry to another geometry's vertices within the given tolerance |
| [`ST_StartPoint`](#st_startpoint) | Returns the start point of a LINESTRING. |
| [`ST_SymDifference`](#st_symdifference) | Returns a geometry that represents the portions of two geometries that do not intersect |
| [`ST_TileEnvelope`](#st_tileenvelope) | The `ST_TileEnvelope` scalar function generates tile envelope rectangular polygons from specified zoom level and tile indices. |
| [`ST_Touches`](#st_touches) | Returns true if the geometries touch |
| [`ST_Transform`](#st_transform) | Transforms a geometry between two coordinate systems |
| [`ST_Union`](#st_union) | Returns the union of two geometries |
| [`ST_VoronoiDiagram`](#st_voronoidiagram) | Returns the Voronoi diagram of the supplied MultiPoint geometry |
| [`ST_Within`](#st_within) | Returns true if the first geometry is within the second |
| [`ST_WithinProperly`](#st_withinproperly) | Returns true if the first geometry \"properly\" is contained by the second geometry |
| [`ST_X`](#st_x) | Returns the X coordinate of a point geometry |
| [`ST_XMax`](#st_xmax) | Returns the maximum X coordinate of a geometry |
| [`ST_XMin`](#st_xmin) | Returns the minimum X coordinate of a geometry |
| [`ST_Y`](#st_y) | Returns the Y coordinate of a point geometry |
| [`ST_YMax`](#st_ymax) | Returns the maximum Y coordinate of a geometry |
| [`ST_YMin`](#st_ymin) | Returns the minimum Y coordinate of a geometry |
| [`ST_Z`](#st_z) | Returns the Z coordinate of a point geometry |
| [`ST_ZMFlag`](#st_zmflag) | Returns a flag indicating the presence of Z and M values in the input geometry. |
| [`ST_ZMax`](#st_zmax) | Returns the maximum Z coordinate of a geometry |
| [`ST_ZMin`](#st_zmin) | Returns the minimum Z coordinate of a geometry |

**[Aggregate Functions](#aggregate-functions)**

| Function | Summary |
| --- | --- |
| [`ST_AsMVT`](#st_asmvt) | Make a Mapbox Vector Tile from a set of geometries and properties |
| [`ST_CoverageInvalidEdges_Agg`](#st_coverageinvalidedges_agg) | Returns the invalid edges of a coverage geometry |
| [`ST_CoverageSimplify_Agg`](#st_coveragesimplify_agg) | Simplifies a set of geometries while maintaining coverage |
| [`ST_CoverageUnion_Agg`](#st_coverageunion_agg) | Unions a set of geometries while maintaining coverage |
| [`ST_Envelope_Agg`](#st_envelope_agg) | Alias for [ST_Extent_Agg](#st_extent_agg). |
| [`ST_Extent_Agg`](#st_extent_agg) | Computes the minimal-bounding-box polygon containing the set of input geometries |
| [`ST_Intersection_Agg`](#st_intersection_agg) | Computes the intersection of a set of geometries |
| [`ST_MemUnion_Agg`](#st_memunion_agg) | Computes the union of a set of input geometries. |
| [`ST_Union_Agg`](#st_union_agg) | Computes the union of a set of input geometries |

**[Macro Functions](#Macro-functions)**

| Function | Summary |
| --- | --- |
| [`ST_Rotate`](#st_rotate) | Alias of ST_RotateZ |
| [`ST_RotateX`](#st_rotatex) | Rotates a geometry around the X axis. This is a shorthand macro for calling ST_Affine. |
| [`ST_RotateY`](#st_rotatey) | Rotates a geometry around the Y axis. This is a shorthand macro for calling ST_Affine. |
| [`ST_RotateZ`](#st_rotatez) | Rotates a geometry around the Z axis. This is a shorthand macro for calling ST_Affine. |
| [`ST_Scale`](#st_scale) |  |
| [`ST_TransScale`](#st_transscale) | Translates and then scales a geometry in X and Y direction. This is a shorthand macro for calling ST_Affine. |
| [`ST_Translate`](#st_translate) |  |

**[Table Functions](#table-functions)**

| Function | Summary |
| --- | --- |
| [`ST_Drivers`](#st_drivers) | Returns the list of supported GDAL drivers and file formats |
| [`ST_GeneratePoints`](#st_generatepoints) | Generates a set of random points within the specified bounding box. |
| [`ST_Read`](#st_read) | Read and import a variety of geospatial file formats using the GDAL library. |
| [`ST_ReadOSM`](#st_readosm) | The `ST_ReadOsm()` table function enables reading compressed OpenStreetMap data directly from a `.osm.pbf file.` |
| [`ST_ReadSHP`](#st_readshp) | Read a Shapefile without relying on the GDAL library |
| [`ST_Read_Meta`](#st_read_meta) | Read the metadata from a variety of geospatial file formats using the GDAL library. |

**[Window Functions](#window-functions)**

| Function | Summary |
| --- | --- |
| [`ST_ClusterDBSCAN`](#st_clusterdbscan) | Window function that returns a cluster id for each input point using the DBSCAN algorithm. |

----

## Scalar Functions

### DuckDB_PROJ_Compiled_Version


#### Signature

```sql
VARCHAR DuckDB_PROJ_Compiled_Version ()
```

#### Description

Returns a text description of the PROJ library version that that this instance of DuckDB was compiled against.

#### Example

```sql
SELECT duckdb_proj_compiled_version();
┌────────────────────────────────┐
│ duckdb_proj_compiled_version() │
│            varchar             │
├────────────────────────────────┤
│ Rel. 9.1.1, December 1st, 2022 │
└────────────────────────────────┘
```

----

### DuckDB_Proj_Version


#### Signature

```sql
VARCHAR DuckDB_Proj_Version ()
```

#### Description

Returns a text description of the PROJ library version that is being used by this instance of DuckDB.

#### Example

```sql
SELECT duckdb_proj_version();
┌───────────────────────┐
│ duckdb_proj_version() │
│        varchar        │
├───────────────────────┤
│ 9.1.1                 │
└───────────────────────┘
```

----

### ST_Affine


#### Signatures

```sql
GEOMETRY ST_Affine (geom GEOMETRY, a DOUBLE, b DOUBLE, c DOUBLE, d DOUBLE, e DOUBLE, f DOUBLE, g DOUBLE, h DOUBLE, i DOUBLE, xoff DOUBLE, yoff DOUBLE, zoff DOUBLE)
GEOMETRY ST_Affine (geom GEOMETRY, a DOUBLE, b DOUBLE, d DOUBLE, e DOUBLE, xoff DOUBLE, yoff DOUBLE)
```

#### Description

Applies an affine transformation to a geometry.

For the 2D variant, the transformation matrix is defined as follows:
```
| a b xoff |
| d e yoff |
| 0 0 1    |
```

For the 3D variant, the transformation matrix is defined as follows:
```
| a b c xoff |
| d e f yoff |
| g h i zoff |
| 0 0 0 1    |
```

The transformation is applied to all vertices of the geometry.

#### Example

```sql
-- Translate a point by (2, 3)
SELECT ST_Affine(ST_Point(1, 1),
                 1, 0,   -- a, b
                 0, 1,   -- d, e
                 2, 3);  -- xoff, yoff
----
POINT (3 4)

-- Scale a geometry by factor 2 in X and Y
SELECT ST_Affine(ST_Point(1, 1),
                 2, 0, 0,   -- a, b, c
                 0, 2, 0,   -- d, e, f
                 0, 0, 1,   -- g, h, i
                 0, 0, 0);  -- xoff, yoff, zoff
----
POINT (2 2)
```

----

### ST_Area


#### Signatures

```sql
DOUBLE ST_Area (geom GEOMETRY)
DOUBLE ST_Area (polygon POLYGON_2D)
DOUBLE ST_Area (linestring LINESTRING_2D)
DOUBLE ST_Area (point POINT_2D)
DOUBLE ST_Area (box BOX_2D)
```

#### Description

Compute the area of a geometry.

Returns `0.0` for any geometry that is not a `POLYGON`, `MULTIPOLYGON` or `GEOMETRYCOLLECTION` containing polygon
geometries.

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
DOUBLE ST_Area_Spheroid (geom GEOMETRY)
DOUBLE ST_Area_Spheroid (poly POLYGON_2D)
```

#### Description

Returns the area of a geometry in meters, using an ellipsoidal model of the earth

The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the area is returned in square meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library, calculating the area using an ellipsoidal model of the earth. This is a highly accurate method for calculating the area of a polygon taking the curvature of the earth into account, but is also the slowest.

Returns `0.0` for any geometry that is not a `POLYGON`, `MULTIPOLYGON` or `GEOMETRYCOLLECTION` containing polygon geometries.

----

### ST_AsGeoJSON


#### Signature

```sql
JSON ST_AsGeoJSON (geom GEOMETRY)
```

#### Description

Returns the geometry as a GeoJSON fragment

This does not return a complete GeoJSON document, only the geometry fragment.
To construct a complete GeoJSON document or feature, look into using the DuckDB JSON extension in conjunction with this function.
This function supports geometries with Z values, but not M values. M values are ignored.

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
VARCHAR ST_AsHEXWKB (geom GEOMETRY)
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

### ST_AsMVTGeom


#### Signatures

```sql
GEOMETRY ST_AsMVTGeom (geom GEOMETRY, bounds BOX_2D, extent BIGINT, buffer BIGINT, clip_geom BOOLEAN)
GEOMETRY ST_AsMVTGeom (geom GEOMETRY, bounds BOX_2D, extent BIGINT, buffer BIGINT)
GEOMETRY ST_AsMVTGeom (geom GEOMETRY, bounds BOX_2D, extent BIGINT)
GEOMETRY ST_AsMVTGeom (geom GEOMETRY, bounds BOX_2D)
```

#### Description

Transform and clip geometry to a tile boundary

See "ST_AsMVT" for more details

----

### ST_AsSVG


#### Signature

```sql
VARCHAR ST_AsSVG (geom GEOMETRY, relative BOOLEAN, precision INTEGER)
```

#### Description

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
VARCHAR ST_AsText (geom GEOMETRY)
VARCHAR ST_AsText (point POINT_2D)
VARCHAR ST_AsText (linestring LINESTRING_2D)
VARCHAR ST_AsText (polygon POLYGON_2D)
VARCHAR ST_AsText (box BOX_2D)
```

#### Description

Returns the geometry as a WKT string

#### Example

```sql
SELECT ST_MakeEnvelope(0,0,1,1);
----
POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0))
```

----

### ST_AsWKB


#### Signature

```sql
WKB_BLOB ST_AsWKB (geom GEOMETRY)
```

#### Description

Returns the geometry as a WKB (Well-Known-Binary) blob

#### Example

```sql
SELECT ST_AsWKB('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'::GEOMETRY)::BLOB;
----
\x01\x03\x00\x00\x00\x01\x00\x00\x00\x05...
```

----

### ST_Azimuth


#### Signatures

```sql
DOUBLE ST_Azimuth (origin GEOMETRY, target GEOMETRY)
DOUBLE ST_Azimuth (origin POINT_2D, target POINT_2D)
```

#### Description

Returns the azimuth (a clockwise angle measured from north) of two points in radian.

#### Example

```sql
SELECT degrees(ST_Azimuth(ST_Point(0, 0), ST_Point(0, 1)));
----
90.0
```

----

### ST_Boundary


#### Signature

```sql
GEOMETRY ST_Boundary (geom GEOMETRY)
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

### ST_BuildArea


#### Signature

```sql
GEOMETRY ST_BuildArea (geom GEOMETRY)
```

#### Description

Creates a polygonal geometry by attemtping to "fill in" the input geometry.

Unlike ST_Polygonize, this function does not fill in holes.

----

### ST_Centroid


#### Signatures

```sql
GEOMETRY ST_Centroid (geom GEOMETRY)
POINT_2D ST_Centroid (point POINT_2D)
POINT_2D ST_Centroid (linestring LINESTRING_2D)
POINT_2D ST_Centroid (polygon POLYGON_2D)
POINT_2D ST_Centroid (box BOX_2D)
POINT_2D ST_Centroid (box BOX_2DF)
```

#### Description

Returns the centroid of a geometry

----

### ST_Collect


#### Signature

```sql
GEOMETRY ST_Collect (geoms GEOMETRY[])
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
GEOMETRY ST_CollectionExtract (geom GEOMETRY, type INTEGER)
GEOMETRY ST_CollectionExtract (geom GEOMETRY)
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

### ST_ConcaveHull


#### Signature

```sql
GEOMETRY ST_ConcaveHull (geom GEOMETRY, ratio DOUBLE, allowHoles BOOLEAN)
```

#### Description

Returns the 'concave' hull of the input geometry, containing all of the source input's points, and which can be used to create polygons from points. The ratio parameter dictates the level of concavity; 1.0 returns the convex hull; and 0 indicates to return the most concave hull possible. Set allowHoles to a non-zero value to allow output containing holes.

----

### ST_Contains


#### Signatures

```sql
BOOLEAN ST_Contains (geom1 POLYGON_2D, geom2 POINT_2D)
BOOLEAN ST_Contains (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if the first geometry contains the second geometry

In contrast to `ST_ContainsProperly`, this function will also return true if `geom2` is contained strictly on the boundary of `geom1`.
A geometry always `ST_Contains` itself, but does not `ST_ContainsProperly` itself.

----

### ST_ContainsProperly


#### Signature

```sql
BOOLEAN ST_ContainsProperly (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if the first geometry \"properly\" contains the second geometry

In contrast to `ST_Contains`, this function does not return true if `geom2` is contained strictly on the boundary of `geom1`.
A geometry always `ST_Contains` itself, but does not `ST_ContainsProperly` itself.

----

### ST_ConvexHull


#### Signature

```sql
GEOMETRY ST_ConvexHull (geom GEOMETRY)
```

#### Description

Returns the convex hull enclosing the geometry

----

### ST_CoverageInvalidEdges


#### Signatures

```sql
GEOMETRY ST_CoverageInvalidEdges (geoms GEOMETRY[], tolerance DOUBLE)
GEOMETRY ST_CoverageInvalidEdges (geoms GEOMETRY[])
```

#### Description

Returns the invalid edges in a polygonal coverage, which are edges that are not shared by two polygons.
Returns NULL if the input is not a polygonal coverage, or if the input is valid.
Tolerance is 0 by default.

----

### ST_CoverageSimplify


#### Signatures

```sql
GEOMETRY ST_CoverageSimplify (geoms GEOMETRY[], tolerance DOUBLE, simplify_boundary BOOLEAN)
GEOMETRY ST_CoverageSimplify (geoms GEOMETRY[], tolerance DOUBLE)
```

#### Description

Simplify the edges in a polygonal coverage, preserving the coverange by ensuring that the there are no seams between the resulting simplified polygons.

By default, the boundary of the coverage is also simplified, but this can be controlled with the optional third 'simplify_boundary' parameter.

----

### ST_CoverageUnion


#### Signature

```sql
GEOMETRY ST_CoverageUnion (geoms GEOMETRY[])
```

#### Description

Union all geometries in a polygonal coverage into a single geometry.
This may be faster than using `ST_Union`, but may use more memory.

----

### ST_CoveredBy


#### Signature

```sql
BOOLEAN ST_CoveredBy (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if geom1 is "covered by" geom2

----

### ST_Covers


#### Signature

```sql
BOOLEAN ST_Covers (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if the geom1 "covers" geom2

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
BOOLEAN ST_DWithin (geom1 GEOMETRY, geom2 GEOMETRY, distance DOUBLE)
```

#### Description

Returns if two geometries are within a target distance of each-other

----

### ST_DWithin_GEOS


#### Signature

```sql
BOOLEAN ST_DWithin_GEOS (geom1 GEOMETRY, geom2 GEOMETRY, distance DOUBLE)
```

#### Description

Returns if two geometries are within a target distance of each-other

----

### ST_DWithin_Spheroid


#### Signature

```sql
BOOLEAN ST_DWithin_Spheroid (p1 POINT_2D, p2 POINT_2D, distance DOUBLE)
```

#### Description

Returns if two POINT_2D's are within a target distance in meters, using an ellipsoidal model of the earths surface

The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the distance is returned in meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library to solve the [inverse geodesic problem](https://en.wikipedia.org/wiki/Geodesics_on_an_ellipsoid#Solution_of_the_direct_and_inverse_problems), calculating the distance between two points using an ellipsoidal model of the earth. This is a highly accurate method for calculating the distance between two arbitrary points taking the curvature of the earths surface into account, but is also the slowest.

----

### ST_Difference


#### Signature

```sql
GEOMETRY ST_Difference (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns the "difference" between two geometries

----

### ST_Dimension


#### Signature

```sql
INTEGER ST_Dimension (geom GEOMETRY)
```

#### Description

Returns the "topological dimension" of a geometry.

- For POINT and MULTIPOINT geometries, returns `0`
- For LINESTRING and MULTILINESTRING, returns `1`
- For POLYGON and MULTIPOLYGON, returns `2`
- For GEOMETRYCOLLECTION, returns the maximum dimension of the contained geometries, or 0 if the collection is empty

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
BOOLEAN ST_Disjoint (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if the geometries are disjoint

----

### ST_Distance


#### Signatures

```sql
DOUBLE ST_Distance (point1 POINT_2D, point2 POINT_2D)
DOUBLE ST_Distance (point POINT_2D, linestring LINESTRING_2D)
DOUBLE ST_Distance (linestring LINESTRING_2D, point POINT_2D)
DOUBLE ST_Distance (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns the planar distance between two geometries

#### Example

```sql
SELECT ST_Distance('POINT (0 0)'::GEOMETRY, 'POINT (3 4)'::GEOMETRY);
----
5.0

-- Z coordinates are ignored
SELECT ST_Distance('POINT Z (0 0 0)'::GEOMETRY, 'POINT Z (3 4 5)'::GEOMETRY);
----
5.0
```

----

### ST_Distance_GEOS


#### Signature

```sql
DOUBLE ST_Distance_GEOS (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns the planar distance between two geometries

----

### ST_Distance_Sphere


#### Signatures

```sql
DOUBLE ST_Distance_Sphere (geom1 GEOMETRY, geom2 GEOMETRY)
DOUBLE ST_Distance_Sphere (point1 POINT_2D, point2 POINT_2D)
```

#### Description

Returns the haversine (great circle) distance between two geometries.

- Only supports POINT geometries.
- Returns the distance in meters.
- The input is expected to be in WGS84 (EPSG:4326) coordinates, using a [latitude, longitude] axis order.

----

### ST_Distance_Spheroid


#### Signature

```sql
DOUBLE ST_Distance_Spheroid (p1 POINT_2D, p2 POINT_2D)
```

#### Description

Returns the distance between two geometries in meters using an ellipsoidal model of the earths surface

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
STRUCT(geom GEOMETRY, path INTEGER[])[] ST_Dump (geom GEOMETRY)
```

#### Description

Dumps a geometry into a list of sub-geometries and their "path" in the original geometry.

You can use the `UNNEST(res, recursive := true)` function to explode  resulting list of structs into multiple rows.

#### Example

```sql
select st_dump('MULTIPOINT(1 2,3 4)'::geometry);
----
[{'geom': 'POINT(1 2)', 'path': [0]}, {'geom': 'POINT(3 4)', 'path': [1]}]

select unnest(st_dump('MULTIPOINT(1 2,3 4)'::geometry), recursive := true);
-- ┌─────────────┬─────────┐
-- │    geom     │  path   │
-- │  geometry   │ int32[] │
-- ├─────────────┼─────────┤
-- │ POINT (1 2) │ [1]     │
-- │ POINT (3 4) │ [2]     │
-- └─────────────┴─────────┘
```

----

### ST_EndPoint


#### Signatures

```sql
GEOMETRY ST_EndPoint (geom GEOMETRY)
POINT_2D ST_EndPoint (line LINESTRING_2D)
```

#### Description

Returns the end point of a LINESTRING.

----

### ST_Envelope


#### Signature

```sql
GEOMETRY ST_Envelope (geom GEOMETRY)
```

#### Description

Returns the minimum bounding rectangle of a geometry as a polygon geometry

----

### ST_Equals


#### Signature

```sql
BOOLEAN ST_Equals (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if the geometries are "equal"

----

### ST_Extent


#### Signatures

```sql
BOX_2D ST_Extent (geom GEOMETRY)
BOX_2D ST_Extent (wkb WKB_BLOB)
```

#### Description

Returns the minimal bounding box enclosing the input geometry

----

### ST_Extent_Approx


#### Signature

```sql
BOX_2DF ST_Extent_Approx (geom GEOMETRY)
```

#### Description

Returns the approximate bounding box of a geometry, if available.

This function is only really used internally, and returns the cached bounding box of the geometry if it exists.
This function may be removed or renamed in the future.

----

### ST_ExteriorRing


#### Signatures

```sql
GEOMETRY ST_ExteriorRing (geom GEOMETRY)
LINESTRING_2D ST_ExteriorRing (polygon POLYGON_2D)
```

#### Description

Returns the exterior ring (shell) of a polygon geometry.

----

### ST_FlipCoordinates


#### Signatures

```sql
GEOMETRY ST_FlipCoordinates (geom GEOMETRY)
POINT_2D ST_FlipCoordinates (point POINT_2D)
LINESTRING_2D ST_FlipCoordinates (linestring LINESTRING_2D)
POLYGON_2D ST_FlipCoordinates (polygon POLYGON_2D)
BOX_2D ST_FlipCoordinates (box BOX_2D)
```

#### Description

Returns a new geometry with the coordinates of the input geometry "flipped" so that x = y and y = x

----

### ST_Force2D


#### Signature

```sql
GEOMETRY ST_Force2D (geom GEOMETRY)
```

#### Description

Forces the vertices of a geometry to have X and Y components

This function will drop any Z and M values from the input geometry, if present. If the input geometry is already 2D, it will be returned as is.

----

### ST_Force3DM


#### Signature

```sql
GEOMETRY ST_Force3DM (geom GEOMETRY, m DOUBLE)
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
GEOMETRY ST_Force3DZ (geom GEOMETRY, z DOUBLE)
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
GEOMETRY ST_Force4D (geom GEOMETRY, z DOUBLE, m DOUBLE)
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
GEOMETRY ST_GeomFromGeoJSON (geojson JSON)
GEOMETRY ST_GeomFromGeoJSON (geojson VARCHAR)
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
GEOMETRY ST_GeomFromHEXEWKB (hexwkb VARCHAR)
```

#### Description

Deserialize a GEOMETRY from a HEX(E)WKB encoded string

DuckDB spatial doesnt currently differentiate between `WKB` and `EWKB`, so `ST_GeomFromHEXWKB` and `ST_GeomFromHEXEWKB" are just aliases of eachother.

----

### ST_GeomFromHEXWKB


#### Signature

```sql
GEOMETRY ST_GeomFromHEXWKB (hexwkb VARCHAR)
```

#### Description

Deserialize a GEOMETRY from a HEX(E)WKB encoded string

DuckDB spatial doesnt currently differentiate between `WKB` and `EWKB`, so `ST_GeomFromHEXWKB` and `ST_GeomFromHEXEWKB" are just aliases of eachother.

----

### ST_GeomFromText


#### Signatures

```sql
GEOMETRY ST_GeomFromText (wkt VARCHAR)
GEOMETRY ST_GeomFromText (wkt VARCHAR, ignore_invalid BOOLEAN)
```

#### Description

Deserialize a GEOMETRY from a WKT encoded string

----

### ST_GeomFromWKB


#### Signatures

```sql
GEOMETRY ST_GeomFromWKB (wkb WKB_BLOB)
GEOMETRY ST_GeomFromWKB (blob BLOB)
```

#### Description

Deserializes a GEOMETRY from a WKB encoded blob

----

### ST_GeometryType


#### Signatures

```sql
ANY ST_GeometryType (geom GEOMETRY)
ANY ST_GeometryType (point POINT_2D)
ANY ST_GeometryType (linestring LINESTRING_2D)
ANY ST_GeometryType (polygon POLYGON_2D)
ANY ST_GeometryType (wkb WKB_BLOB)
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
BOOLEAN ST_HasM (geom GEOMETRY)
BOOLEAN ST_HasM (wkb WKB_BLOB)
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
BOOLEAN ST_HasZ (geom GEOMETRY)
BOOLEAN ST_HasZ (wkb WKB_BLOB)
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
UINTEGER ST_Hilbert (x DOUBLE, y DOUBLE, bounds BOX_2D)
UINTEGER ST_Hilbert (geom GEOMETRY, bounds BOX_2D)
UINTEGER ST_Hilbert (geom GEOMETRY)
UINTEGER ST_Hilbert (box BOX_2D, bounds BOX_2D)
UINTEGER ST_Hilbert (box BOX_2DF, bounds BOX_2DF)
```

#### Description

Encodes the X and Y values as the hilbert curve index for a curve covering the given bounding box.
If a geometry is provided, the center of the approximate bounding box is used as the point to encode.
If no bounding box is provided, the hilbert curve index is mapped to the full range of a single-presicion float.
For the BOX_2D and BOX_2DF variants, the center of the box is used as the point to encode.

----

### ST_InterpolatePoint


#### Signature

```sql
DOUBLE ST_InterpolatePoint (line GEOMETRY, point GEOMETRY)
```

#### Description

Computes the closest point on a LINESTRING to a given POINT and returns the interpolated M value of that point.

First argument must be a linestring and must have a M dimension. The second argument must be a point. 
Neither argument can be empty.

----

### ST_Intersection


#### Signature

```sql
GEOMETRY ST_Intersection (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns the intersection of two geometries

----

### ST_Intersects


#### Signatures

```sql
BOOLEAN ST_Intersects (box1 BOX_2D, box2 BOX_2D)
BOOLEAN ST_Intersects (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if the geometries intersect

----

### ST_Intersects_Extent


#### Signature

```sql
BOOLEAN ST_Intersects_Extent (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if the extent of two geometries intersects

----

### ST_IsClosed


#### Signature

```sql
BOOLEAN ST_IsClosed (geom GEOMETRY)
```

#### Description

Check if a geometry is 'closed'

----

### ST_IsEmpty


#### Signatures

```sql
BOOLEAN ST_IsEmpty (geom GEOMETRY)
BOOLEAN ST_IsEmpty (linestring LINESTRING_2D)
BOOLEAN ST_IsEmpty (polygon POLYGON_2D)
```

#### Description

Returns true if the geometry is "empty".

----

### ST_IsRing


#### Signature

```sql
BOOLEAN ST_IsRing (geom GEOMETRY)
```

#### Description

Returns true if the geometry is a ring (both ST_IsClosed and ST_IsSimple).

----

### ST_IsSimple


#### Signature

```sql
BOOLEAN ST_IsSimple (geom GEOMETRY)
```

#### Description

Returns true if the geometry is simple

----

### ST_IsValid


#### Signature

```sql
BOOLEAN ST_IsValid (geom GEOMETRY)
```

#### Description

Returns true if the geometry is valid

----

### ST_Length


#### Signatures

```sql
DOUBLE ST_Length (geom GEOMETRY)
DOUBLE ST_Length (linestring LINESTRING_2D)
```

#### Description

Returns the length of the input line geometry

----

### ST_Length_Spheroid


#### Signatures

```sql
DOUBLE ST_Length_Spheroid (geom GEOMETRY)
DOUBLE ST_Length_Spheroid (line LINESTRING_2D)
```

#### Description

Returns the length of the input geometry in meters, using an ellipsoidal model of the earth

The input geometry is assumed to be in the [EPSG:4326](https://en.wikipedia.org/wiki/World_Geodetic_System) coordinate system (WGS84), with [latitude, longitude] axis order and the length is returned in meters. This function uses the [GeographicLib](https://geographiclib.sourceforge.io/) library, calculating the length using an ellipsoidal model of the earth. This is a highly accurate method for calculating the length of a line geometry taking the curvature of the earth into account, but is also the slowest.

Returns `0.0` for any geometry that is not a `LINESTRING`, `MULTILINESTRING` or `GEOMETRYCOLLECTION` containing line geometries.

----

### ST_LineInterpolatePoint


#### Signature

```sql
GEOMETRY ST_LineInterpolatePoint (line GEOMETRY, fraction DOUBLE)
```

#### Description

Returns a point interpolated along a line at a fraction of total 2D length.

----

### ST_LineInterpolatePoints


#### Signature

```sql
GEOMETRY ST_LineInterpolatePoints (line GEOMETRY, fraction DOUBLE, repeat BOOLEAN)
```

#### Description

Returns a multi-point interpolated along a line at a fraction of total 2D length.

if repeat is false, the result is a single point, (and equivalent to ST_LineInterpolatePoint),
otherwise, the result is a multi-point with points repeated at the fraction interval.

----

### ST_LineLocatePoint


#### Signature

```sql
DOUBLE ST_LineLocatePoint (line GEOMETRY, point GEOMETRY)
```

#### Description

Returns the location on a line closest to a point as a fraction of the total 2D length of the line.

----

### ST_LineMerge


#### Signatures

```sql
GEOMETRY ST_LineMerge (geom GEOMETRY)
GEOMETRY ST_LineMerge (geom GEOMETRY, preserve_direction BOOLEAN)
```

#### Description

"Merges" the input line geometry, optionally taking direction into account.

----

### ST_LineString2DFromWKB


#### Signature

```sql
GEOMETRY ST_LineString2DFromWKB (linestring LINESTRING_2D)
```

#### Description

Deserialize a LINESTRING_2D from a WKB encoded blob

----

### ST_LineSubstring


#### Signature

```sql
GEOMETRY ST_LineSubstring (line GEOMETRY, start_fraction DOUBLE, end_fraction DOUBLE)
```

#### Description

Returns a substring of a line between two fractions of total 2D length.

----

### ST_LocateAlong


#### Signatures

```sql
GEOMETRY ST_LocateAlong (line GEOMETRY, measure DOUBLE, offset DOUBLE)
GEOMETRY ST_LocateAlong (line GEOMETRY, measure DOUBLE)
```

#### Description

Returns a point or multi-point, containing the point(s) at the geometry with the given measure

For a LINESTRING, or MULTILINESTRING, the location is determined by interpolating between M values
For a POINT and MULTIPOINT, the point is returned if the measure matches the M value of the vertex, otherwise an empty geometry is returned
For a POLYGON, only the exterior ring is considered, and treated as a LINESTRING

If offset is provided, the resulting point(s) is offset by the given amount perpendicular to the line direction.

----

### ST_LocateBetween


#### Signatures

```sql
GEOMETRY ST_LocateBetween (line GEOMETRY, start_measure DOUBLE, end_measure DOUBLE, offset DOUBLE)
GEOMETRY ST_LocateBetween (line GEOMETRY, start_measure DOUBLE, end_measure DOUBLE)
```

#### Description

Returns a geometry or geometry collection created by filtering and interpolating vertices within a range of "M" values

Creates a geometry or geometry collection, containing the parts formed by vertices that have an "M" value within the "start_measure" and "end_measure" range

For LINESTRING or MULTILINESTRING, if a line segment would cross either the upper or lower bound, a vertex is added by interpolating the coordinates at the "intersection"
For a POINT and MULTIPOINT, the point is added to the collection if its vertex has an "M" value within the range, otherwise it is skipped
For a POLYGON, only the exterior ring is considered, and treated like a LINESTRING

If offset is provided, the resulting vertices are offset by the given amount perpendicular to the line direction.

----

### ST_M


#### Signature

```sql
DOUBLE ST_M (geom GEOMETRY)
```

#### Description

Returns the M coordinate of a point geometry

#### Example

```sql
SELECT ST_M(ST_Point(1, 2, 3, 4))
```

----

### ST_MMax


#### Signature

```sql
DOUBLE ST_MMax (geom GEOMETRY)
```

#### Description

Returns the maximum M coordinate of a geometry

#### Example

```sql
SELECT ST_MMax(ST_Point(1, 2, 3, 4))
```

----

### ST_MMin


#### Signature

```sql
DOUBLE ST_MMin (geom GEOMETRY)
```

#### Description

Returns the minimum M coordinate of a geometry

#### Example

```sql
SELECT ST_MMin(ST_Point(1, 2, 3, 4))
```

----

### ST_MakeBox2D


#### Signature

```sql
BOX_2D ST_MakeBox2D (point1 GEOMETRY, point2 GEOMETRY)
```

#### Description

Create a BOX2D from two POINT geometries

#### Example

```sql
SELECT ST_MakeBox2D(ST_Point(0, 0), ST_Point(1, 1));
----
BOX(0 0, 1 1)
```

----

### ST_MakeEnvelope


#### Signature

```sql
GEOMETRY ST_MakeEnvelope (min_x DOUBLE, min_y DOUBLE, max_x DOUBLE, max_y DOUBLE)
```

#### Description

Create a rectangular polygon from min/max coordinates

----

### ST_MakeLine


#### Signatures

```sql
GEOMETRY ST_MakeLine (geoms GEOMETRY[])
GEOMETRY ST_MakeLine (start GEOMETRY, end GEOMETRY)
```

#### Description

Create a LINESTRING from a list of POINT geometries

#### Example

```sql
SELECT ST_MakeLine([ST_Point(0, 0), ST_Point(1, 1)]);
----
LINESTRING(0 0, 1 1)
```

----

### ST_MakePoint


#### Signatures

```sql
POINT_2D ST_MakePoint (x DOUBLE, y DOUBLE)
POINT_3D ST_MakePoint (x DOUBLE, y DOUBLE, z DOUBLE)
POINT_4D ST_MakePoint (x DOUBLE, y DOUBLE, z DOUBLE, m DOUBLE)
```

#### Description

Creates a GEOMETRY point from an pair of floating point numbers.

For geodetic coordinate systems, x is typically the longitude value and y is the latitude value.

Note that ST_Point is equivalent. ST_MakePoint is provided for PostGIS compatibility.

#### Example

```sql
SELECT ST_AsText(ST_MakePoint(143.3, -24.2));
----
POINT (143.3 -24.2)
```

----

### ST_MakePolygon


#### Signatures

```sql
GEOMETRY ST_MakePolygon (shell GEOMETRY)
GEOMETRY ST_MakePolygon (shell GEOMETRY, holes GEOMETRY[])
```

#### Description

Create a POLYGON from a LINESTRING shell

#### Example

```sql
SELECT ST_MakePolygon(ST_LineString([ST_Point(0, 0), ST_Point(1, 0), ST_Point(1, 1), ST_Point(0, 0)]));
```

----

### ST_MakeValid


#### Signature

```sql
GEOMETRY ST_MakeValid (geom GEOMETRY)
GEOMETRY ST_MakeValid (geom GEOMETRY, method VARCHAR)
GEOMETRY ST_MakeValid (geom GEOMETRY, method VARCHAR, keepCollapsed BOOLEAN)
```

#### Description

Returns a valid representation of the geometry.

`method`: accepts `LINEWORK` or `STRUCTURE`. This parameter is case-insensitive. The default value is `LINEWORK`.

- The `LINEWORK` method combines all rings into a set of noded lines and then extracts valid polygons from that linework. This method keeps all input vertices.
- The `STRUCTURE` method first makes all rings valid then merges shells and subtracts holes from shells to generate valid result. It assumes that holes and shells are correctly categorized.

`keepCollapsed`: whether or not to retain components that have collapsed into a lower dimensionality. Only works with the  `STRUCTURE` method. The default value is `true`.

----

### ST_MaximumInscribedCircle


#### Signatures

```sql
STRUCT(center GEOMETRY, nearest GEOMETRY, radius DOUBLE) ST_MaximumInscribedCircle (geom GEOMETRY)
STRUCT(center GEOMETRY, nearest GEOMETRY, radius DOUBLE) ST_MaximumInscribedCircle (geom GEOMETRY, tolerance DOUBLE)
```

#### Description

Returns the maximum inscribed circle of the input geometry, optionally with a tolerance.

By default, the tolerance is computed as `max(width, height) / 1000`.
The return value is a struct with the center of the circle, the nearest point to the center on the boundary of the geometry, and the radius of the circle.

#### Example

```sql
-- Find the maximum inscribed circle of a square
SELECT ST_MaximumInscribedCircle(
    ST_GeomFromText('POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))')
);
----
{'center': POINT (5 5), 'nearest': POINT (5 0), 'radius': 5.0}
```

----

### ST_MinimumRotatedRectangle


#### Signature

```sql
GEOMETRY ST_MinimumRotatedRectangle (geom GEOMETRY)
```

#### Description

Returns the minimum rotated rectangle that bounds the input geometry, finding the surrounding box that has the lowest area by using a rotated rectangle, rather than taking the lowest and highest coordinate values as per ST_Envelope().

----

### ST_Multi


#### Signature

```sql
GEOMETRY ST_Multi (geom GEOMETRY)
```

#### Description

Turns a single geometry into a multi geometry.

If the geometry is already a multi geometry, it is returned as is.

#### Example

```sql
SELECT ST_Multi(ST_GeomFromText('POINT(1 2)'));
----
MULTIPOINT (1 2)

SELECT ST_Multi(ST_GeomFromText('LINESTRING(1 1, 2 2)'));
----
MULTILINESTRING ((1 1, 2 2))

SELECT ST_Multi(ST_GeomFromText('POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))'));
----
MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)))
```

----

### ST_NGeometries


#### Signature

```sql
INTEGER ST_NGeometries (geom GEOMETRY)
```

#### Description

Returns the number of component geometries in a collection geometry.
If the input geometry is not a collection, this function returns 0 or 1 depending on if the geometry is empty or not.

----

### ST_NInteriorRings


#### Signatures

```sql
INTEGER ST_NInteriorRings (geom GEOMETRY)
INTEGER ST_NInteriorRings (polygon POLYGON_2D)
```

#### Description

Returns the number of interior rings of a polygon

----

### ST_NPoints


#### Signatures

```sql
UINTEGER ST_NPoints (geom GEOMETRY)
UBIGINT ST_NPoints (point POINT_2D)
UBIGINT ST_NPoints (linestring LINESTRING_2D)
UBIGINT ST_NPoints (polygon POLYGON_2D)
UBIGINT ST_NPoints (box BOX_2D)
```

#### Description

Returns the number of vertices within a geometry

----

### ST_Node


#### Signature

```sql
GEOMETRY ST_Node (geom GEOMETRY)
```

#### Description

Returns a "noded" MultiLinestring, produced by combining a collection of input linestrings and adding additional vertices where they intersect.

#### Example

```sql
-- Create a noded multilinestring from two intersecting lines
SELECT ST_Node(
    ST_GeomFromText('MULTILINESTRING((0 0, 2 2), (0 2, 2 0))')
);
----
MULTILINESTRING ((0 0, 1 1), (1 1, 2 2), (0 2, 1 1), (1 1, 2 0))
```

----

### ST_Normalize


#### Signature

```sql
GEOMETRY ST_Normalize (geom GEOMETRY)
```

#### Description

Returns the "normalized" representation of the geometry

----

### ST_NumGeometries


#### Signature

```sql
INTEGER ST_NumGeometries (geom GEOMETRY)
```

#### Description

Returns the number of component geometries in a collection geometry.
If the input geometry is not a collection, this function returns 0 or 1 depending on if the geometry is empty or not.

----

### ST_NumInteriorRings


#### Signatures

```sql
INTEGER ST_NumInteriorRings (geom GEOMETRY)
INTEGER ST_NumInteriorRings (polygon POLYGON_2D)
```

#### Description

Returns the number of interior rings of a polygon

----

### ST_NumPoints


#### Signatures

```sql
UINTEGER ST_NumPoints (geom GEOMETRY)
UBIGINT ST_NumPoints (point POINT_2D)
UBIGINT ST_NumPoints (linestring LINESTRING_2D)
UBIGINT ST_NumPoints (polygon POLYGON_2D)
UBIGINT ST_NumPoints (box BOX_2D)
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

Returns true if the geometries overlap

----

### ST_Perimeter


#### Signatures

```sql
DOUBLE ST_Perimeter (geom GEOMETRY)
DOUBLE ST_Perimeter (polygon POLYGON_2D)
DOUBLE ST_Perimeter (box BOX_2D)
```

#### Description

Returns the length of the perimeter of the geometry

----

### ST_Perimeter_Spheroid


#### Signatures

```sql
DOUBLE ST_Perimeter_Spheroid (geom GEOMETRY)
DOUBLE ST_Perimeter_Spheroid (poly POLYGON_2D)
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

### ST_Point2DFromWKB


#### Signature

```sql
GEOMETRY ST_Point2DFromWKB (point POINT_2D)
```

#### Description

Deserialize a POINT_2D from a WKB encoded blob

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
GEOMETRY ST_PointN (geom GEOMETRY, index INTEGER)
POINT_2D ST_PointN (linestring LINESTRING_2D, index INTEGER)
```

#### Description

Returns the n'th vertex from the input geometry as a point geometry

----

### ST_PointOnSurface


#### Signature

```sql
GEOMETRY ST_PointOnSurface (geom GEOMETRY)
```

#### Description

Returns a point guaranteed to lie on the surface of the geometry

----

### ST_Points


#### Signature

```sql
GEOMETRY ST_Points (geom GEOMETRY)
```

#### Description

Collects all the vertices in the geometry into a MULTIPOINT

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

### ST_Polygon2DFromWKB


#### Signature

```sql
GEOMETRY ST_Polygon2DFromWKB (polygon POLYGON_2D)
```

#### Description

Deserialize a POLYGON_2D from a WKB encoded blob

----

### ST_Polygonize


#### Signature

```sql
GEOMETRY ST_Polygonize (geometries GEOMETRY[])
```

#### Description

Returns a polygonized representation of the input geometries

#### Example

```sql
-- Create a polygon from a closed linestring ring
SELECT ST_Polygonize([
    ST_GeomFromText('LINESTRING(0 0, 0 10, 10 10, 10 0, 0 0)')
]);
---
GEOMETRYCOLLECTION (POLYGON ((0 0, 0 10, 10 10, 10 0, 0 0)))
```

----

### ST_QuadKey


#### Signatures

```sql
VARCHAR ST_QuadKey (longitude DOUBLE, latitude DOUBLE, level INTEGER)
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
GEOMETRY ST_ReducePrecision (geom GEOMETRY, precision DOUBLE)
```

#### Description

Returns the geometry with all vertices reduced to the given precision

----

### ST_RemoveRepeatedPoints


#### Signatures

```sql
LINESTRING_2D ST_RemoveRepeatedPoints (line LINESTRING_2D)
LINESTRING_2D ST_RemoveRepeatedPoints (line LINESTRING_2D, tolerance DOUBLE)
GEOMETRY ST_RemoveRepeatedPoints (geom GEOMETRY)
GEOMETRY ST_RemoveRepeatedPoints (geom GEOMETRY, tolerance DOUBLE)
```

#### Description

Remove repeated points from a LINESTRING.

----

### ST_Reverse


#### Signature

```sql
GEOMETRY ST_Reverse (geom GEOMETRY)
```

#### Description

Returns the geometry with the order of its vertices reversed

----

### ST_ShortestLine


#### Signature

```sql
GEOMETRY ST_ShortestLine (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns the shortest line between two geometries

----

### ST_Simplify


#### Signature

```sql
GEOMETRY ST_Simplify (geom GEOMETRY, tolerance DOUBLE)
```

#### Description

Returns a simplified version of the geometry

----

### ST_SimplifyPreserveTopology


#### Signature

```sql
GEOMETRY ST_SimplifyPreserveTopology (geom GEOMETRY, tolerance DOUBLE)
```

#### Description

Returns a simplified version of the geometry that preserves topology

----

### ST_Snap


#### Signature

```sql
GEOMETRY ST_Snap (geom GEOMETRY, reference GEOMETRY, tolerance DOUBLE)
```

#### Description

Snaps the vertices and segments of a geometry to another geometry's vertices within the given tolerance

#### Example

```sql
-- Multipolygon snapped to linestring at 1.01x distance
SELECT ST_AsText(ST_Snap(poly, line, ST_Distance(poly, line) * 1.01))
FROM (SELECT
    ST_GeomFromText('MULTIPOLYGON(((26 125,26 200,126 200,126 125,26 125),(51 150,101 150,76 175,51 150)),((151 100,151 200,176 175,151 100)))') AS poly,
    ST_GeomFromText('LINESTRING(5 107,54 84,101 100)') AS line
) AS foo;
----
MULTIPOLYGON (((26 125, 26 200, 126 200, 126 125, 101 100, 26 125), (51 150, 101 150, 76 175, 51 150)), ((151 100, 151 200, 176 175, 151 100)))

-- Multipolygon snapped to linestring at 1.25x distance (more vertices snap)
SELECT ST_AsText(ST_Snap(poly, line, ST_Distance(poly, line) * 1.25))
FROM (SELECT
    ST_GeomFromText('MULTIPOLYGON(((26 125,26 200,126 200,126 125,26 125),(51 150,101 150,76 175,51 150)),((151 100,151 200,176 175,151 100)))') AS poly,
    ST_GeomFromText('LINESTRING(5 107,54 84,101 100)') AS line
) AS foo;
----
MULTIPOLYGON (((5 107, 26 200, 126 200, 126 125, 101 100, 54 84, 5 107), (51 150, 101 150, 76 175, 51 150)), ((151 100, 151 200, 176 175, 151 100)))
```

----

### ST_StartPoint


#### Signatures

```sql
GEOMETRY ST_StartPoint (geom GEOMETRY)
POINT_2D ST_StartPoint (line LINESTRING_2D)
```

#### Description

Returns the start point of a LINESTRING.

----

### ST_SymDifference


#### Signatures

```sql
GEOMETRY ST_SymDifference(geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns a geometry that represents the portions of geom1 and geom2 that do not intersect.

----

### ST_TileEnvelope


#### Signature

```sql
GEOMETRY ST_TileEnvelope (tile_zoom INTEGER, tile_x INTEGER, tile_y INTEGER)
```

#### Description

The `ST_TileEnvelope` scalar function generates tile envelope rectangular polygons from specified zoom level and tile indices.

This is used in MVT generation to select the features corresponding to the tile extent. The envelope is in the Web Mercator
coordinate reference system (EPSG:3857). The tile pyramid starts at zoom level 0, corresponding to a single tile for the
world. Each zoom level doubles the number of tiles in each direction, such that zoom level 1 is 2 tiles wide by 2 tiles high,
zoom level 2 is 4 tiles wide by 4 tiles high, and so on. Tile indices start at `[x=0, y=0]` at the top left, and increase
down and right. For example, at zoom level 2, the top right tile is `[x=3, y=0]`, the bottom left tile is `[x=0, y=3]`, and
the bottom right is `[x=3, y=3]`.

```sql
SELECT ST_TileEnvelope(2, 3, 1);
```

#### Example

```sql
SELECT ST_TileEnvelope(2, 3, 1);
┌───────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                         st_tileenvelope(2, 3, 1)                                          │
│                                                 geometry                                                  │
├───────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ POLYGON ((1.00188E+07 0, 1.00188E+07 1.00188E+07, 2.00375E+07 1.00188E+07, 2.00375E+07 0, 1.00188E+07 0)) │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

----

### ST_Touches


#### Signature

```sql
BOOLEAN ST_Touches (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if the geometries touch

----

### ST_Transform


#### Signatures

```sql
BOX_2D ST_Transform (box BOX_2D, source_crs VARCHAR, target_crs VARCHAR)
BOX_2D ST_Transform (box BOX_2D, source_crs VARCHAR, target_crs VARCHAR, always_xy BOOLEAN)
POINT_2D ST_Transform (point POINT_2D, source_crs VARCHAR, target_crs VARCHAR)
POINT_2D ST_Transform (point POINT_2D, source_crs VARCHAR, target_crs VARCHAR, always_xy BOOLEAN)
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

SELECT
    ST_Transform(
        st_point(52.373123, 4.892360),
        'EPSG:4326',
        'EPSG:3857'
    );
----
POINT (544615.0239773799 6867874.103539125)

-- Alternatively, let's say we got our input point from e.g. a GeoJSON file,
-- which uses WGS84 but with [longitude, latitude] axis order. We can use the
-- `always_xy` parameter to force the input geometry to be interpreted as having
-- a [northing, easting] axis order instead, even though the source coordinate
-- reference system definition (WGS84) says otherwise.

SELECT 
    ST_Transform(
        -- note the axis order is reversed here
        st_point(4.892360, 52.373123),
        'EPSG:4326',
        'EPSG:3857',
        always_xy := true
    );
----
POINT (544615.0239773799 6867874.103539125)

-- Transform a geometry from OSG36 British National Grid EPSG:27700 to EPSG:4326 WGS84
-- Standard transform is often fine for the first few decimal places before being wrong
-- which could result in an error starting at about 10m and possibly much more
SELECT ST_Transform(bng, 'EPSG:27700', 'EPSG:4326', xy := true) AS without_grid_file
FROM (SELECT ST_GeomFromText('POINT( 170370.718 11572.405 )') AS bng);
----
POINT (-5.202992651563592 49.96007490162923)

-- By using an official NTv2 grid file, we can reduce the error down around the 9th decimal place
-- which in theory is below a millimetre, and in practise unlikely that your coordinates are that precise
-- British National Grid "NTv2 format files" download available here:
-- https://www.ordnancesurvey.co.uk/products/os-net/for-developers
SELECT ST_Transform(bng
    , '+proj=tmerc +lat_0=49 +lon_0=-2 +k=0.9996012717 +x_0=400000 +y_0=-100000 +ellps=airy +units=m +no_defs +nadgrids=/full/path/to/OSTN15-NTv2/OSTN15_NTv2_OSGBtoETRS.gsb +type=crs'
    , 'EPSG:4326', xy := true) AS with_grid_file
FROM (SELECT ST_GeomFromText('POINT( 170370.718 11572.405 )') AS bng) t;
----
POINT (-5.203046090608746 49.96006137018598)
```

----

### ST_Union


#### Signature

```sql
GEOMETRY ST_Union (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns the union of two geometries

----

### ST_VoronoiDiagram


#### Signature

```sql
GEOMETRY ST_VoronoiDiagram (geom GEOMETRY)
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

Returns true if the first geometry is within the second

----

### ST_WithinProperly


#### Signature

```sql
BOOLEAN ST_WithinProperly (geom1 GEOMETRY, geom2 GEOMETRY)
```

#### Description

Returns true if the first geometry \"properly\" is contained by the second geometry

This function functions the same as `ST_ContainsProperly`, but the arguments are swapped.

----

### ST_X


#### Signatures

```sql
DOUBLE ST_X (geom GEOMETRY)
DOUBLE ST_X (point POINT_2D)
```

#### Description

Returns the X coordinate of a point geometry

#### Example

```sql
SELECT ST_X(ST_Point(1, 2))
```

----

### ST_XMax


#### Signatures

```sql
DOUBLE ST_XMax (geom GEOMETRY)
DOUBLE ST_XMax (point POINT_2D)
DOUBLE ST_XMax (line LINESTRING_2D)
DOUBLE ST_XMax (polygon POLYGON_2D)
DOUBLE ST_XMax (box BOX_2D)
FLOAT ST_XMax (box BOX_2DF)
```

#### Description

Returns the maximum X coordinate of a geometry

#### Example

```sql
SELECT ST_XMax(ST_Point(1, 2))
```

----

### ST_XMin


#### Signatures

```sql
DOUBLE ST_XMin (geom GEOMETRY)
DOUBLE ST_XMin (point POINT_2D)
DOUBLE ST_XMin (line LINESTRING_2D)
DOUBLE ST_XMin (polygon POLYGON_2D)
DOUBLE ST_XMin (box BOX_2D)
FLOAT ST_XMin (box BOX_2DF)
```

#### Description

Returns the minimum X coordinate of a geometry

#### Example

```sql
SELECT ST_XMin(ST_Point(1, 2))
```

----

### ST_Y


#### Signatures

```sql
DOUBLE ST_Y (geom GEOMETRY)
DOUBLE ST_Y (point POINT_2D)
```

#### Description

Returns the Y coordinate of a point geometry

#### Example

```sql
SELECT ST_Y(ST_Point(1, 2))
```

----

### ST_YMax


#### Signatures

```sql
DOUBLE ST_YMax (geom GEOMETRY)
DOUBLE ST_YMax (point POINT_2D)
DOUBLE ST_YMax (line LINESTRING_2D)
DOUBLE ST_YMax (polygon POLYGON_2D)
DOUBLE ST_YMax (box BOX_2D)
FLOAT ST_YMax (box BOX_2DF)
```

#### Description

Returns the maximum Y coordinate of a geometry

#### Example

```sql
SELECT ST_YMax(ST_Point(1, 2))
```

----

### ST_YMin


#### Signatures

```sql
DOUBLE ST_YMin (geom GEOMETRY)
DOUBLE ST_YMin (point POINT_2D)
DOUBLE ST_YMin (line LINESTRING_2D)
DOUBLE ST_YMin (polygon POLYGON_2D)
DOUBLE ST_YMin (box BOX_2D)
FLOAT ST_YMin (box BOX_2DF)
```

#### Description

Returns the minimum Y coordinate of a geometry

#### Example

```sql
SELECT ST_YMin(ST_Point(1, 2))
```

----

### ST_Z


#### Signature

```sql
DOUBLE ST_Z (geom GEOMETRY)
```

#### Description

Returns the Z coordinate of a point geometry

#### Example

```sql
SELECT ST_Z(ST_Point(1, 2, 3))
```

----

### ST_ZMFlag


#### Signatures

```sql
UTINYINT ST_ZMFlag (geom GEOMETRY)
UTINYINT ST_ZMFlag (wkb WKB_BLOB)
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
DOUBLE ST_ZMax (geom GEOMETRY)
```

#### Description

Returns the maximum Z coordinate of a geometry

#### Example

```sql
SELECT ST_ZMax(ST_Point(1, 2, 3))
```

----

### ST_ZMin


#### Signature

```sql
DOUBLE ST_ZMin (geom GEOMETRY)
```

#### Description

Returns the minimum Z coordinate of a geometry

#### Example

```sql
SELECT ST_ZMin(ST_Point(1, 2, 3))
```

----

## Aggregate Functions

### ST_AsMVT


#### Signatures

```sql
BLOB ST_AsMVT (col0 ANY)
BLOB ST_AsMVT (col0 ANY, col1 VARCHAR)
BLOB ST_AsMVT (col0 ANY, col1 VARCHAR, col2 INTEGER)
BLOB ST_AsMVT (col0 ANY, col1 VARCHAR, col2 INTEGER, col3 VARCHAR)
BLOB ST_AsMVT (col0 ANY, col1 VARCHAR, col2 INTEGER, col3 VARCHAR, col4 VARCHAR)
```

#### Description

Make a Mapbox Vector Tile from a set of geometries and properties
The function takes as input a row type (STRUCT) containing a geometry column and any number of property columns.
It returns a single binary BLOB containing the Mapbox Vector Tile.

The function has the following signature:

`ST_AsMVT(row STRUCT, layer_name VARCHAR DEFAULT 'layer', extent INTEGER DEFAULT 4096, geom_column_name VARCHAR DEFAULT NULL, feature_id_column_name VARCHAR DEFAULT NULL) -> BLOB`

- The first argument is a struct containing the geometry and properties.
- The second argument is the name of the layer in the vector tile. This argument is optional and defaults to 'layer'.
- The third argument is the extent of the tile. This argument is optional and defaults to 4096.
- The fourth argument is the name of the geometry column in the input row. This argument is optional. If not provided, the first geometry column in the input row will be used. If multiple geometry columns are present, an error will be raised.
- The fifth argument is the name of the feature id column in the input row. This argument is optional. If provided, the values in this column will be used as feature ids in the vector tile. The column must be of type INTEGER or BIGINT. If set to negative or NULL, a feature id will not be assigned to the corresponding feature.

The input struct must contain exactly one geometry column of type GEOMETRY. It can contain any number of property columns of types VARCHAR, FLOAT, DOUBLE, INTEGER, BIGINT, or BOOLEAN.

Example:
```sql
SELECT ST_AsMVT({'geom': geom, 'id': id, 'name': name}, 'cities', 4096, 'geom', 'id') AS tile
FROM cities;
 ```

This example creates a vector tile named 'cities' with an extent of 4096 from the 'cities' table, using 'geom' as the geometry column and 'id' as the feature id column.

However, you probably want to use the ST_AsMVTGeom function to first transform and clip your geometries to the tile extent.
The following example assumes the geometry is in WebMercator ("EPSG:3857") coordinates.
Replace `{z}`, `{x}`, and `{y}` with the appropriate tile coordinates, `{your table}` with your table name, and `{tile_path}` with the path to write the tile to.

```sql
COPY (
    SELECT ST_AsMVT({{
        "geometry": ST_AsMVTGeom(
            geometry,
            ST_Extent(ST_TileEnvelope({z}, {x}, {y})),
            4096,
            256,
            false
        )
    }})
    FROM {your table} WHERE ST_Intersects(geometry, ST_TileEnvelope({z}, {x}, {y}))
) to {tile_path} (FORMAT 'BLOB');
```

----

### ST_CoverageInvalidEdges_Agg


#### Signatures

```sql
GEOMETRY ST_CoverageInvalidEdges_Agg (col0 GEOMETRY)
GEOMETRY ST_CoverageInvalidEdges_Agg (col0 GEOMETRY, col1 DOUBLE)
```

#### Description

Returns the invalid edges of a coverage geometry

----

### ST_CoverageSimplify_Agg


#### Signatures

```sql
GEOMETRY ST_CoverageSimplify_Agg (col0 GEOMETRY, col1 DOUBLE)
GEOMETRY ST_CoverageSimplify_Agg (col0 GEOMETRY, col1 DOUBLE, col2 BOOLEAN)
```

#### Description

Simplifies a set of geometries while maintaining coverage

----

### ST_CoverageUnion_Agg


#### Signature

```sql
GEOMETRY ST_CoverageUnion_Agg (col0 GEOMETRY)
```

#### Description

Unions a set of geometries while maintaining coverage

----

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

### ST_MemUnion_Agg


#### Signature

```sql
GEOMETRY ST_MemUnion_Agg (col0 GEOMETRY)
```

#### Description

Computes the union of a set of input geometries.
                "Slower, but might be more memory efficient than ST_UnionAgg as each geometry is merged into the union individually rather than all at once.

----

### ST_Union_Agg


#### Signature

```sql
GEOMETRY ST_Union_Agg (col0 GEOMETRY)
```

#### Description

Computes the union of a set of input geometries

----

## Macro Functions

### ST_Rotate


#### Signature

```sql
GEOMETRY ST_Rotate (geom GEOMETRY, radians double)
```

#### Description

Alias of ST_RotateZ

----

### ST_RotateX


#### Signature

```sql
GEOMETRY ST_RotateX (geom GEOMETRY, radians double)
```

#### Description

Rotates a geometry around the X axis. This is a shorthand macro for calling ST_Affine.

#### Example

```sql
-- Rotate a 3D point 90 degrees (π/2 radians) around the X-axis
SELECT ST_RotateX(ST_GeomFromText('POINT Z(0 1 0)'), pi()/2);
----
POINT Z (0 0 1)
```

----

### ST_RotateY


#### Signature

```sql
GEOMETRY ST_RotateY (geom GEOMETRY, radians double)
```

#### Description

Rotates a geometry around the Y axis. This is a shorthand macro for calling ST_Affine.

#### Example

```sql
-- Rotate a 3D point 90 degrees (π/2 radians) around the Y-axis
SELECT ST_RotateY(ST_GeomFromText('POINT Z(1 0 0)'), pi()/2);
----
POINT Z (0 0 -1)
```

----

### ST_RotateZ


#### Signature

```sql
GEOMETRY ST_RotateZ (geom GEOMETRY, radians double)
```

#### Description

Rotates a geometry around the Z axis. This is a shorthand macro for calling ST_Affine.

#### Example

```sql
-- Rotate a point 90 degrees (π/2 radians) around the Z-axis
SELECT ST_RotateZ(ST_Point(1, 0), pi()/2);
----
POINT (0 1)
```

----

### ST_Scale


#### Signatures

```sql
GEOMETRY ST_Scale (geom GEOMETRY, xs double, ys double, zs double)
GEOMETRY ST_Scale (geom GEOMETRY, xs double, ys double)
```

----

### ST_TransScale


#### Signature

```sql
GEOMETRY ST_TransScale (geom GEOMETRY, dx double, dy double, xs double, ys double)
```

#### Description

Translates and then scales a geometry in X and Y direction. This is a shorthand macro for calling ST_Affine.

#### Example

```sql
-- Translate by (1, 2) then scale by (2, 3)
SELECT ST_TransScale(ST_Point(1, 1), 1, 2, 2, 3);
----
POINT (4 9)
```

----

### ST_Translate


#### Signatures

```sql
GEOMETRY ST_Translate (geom GEOMETRY, dx double, dy double, dz double)
GEOMETRY ST_Translate (geom GEOMETRY, dx double, dy double)
```

----

## Table Functions

### ST_Drivers

#### Signature

```sql
ST_Drivers ()
```

#### Description

Returns the list of supported GDAL drivers and file formats

Note that far from all of these drivers have been tested properly.
Some may require additional options to be passed to work as expected.
If you run into any issues please first consult the [consult the GDAL docs](https://gdal.org/drivers/vector/index.html).

#### Example

```sql
SELECT * FROM ST_Drivers();
```

----

### ST_GeneratePoints

#### Signature

```sql
ST_GeneratePoints (col0 BOX_2D, col1 BIGINT)
ST_GeneratePoints (col0 BOX_2D, col1 BIGINT, col2 BIGINT)
```

#### Description

Generates a set of random points within the specified bounding box.

Takes a bounding box (min_x, min_y, max_x, max_y), a count of points to generate, and optionally a seed for the random number generator.

#### Example

```sql
SELECT * FROM ST_GeneratePoints({min_x: 0, min_y:0, max_x:10, max_y:10}::BOX_2D, 5, 42);
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

### ST_ReadSHP

#### Signature

```sql
ST_ReadSHP (col0 VARCHAR, encoding VARCHAR)
```

#### Description

Read a Shapefile without relying on the GDAL library

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

## Window Functions

### ST_ClusterDBSCAN

#### Signatures

```sql
INTEGER ST_ClusterDBSCAN (point POINT_2D, eps DOUBLE, minpoints BIGINT)
```

#### Description

A window function that returns a cluster number (0-indexed integer) for each input point using the 2D [Density-Based Spatial Clustering of Applications with Noise (DBSCAN)](https://en.wikipedia.org/wiki/DBSCAN) algorithm.

Unlike centroid-based clustering (such as $k$-means), DBSCAN does not require the number of clusters to be specified in advance. Instead, it discovers arbitrarily shaped spatial clusters based on density parameters:
- `eps`: The maximum distance threshold ($\le \varepsilon$) for neighborhood expansion. Distances are evaluated as Cartesian Euclidean distances in the coordinate units of the input geometry.
- `minpoints`: The minimum number of points required within the `eps`-neighborhood (including the point itself) to form a dense "core" cluster.

A point is assigned to a cluster if it is either:
- A **core point**: has at least `minpoints` within `eps` distance (including itself); or
- A **border point**: is within `eps` distance of a core point.

Points that do not meet the criteria to join any cluster are considered **noise** and are assigned a cluster number of `NULL`.

To cluster within independent partitions (e.g. per city, date, or category), use the `OVER (PARTITION BY ...)` clause. Cluster IDs restart from 0 within each partition. To ensure deterministic assignment of border points that lie between two clusters across repeated runs, include an `ORDER BY` clause in the window definition.

#### Example

```sql
-- Create a table of sample points with two clusters and one isolated noise point
CREATE TABLE points AS SELECT {'x': x::DOUBLE, 'y': y::DOUBLE}::POINT_2D AS pt, id FROM (
    VALUES
        (0.0, 0.0, 1),
        (0.1, 0.0, 2),
        (0.0, 0.1, 3),
        (0.1, 0.1, 4),
        (5.0, 5.0, 5),
        (5.1, 5.0, 6),
        (5.0, 5.1, 7),
        (5.1, 5.1, 8),
        (2.5, 2.5, 9)
) t(x, y, id);

-- Cluster points with eps = 0.5 and minpoints = 3
SELECT id, pt, ST_ClusterDBSCAN(pt, 0.5, 3) OVER () AS cluster_id
FROM points
ORDER BY id;
----
-- Output:
-- 1 | {'x': 0.0, 'y': 0.0} | 0
-- 2 | {'x': 0.1, 'y': 0.0} | 0
-- 3 | {'x': 0.0, 'y': 0.1} | 0
-- 4 | {'x': 0.1, 'y': 0.1} | 0
-- 5 | {'x': 5.0, 'y': 5.0} | 1
-- 6 | {'x': 5.1, 'y': 5.0} | 1
-- 7 | {'x': 5.0, 'y': 5.1} | 1
-- 8 | {'x': 5.1, 'y': 5.1} | 1
-- 9 | {'x': 2.5, 'y': 2.5} | NULL

-- Summarize clusters with point counts and centroids
SELECT 
    cluster_id,
    count(*) AS point_count,
    avg(pt.x) AS centroid_x,
    avg(pt.y) AS centroid_y
FROM (
    SELECT pt, ST_ClusterDBSCAN(pt, 0.5, 3) OVER () AS cluster_id
    FROM points
)
WHERE cluster_id IS NOT NULL
GROUP BY cluster_id
ORDER BY cluster_id;
```

----
