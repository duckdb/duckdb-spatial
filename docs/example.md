# Example Usage
The following is a slightly contrived example of how you can use this extension to read and export multiple geospatial data formats, transform geometries between different coordinate reference systems and work with spatial property and predicate functions.

For a complete list of all supported functions, please have a look at the [function reference](functions.md).

Let's start by loading the spatial extension and the parquet extension so we can import the NYC taxi ride data in parquet format, and the accompanying taxi zone data from a shapefile, using the spatial `ST_Read` gdal-based table function. We then create a table for the rides and a table for the zones. Note that `ST_Read` produces a table with a `wkb_geometry` column that contains the geometry data encoded as a WKB (Well-Known Binary) blob, which we then convert to the `GEOMETRY` type using the `ST_GeomFromWKB` function.
```sql
LOAD spatial;
LOAD parquet;

CREATE TABLE rides AS SELECT *
FROM './test/data/nyc_taxi/yellow_tripdata_2010-01-limit1mil.parquet';

-- Load the NYC taxi zone data from a shapefile using the gdal-based st_read function
CREATE TABLE zones AS SELECT zone, LocationId, borough, geom
FROM st_read('./test/data/nyc_taxi/taxi_zones/taxi_zones.shp');
```
<details>
<summary>
    SELECT * FROM rides LIMIT 10;
</summary>

| vendor_id |   pickup_datetime   |  dropoff_datetime   | passenger_count |   trip_distance    |  pickup_longitude  | pickup_latitude | rate_code | store_and_fwd_flag | dropoff_longitude  | dropoff_latitude | payment_type | fare_amount | surcharge | mta_tax | tip_amount | tolls_amount | total_amount |
|-----------|---------------------|---------------------|-----------------|--------------------|--------------------|-----------------|-----------|--------------------|--------------------|------------------|--------------|-------------|-----------|---------|------------|--------------|--------------|
| VTS       | 2010-01-01 00:00:17 | 2010-01-01 00:00:17 | 3               | 0.0                | -73.87105699999998 | 40.773522       | 1         |                    | -73.871048         | 40.773545        | CAS          | 45.0        | 0.0       | 0.5     | 0.0        | 0.0          | 45.5         |
| VTS       | 2010-01-01 00:00:20 | 2010-01-01 00:00:20 | 1               | 0.05               | -73.97512999999998 | 40.789973       | 1         |                    | -73.97498799999998 | 40.790598        | CAS          | 2.5         | 0.5       | 0.5     | 0.0        | 0.0          | 3.5          |
| CMT       | 2010-01-01 00:00:23 | 2010-01-01 00:00:25 | 1               | 0.0                | -73.999431         | 40.71216        | 1         | 0                  | -73.99915799999998 | 40.712421        | No           | 2.5         | 0.5       | 0.5     | 0.0        | 0.0          | 3.5          |
| CMT       | 2010-01-01 00:00:33 | 2010-01-01 00:00:55 | 1               | 0.0                | -73.97721699999998 | 40.749633       | 1         | 0                  | -73.97732899999998 | 40.749629        | Cas          | 2.5         | 0.5       | 0.5     | 0.0        | 0.0          | 3.5          |
| VTS       | 2010-01-01 00:01:00 | 2010-01-01 00:01:00 | 1               | 0.0                | -73.942313         | 40.784332       | 1         |                    | -73.942313         | 40.784332        | Cre          | 10.0        | 0.0       | 0.5     | 2.0        | 0.0          | 12.5         |
| VTS       | 2010-01-01 00:01:06 | 2010-01-01 00:01:06 | 2               | 0.38               | -73.97463          | 40.756687       | 1         |                    | -73.979872         | 40.759143        | CAS          | 3.7         | 0.5       | 0.5     | 0.0        | 0.0          | 4.7          |
| VTS       | 2010-01-01 00:01:07 | 2010-01-01 00:01:07 | 2               | 0.23               | -73.987358         | 40.718475       | 1         |                    | -73.98518          | 40.720468        | CAS          | 2.9         | 0.5       | 0.5     | 0.0        | 0.0          | 3.9          |
| CMT       | 2010-01-01 00:00:02 | 2010-01-01 00:01:08 | 1               | 0.1                | -73.992807         | 40.741418       | 1         | 0                  | -73.995799         | 40.742596        | No           | 2.9         | 0.5       | 0.5     | 0.0        | 0.0          | 3.9          |
| VTS       | 2010-01-01 00:01:23 | 2010-01-01 00:01:23 | 1               | 0.6099999999999999 | -73.98003799999998 | 40.74306        | 1         |                    | -73.974862         | 40.750387        | CAS          | 3.7         | 0.5       | 0.5     | 0.0        | 0.0          | 4.7          |
| VTS       | 2010-01-01 00:01:34 | 2010-01-01 00:01:34 | 1               | 0.02               | -73.954122         | 40.801173       | 1         |                    | -73.95431499999998 | 40.800897        | CAS          | 45.0        | 0.0       | 0.5     | 0.0        | 0.0          | 45.5         |

</details>

<details>
<summary>
    SELECT * FROM zones LIMIT 10;
</summary>

|          zone           | LocationID |    borough    |  geom              |
|-------------------------|------------|---------------|--------------------|
| Newark Airport          | 1          | EWR           | POLYGON (...)      |
| Jamaica Bay             | 2          | Queens        | MULTIPOLYGON (...) |
| Allerton/Pelham Gardens | 3          | Bronx         | POLYGON (...)      |
| Alphabet City           | 4          | Manhattan     | POLYGON (...)      |
| Arden Heights           | 5          | Staten Island | POLYGON (...)      |
| Arrochar/Fort Wadsworth | 6          | Staten Island | POLYGON (...)      |
| Astoria                 | 7          | Queens        | POLYGON (...)      |
| Astoria Park            | 8          | Queens        | POLYGON (...)      |
| Auburndale              | 9          | Queens        | POLYGON (...)      |
| Baisley Park            | 10         | Queens        | POLYGON (...)      |

</details>

Let's compare the trip distance to the linear distance between the pickup and dropoff points to figure out how efficient the taxi drivers are (or how dirty the data is, since some diffs seem to be negative). We transform the coordinates from WGS84 (EPSG:4326) (lat/lon) to the NAD83 / New York Long Island ftUS (ESRI:102718) projection and calculate the distance using the `ST_Distance` function, which in this case gives the distance in feet, which we then convert to miles (5280 ft/mile). Trips that are smaller than the aerial distance are likely to be erroneous, so we use this query to filter out some bad data. Although this is not entirely accurate since the distance we use does not take into account the curvature of the earth, but it is a good enough approximation for our purposes.
```sql
CREATE TABLE cleaned_rides AS SELECT
    st_point(pickup_latitude, pickup_longitude) as pickup_point,
    st_point(dropoff_latitude, dropoff_longitude) as dropoff_point,
    dropoff_datetime::TIMESTAMP - pickup_datetime::TIMESTAMP as time,
    trip_distance,
    st_distance(
        st_transform(pickup_point, 'EPSG:4326', 'ESRI:102718'),
        st_transform(dropoff_point, 'EPSG:4326', 'ESRI:102718')) / 5280 as aerial_distance,
    trip_distance - aerial_distance as diff
FROM rides
WHERE diff > 0
ORDER BY diff DESC;
```
<details>
<summary>
    SELECT * FROM cleaned_rides LIMIT 10;
</summary>

|             pickup_point             |            dropoff_point             |   time   | trip_distance |   aerial_distance    |        diff        |
|--------------------------------------|--------------------------------------|----------|---------------|----------------------|--------------------|
| POINT (40.758149 -73.963267)         | POINT (40.743807 -73.915763)         | 01:49:25 | 47.4          | 2.6820365663951677   | 44.71796343360483  |
| POINT (40.764592 -73.971798)         | POINT (40.743878 -73.991015)         | 01:09:29 | 45.9          | 1.7492118606943174   | 44.15078813930568  |
| POINT (40.733306 -73.987289)         | POINT (40.758895 -73.987341)         | 02:15:53 | 45.2          | 1.7657013363262366   | 43.434298663673765 |
| POINT (40.755965 -73.973138)         | POINT (40.756137 -73.973535)         | 02:48:19 | 41.9          | 0.02397481410159387  | 41.876025185898406 |
| POINT (40.645337 -73.77656899999998) | POINT (40.645389 -73.77632699999998) | 00:50:09 | 41.4          | 0.013215531558232116 | 41.38678446844177  |
| POINT (40.743983 -73.986063)         | POINT (40.759483 -73.985377)         | 00:00:18 | 41.9          | 1.070141742954722    | 40.829858257045274 |
| POINT (40.782197 -73.98247)          | POINT (40.713868 -74.03883)          | 02:14:00 | 45.05         | 5.565739906339228    | 39.484260093660765 |
| POINT (40.662826 -73.78962799999998) | POINT (40.656764 -73.794222)         | 02:19:32 | 39.8          | 0.4829476946393737   | 39.317052305360626 |
| POINT (40.644837 -73.781722)         | POINT (40.646732 -73.801497)         | 01:29:00 | 39.8          | 1.0475276883275062   | 38.75247231167249  |
| POINT (40.754573 -73.98841199999998) | POINT (40.763428 -73.968002)         | 01:31:00 | 39.74         | 1.2329411412165936   | 38.50705885878341  |
</details>

Now lets join the taxi rides with the taxi zones to get the start and end zone for each ride using the `ST_Within` function to check if a point is within a polygon. Again we need to transform the coordinates from WGS84 to the NAD83 since the taxi zone data also use that projection.
```sql
-- Since we dont have spatial indexes yet, use a smaller dataset for the following example.
DELETE FROM cleaned_rides WHERE rowid > 5000;

CREATE TABLE joined AS
SELECT
    pickup_point,
    dropoff_point,
    start_zone.zone as start_zone,
    end_zone.zone as end_zone,
    trip_distance,
    time,
FROM cleaned_rides
JOIN zones as start_zone
ON ST_Within(st_transform(pickup_point, 'EPSG:4326', 'ESRI:102718'), start_zone.geom)
JOIN zones as end_zone
ON ST_Within(st_transform(dropoff_point, 'EPSG:4326', 'ESRI:102718'), end_zone.geom);
```
<details>
<summary>
    SELECT * FROM joined USING SAMPLE 10 ROWS;
</summary>

|             pickup_point             |            dropoff_point             |        start_zone        |           end_zone            | trip_distance |   time   |
|--------------------------------------|--------------------------------------|--------------------------|-------------------------------|---------------|----------|
| POINT (40.722223 -73.98385299999998) | POINT (40.715507 -73.992438)         | East Village             | Lower East Side               | 10.3          | 00:19:16 |
| POINT (40.648687 -73.783522)         | POINT (40.649567 -74.005812)         | JFK Airport              | Sunset Park West              | 23.57         | 00:28:00 |
| POINT (40.761603 -73.96661299999998) | POINT (40.760232 -73.96344499999998) | Upper East Side South    | Sutton Place/Turtle Bay North | 17.6          | 00:27:05 |
| POINT (40.697212 -73.937495)         | POINT (40.652377 -73.93983299999998) | Stuyvesant Heights       | East Flatbush/Farragut        | 13.55         | 00:24:00 |
| POINT (40.721462 -73.993583)         | POINT (40.774205 -73.90441699999998) | Lower East Side          | Steinway                      | 28.75         | 01:03:00 |
| POINT (40.716955 -74.004328)         | POINT (40.754688 -73.991612)         | TriBeCa/Civic Center     | Garment District              | 18.4          | 00:46:12 |
| POINT (40.740052 -73.994918)         | POINT (40.75439 -73.98587499999998)  | Flatiron                 | Garment District              | 24.2          | 00:35:25 |
| POINT (40.763017 -73.95949199999998) | POINT (40.763615 -73.959182)         | Lenox Hill East          | Lenox Hill West               | 18.4          | 00:33:46 |
| POINT (40.865663 -73.927458)         | POINT (40.86537 -73.927352)          | Washington Heights North | Washington Heights North      | 10.47         | 00:27:00 |
| POINT (40.738408 -73.980345)         | POINT (40.696038 -73.955493)         | Gramercy                 | Bedford                       | 16.4          | 00:21:47 |
</details>

We can export the joined table to a GeoJSONSeq file using the GDAL copy function, passing in a GDAL layer creation option.
Since GeoJSON only supports a single geometry per feature, we can use the `ST_MakeLine` function to combine the pickup and dropoff points into a single line geometry. The default coordinate reference system for GeoJSON is WGS84, but the coordinates are expected to be in longitude/latitude, so we need to flip the geometry using the `ST_FlipCoordinates` function.

```sql
COPY (
    SELECT
        ST_AsWKB(ST_FlipCoordinates(ST_MakeLine(pickup_point, dropoff_point))) as wkb_geometry,
        start_zone,
        end_zone,
        time::VARCHAR as trip_time
    FROM joined)
TO 'joined.geojsonseq'
WITH (FORMAT GDAL, DRIVER 'GeoJSONSeq', LAYER_CREATION_OPTIONS 'WRITE_BBOX=YES');
```
<details>
<summary>
    head -n 10 joined.geojsonseq
</summary>

```json
{ "type": "Feature", "properties": { "start_zone": "JFK Airport", "end_zone": "Park Slope", "trip_time": "00:52:00" }, "geometry": { "type": "LineString", "coordinates": [ [ -73.789923, 40.643515 ], [ -73.97608, 40.680395 ] ] } }
{ "type": "Feature", "properties": { "start_zone": "JFK Airport", "end_zone": "Park Slope", "trip_time": "00:35:00" }, "geometry": { "type": "LineString", "coordinates": [ [ -73.776445, 40.645422 ], [ -73.98427, 40.670782 ] ] } }
{ "type": "Feature", "properties": { "start_zone": "JFK Airport", "end_zone": "Park Slope", "trip_time": "00:45:42" }, "geometry": { "type": "LineString", "coordinates": [ [ -73.776878, 40.645065 ], [ -73.992153, 40.662571 ] ] } }
{ "type": "Feature", "properties": { "start_zone": "JFK Airport", "end_zone": "Park Slope", "trip_time": "00:36:00" }, "geometry": { "type": "LineString", "coordinates": [ [ -73.788028, 40.641508 ], [ -73.97584, 40.670927 ] ] } }
{ "type": "Feature", "properties": { "start_zone": "JFK Airport", "end_zone": "Park Slope", "trip_time": "00:47:58" }, "geometry": { "type": "LineString", "coordinates": [ [ -73.781855, 40.644749 ], [ -73.980129, 40.663663 ] ] } }
{ "type": "Feature", "properties": { "start_zone": "JFK Airport", "end_zone": "Park Slope", "trip_time": "00:32:10" }, "geometry": { "type": "LineString", "coordinates": [ [ -73.787494, 40.641559 ], [ -73.974694, 40.673479 ] ] } }
{ "type": "Feature", "properties": { "start_zone": "JFK Airport", "end_zone": "Park Slope", "trip_time": "00:36:59" }, "geometry": { "type": "LineString", "coordinates": [ [ -73.790138, 40.643342 ], [ -73.982721, 40.662379 ] ] } }
{ "type": "Feature", "properties": { "start_zone": "JFK Airport", "end_zone": "Park Slope", "trip_time": "00:32:00" }, "geometry": { "type": "LineString", "coordinates": [ [ -73.786952, 40.641248 ], [ -73.97421, 40.676237 ] ] } }
{ "type": "Feature", "properties": { "start_zone": "JFK Airport", "end_zone": "Park Slope", "trip_time": "00:33:21" }, "geometry": { "type": "LineString", "coordinates": [ [ -73.783892, 40.648514 ], [ -73.979283, 40.669721 ] ] } }
{ "type": "Feature", "properties": { "start_zone": "JFK Airport", "end_zone": "Park Slope", "trip_time": "00:35:45" }, "geometry": { "type": "LineString", "coordinates": [ [ -73.776643, 40.645272 ], [ -73.978873, 40.66723 ] ] } }
```
</details>

## GDAL Layer creation option

If you need to set some "Layer creation option" (format specific), i.e. those options that you set via CLI with `-lco`, you can use this kind of syntax, in which the `WRITE_BBOX` and `RFC7946` - available in [`GeoJSON` format](https://gdal.org/drivers/vector/geojson.html#layer-creation-options) - options are set:

```
COPY (SELECT * from st_read('input.shp'))
TO 'output.geojson'
WITH (FORMAT GDAL, DRIVER 'GeoJSON',LAYER_CREATION_OPTIONS ('WRITE_BBOX=YES', 'RFC7946=YES'))
```

## Spatial Density Clustering with DBSCAN (`ST_ClusterDBSCAN`)

We can discover high-density taxi pickup hotspots and identify isolated outlier trips using the `ST_ClusterDBSCAN` window aggregate function.

Because DBSCAN evaluates Cartesian Euclidean distance, we first project the pickup coordinates into a planar metric coordinate system such as State Plane New York Long Island ftUS (`ESRI:102718`) or UTM Zone 18N (`EPSG:32618`), where distance values represent physical lengths (e.g. feet or meters) rather than angular degrees:

```sql
-- Cluster taxi pickup points within 200 feet of each other with at least 5 pickups
CREATE TABLE pickup_clusters AS
WITH projected_pickups AS (
    SELECT 
        rowid,
        pickup_point,
        -- Transform pickup coordinates to projected planar feet
        st_transform(pickup_point, 'EPSG:4326', 'ESRI:102718') AS projected_geom
    FROM cleaned_rides
    WHERE pickup_point IS NOT NULL
)
SELECT 
    rowid,
    pickup_point,
    -- Execute DBSCAN: eps = 200 feet, minpoints = 5
    ST_ClusterDBSCAN(
        {'x': st_x(projected_geom), 'y': st_y(projected_geom)}::POINT_2D, 
        200.0, 
        5
    ) OVER () AS cluster_id
FROM projected_pickups;
```
<details>
<summary>
    SELECT * FROM pickup_clusters LIMIT 10;
</summary>

| rowid |         pickup_point         | cluster_id |
|-------|------------------------------|------------|
| 1     | POINT (40.773522 -73.871057) | 0          |
| 2     | POINT (40.789973 -73.975130) | 1          |
| 3     | POINT (40.712160 -73.999431) | NULL       |
| 4     | POINT (40.749633 -73.977217) | NULL       |
| 5     | POINT (40.784332 -73.942313) | NULL       |
| 6     | POINT (40.756687 -73.974630) | NULL       |
| 7     | POINT (40.718475 -73.987358) | 180        |
| 8     | POINT (40.741418 -73.992807) | 2          |
| 9     | POINT (40.743060 -73.980038) | 3          |
| 10    | POINT (40.801173 -73.954122) | NULL       |

</details>

We can then aggregate the discovered clusters to identify the most active pickup hubs, filter out noise (`NULL`), and compute cluster centroids:

```sql
SELECT 
    cluster_id,
    count(*) AS total_pickups,
    round(avg(st_x(pickup_point)), 6) AS centroid_lat,
    round(avg(st_y(pickup_point)), 6) AS centroid_lon
FROM pickup_clusters
WHERE cluster_id IS NOT NULL
GROUP BY cluster_id
ORDER BY total_pickups DESC
LIMIT 10;
```
<details>
<summary>
    Top 10 Taxi Pickup Hotspots
</summary>

| cluster_id | total_pickups | centroid_lat | centroid_lon | Approximate Location |
|------------|---------------|--------------|--------------|----------------------|
| 9          | 71            | 40.766056    | -73.983470   | Columbus Circle / Central Park South |
| 69         | 66            | 40.777125    | -73.952293   | Upper East Side (2nd Ave & 79th St) |
| 30         | 65            | 40.773499    | -73.982080   | Lincoln Center / Broadway |
| 40         | 62            | 40.755625    | -73.967997   | Midtown East / United Nations |
| 33         | 56            | 40.740448    | -74.005673   | Chelsea / Meatpacking District |
| 34         | 56            | 40.759886    | -73.980512   | Rockefeller Center / Midtown |
| 24         | 55            | 40.749983    | -73.991688   | Herald Square / Penn Station |
| 51         | 54            | 40.755785    | -73.983498   | Times Square / Theater District |
| 23         | 50            | 40.763401    | -73.973502   | 5th Avenue & 59th St (The Plaza) |
| 104        | 50            | 40.734219    | -73.990966   | Union Square |

</details>



