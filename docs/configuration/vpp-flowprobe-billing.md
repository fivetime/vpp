# VPP Flowprobe/IPFIX 计量采集方案及部署指南

## 1. 背景和目标

本文档描述一种基于 VPP 原生 `flowprobe` plugin 和 IPFIX exporter 的轻量级流量计量方案，用于按客户 IP 维度向 BSS 提供计费数据。

目标需求：

- 不改变 VPP 现有转发逻辑、路由、NAT、ACL 或业务链路。
- 采集经过计费边界接口的所有 IPv4/IPv6 流量。
- egress/tx 方向按源 IP 计量。
- ingress/rx 方向按目标 IP 计量。
- 导出 IPFIX/NetFlow 风格记录到 pmacct，再进入 ClickHouse，最后供 BSS 聚合计费。
- 优先使用 VPP 原生能力，避免额外 datapath 进程和旁路抓包。

推荐链路：

```text
VPP flowprobe
  -> VPP IPFIX exporter
  -> pmacct/nfacctd
  -> ClickHouse raw flow table
  -> ClickHouse aggregate table
  -> BSS
```

## 2. 环境和版本要求

VPP 侧要求：

- VPP 镜像或安装包包含 `flowprobe_plugin.so`。
- VPP 包含 `ipfix-export` 支持。
- 推荐使用当前 CI 构建镜像：

```text
ghcr.io/fivetime/vpp:master
```

本机 lab 已验证版本：

```text
vpp v1.0.0-16050~gc1e21a65c
built at 2026-06-24T05:52:20
image: ghcr.io/fivetime/vpp:master
```

外部系统要求：

- pmacct/nfacctd 可监听 IPFIX UDP，默认端口 `4739`。
- ClickHouse 用于保存 raw flow 和聚合计费结果。
- BSS 使用 ClickHouse 聚合后的账单数据，不建议直接消费原始 flow。

限制和注意事项：

- VPP flowprobe 当前导出的 `octetDeltaCount` 是 L3/IP packet length，不是二层 wire bytes。
- IPFIX export 当前使用 UDP/IPv4，`flowprobe` FEATURE 里标注 IPv6 export、TCP/SCTP export 未实现。
- 该方案是逐包 flow accounting，不是采样；但仍然会在启用接口方向上增加 feature node 和 flow hash 更新开销。

## 3. 计费口径

推荐计费口径：

```text
flowDirection = tx/1 -> bill_ip = sourceIPv4Address 或 sourceIPv6Address
flowDirection = rx/0 -> bill_ip = destinationIPv4Address 或 destinationIPv6Address
```

VPP flowprobe 导出字段包括：

```text
ingressInterface
egressInterface
flowDirection
packetDeltaCount
octetDeltaCount
flowStartNanoseconds
flowEndNanoseconds
sourceIPv4Address / destinationIPv4Address
sourceIPv6Address / destinationIPv6Address
protocolIdentifier
```

如果只做按 IP 计费，建议只启用 `record l3`。不要一开始启用 `record l4`，否则端口会进入 flow key，flow 数量和导出量会明显增加。

## 4. L3 字节与二层 wire 字节

`flowprobe record l3` 的 `octetDeltaCount` 来自 IP header 中的长度：

- IPv4：`ip4->length`
- IPv6：`payload_length + sizeof(ip6_header_t)`

因此它是 L3/IP bytes，不包含：

```text
Ethernet header
VLAN/QinQ header
FCS
Preamble/SFD
IFG
```

如果 BSS 需要近似二层 wire bytes，可在 ClickHouse 聚合层修正：

无 VLAN：

```sql
sum(octetDeltaCount + packetDeltaCount * 38) AS billing_bytes
```

一层 VLAN：

```sql
sum(octetDeltaCount + packetDeltaCount * 42) AS billing_bytes
```

QinQ：

```sql
sum(octetDeltaCount + packetDeltaCount * 46) AS billing_bytes
```

如果合同口径是按 IP 层流量计费，建议直接使用 L3 bytes，并在 BSS 计费说明中明确：

```text
计费流量按 IP 层字节统计，不包含以太网 FCS、preamble、IFG 等二层链路开销。
```

## 5. VPP 生产部署配置

### 5.1 确认插件存在

容器内检查：

```bash
ls /usr/lib/*/vpp_plugins/flowprobe_plugin.so
vppctl show plugins | grep flowprobe
```

### 5.2 配置 IPFIX exporter

示例：

```bash
vppctl set ipfix exporter \
  collector <pmacct_ip> \
  src <vpp_export_src_ip> \
  template-interval 60 \
  port 4739 \
  path-mtu 1450
```

参数建议：

- `collector`：pmacct/nfacctd 监听地址。
- `src`：VPP 发出 IPFIX 包的源地址，需要 collector 可达。
- `template-interval`：建议 60 秒；测试环境可缩短到 5 秒。
- `path-mtu`：建议 1450，避免跨网络路径分片。

### 5.3 配置 flowprobe 参数

轻量计费推荐：

```bash
vppctl flowprobe params record l3 active 60 passive 300
```

说明：

- `record l3`：只记录 IP 维度，适合按源/目标 IP 计费。
- `active 60`：每个活跃 flow 每 60 秒导出一次 delta。
- `passive 300`：空闲 300 秒后清理 flow。

更低实时性、更低开销：

```bash
vppctl flowprobe params record l3 active 300 passive 900
```

测试环境可以使用：

```bash
vppctl flowprobe params record l3 active 2 passive 5
```

### 5.4 在计费边界接口启用

只在计费边界接口启用，不要在所有内部接口都启用，避免双计和额外开销。

IPv4：

```bash
vppctl flowprobe feature add-del <billing-interface> ip4 both
```

IPv6：

```bash
vppctl flowprobe feature add-del <billing-interface> ip6 both
```

如果方向明确，也可以只启用单方向：

```bash
vppctl flowprobe feature add-del <billing-interface> ip4 rx
vppctl flowprobe feature add-del <billing-interface> ip4 tx
```

### 5.5 查看状态

```bash
vppctl show flowprobe params
vppctl show flowprobe feature
vppctl show runtime | grep flowprobe
vppctl show error | grep flowprobe
vppctl show interface
```

## 6. pmacct/nfacctd 接入建议

pmacct 侧监听 IPFIX UDP/4739，并把 raw flow 写入 ClickHouse 或中间队列。

配置方向：

```text
daemonize: true
plugins: print[flows]
nfacctd_port: 4739
nfacctd_ip: <pmacct_ip>
aggregate: src_host,dst_host,proto,flow_direction,in_iface,out_iface
```

实际字段名以你现有 pmacct 配置为准。关键是保留：

```text
source IP
destination IP
flow direction
packet count
octet count
start/end time
ingress/egress interface
```

## 7. ClickHouse 聚合建议

Raw flow 表保留原始 IPFIX 字段，聚合表生成 BSS 使用的计费口径。

IPv4 L3 bytes 聚合示例：

```sql
SELECT
  toStartOfMinute(flow_end_time) AS ts,
  multiIf(
    flowDirection = 1, sourceIPv4Address,
    flowDirection = 0, destinationIPv4Address,
    '0.0.0.0'
  ) AS bill_ip,
  flowDirection AS direction,
  sum(octetDeltaCount) AS l3_bytes,
  sum(packetDeltaCount) AS packets
FROM raw_ipfix
WHERE sourceIPv4Address != '0.0.0.0'
  AND destinationIPv4Address != '0.0.0.0'
GROUP BY
  ts,
  bill_ip,
  direction;
```

估算 wire bytes：

```sql
SELECT
  toStartOfMinute(flow_end_time) AS ts,
  multiIf(
    flowDirection = 1, sourceIPv4Address,
    flowDirection = 0, destinationIPv4Address,
    '0.0.0.0'
  ) AS bill_ip,
  flowDirection AS direction,
  sum(octetDeltaCount) AS l3_bytes,
  sum(packetDeltaCount) AS packets,
  sum(octetDeltaCount + packetDeltaCount * 38) AS estimated_wire_bytes
FROM raw_ipfix
GROUP BY
  ts,
  bill_ip,
  direction;
```

## 8. 性能和风险控制

推荐原则：

- 只在计费边界接口启用。
- 只启用 `record l3`。
- active timer 不要太短，生产建议从 60 秒或 300 秒开始。
- 不要在同一条业务流经过的多个接口重复启用，避免双计。
- pmacct 和 ClickHouse 应有足够 ingest 能力，避免 collector 丢包。

重点观察：

```bash
vppctl show runtime | grep flowprobe
vppctl show interface
vppctl show error | grep flowprobe
```

如果 `flowprobe-input-ip4` 或 `flowprobe-output-ip4` 在 `show runtime` 中占比过高：

1. 拉长 active timer。
2. 确认没有启用 `record l4`。
3. 减少启用接口范围。
4. 确认没有重复计费路径。

## 9. 风险场景和处置

本方案按周期导出 delta counter。对于少量源/目标 IP 的大包大带宽攻击，ClickHouse 看到的主要是 `octetDeltaCount` 数值变大，不会因为字节数变大而被直接打死。

实际 DDoS 更常见的是多源肉鸡攻击。此时 VPP 侧风险是 `src_ip/dst_ip/proto/direction/interface` 组合增加，导致 flow cardinality 上升；ClickHouse 侧是否受影响，取决于是否把 raw flow 全量写入。如果 collector 已按 `bill_ip + direction + minute` 预聚合，ClickHouse 压力主要由活跃计费 IP 数和时间粒度决定，而不是包数或带宽本身。

计量链路仍必须按单向、异步、可丢弃、有限缓冲设计。VPP 被攻击打满时，业务面已经不可用，ClickHouse 不应该被同步写入、无限缓存或反压链路一起拖死。

推荐保护链路：

```text
VPP flowprobe/IPFIX UDP
  -> collector/pmacct
  -> queue/spool/分钟级预聚合
  -> ClickHouse batch insert
  -> BSS
```

关键原则：

- VPP IPFIX 使用 UDP，collector 慢或 ClickHouse 慢时不能反压 VPP datapath。
- collector、队列、本地 spool 必须有容量上限，超限优先丢 raw flow。
- ClickHouse 只接受批量写入，不做逐条 insert。
- BSS 读取分钟级或五分钟级聚合表，不直接依赖 raw flow 表。
- raw flow 表只用于短 TTL 审计和问题回放，不作为 BSS 主账单源。
- 攻击期间可以降级为只保留 `billing_1m` 或 `billing_5m` 聚合结果。
- 普通 volumetric DDoS 只会让已有 flow 的 bytes 增大；多源肉鸡、随机源/目标 IP 扫射、L4 误开启、raw flow 全量入库时，才会显著增加后端写入行数。

| 风险场景 | 潜在后果 | 监控指标 / 命令行排查 | 预防与纠正措施 |
| --- | --- | --- | --- |
| 四层记录被误开启 | 并发哈希流表瞬间膨胀，VPP 内存急剧消耗，IPFIX export 记录数暴涨 | `vppctl show flowprobe params`，检查输出是否包含 `l4`；`vppctl show runtime | grep flowprobe`，观察 flowprobe node 占比；观察 VPP RSS 内存 | 立即执行 `vppctl flowprobe params record l3 active 60 passive 300` 切回纯三层统计；确认自动化配置中没有 `record l4`；恢复前检查 collector/ClickHouse 是否积压 |
| 少量 IP 的大流量 DDoS | VPP 和链路带宽承压；ClickHouse 侧主要表现为同一批 flow 的 bytes 数值变大，写入行数不会按带宽线性增长 | VPP interface counter、`vppctl show runtime`、链路利用率；ClickHouse 侧观察 bytes 聚合值是否突增 | 保持 `record l3` 和周期聚合；BSS 正常按 bytes 计费；攻击处置重点在上游清洗、ACL、RTBH 或限速，不需要因为 bytes 数值变大而特殊保护 ClickHouse |
| 多源肉鸡 / 随机源目标扫射攻击 | L3 flow key 数量增加，flow 表和导出队列承压；极端情况下新 flow 统计遗漏；如果 raw flow 全量入库，后端写入行数上涨 | `vppctl show error | grep flowprobe`；`vppctl show runtime | grep flowprobe`；collector UDP drop、网卡 drop、pmacct 队列积压；ClickHouse `system.metrics`、`system.asynchronous_metrics`、`system.parts` | 若当前版本/分支支持 `max-entries`，应配置 flow 表上限；当前本仓库 flowprobe CLI 未查到 `max-entries` 参数时，使用 `record l3`、缩短 `passive`、拉长 `active`、减少启用接口范围；collector 侧按 `bill_ip + direction + minute` 预聚合；在上游实施 BFD/ACL/RTBH 黑洞路由联动拦截 |
| 边界拓扑双计 | 同一物理报文被重复统计，客户账单翻倍 | `vppctl show flowprobe feature`，检查多个接口是否对同一业务路径重复开启；抽样比对 VPP interface counter 与 ClickHouse 聚合结果 | 规范网络拓扑，只在面向客户侧的物理/虚拟边界子接口应用 `both`；不要在同一报文经过的内外侧接口同时启用；上线前用小流量做端到端校验 |
| 时区跨天错位 | 聚合账单与客户体感天数不一致，引发客诉 | 校验 ClickHouse 聚合后的 `ts` 与原始 UTC 时间的天数边界；抽查跨 00:00 的账单窗口 | 在 SQL 中使用 `toStartOfMinute(time, 'Timezone')` 或同类函数强制绑定计费时区；BSS、ClickHouse、报表统一使用同一账期时区 |
| IPFIX 报文分片丢包 | Collector 无法重新组包，大批统计数据丢失 | collector 侧 UDP 丢包计数；Linux `ip -s link` / `ethtool -S` 网卡 drop；pmacct/nfacctd 日志；抓包确认是否有 IP fragmentation | 在 VPP exporter 中设置 `path-mtu 1450`，overlay 或多层封装场景可降到 `1400`；collector 和 VPP export 源之间避免经过低 MTU 隧道；必要时把 collector 放到 VPP 近端 |
| collector 或队列无限缓存 | 攻击期间 raw flow 积压把内存或磁盘打满，进一步影响 ClickHouse 或宿主机 | collector 进程 RSS、队列长度、本地 spool 目录大小、磁盘 IO wait、磁盘剩余空间 | 设置内存、队列、spool 上限；满了丢 raw flow，不阻塞接收线程；collector 独立机器或独立 cgroup，避免与 VPP 抢资源 |
| ClickHouse 被 raw flow 写入打满 | merge backlog、parts 暴涨、查询变慢，BSS 账单延迟 | `system.parts` 活跃 part 数、`system.merges`、insert 延迟、磁盘 IO、CPU、ZooKeeper/Keeper 延迟；观察 raw 表写入 QPS 和 bytes/s | raw flow 短 TTL；优先写 `billing_1m` 聚合表；batch insert；攻击时允许 raw 降采样或丢弃，但保留按 IP/方向/分钟聚合结果 |

### 9.1 ClickHouse 降级策略

正常情况下可以同时写 raw flow 和聚合表，但 BSS 应只读取聚合表：

```text
raw_ipfix: 短 TTL，只用于审计和问题回放
billing_1m / billing_5m: 长 TTL，供 BSS 计费
```

攻击或 collector 积压时按以下顺序降级：

1. 停止或降低 raw flow 写入。
2. 保留 `bill_ip + direction + minute` 聚合写入。
3. 增大 ClickHouse batch size，降低 insert 频率。
4. 如果 collector 队列仍持续增长，丢弃 raw flow，避免影响 VPP 和 ClickHouse。

聚合后的 ClickHouse 压力主要取决于：

```text
活跃计费 IP 数量 * 时间粒度 * 方向数
```

而不是：

```text
flow 数量 * packet churn * export records
```

## 10. 本机 Docker Lab 复现

本节记录已在本机执行过的真实 lab，用于验证：

- flowprobe 不是采样。
- `packetDeltaCount` 和 `octetDeltaCount` 与真实 L3 packet 数和字节数一致。
- `rx` 方向可按目标 IP 计费。
- `tx` 方向可按源 IP 计费。

### 10.1 Lab 拓扑

```text
Linux netns vpp-fp-h1
  10.10.1.2/24
  default via 10.10.1.1
        |
        | veth
        |
VPP container ghcr.io/fivetime/vpp:master
  host-fp-vpp1 10.10.1.1/24
  host-fp-vpp2 10.10.2.1/24
  host-fp-vppc 172.31.255.2/30
        |
        | veth
        |
Linux netns vpp-fp-h2
  10.10.2.2/24
  default via 10.10.2.1

Host IPFIX collector
  fp-col 172.31.255.1/30
  UDP/4739
```

### 10.2 VPP startup 配置

文件：`/tmp/vpp_flowprobe_startup.conf`

```conf
unix {
  nodaemon
  cli-listen /run/vpp/cli.sock
  log /tmp/vpp.log
}

api-segment {
  gid vpp
}

statseg {
  default
}

plugins {
  plugin default { enable }
}
```

### 10.3 启动容器和拓扑

```bash
set -euo pipefail

for ns in vpp-fp-h1 vpp-fp-h2; do
  ip netns del "$ns" 2>/dev/null || true
done
docker rm -f vpp-flowprobe-lab >/dev/null 2>&1 || true
for link in fp-h1 fp-h2 fp-col; do
  ip link del "$link" 2>/dev/null || true
done

docker run -d --name vpp-flowprobe-lab --privileged --network none \
  -v /tmp/vpp_flowprobe_startup.conf:/etc/vpp/startup.conf:ro \
  ghcr.io/fivetime/vpp:master

for i in {1..30}; do
  if docker exec vpp-flowprobe-lab vppctl show version >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

ip netns add vpp-fp-h1
ip netns add vpp-fp-h2
pid=$(docker inspect -f '{{.State.Pid}}' vpp-flowprobe-lab)

ip link add fp-h1 type veth peer name fp-vpp1
ip link set fp-h1 netns vpp-fp-h1
ip link set fp-vpp1 netns "$pid"

ip link add fp-h2 type veth peer name fp-vpp2
ip link set fp-h2 netns vpp-fp-h2
ip link set fp-vpp2 netns "$pid"

ip link add fp-col type veth peer name fp-vppc
ip link set fp-vppc netns "$pid"

ip addr add 172.31.255.1/30 dev fp-col
ip link set fp-col up

ip -n vpp-fp-h1 addr add 10.10.1.2/24 dev fp-h1
ip -n vpp-fp-h1 link set lo up
ip -n vpp-fp-h1 link set fp-h1 up
ip -n vpp-fp-h1 route add default via 10.10.1.1

ip -n vpp-fp-h2 addr add 10.10.2.2/24 dev fp-h2
ip -n vpp-fp-h2 link set lo up
ip -n vpp-fp-h2 link set fp-h2 up
ip -n vpp-fp-h2 route add default via 10.10.2.1

nsenter -t "$pid" -n ip link set lo up
nsenter -t "$pid" -n ip link set fp-vpp1 up
nsenter -t "$pid" -n ip link set fp-vpp2 up
nsenter -t "$pid" -n ip link set fp-vppc up
```

### 10.4 配置 VPP 接口、IPFIX 和 flowprobe

```bash
vppctl='docker exec vpp-flowprobe-lab vppctl'

$vppctl create host-interface name fp-vpp1
$vppctl create host-interface name fp-vpp2
$vppctl create host-interface name fp-vppc

$vppctl set interface state host-fp-vpp1 up
$vppctl set interface state host-fp-vpp2 up
$vppctl set interface state host-fp-vppc up

$vppctl set interface ip address host-fp-vpp1 10.10.1.1/24
$vppctl set interface ip address host-fp-vpp2 10.10.2.1/24
$vppctl set interface ip address host-fp-vppc 172.31.255.2/30

$vppctl set ipfix exporter \
  collector 172.31.255.1 \
  src 172.31.255.2 \
  template-interval 5 \
  port 4739 \
  path-mtu 1450

$vppctl flowprobe params record l3 active 2 passive 5
$vppctl flowprobe feature add-del host-fp-vpp1 ip4 both

$vppctl show interface addr
$vppctl show flowprobe params
$vppctl show flowprobe feature
```

预期：

```text
host-fp-vpp1 ip4 rx tx
l3 active: 2 passive: 5
```

### 10.5 验证连通性

```bash
ip netns exec vpp-fp-h1 ping -c 3 -W 1 10.10.2.2
ip netns exec vpp-fp-h2 ping -c 3 -W 1 10.10.1.2
```

### 10.6 IPFIX collector

临时 collector：`/tmp/vpp_flowprobe_collector.py`

```python
#!/usr/bin/env python3
import ipaddress
import json
import socket
import struct
import sys
import time

FIELDS = {
    1: ("octets", 8),
    2: ("packets", 8),
    4: ("proto", 1),
    8: ("src4", 4),
    10: ("ingress_if", 4),
    12: ("dst4", 4),
    14: ("egress_if", 4),
    61: ("direction", 1),
    156: ("start_ns", 8),
    157: ("end_ns", 8),
}

def read_int(data):
    if len(data) == 1:
        return data[0]
    if len(data) == 2:
        return struct.unpack("!H", data)[0]
    if len(data) == 4:
        return struct.unpack("!I", data)[0]
    if len(data) == 8:
        return struct.unpack("!Q", data)[0]
    return data.hex()

def decode_value(field_id, data):
    name, _ = FIELDS.get(field_id, (f"ie_{field_id}", len(data)))
    if field_id in (8, 12):
        return name, str(ipaddress.IPv4Address(data))
    return name, read_int(data)

def parse_packet(pkt, templates, records):
    if len(pkt) < 16:
        return
    version, length, export_time, seq, domain = struct.unpack("!HHIII", pkt[:16])
    if version != 10:
        return
    offset = 16
    end = min(length, len(pkt))
    while offset + 4 <= end:
        set_id, set_len = struct.unpack("!HH", pkt[offset:offset + 4])
        set_end = offset + set_len
        body = offset + 4
        if set_len < 4 or set_end > end:
            return
        if set_id == 2:
            pos = body
            while pos + 4 <= set_end:
                tid, count = struct.unpack("!HH", pkt[pos:pos + 4])
                pos += 4
                fields = []
                for _ in range(count):
                    if pos + 4 > set_end:
                        break
                    eid_len = struct.unpack("!H", pkt[pos:pos + 2])[0]
                    flen = struct.unpack("!H", pkt[pos + 2:pos + 4])[0]
                    pos += 4
                    enterprise = bool(eid_len & 0x8000)
                    eid = eid_len & 0x7fff
                    if enterprise:
                        pos += 4
                    fields.append((eid, flen))
                if fields:
                    templates[tid] = fields
        elif set_id >= 256 and set_id in templates:
            fields = templates[set_id]
            rec_len = sum(flen for _, flen in fields)
            pos = body
            while rec_len and pos + rec_len <= set_end:
                rec = {"template_id": set_id, "domain": domain}
                roff = pos
                for eid, flen in fields:
                    name, value = decode_value(eid, pkt[roff:roff + flen])
                    rec[name] = value
                    roff += flen
                records.append(rec)
                pos += rec_len
        offset = set_end

def main():
    bind = sys.argv[1] if len(sys.argv) > 1 else "172.31.255.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 4739
    duration = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind, port))
    sock.settimeout(1)
    templates = {}
    records = []
    deadline = time.time() + duration
    packets = 0
    while time.time() < deadline:
        try:
            data, _ = sock.recvfrom(65535)
        except socket.timeout:
            continue
        packets += 1
        parse_packet(data, templates, records)
    print(json.dumps({
        "udp_packets": packets,
        "templates": {str(k): v for k, v in templates.items()},
        "records": records,
    }, indent=2, sort_keys=True))

if __name__ == "__main__":
    main()
```

启动 collector：

```bash
python3 /tmp/vpp_flowprobe_collector.py 172.31.255.1 4739 35 > /tmp/vpp_flowprobe_out.json
```

### 10.7 打真实测试流量

在另一个 shell 执行：

```bash
ip netns exec vpp-fp-h1 ping -i 0.2 -c 30 -s 1000 10.10.2.2
sleep 3
ip netns exec vpp-fp-h1 ping -c 1 -s 1000 10.10.2.2
```

该流量共 31 个 ICMP echo request，31 个 echo reply。

每个 IP packet 长度：

```text
IPv4 header 20 + ICMP header 8 + payload 1000 = 1028 bytes
```

每个方向预期：

```text
31 packets
31 * 1028 = 31868 bytes
```

### 10.8 汇总解析结果

```bash
python3 - <<'PY'
import json, collections
d = json.load(open('/tmp/vpp_flowprobe_out.json'))
print('udp_packets', d['udp_packets'])
print('records', len(d['records']))
agg = collections.defaultdict(lambda: [0, 0, 0])
for r in d['records']:
    key = (
        r.get('direction'),
        r.get('src4'),
        r.get('dst4'),
        r.get('proto'),
        r.get('ingress_if'),
        r.get('egress_if'),
    )
    agg[key][0] += 1
    agg[key][1] += int(r.get('packets', 0))
    agg[key][2] += int(r.get('octets', 0))
for key, val in sorted(agg.items()):
    print(key, 'records=%d packets=%d octets=%d' % tuple(val))
print('expected per direction packets=31 octets=31868')
PY
```

实际验证结果：

```text
direction=0 src=10.10.1.2 dst=10.10.2.2 packets=31 octets=31868
direction=1 src=10.10.2.2 dst=10.10.1.2 packets=31 octets=31868
expected per direction packets=31 octets=31868
```

结论：

- 该方案是真 packet/octet delta count，不是采样。
- 在 lab 中 L3 packet/byte count 与预期完全一致。
- `direction=0` 表示 ingress/rx flow。
- `direction=1` 表示 egress/tx flow。
- 后端按 `rx -> destination IP`、`tx -> source IP` 聚合可满足 BSS per-IP 计费。

### 10.9 清理 lab

```bash
docker rm -f vpp-flowprobe-lab >/dev/null 2>&1 || true
for ns in vpp-fp-h1 vpp-fp-h2; do
  ip netns del "$ns" 2>/dev/null || true
done
for link in fp-col fp-h1 fp-h2 fp-vpp1 fp-vpp2 fp-vppc; do
  ip link del "$link" 2>/dev/null || true
done
```

## 11. 上线检查清单

- [ ] 确认计费边界接口，不在双向路径多个点重复启用。
- [ ] 确认 BSS 使用 L3 bytes 还是估算 wire bytes。
- [ ] 确认 pmacct 没有丢 IPFIX 包。
- [ ] 确认 ClickHouse raw table 只做短 TTL 审计回放，BSS 不直接读取 raw flow。
- [ ] 确认聚合规则：`tx -> src IP`，`rx -> dst IP`。
- [ ] 确认 flowprobe 参数没有启用 `record l4`。
- [ ] 确认 collector/队列/spool 有容量上限，超限不反压 VPP。
- [ ] 确认 ClickHouse 使用 batch insert，并有 `billing_1m` 聚合表。
- [ ] 确认 IPFIX exporter 使用合适的 `path-mtu`，overlay 场景建议 `1400` 或 `1450`。
- [ ] 先用小流量灰度，校验 VPP interface counter、IPFIX records、ClickHouse 聚合数。
- [ ] 观察 `show runtime | grep flowprobe`，确认 CPU 开销可接受。
- [ ] 生产 active timer 建议从 60 秒或 300 秒开始。
