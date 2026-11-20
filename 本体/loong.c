/* loong.c —— LOONG 包管理器纯逻辑实现（freestanding，零内核依赖） */
#include "loong.h"

/* ---- 不依赖 libc 的小工具 ---- */
static int l_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int l_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
/* s 的前 n 字节恰等于字面量 lit（长度也必须相等） */
static int l_keyeq(const char *s, u32 n, const char *lit)
{
    if (l_strlen(lit) != (int)n) return 0;
    for (u32 i = 0; i < n; i++) if (s[i] != lit[i]) return 0;
    return 1;
}
static int l_isspace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int l_isdigit(char c) { return c >= '0' && c <= '9'; }
static int l_ishex(char c)
{
    return l_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
/* 把 s 复制到 dst（容量 cap），NUL 结尾；返回写出长度。 */
static int l_copy(char *dst, u32 cap, const char *s, u32 n)
{
    u32 i = 0;
    if (cap == 0) return 0;
    for (i = 0; i < n && i + 1 < cap; i++) dst[i] = s[i];
    dst[i] = 0;
    return (int)i;
}
/* trim：返回去首尾空白后的 [beg,end) */
static void l_trim(const char *s, u32 n, u32 *beg, u32 *end)
{
    u32 b = 0, e = n;
    while (b < e && l_isspace(s[b])) b++;
    while (e > b && l_isspace(s[e - 1])) e--;
    *beg = b; *end = e;
}
static void l_zero(void *p, u32 n) { u8 *b = (u8 *)p; while (n--) *b++ = 0; }
/* 逐字段拷贝包记录：避免结构体整体赋值被编译器生成 memcpy（freestanding 无 libc） */
static void l_pkg_copy(struct loong_pkg *d, const struct loong_pkg *s)
{
    /* volatile 逐字节阻止 -O2 把拷环折叠成 memcpy 调用（freestanding 无 libc） */
    volatile u8 *vd = (volatile u8 *)d;
    const volatile u8 *vs = (const volatile u8 *)s;
    u32 n = (u32)sizeof(struct loong_pkg);
    while (n--) *vd++ = *vs++;
}

/* ---- 版本比较：点分数字，缺段补 0，忽略前导 0 ---- */
int loong_vercmp(const char *a, const char *b)
{
    if (!a || !b) return 0;
    const char *pa = a, *pb = b;
    while (*pa || *pb) {
        u32 va = 0, vb = 0;
        while (l_isdigit(*pa)) { va = va * 10u + (u32)(*pa - '0'); pa++; }
        while (l_isdigit(*pb)) { vb = vb * 10u + (u32)(*pb - '0'); pb++; }
        if (va < vb) return -1;
        if (va > vb) return 1;
        if (*pa == '.') pa++;
        if (*pb == '.') pb++;
    }
    return 0;
}

/* ---- 索引解析：Debian 风格 Packages，字段 Key: value，空行分条 ---- */
int loong_parse_index(const char *text, u32 len, struct loong_index *out)
{
    if (!text || !out) return -1;
    out->n = 0;
    struct loong_pkg cur; l_zero(&cur, sizeof cur);
    u32 i = 0, have = 0;
    while (i < len) {
        u32 ls = i;
        while (i < len && text[i] != '\n') i++;
        u32 le = i;                       /* [ls,le) 一行，不含 \n */
        if (i < len) i++;                 /* 跳过 \n */
        u32 b, e; l_trim(text + ls, le - ls, &b, &e);
        u32 llen = e - b;
        const char *line = text + ls + b;
        if (llen == 0) {                  /* 空行：提交当前条目 */
            if (have && cur.name[0] && out->n < LOONG_MAX_PKGS) {
                cur.used = 1;
                l_pkg_copy(&out->pkg[out->n], &cur);
                out->n++;
            }
            l_zero(&cur, sizeof cur); have = 0;
            continue;
        }
        /* 找 ':' */
        u32 c = 0; while (c < llen && line[c] != ':') c++;
        if (c == 0 || c + 1 >= llen) continue;
        u32 kb, ke; l_trim(line, c, &kb, &ke);
        u32 vpos = c + 1, vb, ve; l_trim(line + vpos, llen - vpos, &vb, &ve);
        const char *val = line + vpos + vb; u32 vlen = ve - vb;
        /* 复制字段值（每行按其目标容量截断） */
        if (l_keyeq(line + kb, ke - kb, "Package")) {
            l_copy(cur.name, LOONG_NAME_LEN, val, vlen); have = 1;
        } else if (l_keyeq(line + kb, ke - kb, "Version")) {
            l_copy(cur.ver, LOONG_VER_LEN, val, vlen); have = 1;
        } else if (l_keyeq(line + kb, ke - kb, "Size")) {
            u32 v = 0; for (u32 k = 0; k < vlen && l_isdigit(val[k]); k++) v = v * 10u + (u32)(val[k] - '0');
            cur.size = v; have = 1;
        } else if (l_keyeq(line + kb, ke - kb, "File")) {
            l_copy(cur.file, LOONG_FILE_LEN, val, vlen); have = 1;
        } else if (l_keyeq(line + kb, ke - kb, "Sha256")) {
            l_copy(cur.sha, LOONG_SHA_LEN, val, vlen); have = 1;
        } else if (l_keyeq(line + kb, ke - kb, "Description") ||
                   l_keyeq(line + kb, ke - kb, "Desc")) {
            l_copy(cur.desc, LOONG_DESC_LEN, val, vlen); have = 1;
        }
    }
    /* 文本末尾无空行也要收尾 */
    if (have && cur.name[0] && out->n < LOONG_MAX_PKGS) {
        cur.used = 1;
        l_pkg_copy(&out->pkg[out->n], &cur);
        out->n++;
    }
    return (int)out->n;
}

int loong_idx_find(const struct loong_index *ix, const char *name)
{
    if (!ix || !name) return -1;
    for (u32 i = 0; i < ix->n; i++)
        if (ix->pkg[i].used && l_strcmp(ix->pkg[i].name, name) == 0) return (int)i;
    return -1;
}

int loong_is_hex64(const char *s)
{
    if (!s || l_strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++) if (!l_ishex(s[i])) return 0;
    return 1;
}

/* ---- .lpkg 头部解析 ----
 * 文本格式（纯文本容器，payload 为 base64）：
 *   LOONGPKG/1
 *   Name: hello
 *   Version: 1.0.0
 *   Path: /home/hello.elf
 *   Sha256: <64hex>
 *   <空行>
 *   <base64 payload ...>
 */
int loong_parse_pkghead(const char *text, u32 len,
                         char *name, u32 nl, char *ver, u32 vl,
                         char *path, u32 pl, char *sha, u32 sl,
                         u32 *payload_off)
{
    if (!text || !payload_off) return -1;
    if (len < 11 || !l_keyeq(text, 10, "LOONGPKG/1")) return -1;
    u32 i = 0, seen_blank = 0;
    while (i < len) {
        u32 ls = i;
        while (i < len && text[i] != '\n') i++;
        u32 le = i; if (i < len) i++;
        u32 b, e; l_trim(text + ls, le - ls, &b, &e);
        u32 llen = e - b; const char *line = text + ls + b;
        if (llen == 0) { seen_blank = 1; break; }   /* 空行后即 payload */
        u32 c = 0; while (c < llen && line[c] != ':') c++;
        if (c == 0 || c + 1 >= llen) continue;
        u32 vb, ve; l_trim(line + c + 1, llen - (c + 1), &vb, &ve);
        const char *val = line + c + 1 + vb; u32 vlen = ve - vb;
        if (l_keyeq(line, c, "Name") && name) l_copy(name, nl, val, vlen);
        else if (l_keyeq(line, c, "Version") && ver) l_copy(ver, vl, val, vlen);
        else if (l_keyeq(line, c, "Path") && path) l_copy(path, pl, val, vlen);
        else if (l_keyeq(line, c, "Sha256") && sha) l_copy(sha, sl, val, vlen);
    }
    if (!seen_blank) return -1;
    *payload_off = i;
    return 0;
}

/* ---- 已安装记账 ---- */
int loong_db_find(const struct loong_db *db, const char *name)
{
    if (!db || !name) return -1;
    for (u32 i = 0; i < db->n; i++)
        if (db->ent[i].used && l_strcmp(db->ent[i].name, name) == 0) return (int)i;
    return -1;
}

int loong_db_install(struct loong_db *db, const char *name,
                      const char *ver, const char *path)
{
    if (!db || !name || !ver || !path) return -1;
    int at = loong_db_find(db, name);
    if (at < 0) {
        if (db->n >= LOONG_MAX_INSTALLED) return -1;
        at = (int)db->n++;
    }
    struct loong_inst *e = &db->ent[at];
    l_copy(e->name, LOONG_NAME_LEN, name, (u32)l_strlen(name));
    l_copy(e->ver, LOONG_VER_LEN, ver, (u32)l_strlen(ver));
    l_copy(e->path, LOONG_FILE_LEN, path, (u32)l_strlen(path));
    e->used = 1;
    return at;
}

int loong_db_remove(struct loong_db *db, const char *name)
{
    if (!db || !name) return -1;              /* 判空须在解引用之前 */
    int at = loong_db_find(db, name);
    if (at < 0) return -1;
    l_zero(&db->ent[at], sizeof db->ent[at]);
    return 0;
}

/* ---- 状态机 ---- */
enum loong_state loong_state_next(enum loong_state cur, enum loong_event ev,
                                   struct loong_stats *st)
{
    enum loong_state nxt = cur;
    switch (cur) {
    case LST_IDLE:
        if (ev == LEV_UPDATE)        nxt = LST_UPDATING;
        else if (ev == LEV_INSTALL)  nxt = LST_FETCHING;
        else if (ev == LEV_RESET)    nxt = LST_IDLE;
        break;
    case LST_UPDATING:
        if (ev == LEV_PARSE_OK)      { nxt = LST_READY; if (st) st->updates++; }
        else if (ev == LEV_PARSE_FAIL) nxt = LST_ERROR;
        else if (ev == LEV_RESET)    nxt = LST_IDLE;
        break;
    case LST_READY:
        if (ev == LEV_INSTALL)       nxt = LST_FETCHING;
        else if (ev == LEV_UPDATE)   nxt = LST_UPDATING;
        else if (ev == LEV_RESET)    nxt = LST_IDLE;
        break;
    case LST_FETCHING:
        if (ev == LEV_FETCH_OK)      nxt = LST_VERIFYING;
        else if (ev == LEV_FETCH_FAIL) nxt = LST_ERROR;
        else if (ev == LEV_RESET)    nxt = LST_IDLE;
        break;
    case LST_VERIFYING:
        if (ev == LEV_VERIFY_OK)     nxt = LST_INSTALLING;
        else if (ev == LEV_VERIFY_FAIL) { nxt = LST_ERROR; if (st) st->verify_fail++; }
        else if (ev == LEV_RESET)    nxt = LST_IDLE;
        break;
    case LST_INSTALLING:
        if (ev == LEV_INSTALL_OK)    { nxt = LST_DONE; if (st) st->install_ok++; }
        else if (ev == LEV_INSTALL_FAIL) { nxt = LST_ERROR; if (st) st->install_fail++; }
        else if (ev == LEV_RESET)    nxt = LST_IDLE;
        break;
    case LST_DONE:
    case LST_ERROR:
        if (ev == LEV_RESET)         nxt = LST_IDLE;
        else if (ev == LEV_INSTALL)  nxt = LST_FETCHING;
        else if (ev == LEV_UPDATE)   nxt = LST_UPDATING;
        break;
    }
    if (nxt == cur && ev != LEV_RESET && st) st->state_errors++;
    return nxt;
}

const char *loong_state_name(enum loong_state s)
{
    switch (s) {
    case LST_IDLE:       return "IDLE";
    case LST_UPDATING:   return "UPDATING";
    case LST_READY:      return "READY";
    case LST_FETCHING:   return "FETCHING";
    case LST_VERIFYING:  return "VERIFYING";
    case LST_INSTALLING: return "INSTALLING";
    case LST_DONE:       return "DONE";
    case LST_ERROR:      return "ERROR";
    }
    return "?";
}
