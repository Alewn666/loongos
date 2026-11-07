/* 生成/校验 auth_cred.h 里的预计算口令哈希 —— 宿主机工具，不进内核镜像。
 *
 * 生成：cc -O2 gen_authhash.c crypto.c -o gen_authhash && ./gen_authhash
 * 校验：./gen_authhash -c   （与 auth_cred.h 一致 → 退出码 0；不一致 → 1）
 *
 * ⚠️ 明文默认口令只存在于这个文件里；换口令改这里，重跑，把输出粘回 auth_cred.h。 */
#include <stdio.h>
#include <string.h>
#include "crypto.h"
#include "auth_cred.h"

/* 教学默认口令（root / demo）——只在这里出现，不编进内核 */
static const char PW_ROOT_TXT[] = "loong2026";
static const char PW_DEMO_TXT[]  = "demo2026";

static void hex_of_pw(const char *pw, char out[65])
{
    u8 h[32];
    sha256_iter(AUTH_SALT, 8, pw, AUTH_ITER, h);
    to_hex(h, 32, out);
}

int main(int argc, char **argv)
{
    char root[65], demo[65];
    hex_of_pw(PW_ROOT_TXT, root);
    hex_of_pw(PW_DEMO_TXT, demo);

    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        int ok = (strcmp(root, PW_ROOT_HASH) == 0) && (strcmp(demo, PW_DEMO_HASH) == 0);
        printf(ok ? "authhash OK：auth_cred.h 与真实口令一致\n"
                  : "authhash MISMATCH：auth_cred.h 里的哈希和真实口令对不上（先跑本工具重新生成再粘贴！）\n");
        return ok ? 0 : 1;
    }
    printf("root hash:\n%s\n", root);
    printf("demo hash:\n%s\n", demo);
    return 0;
}
