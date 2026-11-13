/* path.h —— 纯函数路径层（无状态，可宿主单测；与 gfx.h 同一定位）
 *
 * 为什么存在（第三十六轮，修安全审计 H1）：
 *   desktop.c 的 fm_full() 曾用无边界 my_strcpy 拼接「当前目录 + '/' + 目录项名」，
 *   且目标缓冲有时就是 fm_path[96] 自身、有时是要拷进 G_launch_path[96] 的 full[160]。
 *   fs.h 的 dirent.name 最长 61 字节，逐层进入深目录时路径可达 158 字节，
 *   确定性写穿 96 字节全局，破坏 G_win / G_apps 等相邻状态（内核单地址空间无保护）。
 *
 * 统一约定：
 *   - PATH_CAP = 160：所有路径缓冲（fm_path / G_launch_path / ed_path / 局部 full、tmp）
 *     一律用这个容量，杜绝「源 160、目标 96」的错配。
 *   - 所有拼接/取父路径都必须走这里的带 cap 版本，失败返回 -1 且不写越界。
 *
 * 约束：out 允许与 parent 同址（文件管理器 in-place 追加需要），但不得与 child 同址。
 * 本文件不依赖任何内核头、不调用任何库函数，freestanding 与宿主 gcc 均可编译。
 */
#ifndef LOONGOS_PATH_H
#define LOONGOS_PATH_H

#define PATH_CAP 160

static int p_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* 拼接 parent + '/' + child 到 out；parent 为 "/" 时结果为 "/child"。
 * 返回拼接后的长度（不含 NUL）；-1 = 结果放不进 cap（此时 out 一个字节都不写）。
 */
static int path_join(char *out, int cap, const char *parent, const char *child)
{
    if (cap <= 0 || !out || !parent || !child) return -1;
    int pl = p_strlen(parent), cl = p_strlen(child);
    int root = (pl == 1 && parent[0] == '/');
    int need = (root ? 1 + cl : pl + 1 + cl) + 1;   /* +1 = 结尾 NUL */
    if (need > cap) return -1;
    int k = 0;
    if (root) {
        out[k++] = '/';
    } else {
        for (int i = 0; i < pl; i++) out[k++] = parent[i];  /* out==parent 时是自拷贝，安全 */
        out[k++] = '/';                                       /* 覆盖原 NUL */
    }
    for (int i = 0; i < cl; i++) out[k++] = child[i];
    out[k] = 0;
    return k;
}

/* 取父目录："/a/b" -> "/a"，"/a" -> "/"，"/" -> "/"（根不可再上）。
 * 返回长度；-1 = cap 不足或非法输入。out 可与 path 同址。
 */
static int path_parent(char *out, int cap, const char *path)
{
    if (cap <= 0 || !out || !path) return -1;
    int n = p_strlen(path);
    if (n == 0 || n + 1 > cap) return -1;
    if (path[0] != '/') return -1;
    int len = n;
    while (len > 1 && path[len - 1] != '/') len--;
    if (len > 1) len--;          /* 去掉分隔父与末段的那条 '/' */
    else len = 1;                /* 已在根 */
    for (int i = 0; i < len; i++) out[i] = path[i];
    out[len] = 0;
    return len;
}

#endif /* LOONGOS_PATH_H */
