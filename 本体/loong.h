/* loong.h —— LOONG 包管理器的纯逻辑层（freestanding，宿主可 -Werror 单测）
 *
 * LOONG = LoongOS Online Network Gateway，系统的包管理器（类比 apt/ATP）。
 * 本文件只做「解析 / 比较 / 记账 / 状态机」这类不碰内核全局的纯逻辑，
 * 网络收发（tcp_*）与落盘（fs_*）与 base64 解码留在 kernel.c 的 cmd_loong 里，
 * 这样核心算法能在 PC 上 1:1 复现验证，符合项目「纯逻辑地基 + 最小接线」惯例。
 *
 * 数据来源：NAS 上 Docker 托管的静态 HTTP 源（详见 loong-repo/README.md）。
 */
#ifndef LOONGOS_LOONG_H
#define LOONGOS_LOONG_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define LOONG_MAX_PKGS      16u
#define LOONG_NAME_LEN      32u
#define LOONG_VER_LEN       24u
#define LOONG_FILE_LEN      72u
#define LOONG_SHA_LEN       65u   /* 64 hex + NUL */
#define LOONG_DESC_LEN      80u
#define LOONG_MAX_INSTALLED 16u

/* Packages 索引里的一条记录 */
struct loong_pkg {
    char name[LOONG_NAME_LEN];
    char ver[LOONG_VER_LEN];
    char file[LOONG_FILE_LEN];   /* 源上的 .lpkg 文件名 */
    char sha[LOONG_SHA_LEN];     /* 包内容 sha256 hex（小写） */
    char desc[LOONG_DESC_LEN];
    u32  size;                   /* 解包后字节数 */
    int  used;
};

struct loong_index {
    struct loong_pkg pkg[LOONG_MAX_PKGS];
    u32 n;
};

/* 已安装包记账 */
struct loong_inst {
    char name[LOONG_NAME_LEN];
    char ver[LOONG_VER_LEN];
    char path[LOONG_FILE_LEN];
    int  used;
};
struct loong_db {
    struct loong_inst ent[LOONG_MAX_INSTALLED];
    u32 n;
};

/* 状态机 */
enum loong_state {
    LST_IDLE = 0, LST_UPDATING, LST_READY, LST_FETCHING,
    LST_VERIFYING, LST_INSTALLING, LST_DONE, LST_ERROR
};
enum loong_event {
    LEV_UPDATE = 0, LEV_PARSE_OK, LEV_PARSE_FAIL,
    LEV_INSTALL, LEV_FETCH_OK, LEV_FETCH_FAIL,
    LEV_VERIFY_OK, LEV_VERIFY_FAIL,
    LEV_INSTALL_OK, LEV_INSTALL_FAIL, LEV_RESET
};

struct loong_stats {
    u32 updates;       /* 成功解析索引次数 */
    u32 fetch_bytes;   /* 累计下载字节（内核层累加） */
    u32 install_ok;
    u32 install_fail;
    u32 verify_fail;
    u32 state_errors;  /* 非法状态迁移次数 */
};

/* —— API —— */
/* 点分数字版本比较：-1 a<b, 0 a==b, 1 a>b；忽略前导零，缺段按 0。 */
int  loong_vercmp(const char *a, const char *b);

/* 解析 Packages 索引文本（不必 NUL 结尾，按 len 边界逐行）。返回条目数；参数错 -1。 */
int  loong_parse_index(const char *text, u32 len, struct loong_index *out);

/* 在索引里按包名查找，返回下标；未找到 -1。 */
int  loong_idx_find(const struct loong_index *ix, const char *name);

/* 解析 .lpkg 头部，抽取字段并给出 base64 payload 起始偏移（相对 text）。
   成功 0；格式错 -1。payload_len = len - *payload_off。 */
int  loong_parse_pkghead(const char *text, u32 len,
                          char *name, u32 nl, char *ver, u32 vl,
                          char *path, u32 pl, char *sha, u32 sl,
                          u32 *payload_off);

/* 64 位小写 hex 串校验：是返回 1，否则 0。 */
int  loong_is_hex64(const char *s);

/* 记账：install 同名则更新（幂等升级），返回槽位 >=0；满 -1。
   remove 返回 0 成功 / -1 未找到；find 返回槽位 / -1。 */
int  loong_db_install(struct loong_db *db, const char *name,
                       const char *ver, const char *path);
int  loong_db_remove(struct loong_db *db, const char *name);
int  loong_db_find(const struct loong_db *db, const char *name);

/* 状态机迁移：返回新状态；非法迁移保持 cur 并 st->state_errors++。 */
enum loong_state loong_state_next(enum loong_state cur, enum loong_event ev,
                                   struct loong_stats *st);
const char *loong_state_name(enum loong_state s);

#endif
