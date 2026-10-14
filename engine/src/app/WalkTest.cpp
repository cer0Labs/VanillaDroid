#include "app/WalkTest.h"

#include "app/PlayerController.h"
#include "core/Log.h"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vd {
namespace {

constexpr f32 kReachDistance = 1.5f;  // yards (2D) from a point to count it as reached
constexpr f32 kStuckSeconds = 1.5f;   // without getting closer: try a way around
constexpr f32 kFallReport = 3.0f;     // falls higher than this are listed
constexpr f32 kOnlineJumpSeconds = 6.0f;

// Patrol paths of vMaNGOS' database (creature_movement, creature_movement_template): x, y, z of each point.
// The coordinates come from the vMaNGOS world database (https://github.com/vmangos/core, GNU GPL v2 or later).
//   Goldshire: from the inn along the road to Stormwind's gate (template 3937), over the bridge into the Valley of
//              Heroes (guard 79857).
//   Stormwind: the Emissaries' round through the city (guid 190520), back to its start.
//   Orgrimmar: the Defilers Emissary's round (guid 190532) and Warcaller Gorlach's (guid 3349).
constexpr WalkPoint kGoldshire[] = {
    {-9456.96f, 65.69f, 56.04f}, {-9420.60f, 84.00f, 56.67f}, {-9406.71f, 91.63f, 57.86f}, {-9385.48f, 104.00f, 59.65f},
    {-9366.48f, 110.45f, 61.13f}, {-9341.06f, 122.63f, 63.36f}, {-9319.76f, 134.25f, 64.43f}, {-9301.26f, 142.17f, 65.74f},
    {-9278.15f, 149.64f, 66.78f}, {-9259.72f, 157.86f, 67.52f}, {-9245.17f, 171.72f, 67.91f}, {-9234.34f, 183.79f, 67.65f},
    {-9221.37f, 199.33f, 68.63f}, {-9204.70f, 216.77f, 70.88f}, {-9191.80f, 232.33f, 71.83f}, {-9184.40f, 250.70f, 73.43f},
    {-9182.43f, 270.72f, 74.97f}, {-9181.05f, 298.80f, 78.01f}, {-9178.72f, 318.85f, 80.03f}, {-9175.71f, 330.35f, 82.21f},
    {-9169.73f, 344.79f, 85.74f}, {-9158.13f, 360.97f, 89.46f}, {-9146.60f, 372.21f, 90.57f}, {-9127.98f, 387.88f, 91.12f},
    {-9077.30f, 442.09f, 93.30f}, {-9057.36f, 456.67f, 93.29f}, {-9038.48f, 463.10f, 93.30f}, {-8995.32f, 496.57f, 96.81f},
    {-8992.20f, 498.92f, 96.77f}, {-8971.48f, 515.03f, 96.59f}, {-8965.63f, 525.85f, 96.60f},
};

constexpr WalkPoint kStormwind[] = {
    {-8570.55f, 536.10f, 101.57f}, {-8562.13f, 527.55f, 100.85f}, {-8545.02f, 501.79f, 98.59f}, {-8518.93f, 522.05f, 98.34f},
    {-8491.88f, 555.91f, 97.52f}, {-8471.63f, 569.62f, 96.34f}, {-8446.56f, 585.96f, 94.04f}, {-8429.33f, 569.96f, 93.91f},
    {-8410.32f, 568.22f, 92.03f}, {-8394.13f, 575.46f, 91.27f}, {-8382.30f, 593.91f, 92.80f}, {-8390.55f, 617.00f, 95.07f},
    {-8372.64f, 633.33f, 94.59f}, {-8352.06f, 628.12f, 95.16f}, {-8372.64f, 633.33f, 94.59f}, {-8390.55f, 617.00f, 95.07f},
    {-8382.30f, 593.91f, 92.80f}, {-8394.13f, 575.46f, 91.27f}, {-8410.32f, 568.22f, 92.03f}, {-8429.33f, 569.96f, 93.91f},
    {-8446.56f, 585.96f, 94.04f}, {-8471.63f, 569.62f, 96.34f}, {-8491.88f, 555.91f, 97.52f}, {-8510.11f, 529.34f, 98.12f},
    {-8544.46f, 500.95f, 98.64f}, {-8562.68f, 528.11f, 100.91f}, {-8577.11f, 542.75f, 101.78f}, {-8597.78f, 526.59f, 106.41f},
    {-8611.79f, 514.08f, 103.46f}, {-8634.36f, 537.27f, 99.95f}, {-8657.58f, 553.58f, 96.94f}, {-8678.60f, 549.38f, 97.44f},
    {-8697.52f, 533.31f, 97.69f}, {-8713.68f, 518.89f, 97.12f}, {-8730.44f, 539.56f, 101.06f}, {-8748.36f, 560.30f, 97.40f},
    {-8735.85f, 575.62f, 97.43f}, {-8770.72f, 608.69f, 97.10f}, {-8799.59f, 588.33f, 97.24f}, {-8819.46f, 617.93f, 94.69f},
    {-8840.70f, 642.22f, 95.53f}, {-8851.28f, 660.08f, 97.00f}, {-8824.99f, 677.83f, 97.66f}, {-8845.73f, 720.75f, 97.31f},
    {-8870.14f, 760.39f, 96.71f}, {-8880.87f, 754.51f, 96.35f}, {-8909.56f, 791.19f, 87.43f}, {-8899.24f, 803.05f, 87.67f},
    {-8898.70f, 825.01f, 92.13f}, {-8907.05f, 846.46f, 96.03f}, {-8894.59f, 866.98f, 97.41f}, {-8887.74f, 890.78f, 103.43f},
    {-8894.48f, 911.81f, 110.76f}, {-8854.07f, 933.52f, 102.07f}, {-8894.48f, 911.81f, 110.76f}, {-8887.74f, 890.78f, 103.43f},
    {-8894.59f, 866.98f, 97.41f}, {-8907.05f, 846.46f, 96.03f}, {-8898.70f, 825.01f, 92.13f}, {-8899.16f, 803.14f, 87.71f},
    {-8909.56f, 791.19f, 87.43f}, {-8880.87f, 754.51f, 96.35f}, {-8870.14f, 760.39f, 96.71f}, {-8845.75f, 720.79f, 97.27f},
    {-8824.99f, 677.83f, 97.66f}, {-8851.28f, 660.08f, 97.00f}, {-8840.74f, 642.26f, 95.53f}, {-8819.46f, 617.93f, 94.69f},
    {-8799.59f, 588.33f, 97.24f}, {-8770.72f, 608.69f, 97.10f}, {-8735.85f, 575.62f, 97.43f}, {-8748.36f, 560.30f, 97.40f},
    {-8730.58f, 539.74f, 101.08f}, {-8713.68f, 518.89f, 97.12f}, {-8697.52f, 533.31f, 97.69f}, {-8678.60f, 549.38f, 97.44f},
    {-8657.58f, 553.58f, 96.94f}, {-8634.36f, 537.27f, 99.95f}, {-8611.79f, 514.08f, 103.46f}, {-8597.78f, 526.59f, 106.41f},
    {-8577.11f, 542.75f, 101.78f}, {-8570.55f, 536.10f, 101.57f},
};

constexpr WalkPoint kOrgrimmar[] = {
    {1954.46f, -4722.56f, 42.67f}, {1951.09f, -4714.80f, 36.91f}, {1948.98f, -4697.22f, 29.00f}, {1945.38f, -4681.89f, 25.25f},
    {1937.36f, -4669.20f, 26.53f}, {1909.86f, -4631.36f, 32.98f}, {1907.75f, -4627.89f, 34.12f}, {1894.59f, -4605.89f, 34.12f},
    {1925.42f, -4584.78f, 34.12f}, {1909.95f, -4559.76f, 34.12f}, {1904.40f, -4549.83f, 28.80f}, {1893.00f, -4531.73f, 28.04f},
    {1893.45f, -4526.07f, 28.58f}, {1899.83f, -4517.27f, 27.36f}, {1906.73f, -4505.23f, 24.96f}, {1907.86f, -4490.92f, 23.00f},
    {1900.89f, -4482.65f, 21.14f}, {1893.16f, -4474.82f, 20.68f}, {1891.65f, -4462.73f, 19.58f}, {1894.99f, -4448.40f, 18.14f},
    {1896.97f, -4437.62f, 16.28f}, {1894.04f, -4426.05f, 13.99f}, {1885.50f, -4418.79f, 11.33f}, {1876.37f, -4416.82f, 9.25f},
    {1856.30f, -4416.08f, 6.08f}, {1843.81f, -4407.75f, 4.68f}, {1840.08f, -4400.54f, 4.16f}, {1817.85f, -4365.10f, -9.09f},
    {1799.58f, -4327.29f, -11.16f}, {1782.70f, -4294.67f, 5.59f}, {1776.15f, -4294.00f, 6.73f}, {1766.70f, -4298.89f, 7.57f},
    {1761.65f, -4304.24f, 7.23f}, {1745.15f, -4297.08f, 13.94f}, {1740.24f, -4292.31f, 15.11f}, {1735.93f, -4277.60f, 17.93f},
    {1742.48f, -4263.41f, 23.84f}, {1750.93f, -4255.88f, 28.27f}, {1769.24f, -4248.43f, 36.02f}, {1780.17f, -4240.32f, 39.65f},
    {1784.91f, -4228.93f, 39.86f}, {1777.11f, -4221.35f, 39.67f}, {1754.45f, -4213.47f, 42.49f}, {1742.98f, -4213.58f, 43.94f},
    {1729.00f, -4220.93f, 45.69f}, {1707.14f, -4231.98f, 49.24f}, {1683.98f, -4245.42f, 51.37f}, {1664.47f, -4256.19f, 51.59f},
    {1651.20f, -4264.80f, 50.27f}, {1637.98f, -4271.10f, 48.05f}, {1623.92f, -4262.35f, 47.12f}, {1603.29f, -4237.15f, 45.23f},
    {1589.27f, -4219.05f, 44.13f}, {1581.14f, -4198.70f, 41.40f}, {1579.60f, -4173.00f, 37.74f}, {1585.85f, -4153.19f, 34.65f},
    {1606.41f, -4122.96f, 29.79f}, {1666.80f, -4097.01f, 35.01f}, {1706.45f, -4087.46f, 39.36f}, {1730.26f, -4073.10f, 43.85f},
    {1744.58f, -4045.25f, 44.16f}, {1746.77f, -4022.89f, 45.65f}, {1743.11f, -4000.24f, 46.48f}, {1732.31f, -3972.21f, 48.12f},
    {1743.11f, -4000.24f, 46.48f}, {1746.77f, -4022.89f, 45.65f}, {1744.58f, -4045.25f, 44.16f}, {1730.26f, -4073.10f, 43.85f},
    {1706.45f, -4087.46f, 39.36f}, {1666.80f, -4097.01f, 35.01f}, {1606.41f, -4122.96f, 29.79f}, {1585.85f, -4153.19f, 34.65f},
    {1579.60f, -4173.00f, 37.74f}, {1581.14f, -4198.70f, 41.40f}, {1589.27f, -4219.05f, 44.13f}, {1603.29f, -4237.15f, 45.23f},
    {1623.92f, -4262.35f, 47.12f}, {1637.98f, -4271.10f, 48.05f}, {1651.20f, -4264.80f, 50.27f}, {1664.47f, -4256.19f, 51.59f},
    {1683.98f, -4245.42f, 51.37f}, {1707.14f, -4231.98f, 49.24f}, {1729.00f, -4220.93f, 45.69f}, {1742.98f, -4213.58f, 43.94f},
    {1754.45f, -4213.47f, 42.49f}, {1777.11f, -4221.35f, 39.67f}, {1784.91f, -4228.93f, 39.86f}, {1780.17f, -4240.32f, 39.65f},
    {1769.24f, -4248.43f, 36.02f}, {1750.93f, -4255.88f, 28.27f}, {1742.48f, -4263.41f, 23.84f}, {1735.93f, -4277.60f, 17.93f},
    {1740.24f, -4292.31f, 15.11f}, {1745.15f, -4297.08f, 13.94f}, {1761.65f, -4304.24f, 7.23f}, {1766.70f, -4298.89f, 7.57f},
    {1776.15f, -4294.00f, 6.73f}, {1782.70f, -4294.67f, 5.59f}, {1799.58f, -4327.29f, -11.16f}, {1817.85f, -4365.10f, -9.09f},
    {1840.08f, -4400.54f, 4.16f}, {1843.81f, -4407.75f, 4.68f}, {1856.30f, -4416.08f, 6.08f}, {1876.37f, -4416.82f, 9.25f},
    {1885.50f, -4418.79f, 11.33f}, {1894.04f, -4426.05f, 13.99f}, {1896.97f, -4437.62f, 16.28f}, {1894.99f, -4448.40f, 18.14f},
    {1891.65f, -4462.73f, 19.58f}, {1893.16f, -4474.82f, 20.68f}, {1900.89f, -4482.65f, 21.14f}, {1907.86f, -4490.92f, 23.00f},
    {1906.73f, -4505.23f, 24.96f}, {1899.83f, -4517.27f, 27.36f}, {1893.45f, -4526.07f, 28.58f}, {1893.00f, -4531.73f, 28.04f},
    {1904.40f, -4549.83f, 28.80f}, {1909.95f, -4559.76f, 34.12f}, {1925.42f, -4584.78f, 34.12f}, {1894.59f, -4605.89f, 34.12f},
    {1907.75f, -4627.89f, 34.12f}, {1909.86f, -4631.36f, 32.98f}, {1937.36f, -4669.20f, 26.53f}, {1945.38f, -4681.89f, 25.25f},
    {1948.98f, -4697.22f, 29.00f}, {1951.09f, -4714.80f, 36.91f}, {1954.46f, -4722.56f, 42.67f},
};

constexpr WalkPoint kOrgrimmarGorlach[] = {
    {1580.47f, -4206.02f, 42.06f}, {1587.25f, -4222.89f, 44.21f}, {1605.17f, -4246.51f, 45.03f}, {1622.45f, -4265.15f, 46.64f},
    {1638.78f, -4272.92f, 47.44f}, {1654.09f, -4265.04f, 50.27f}, {1667.76f, -4255.35f, 51.27f}, {1685.95f, -4246.67f, 51.28f},
    {1701.53f, -4235.36f, 49.89f}, {1720.95f, -4227.39f, 46.80f}, {1744.37f, -4214.91f, 43.62f}, {1761.61f, -4216.05f, 41.31f},
    {1777.69f, -4223.29f, 39.67f}, {1801.67f, -4232.64f, 39.57f}, {1823.00f, -4239.40f, 40.77f}, {1852.22f, -4248.08f, 41.86f},
    {1860.91f, -4254.21f, 39.62f}, {1868.91f, -4261.20f, 33.92f}, {1891.98f, -4284.07f, 30.32f}, {1912.98f, -4284.62f, 30.19f},
    {1930.05f, -4295.18f, 25.45f}, {1932.63f, -4321.80f, 20.15f}, {1933.91f, -4358.71f, 20.20f}, {1934.60f, -4367.63f, 21.56f},
    {1935.35f, -4378.52f, 20.36f}, {1934.53f, -4409.56f, 21.38f}, {1938.98f, -4443.94f, 22.97f}, {1921.48f, -4480.03f, 22.83f},
    {1903.96f, -4508.35f, 25.05f}, {1888.47f, -4525.05f, 27.66f}, {1866.30f, -4530.87f, 24.72f}, {1833.02f, -4536.74f, 20.30f},
    {1813.33f, -4534.14f, 20.44f}, {1776.90f, -4527.91f, 24.25f}, {1752.64f, -4516.54f, 26.49f}, {1735.03f, -4497.45f, 28.33f},
    {1729.26f, -4474.82f, 32.93f}, {1736.43f, -4454.20f, 36.60f}, {1742.92f, -4433.71f, 37.48f}, {1738.97f, -4418.43f, 35.83f},
    {1732.58f, -4408.23f, 36.62f}, {1727.63f, -4400.44f, 34.30f}, {1715.30f, -4381.82f, 30.44f}, {1700.33f, -4374.24f, 25.25f},
    {1684.55f, -4387.58f, 23.19f}, {1676.61f, -4398.09f, 19.78f}, {1657.14f, -4417.21f, 16.50f}, {1640.80f, -4421.71f, 15.04f},
    {1637.33f, -4404.73f, 16.39f}, {1654.34f, -4410.04f, 17.07f}, {1673.99f, -4401.42f, 19.13f}, {1682.70f, -4391.55f, 21.85f},
    {1694.91f, -4376.23f, 24.42f}, {1710.68f, -4377.65f, 28.35f}, {1727.63f, -4398.49f, 33.82f}, {1734.11f, -4407.93f, 36.88f},
    {1740.96f, -4422.04f, 36.36f}, {1741.53f, -4441.05f, 37.51f}, {1736.87f, -4453.62f, 36.66f}, {1727.19f, -4475.37f, 33.16f},
    {1736.60f, -4497.38f, 28.33f}, {1751.56f, -4516.51f, 26.51f}, {1775.47f, -4530.77f, 24.40f}, {1803.81f, -4528.13f, 20.79f},
    {1823.55f, -4519.05f, 20.86f}, {1841.02f, -4506.00f, 20.26f}, {1862.83f, -4484.39f, 19.79f}, {1889.91f, -4477.46f, 19.77f},
    {1889.88f, -4462.18f, 19.00f}, {1894.65f, -4447.27f, 17.81f}, {1896.75f, -4431.82f, 14.94f}, {1888.61f, -4420.59f, 11.84f},
    {1877.19f, -4417.81f, 9.09f}, {1851.74f, -4417.55f, 5.21f}, {1840.35f, -4404.61f, 4.11f}, {1837.02f, -4398.15f, 3.75f},
    {1831.03f, -4388.35f, -1.41f}, {1821.20f, -4371.40f, -7.98f}, {1809.19f, -4353.85f, -10.62f}, {1800.18f, -4333.36f, -11.44f},
    {1794.08f, -4320.78f, -10.21f}, {1787.99f, -4308.19f, -2.77f}, {1779.24f, -4294.87f, 6.17f}, {1768.52f, -4299.63f, 7.03f},
    {1760.24f, -4307.54f, 6.66f}, {1755.37f, -4306.41f, 6.30f}, {1747.12f, -4301.11f, 12.80f}, {1741.05f, -4296.64f, 14.78f},
    {1736.53f, -4285.81f, 15.62f}, {1732.74f, -4271.19f, 18.58f}, {1739.02f, -4263.34f, 21.84f}, {1748.89f, -4255.56f, 27.65f},
    {1760.98f, -4251.23f, 32.05f}, {1773.71f, -4242.66f, 37.83f}, {1782.27f, -4230.90f, 39.61f}, {1770.15f, -4219.37f, 40.23f},
    {1757.31f, -4215.54f, 41.98f}, {1740.54f, -4215.65f, 44.00f}, {1729.48f, -4224.89f, 45.60f}, {1715.22f, -4227.83f, 47.75f},
    {1703.46f, -4233.73f, 49.62f}, {1682.48f, -4249.11f, 51.05f}, {1665.30f, -4255.05f, 51.52f}, {1647.57f, -4270.46f, 49.34f},
    {1633.61f, -4272.96f, 47.84f}, {1617.63f, -4258.57f, 46.12f}, {1598.42f, -4234.20f, 44.54f}, {1581.46f, -4213.38f, 43.22f},
};
// Phase 12, instances: from where the entrance teleport puts the player (areatrigger_teleport) into the dungeon.
//   Deadmines: past the mine cart, then the Defias Evokers' rounds (guids 79188, 79374, 79171) into the
//              goblin workshop's cave. The intermediate points were checked with `vd_baker probe-walk`.
//   Zul'Gurub: down the entrance ramp (east of the steep slope; the client's triggers 3958 and 3956 mark the way)
//              and up a Gurubashi Berserker's path (guid 49761).
constexpr WalkPoint kDeadmines[] = {
    {-14.57f, -385.48f, 62.46f},  {-26.00f, -378.00f, 61.08f},  {-44.89f, -379.54f, 55.55f},  {-61.48f, -383.73f, 53.73f},
    {-81.89f, -377.98f, 56.24f},  {-99.19f, -380.02f, 58.72f},  {-111.18f, -390.34f, 56.88f}, {-126.69f, -402.70f, 58.74f},
    {-149.12f, -402.95f, 56.97f}, {-164.56f, -400.44f, 56.77f}, {-182.11f, -415.77f, 54.97f}, {-193.18f, -434.59f, 53.37f},
    {-192.28f, -459.33f, 54.60f}, {-188.83f, -484.76f, 54.05f}, {-195.51f, -504.18f, 53.14f}, {-209.55f, -505.99f, 51.33f},
    {-222.60f, -490.61f, 48.35f}, {-250.93f, -482.92f, 49.45f},
};
constexpr WalkPoint kZulGurub[] = {
    {-11916.60f, -1243.50f, 92.53f}, {-11915.00f, -1312.70f, 77.50f}, {-11916.00f, -1360.00f, 79.85f},
    {-11880.80f, -1394.20f, 63.80f}, {-11917.06f, -1432.33f, 45.52f}, {-11944.51f, -1539.23f, 39.60f},
    {-11960.70f, -1529.38f, 40.45f}, {-11977.90f, -1522.16f, 45.90f}, {-11998.20f, -1519.40f, 57.98f},
    {-12016.70f, -1516.81f, 68.73f}, {-12026.80f, -1511.10f, 74.76f}, {-12025.40f, -1500.97f, 77.25f},
    {-12033.40f, -1483.15f, 80.09f}, {-12043.70f, -1479.09f, 82.84f}, {-12061.40f, -1478.62f, 96.74f},
    {-12081.90f, -1478.66f, 115.19f}, {-12100.70f, -1478.60f, 128.78f},
};

constexpr WalkRoute kRoutes[] = {
    {"walk_goldshire", "Goldshire -> Stormwind (on foot)", "Azeroth", kGoldshire},
    {"walk_stormwind", "Stormwind: the emissaries' round", "Azeroth", kStormwind},
    {"walk_orgrimmar", "Orgrimmar: the emissary's round", "Kalimdor", kOrgrimmar},
    {"walk_gorlach", "Orgrimmar: Gorlach's round", "Kalimdor", kOrgrimmarGorlach},
    {"walk_deadmines", "The Deadmines: entrance to the goblin workshop", "DeadminesInstance", kDeadmines},
    {"walk_zulgurub", "Zul'Gurub: down the entrance ramp", "Zul'gurub", kZulGurub},
};

f32 percentile(std::vector<f32>& values, f32 p) {
    if (values.empty()) {
        return 0.0f;
    }
    const usize index = std::min(values.size() - 1, static_cast<usize>(p * static_cast<f32>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

} // namespace

std::span<const WalkRoute> walkRoutes() {
    return kRoutes;
}

const WalkRoute* findWalkRoute(std::string_view id) {
    for (const WalkRoute& route : kRoutes) {
        if (id == route.id) {
            return &route;
        }
    }
    return nullptr;
}

void WalkTest::start(const WalkRoute& route, bool online) {
    m_route = &route;
    m_online = online;
    m_next = 1;
    m_first = 1;
    m_sinceJump = 0.0f;
    m_jumps = 0;
    m_warmup = kWarmupSeconds;
    m_time = 0.0f;
    m_best = 1e9f;
    m_sinceBest = 0.0f;
    m_detour = 0;
    m_reached = m_stuck = m_falls = 0;
    m_worstFall = 0.0f;
    m_inAir = false;
    m_travelled = 0.0;
    m_errorSum = m_errorMax = 0.0f;
    m_errorCount = m_farPoints = 0;
    m_samples.clear();
    m_samples.reserve(60 * 60 * 10);
    m_report.clear();
    VD_LOG_INFO("Walk %s: %s, %zu points", route.id, route.name, route.points.size());
}

void WalkTest::stop() {
    m_route = nullptr;
}

f32 WalkTest::progress() const {
    return m_route ? static_cast<f32>(m_next) / static_cast<f32>(m_route->points.size()) : 0.0f;
}

void WalkTest::record(f32 frameMs, f32 cpuMs, f32 gpuMs, f32 playerMs) {
    if (m_route && m_warmup <= 0.0f && m_samples.size() < m_samples.capacity()) {
        m_samples.push_back({frameMs, cpuMs, gpuMs, playerMs});
    }
}

bool WalkTest::update(PlayerController& player, f32 dt) {
    if (!m_route) {
        return false;
    }
    player.drive.active = true;
    player.drive.input = physics::CharacterInput{};
    player.drive.input.facing = player.character().facing();
    if (player.waiting() || m_warmup > 0.0f) {
        if (!player.waiting()) {
            m_warmup -= dt;
        }
        m_last = player.character().position();
        return true;
    }
    m_time += dt;
    const physics::CharacterController& c = player.character();
    const glm::dvec3 p = c.position();
    if (m_online && m_time <= dt) {
        // The server's character starts wherever it is: the route is walked from its closest point on.
        f32 closest = 1e30f;
        for (u32 i = 0; i + 1 < m_route->points.size(); ++i) {
            const f32 d = glm::length(glm::vec2(m_route->points[i].x - static_cast<f32>(p.x),
                                                m_route->points[i].y - static_cast<f32>(p.y)));
            if (d < closest) {
                closest = d;
                m_next = i;
            }
        }
        m_first = m_next;
        VD_LOG_INFO("Walk %s: starting at point %u, %.1f yd away", m_route->id, m_next, closest);
    }
    const glm::dvec2 moved(p - m_last);
    m_travelled += glm::length(moved);
    // Running freely: last frame it covered its speed along where it faces (not sliding along a wall).
    const glm::dvec2 facing(std::cos(c.facing()), std::sin(c.facing()));
    const bool free = c.mode() == physics::MotionMode::Ground &&
                      glm::dot(moved, facing) > 0.9 * static_cast<f64>(c.speed()) * static_cast<f64>(dt);
    m_last = p;

    // Falls: from the highest point of the flight to where it lands.
    if (c.mode() == physics::MotionMode::Air) {
        m_airTop = m_inAir ? std::max(m_airTop, static_cast<f32>(p.z)) : static_cast<f32>(p.z);
        m_inAir = true;
    } else if (m_inAir) {
        m_inAir = false;
        const f32 drop = m_airTop - static_cast<f32>(p.z);
        if (drop > kFallReport) {
            ++m_falls;
            m_worstFall = std::max(m_worstFall, drop);
            VD_LOG_WARN("Walk %s: fell %.1f yd, landing at (%.2f, %.2f, %.2f)", m_route->id, drop, p.x, p.y, p.z);
        }
    }

    const WalkPoint* target = &m_route->points[m_next];
    glm::vec2 toTarget(target->x - static_cast<f32>(p.x), target->y - static_cast<f32>(p.y));
    f32 distance = glm::length(toTarget);
    if (distance < kReachDistance) {
        const f32 error = static_cast<f32>(p.z) - target->z;
        const bool standing = c.mode() == physics::MotionMode::Ground; // mid-jump heights say nothing
        if (standing) {
            m_errorSum += std::abs(error);
            m_errorMax = std::max(m_errorMax, std::abs(error));
            ++m_errorCount;
        }
        if (std::abs(error) > 1.0f && standing) {
            ++m_farPoints;
            VD_LOG_WARN("Walk %s: point %u (%.2f, %.2f, %.2f) reached %.2f yd %s the server's height", m_route->id,
                        m_next, target->x, target->y, target->z, std::abs(error), error > 0.0f ? "over" : "under");
        }
        ++m_reached;
        ++m_next;
        m_best = 1e9f;
        m_sinceBest = 0.0f;
        m_detour = 0;
        if (m_next >= m_route->points.size()) {
            finish(player);
            return false;
        }
        // On towards the next point in the same frame: a frame without input would send a STOP and a START_FORWARD
        // at every point.
        target = &m_route->points[m_next];
        toTarget = glm::vec2(target->x - static_cast<f32>(p.x), target->y - static_cast<f32>(p.y));
        distance = glm::length(toTarget);
    }
    if (distance < m_best - 0.3f) {
        m_best = distance;
        m_sinceBest = 0.0f;
        m_detour = 0;
    } else {
        m_sinceBest += dt;
    }
    physics::CharacterInput& input = player.drive.input;
    input.forward = true;
    input.facing = std::atan2(toTarget.y, toTarget.x);
    // Blocked (a patrol cuts corners the server's NPCs walk through): around one side, then the other, then a jump.
    // Only then is it stuck: counted, and carried to the point it could not reach.
    if (m_sinceBest > kStuckSeconds) {
        m_sinceBest = 0.0f;
        ++m_detour;
        if (m_detour > 3) {
            ++m_stuck;
            static constexpr const char* kModes[] = {"ground", "air", "swimming"};
            VD_LOG_WARN("Walk %s: stuck at (%.2f, %.2f, %.2f), %s, on the way to point %u (%.2f, %.2f, %.2f), %.1f yd "
                        "away",
                        m_route->id, p.x, p.y, p.z, kModes[static_cast<u32>(c.mode())], m_next, target->x, target->y,
                        target->z, distance);
            if (m_online) {
                ++m_next; // on towards the next point from where it is
                if (m_next >= m_route->points.size()) {
                    finish(player);
                    return false;
                }
            } else {
                player.character().place(glm::dvec3(target->x, target->y, target->z), input.facing);
                m_last = glm::dvec3(target->x, target->y, target->z);
            }
            m_detour = 0;
            m_best = 1e9f;
        }
    }
    input.strafeRight = m_detour == 1;
    input.strafeLeft = m_detour == 2;
    input.jump = m_detour == 3 && m_sinceBest < 0.1f;
    // Online the character also jumps every few seconds while running, like players do: the server checks each
    // jump and landing (speeds, fall time, one jump per flight). Only running freely: a jump while pressed against a
    // parapet (patrols cut corners) keeps that momentum and carries it over, into a canal or off a bridge.
    if (m_online) {
        m_sinceJump += dt;
        if (m_sinceJump > kOnlineJumpSeconds && free) {
            input.jump = true;
            m_sinceJump = 0.0f;
            ++m_jumps;
        }
    }
    return true;
}

void WalkTest::finish(PlayerController& player) {
    player.drive.active = false;
    std::vector<f32> frames, cpu, gpu, controller;
    for (const Sample& s : m_samples) {
        frames.push_back(s.frameMs);
        cpu.push_back(s.cpuMs);
        gpu.push_back(s.gpuMs);
        controller.push_back(s.playerMs);
    }
    f64 frameSum = 0.0, cpuSum = 0.0, gpuSum = 0.0, playerSum = 0.0;
    u32 slow = 0;
    for (const Sample& s : m_samples) {
        frameSum += s.frameMs;
        cpuSum += s.cpuMs;
        gpuSum += s.gpuMs;
        playerSum += s.playerMs;
        slow += s.frameMs > 20.0f ? 1 : 0;
    }
    const f64 n = static_cast<f64>(std::max<usize>(1, m_samples.size()));
    char text[768];
    std::snprintf(text, sizeof(text),
                  "Walk %s: %u/%zu points reached, stuck %u, falls %u (worst %.1f yd), feet vs server height: mean %.2f "
                  "max %.2f yd (%u over 1 yd), jumps %u | %.0f yd in %.0f s (%.2f yd/s) | %zu frames, %.1f fps, p99 %.2f ms, "
                  "> 20 ms %u | cpu %.2f ms (p99 %.2f), gpu %.2f ms, controller %.3f ms (max %.3f)",
                  m_route->id, m_reached, m_route->points.size() - m_first, m_stuck, m_falls, m_worstFall,
                  m_errorCount ? m_errorSum / m_errorCount : 0.0f, m_errorMax, m_farPoints, m_jumps, m_travelled, m_time,
                  m_time > 0.0f ? m_travelled / m_time : 0.0, m_samples.size(), frameSum > 0.0 ? n * 1000.0 / frameSum : 0.0,
                  percentile(frames, 0.99f), slow, cpuSum / n, percentile(cpu, 0.99f), gpuSum / n, playerSum / n,
                  controller.empty() ? 0.0f : *std::max_element(controller.begin(), controller.end()));
    m_report = text;
    VD_LOG_INFO("%s", m_report.c_str());
    m_route = nullptr;
}

} // namespace vd
