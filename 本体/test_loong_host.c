/* test_loong_host.c —— LOONG 纯逻辑层宿主单测（freestanding，-Werror）
 * 覆盖：版本比较、索引解析（正/负/边界）、pkghead 解析、hex 校验、
 *       记账（增/改/删/查/满）、状态机全迁移 + 非法迁移计数。
 */
#include <stdio.h>
#include <string.h>
#include "loong.h"

static int T = 0, F = 0;
#define CK(cond, msg) do { if (cond) { T++; } else { F++; printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static void test_vercmp(void)
{
    printf("[版本比较]\n");
    CK(loong_vercmp("1.0.0", "1.0.0") == 0, "相等");
    CK(loong_vercmp("1.2.0", "1.10.0") < 0, "2<10 逐段数值比较");
    CK(loong_vercmp("2.0.0", "1.99.99") > 0, "主版本优先");
    CK(loong_vercmp("1.2", "1.2.0") == 0, "缺段补0");
    CK(loong_vercmp("1.02.0", "1.2.0") == 0, "前导零忽略");
    CK(loong_vercmp("0.9.9", "0.10.0") < 0, "次版本数值");
    CK(loong_vercmp("10", "9") > 0, "多位数");
    CK(loong_vercmp("1.0.1", "1.0.0") > 0, "修订号");
    CK(loong_vercmp("", "0.0.0") == 0, "空按0");
}

static void test_index(void)
{
    printf("[索引解析]\n");
    const char *ix =
        "Package: hello\n"
        "Version: 1.2.0\n"
        "Size: 5624\n"
        "File: hello_1.2.0.lpkg\n"
        "Sha256: 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        "Desc: Hello World demo\n"
        "\n"
        "Package: demo\n"
        "Version: 0.9\n"
        "File: demo.lpkg\n"
        "\n";
    struct loong_index o; memset(&o, 0, sizeof o);
    int n = loong_parse_index(ix, (u32)strlen(ix), &o);
    CK(n == 2, "解析出2条");
    CK(o.n == 2, "o.n=2");
    CK(strcmp(o.pkg[0].name, "hello") == 0, "包名hello");
    CK(strcmp(o.pkg[0].ver, "1.2.0") == 0, "版本1.2.0");
    CK(o.pkg[0].size == 5624, "size=5624");
    CK(strcmp(o.pkg[0].file, "hello_1.2.0.lpkg") == 0, "文件名");
    CK(strcmp(o.pkg[0].desc, "Hello World demo") == 0, "描述");
    CK(loong_is_hex64(o.pkg[0].sha), "sha是64hex");
    CK(strcmp(o.pkg[1].name, "demo") == 0 && strcmp(o.pkg[1].ver, "0.9") == 0, "第二条");
    CK(loong_idx_find(&o, "hello") == 0, "find hello=0");
    CK(loong_idx_find(&o, "demo") == 1, "find demo=1");
    CK(loong_idx_find(&o, "nope") == -1, "find 不存在=-1");
    CK(o.pkg[0].used == 1, "used置位");

    /* 末尾无空行仍收尾 */
    const char *one = "Package: solo\nVersion: 1.0\n";
    memset(&o, 0, sizeof o);
    CK(loong_parse_index(one, (u32)strlen(one), &o) == 1, "末尾无空行收尾");
    CK(strcmp(o.pkg[0].name, "solo") == 0, "solo");

    /* 非 NUL 结尾 + 精确长度（模拟 HTTP buffer） */
    const char *buf = "Package: x\nVersion: 2\n\nZZZZ";
    memset(&o, 0, sizeof o);
    CK(loong_parse_index(buf, 21, &o) == 1, "按len边界解析,忽略尾随垃圾");
    CK(strcmp(o.pkg[0].name, "x") == 0, "包名x");

    /* 负例 */
    memset(&o, 0, sizeof o);
    CK(loong_parse_index(NULL, 10, &o) == -1, "NULL文本-1");
    CK(loong_parse_index("x", 1, NULL) == -1, "NULL输出-1");
    CK(loong_parse_index("", 0, &o) == 0, "空文本0条");

    /* 容量上限：18 条只收 16 */
    char big[768]; big[0] = 0;
    for (int i = 0; i < 18; i++) {
        char t[48]; snprintf(t, sizeof t, "Package: p%d\nVersion: 1\n\n", i);
        strcat(big, t);   /* snprintf 已补 NUL，strcat 安全拼接 */
    }
    memset(&o, 0, sizeof o);
    CK(loong_parse_index(big, (u32)strlen(big), &o) == (int)LOONG_MAX_PKGS, "容量上限截断16");
}

static void test_pkghead(void)
{
    printf("[包容器解析]\n");
    const char *p =
        "LOONGPKG/1\n"
        "Name: hello\n"
        "Version: 1.2.0\n"
        "Path: /home/hello.elf\n"
        "Sha256: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "\n"
        "QUJDREVGRw==";
    char name[32], ver[24], path[72], sha[65]; u32 off = 0;
    int r = loong_parse_pkghead(p, (u32)strlen(p), name, sizeof name, ver, sizeof ver,
                                path, sizeof path, sha, sizeof sha, &off);
    CK(r == 0, "pkghead成功");
    CK(strcmp(name, "hello") == 0, "name");
    CK(strcmp(ver, "1.2.0") == 0, "ver");
    CK(strcmp(path, "/home/hello.elf") == 0, "path");
    CK(loong_is_hex64(sha), "sha hex");
    CK(off > 0 && p[off] == 'Q' && (u32)strlen(p) - off == 12, "payload偏移正确");

    CK(loong_parse_pkghead("BAD\nName: x\n\nYQ==", 17, name, sizeof name, ver, sizeof ver,
                           path, sizeof path, sha, sizeof sha, &off) == -1, "错误magic=-1");
    CK(loong_parse_pkghead("LOONGPKG/1\nName: x", 20, name, sizeof name, ver, sizeof ver,
                           path, sizeof path, sha, sizeof sha, &off) == -1, "无空行分隔=-1");
}

static void test_hex(void)
{
    printf("[hex校验]\n");
    CK(loong_is_hex64("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"), "合法小写hex");
    CK(loong_is_hex64("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789"), "合法大写hex");
    CK(!loong_is_hex64("0123"), "太短");
    CK(!loong_is_hex64("gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg"), "非法字符");
    CK(!loong_is_hex64(NULL), "NULL");
}

static void test_db(void)
{
    printf("[安装记账]\n");
    struct loong_db db; memset(&db, 0, sizeof db);
    CK(loong_db_install(&db, "hello", "1.0", "/home/hello.elf") == 0, "装hello槽0");
    CK(loong_db_install(&db, "demo", "0.9", "/home/demo.elf") == 1, "装demo槽1");
    CK(db.n == 2, "n=2");
    CK(loong_db_find(&db, "hello") == 0, "find hello");
    /* 同名升级：幂等，槽位不变，版本更新 */
    CK(loong_db_install(&db, "hello", "2.0", "/home/hello.elf") == 0, "升级hello仍槽0");
    CK(db.n == 2, "升级不加条目");
    CK(strcmp(db.ent[0].ver, "2.0") == 0, "版本更新为2.0");
    /* 删除 */
    CK(loong_db_remove(&db, "demo") == 0, "删demo");
    CK(loong_db_find(&db, "demo") == -1, "demo已删");
    CK(loong_db_remove(&db, "nope") == -1, "删不存在=-1");
    /* 填满 */
    memset(&db, 0, sizeof db);
    for (u32 i = 0; i < LOONG_MAX_INSTALLED; i++) {
        char nm[16]; snprintf(nm, sizeof nm, "p%u", i);
        CK(loong_db_install(&db, nm, "1", "/x") >= 0, "填充");
    }
    CK(loong_db_install(&db, "overflow", "1", "/x") == -1, "满=-1");
}

static void test_state(void)
{
    printf("[状态机]\n");
    struct loong_stats st; memset(&st, 0, sizeof st);
    enum loong_state s = LST_IDLE;
    s = loong_state_next(s, LEV_UPDATE, &st);     CK(s == LST_UPDATING, "IDLE→UPDATING");
    s = loong_state_next(s, LEV_PARSE_OK, &st);   CK(s == LST_READY, "UPDATING→READY");
    CK(st.updates == 1, "updates=1");
    s = loong_state_next(s, LEV_INSTALL, &st);    CK(s == LST_FETCHING, "READY→FETCHING");
    s = loong_state_next(s, LEV_FETCH_OK, &st);   CK(s == LST_VERIFYING, "FETCH→VERIFY");
    s = loong_state_next(s, LEV_VERIFY_OK, &st);  CK(s == LST_INSTALLING, "VERIFY→INSTALLING");
    s = loong_state_next(s, LEV_INSTALL_OK, &st); CK(s == LST_DONE, "INSTALLING→DONE");
    CK(st.install_ok == 1, "install_ok=1");
    s = loong_state_next(s, LEV_RESET, &st);      CK(s == LST_IDLE, "DONE→IDLE");

    /* 失败分支 */
    memset(&st, 0, sizeof st); s = LST_IDLE;
    s = loong_state_next(s, LEV_UPDATE, &st);
    s = loong_state_next(s, LEV_PARSE_FAIL, &st); CK(s == LST_ERROR, "解析失败→ERROR");
    s = loong_state_next(s, LEV_RESET, &st);

    s = LST_FETCHING;
    s = loong_state_next(s, LEV_FETCH_FAIL, &st); CK(s == LST_ERROR, "下载失败→ERROR");
    s = LST_VERIFYING;
    s = loong_state_next(s, LEV_VERIFY_FAIL, &st); CK(s == LST_ERROR, "校验失败→ERROR");
    CK(st.verify_fail == 1, "verify_fail=1");
    s = LST_INSTALLING;
    s = loong_state_next(s, LEV_INSTALL_FAIL, &st); CK(s == LST_ERROR, "安装失败→ERROR");
    CK(st.install_fail == 1, "install_fail=1");

    /* ERROR 可重来 */
    s = loong_state_next(s, LEV_INSTALL, &st);    CK(s == LST_FETCHING, "ERROR→FETCHING重试");

    /* 非法迁移：IDLE 收到 FETCH_OK */
    memset(&st, 0, sizeof st); s = LST_IDLE;
    s = loong_state_next(s, LEV_FETCH_OK, &st);
    CK(s == LST_IDLE && st.state_errors == 1, "非法迁移保持并计数");

    CK(strcmp(loong_state_name(LST_READY), "READY") == 0, "状态名");
    CK(strcmp(loong_state_name(LST_ERROR), "ERROR") == 0, "状态名ERROR");
}

int main(void)
{
    printf("=== LOONG 纯逻辑宿主单测 ===\n");
    test_vercmp(); test_index(); test_pkghead(); test_hex(); test_db(); test_state();
    printf("=== 结果: %d PASS / %d FAIL ===\n", T, F);
    return F == 0 ? 0 : 1;
}
