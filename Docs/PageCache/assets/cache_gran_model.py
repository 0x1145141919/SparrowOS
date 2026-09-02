#!/usr/bin/env python3
# cache_gran_model.py — SparrowOS 页缓存/FS单元粒度 g 的代价模型
# 目标: L(g) = Σ wᵢ·costᵢ(g)  → 网格搜索 argmin, 输出灵敏度
# 单位: 所有 cost 折算成 "每 4K 有效需求流量" 的 µs
#
# 用法: python3 cache_gran_model.py [--scenario all|sync|bulk|mixed]
# 所有参数可改, 用 SparrowOS 自己的实测常数替换即可.

# ============ 常量区 (标定来源标注清楚) ============
# --- 本机实测 (WD SN5000 /dev/nvme1n1, O_DIRECT 阻塞读) ---
TAU_C   = 60.0    # 单命令固定往返 µs (T(s) 拟合截距 60.3; 含提交+完成)
TAU_1   = 0.198   # 数据边际 µs/KiB (实测斜率)
QD      = 1       # 队列深度: 吞吐侧固定成本按 τ_c/QD 摊销; SparrowOS 若流水线>1 改这里
# --- 写侧 (未实测, 假设与读同量级; 以后用驱动实测替换) ---
TAU_CW  = 60.0    # 写命令固定 µs
NAND_PAGE = 16    # NAND 页 KiB (TLC 典型 16K; 实际取不到, 不确定±, 见灵敏度)
WA_RAND = 2.5     # 随机写 FTL 写放大 (< NAND页 的对齐写); 大块对齐写 ~1.15
# --- 内存/CPU ---
MEM_BW  = 10.0    # GiB/s (归零带宽估计)
ZERO_EN = True    # 新单元是否要归零 (安全/内容校验)
CKSUM_EN= False   # 整单元校验和 (ZFS式); 开了每有用4K加 CKSUM_PER_KIB*g 的CPU
CKSUM_PER_KIB = 0.05  # µs/KiB (crc32c 量级)
# --- 设计开关 ---
DIRTY_BITMAP = True  # 单元内 4K 脏位图 → 回写粒度 4K, OS 层写放大≈0
SG_ALLOC     = True  # order-0 分散页 + PRP(该盘 sgls=0, PRP 本就每4K一条, 连续无收益)
                      # → 伙伴碎片代价≈0; 若改 True 要求物理连续, 需另加碎片项
# --- 工作负载 (字节混合, 归一) ---
M_CACHE = 0.5     # 缓存容量 GiB
W_SET   = 100.0   # 工作集 GiB
SKEW    = 0.35    # 访问偏斜指数 s∈[0,1]: 0=均匀(大单元无容量惩罚), 1=极偏斜
BMAX    = 16      # 回写合并上限 (单元内最多累积多少脏4K才回写; fsync多就调小)
# 场景: (seq_read, rand_read, rand_write) 需求字节比例, 回写合并上限 b_max
# b_max: 写回前单元内最多累积几个脏4K。fsync/同步压力大 → 1; 后台写回 → 16+
SCEN = {
 'mixed': ((0.40, 0.30, 0.30), 16),
 'bulk' : ((0.80, 0.10, 0.10), 16),
 'rand' : ((0.05, 0.45, 0.50), 8),
 'sync' : ((0.10, 0.30, 0.60), 1),   # 每更新即落盘: 无合并
}

def waf_nand(g_kib):
    """g 对齐写时的 FTL 写放大: g<NAND页 部分页写; ≥页 整页对齐"""
    return WA_RAND if g_kib < NAND_PAGE else 1.15

def main(scenario):
    gs = [4, 8, 16, 32, 64, 128, 256, 1024]   # KiB 候选
    r = 4.0                                    # 需求逻辑粒度 KiB
    ws, BMAX = SCEN[scenario]
    tau_c = TAU_C / QD
    rows = []
    for g in gs:
        # A 顺序/大块读: 每需求4K摊销的命令固定 + 数据边际 (数据边际与g无关, 恒定小)
        A = tau_c * (r / g) + TAU_1 * r
        # B 随机读(4K需求): 每次=自己一条命令, 与 g 无关(除非整单元抓取/预读错)
        #   × 容量效率惩罚 cm(g): 偏斜下大单元=把热点邻居一起占住 → 有效容量降
        e = (r / g) ** SKEW          # 容量效率: g=r 时=1
        lam0 = 1 - M_CACHE / W_SET   # 基准 miss 率(字节级)
        lamg = 1 - M_CACHE * e / W_SET
        cm = max(1.0, lamg / lam0)
        B = (tau_c + TAU_1 * r) * cm
        # C 随机小更新: 假设 CoW/整节点重写 (B+树节点=缓存单元) →
        #    每更新摊: 读节点(可能命中, 算τ_c) + 写回节点 g (对齐写 WAF) + 归零
        #    合并因子 b: 写回前单元内累积脏4K数 ≈ min(g/r, BMAX)
        b = min(g / r, BMAX)
        node_rw = (TAU_C + TAU_CW) / 2 + TAU_1 * g        # 节点读+写命令固定均摊 + 数据
        wb = waf_nand(g) * (TAU_CW + TAU_1 * g) + (ZERO_EN * g / (MEM_BW*1024) * 1000)
        C = (node_rw / b) + (wb / b)
        # D CPU 附加 (可选校验)
        D = (CKSUM_PER_KIB * g * (r/g)) if CKSUM_EN else 0.0
        total = ws[0]*A + ws[1]*B + ws[2]*C + D
        rows.append((g, A, B, C, total))
    best = min(rows, key=lambda x: x[4])
    print(f"\n===== 场景 {scenario}: 权重{ws} b_max={BMAX} =====")
    print(f"{'g(KiB)':>7} {'A顺序读':>9} {'B随机读':>9} {'C随机更新':>10} {'L(g)合计':>9}")
    for g, A, B, C, t in rows:
        mark = '  <-- g*' if g == best[0] else ''
        print(f"{g:>7} {A:>9.1f} {B:>9.1f} {C:>10.1f} {t:>9.1f}{mark}")
    # 灵敏度: g* 邻域是否平坦
    r0 = [x for x in rows if x[0] == best[0]][0]
    flat = [x for x in rows if abs(x[4]-best[4]) <= 0.05*best[4]+0.01]
    print(f"g* = {best[0]} KiB, L = {best[4]:.1f} µs/4K | 平坦带(±5%): {[x[0] for x in flat]} KiB")

if __name__ == '__main__':
    import sys
    scen = sys.argv[1] if len(sys.argv) > 1 else 'mixed'
    for s in ([scen] if scen != 'all' else list(SCEN)):
        main(s)
