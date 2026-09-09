-- Five-city training catalog; these are synthetic demonstration stations,
-- not verified operating locations. Coordinates approximate city districts.
-- Keep all existing rows (including operator edits and charger state/counters).
-- Natural codes identify demo records; database-generated IDs avoid collisions.
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;
BEGIN IMMEDIATE;

INSERT OR IGNORE INTO stations (
    code, name, address, latitude, longitude, price_cents_per_kwh, status
) VALUES
    ('STA-DEMO-001', '高新园区示范充电站', '大连市高新园区软件园路示范点', 38.884700, 121.526900, 120, 'ACTIVE'),
    ('STA-DEMO-002', '海创中心示范充电站', '大连市高新园区黄浦路示范点', 38.866800, 121.533200, 98, 'ACTIVE'),
    ('STA-DEMO-003', '生态科技城示范充电站', '大连市甘井子区生态科技城示范点', 39.010900, 121.505500, 150, 'ACTIVE'),
    ('STA-CITY-DL-004', '大连沙河口示范充电站', '大连市沙河口区星海片区实训示范点', 38.886000, 121.586000, 110, 'ACTIVE'),
    ('STA-CITY-DL-005', '大连中山示范充电站', '大连市中山区东港片区实训示范点', 38.921000, 121.655000, 135, 'ACTIVE'),
    ('STA-CITY-SY-001', '沈阳沈河示范充电站', '沈阳市沈河区市府片区实训示范点', 41.806000, 123.439000, 105, 'ACTIVE'),
    ('STA-CITY-SY-002', '沈阳和平示范充电站', '沈阳市和平区太原街片区实训示范点', 41.795000, 123.404000, 120, 'ACTIVE'),
    ('STA-CITY-SY-003', '沈阳铁西示范充电站', '沈阳市铁西区兴华街片区实训示范点', 41.803000, 123.357000, 98, 'ACTIVE'),
    ('STA-CITY-SY-004', '沈阳皇姑示范充电站', '沈阳市皇姑区北陵片区实训示范点', 41.835000, 123.425000, 135, 'ACTIVE'),
    ('STA-CITY-SY-005', '沈阳浑南示范充电站', '沈阳市浑南区奥体片区实训示范点', 41.748000, 123.455000, 110, 'ACTIVE'),
    ('STA-CITY-BJ-001', '北京东城示范充电站', '北京市东城区东单片区实训示范点', 39.913000, 116.420000, 145, 'ACTIVE'),
    ('STA-CITY-BJ-002', '北京西城示范充电站', '北京市西城区西单片区实训示范点', 39.913000, 116.367000, 130, 'ACTIVE'),
    ('STA-CITY-BJ-003', '北京海淀示范充电站', '北京市海淀区中关村片区实训示范点', 39.984000, 116.316000, 125, 'ACTIVE'),
    ('STA-CITY-BJ-004', '北京朝阳示范充电站', '北京市朝阳区朝阳公园片区实训示范点', 39.936000, 116.478000, 150, 'ACTIVE'),
    ('STA-CITY-BJ-005', '北京丰台示范充电站', '北京市丰台区科技园片区实训示范点', 39.834000, 116.287000, 110, 'ACTIVE'),
    ('STA-CITY-SH-001', '上海黄浦示范充电站', '上海市黄浦区人民广场片区实训示范点', 31.233000, 121.475000, 150, 'ACTIVE'),
    ('STA-CITY-SH-002', '上海徐汇示范充电站', '上海市徐汇区徐家汇片区实训示范点', 31.191000, 121.437000, 140, 'ACTIVE'),
    ('STA-CITY-SH-003', '上海静安示范充电站', '上海市静安区静安寺片区实训示范点', 31.224000, 121.446000, 135, 'ACTIVE'),
    ('STA-CITY-SH-004', '上海浦东示范充电站', '上海市浦东新区世纪公园片区实训示范点', 31.219000, 121.553000, 120, 'ACTIVE'),
    ('STA-CITY-SH-005', '上海闵行示范充电站', '上海市闵行区莘庄片区实训示范点', 31.115000, 121.385000, 105, 'ACTIVE'),
    ('STA-CITY-SZ-001', '深圳福田示范充电站', '深圳市福田区市民中心片区实训示范点', 22.543000, 114.058000, 145, 'ACTIVE'),
    ('STA-CITY-SZ-002', '深圳罗湖示范充电站', '深圳市罗湖区东门片区实训示范点', 22.549000, 114.122000, 130, 'ACTIVE'),
    ('STA-CITY-SZ-003', '深圳南山示范充电站', '深圳市南山区科技园片区实训示范点', 22.540000, 113.950000, 125, 'ACTIVE'),
    ('STA-CITY-SZ-004', '深圳宝安示范充电站', '深圳市宝安区中心片区实训示范点', 22.555000, 113.885000, 110, 'ACTIVE'),
    ('STA-CITY-SZ-005', '深圳龙岗示范充电站', '深圳市龙岗区龙城片区实训示范点', 22.723000, 114.246000, 98, 'ACTIVE');

-- Original Dalian chargers retain their canonical codes. Two missing chargers
-- are added to make three per station without changing the seven existing rows.
WITH catalog(station_code, code, type, power_watts, status) AS (
    VALUES
    ('STA-DEMO-001', 'CHG-DEMO-001-A1', 'FAST', 120000, 'AVAILABLE'),
    ('STA-DEMO-001', 'CHG-DEMO-001-A2', 'FAST', 120000, 'AVAILABLE'),
    ('STA-DEMO-001', 'CHG-DEMO-001-B1', 'SLOW', 7000, 'FAULT'),
    ('STA-DEMO-002', 'CHG-DEMO-002-A1', 'FAST', 60000, 'AVAILABLE'),
    ('STA-DEMO-002', 'CHG-DEMO-002-A2', 'FAST', 60000, 'AVAILABLE'),
    ('STA-DEMO-002', 'CHG-DEMO-002-B1', 'SLOW', 7000, 'AVAILABLE'),
    ('STA-DEMO-003', 'CHG-DEMO-003-A1', 'FAST', 180000, 'AVAILABLE'),
    ('STA-DEMO-003', 'CHG-DEMO-003-A2', 'FAST', 180000, 'OFFLINE'),
    ('STA-DEMO-003', 'CHG-DEMO-003-B1', 'SLOW', 7000, 'AVAILABLE')
)
INSERT OR IGNORE INTO chargers(station_id, code, type, power_watts, status)
SELECT stations.id, catalog.code, catalog.type, catalog.power_watts, catalog.status
FROM catalog JOIN stations ON stations.code = catalog.station_code;

WITH station_codes(code) AS (
    VALUES
    ('STA-CITY-DL-004'),
    ('STA-CITY-DL-005'),
    ('STA-CITY-SY-001'),
    ('STA-CITY-SY-002'),
    ('STA-CITY-SY-003'),
    ('STA-CITY-SY-004'),
    ('STA-CITY-SY-005'),
    ('STA-CITY-BJ-001'),
    ('STA-CITY-BJ-002'),
    ('STA-CITY-BJ-003'),
    ('STA-CITY-BJ-004'),
    ('STA-CITY-BJ-005'),
    ('STA-CITY-SH-001'),
    ('STA-CITY-SH-002'),
    ('STA-CITY-SH-003'),
    ('STA-CITY-SH-004'),
    ('STA-CITY-SH-005'),
    ('STA-CITY-SZ-001'),
    ('STA-CITY-SZ-002'),
    ('STA-CITY-SZ-003'),
    ('STA-CITY-SZ-004'),
    ('STA-CITY-SZ-005')
), connectors(suffix, type, power_watts) AS (
    VALUES ('-A1', 'FAST', 120000), ('-A2', 'FAST', 60000), ('-B1', 'SLOW', 7000)
)
INSERT OR IGNORE INTO chargers(station_id, code, type, power_watts, status)
SELECT stations.id, 'CHG' || substr(stations.code, 4) || connectors.suffix,
       connectors.type, connectors.power_watts, 'AVAILABLE'
FROM station_codes
JOIN stations ON stations.code = station_codes.code
CROSS JOIN connectors;

COMMIT;
