/* LoongOS 默认账号凭据（预计算常量）—— 内核镜像里不出现任何明文口令。
 * 口令从没进过内核：auth_init 只是把下面的哈希抄进 /etc/shadow；
 * 登录时 shadow_verify 拿用户敲的口令现场算哈希再比对。
 *
 * 更换口令三步（都在宿主机做，不碰内核）：
 *   1) 改 gen_authhash.c 里的 PW_ROOT_TXT / PW_DEMO_TXT（明文只在这个工具里）；
 *   2) cc -O2 gen_authhash.c crypto.c -o gen_authhash && ./gen_authhash
 *      → 打印两行 64 字符哈希，粘回下面的 PW_ROOT_HASH / PW_DEMO_HASH；
 *   3) ./gen_authhash -c 自检一致性（测试脚本会跑这一条）。
 * shadow 文件格式不变：name:盐hex:哈希hex:迭代数。 */
#ifndef LOONGOS_AUTH_CRED_H
#define LOONGOS_AUTH_CRED_H

#define AUTH_ITER 50000        /* 迭代轮数：猜一次口令要算 5 万次 SHA-256（旧值 2000 太低） */
static const u8 AUTH_SALT[8] = { 'L','o','o','n','g','S','l','t' };

/* sha256_iter(AUTH_SALT, 8, 口令, AUTH_ITER) 的结果（hex）：
   上 = root，下 = demo。教学默认口令见 gen_authhash.c 与 docs（不进镜像）。 */
static const char PW_ROOT_HASH[65] =
    "36fc7b69ea950025fb3f6ee4d5c05a4044ca383829fd8ae41806c816176bcd20";
static const char PW_DEMO_HASH[65] =
    "70c9c113352c7e9348eb01c2bdd58ed9bdb4bb06329c7f44e40240739c23dd81";

#endif
