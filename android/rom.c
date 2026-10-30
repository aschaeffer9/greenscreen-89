/* The game itself is not part of the app: the player supplies their own ROM
 * image once (opened from the Files app / share sheet, or found in Downloads),
 * it's checked against the World Rev A fingerprint and kept as a private copy.
 * Accepts a bare .gb file or a .zip containing one. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "rom.h"

/* --------------------------------------------------------------- MD5 */
static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
static const uint32_t K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
static const int S[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                           4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
void rom_md5(const uint8_t *msg, size_t len, uint8_t out[16]) {
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    size_t total = ((len + 8) / 64 + 1) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t blk[64];
        for (int i = 0; i < 64; i++) {
            size_t p = off + (size_t)i;
            blk[i] = p < len ? msg[p] : p == len ? 0x80 : 0;
        }
        if (off + 64 == total) { uint64_t bits = (uint64_t)len * 8; for (int i = 0; i < 8; i++) blk[56 + i] = (uint8_t)(bits >> (8 * i)); }
        uint32_t w[16];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)blk[i * 4] | (uint32_t)blk[i * 4 + 1] << 8 | (uint32_t)blk[i * 4 + 2] << 16 | (uint32_t)blk[i * 4 + 3] << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f; int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
            else { f = c ^ (b | ~d); g = (7 * i) & 15; }
            uint32_t t = d; d = c; c = b;
            b = b + rol(a + f + K[i] + w[g], S[i]);
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (8 * j));
}

/* the cartridge this port's addresses were mapped from: Tetris (World) (Rev A) */
static const uint8_t WANT[16] = { 0x98,0x2e,0xd5,0xd2,0xb1,0x2a,0x03,0x77,0xeb,0x14,0xbc,0xdc,0x41,0x23,0x74,0x4e };

/* ---------------------------------------------------- raw DEFLATE (RFC 1951) */
typedef struct { const uint8_t *in; size_t inlen, ip; uint32_t bitbuf; int bitcnt; uint8_t *out; size_t outlen, op; } Inf;
typedef struct { short count[16], sym[320]; } Huff;
static int bits(Inf *s, int need) {
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->ip >= s->inlen) return -1;
        v |= (uint32_t)s->in[s->ip++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need; s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}
static int decode(Inf *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int b = bits(s, 1); if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static int build(Huff *h, const short *len, int n) {
    short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; l++) { left <<= 1; left -= h->count[l]; if (left < 0) return -1; }
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (short)i;
    return left;
}
static const short LBASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short LEXT[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short DBASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short DEXT[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static int codes(Inf *s, const Huff *lc, const Huff *dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) { if (s->op >= s->outlen) return -2; s->out[s->op++] = (uint8_t)sym; }
        else if (sym == 256) return 0;
        else {
            sym -= 257; if (sym >= 29) return -1;
            int e = bits(s, LEXT[sym]); if (e < 0) return -1;
            int len = LBASE[sym] + e;
            int ds = decode(s, dc); if (ds < 0 || ds >= 30) return -1;
            int de = bits(s, DEXT[ds]); if (de < 0) return -1;
            size_t dist = (size_t)(DBASE[ds] + de);
            if (dist > s->op) return -1;
            if (s->op + (size_t)len > s->outlen) return -2;
            for (int i = 0; i < len; i++) { s->out[s->op] = s->out[s->op - dist]; s->op++; }
        }
    }
}
static int inflate_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen, size_t *produced) {
    Inf s = { in, inlen, 0, 0, 0, out, outlen, 0 };
    static Huff lc, dc;
    int last;
    do {
        last = bits(&s, 1); int type = bits(&s, 2);
        if (last < 0 || type < 0) return -1;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.ip + 4 > s.inlen) return -1;
            unsigned len = s.in[s.ip] | s.in[s.ip + 1] << 8; s.ip += 4;
            if (s.ip + len > s.inlen || s.op + len > s.outlen) return -1;
            memcpy(s.out + s.op, s.in + s.ip, len); s.ip += len; s.op += len;
        } else if (type == 1) {
            short l[320];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            build(&lc, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            build(&dc, l, 30);
            if (codes(&s, &lc, &dc) != 0) return -1;
        } else if (type == 2) {
            static const short ord[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            short l[320];
            int nlen = bits(&s, 5), ndist = bits(&s, 5), ncode = bits(&s, 4);
            if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
            nlen += 257; ndist += 1; ncode += 4;
            if (nlen > 286 || ndist > 30) return -1;
            int i;
            for (i = 0; i < ncode; i++) { int b = bits(&s, 3); if (b < 0) return -1; l[ord[i]] = (short)b; }
            for (; i < 19; i++) l[ord[i]] = 0;
            if (build(&lc, l, 19) != 0) return -1;
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = decode(&s, &lc);
                if (sym < 0) return -1;
                if (sym < 16) l[idx++] = (short)sym;
                else {
                    int len = 0, rep;
                    if (sym == 16) { if (idx == 0) return -1; len = l[idx - 1]; rep = 3 + bits(&s, 2); }
                    else if (sym == 17) rep = 3 + bits(&s, 3);
                    else rep = 11 + bits(&s, 7);
                    if (rep < 3 || idx + rep > nlen + ndist) return -1;
                    while (rep--) l[idx++] = (short)len;
                }
            }
            if (l[256] == 0) return -1;
            if (build(&lc, l, nlen) < 0) return -1;
            if (build(&dc, l + nlen, ndist) < 0) return -1;
            if (codes(&s, &lc, &dc) != 0) return -1;
        } else return -1;
    } while (!last);
    *produced = s.op;
    return 0;
}

/* --------------------------------------------------------------- check */
static int check_bare(const uint8_t *d, size_t n, uint8_t *out) {
    if (n != ROM_SIZE) return ROM_NOT_FOUND;
    uint8_t h[16]; rom_md5(d, n, h);
    if (memcmp(h, WANT, 16) != 0) {
        /* a Game Boy Tetris of another revision, or another game */
        return (memcmp(d + 0x134, "TETRIS", 6) == 0) ? ROM_WRONG_VERSION : ROM_NOT_FOUND;
    }
    memcpy(out, d, ROM_SIZE);
    return ROM_OK;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

int rom_check(const uint8_t *d, size_t n, uint8_t *out) {
    if (n >= 4 && d[0] == 'P' && d[1] == 'K' && d[2] == 3 && d[3] == 4) {
        /* zip: walk the central directory, try every 32 KiB entry */
        size_t eocd = 0; int found = 0;
        for (size_t i = n >= 22 ? n - 22 : 0; ; i--) {
            if (le32(d + i) == 0x06054b50) { eocd = i; found = 1; break; }
            if (i == 0 || n - i > 70000) break;
        }
        if (!found) return ROM_NOT_FOUND;
        size_t cd = le32(d + eocd + 16), entries = le16(d + eocd + 10);
        int best = ROM_NOT_FOUND;
        static uint8_t tmp[ROM_SIZE];
        for (size_t e = 0; e < entries && cd + 46 <= n; e++) {
            if (le32(d + cd) != 0x02014b50) break;
            uint32_t method = le16(d + cd + 10), csize = le32(d + cd + 20), usize = le32(d + cd + 24);
            uint32_t fnl = le16(d + cd + 28), exl = le16(d + cd + 30), cml = le16(d + cd + 32), lho = le32(d + cd + 42);
            cd += 46 + fnl + exl + cml;
            if (usize != ROM_SIZE || (size_t)lho + 30 > n || le32(d + lho) != 0x04034b50) continue;
            size_t data = lho + 30 + le16(d + lho + 26) + le16(d + lho + 28);
            if (data + csize > n) continue;
            int r = ROM_NOT_FOUND;
            if (method == 0 && csize == ROM_SIZE) r = check_bare(d + data, ROM_SIZE, out);
            else if (method == 8) {
                size_t got = 0;
                if (inflate_raw(d + data, csize, tmp, ROM_SIZE, &got) == 0 && got == ROM_SIZE) r = check_bare(tmp, ROM_SIZE, out);
            }
            if (r == ROM_OK) return ROM_OK;
            if (r == ROM_WRONG_VERSION) best = r;
        }
        return best;
    }
    return check_bare(d, n, out);
}

int rom_check_file(const char *path, uint8_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return ROM_NOT_FOUND;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > ROM_MAX_FILE) { fclose(f); return ROM_NOT_FOUND; }
    uint8_t *d = malloc((size_t)n);
    if (!d) { fclose(f); return ROM_NOT_FOUND; }
    size_t g = fread(d, 1, (size_t)n, f); fclose(f);
    int r = g == (size_t)n ? rom_check(d, (size_t)n, out) : ROM_NOT_FOUND;
    free(d);
    return r;
}

int rom_save(const char *path, const uint8_t *rom) {
    char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(rom, 1, ROM_SIZE, f) == ROM_SIZE;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); return -1; }
    return rename(tmp, path);
}

static int ends_with_ci(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++) { char c = s[a - b + i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != suf[i]) return 0; }
    return 1;
}
static int scan_dir(const char *dir, int depth, uint8_t *out, int *saw_wrong) {
    DIR *d = opendir(dir);
    if (!d) return ROM_NOT_FOUND;
    struct dirent *de;
    int r = ROM_NOT_FOUND;
    while ((de = readdir(d)) && r != ROM_OK) {
        if (de->d_name[0] == '.') continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth > 0) r = scan_dir(p, depth - 1, out, saw_wrong); continue; }
        if (!(ends_with_ci(de->d_name, ".gb") || ends_with_ci(de->d_name, ".zip"))) continue;
        if (st.st_size <= 0 || st.st_size > ROM_MAX_FILE) continue;
        int k = rom_check_file(p, out);
        if (k == ROM_OK) r = ROM_OK;
        else if (k == ROM_WRONG_VERSION) *saw_wrong = 1;
    }
    closedir(d);
    return r;
}
int rom_scan(const char *const *dirs, int ndirs, uint8_t *out) {
    int wrong = 0;
    for (int i = 0; i < ndirs; i++) if (scan_dir(dirs[i], 1, out, &wrong) == ROM_OK) return ROM_OK;
    return wrong ? ROM_WRONG_VERSION : ROM_NOT_FOUND;
}

/* ------------------------------------------------------------ Android */
#ifdef __ANDROID__
#include <jni.h>
static int jx(JNIEnv *e) { if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); return 1; } return 0; }
static JNIEnv *env_of(JavaVM *vm) {
    JNIEnv *e = NULL;
    if ((*vm)->GetEnv(vm, (void **)&e, JNI_VERSION_1_6) == JNI_OK) return e;
    if ((*vm)->AttachCurrentThread(vm, &e, NULL) != JNI_OK) return NULL;
    return e;
}

/* the URI this activity was opened with (Open with / Share), or NULL */
static jobject intent_uri(JNIEnv *e, jobject act) {
    jclass cA = (*e)->FindClass(e, "android/app/Activity");
    jobject it = (*e)->CallObjectMethod(e, act, (*e)->GetMethodID(e, cA, "getIntent", "()Landroid/content/Intent;"));
    if (jx(e) || !it) return NULL;
    jclass cI = (*e)->FindClass(e, "android/content/Intent");
    jobject uri = (*e)->CallObjectMethod(e, it, (*e)->GetMethodID(e, cI, "getData", "()Landroid/net/Uri;"));
    if (jx(e)) uri = NULL;
    if (!uri) {
        jstring k = (*e)->NewStringUTF(e, "android.intent.extra.STREAM");
        uri = (*e)->CallObjectMethod(e, it, (*e)->GetMethodID(e, cI, "getParcelableExtra", "(Ljava/lang/String;)Landroid/os/Parcelable;"), k);
        if (jx(e)) uri = NULL;
    }
    return uri;
}

int rom_intent_has_file(JavaVM *vm, jobject act) {
    JNIEnv *e = env_of(vm); if (!e) return 0;
    jobject u = intent_uri(e, act);
    return u != NULL;
}

int rom_from_intent(JavaVM *vm, jobject act, uint8_t *out) {
    JNIEnv *e = env_of(vm); if (!e) return ROM_NOT_FOUND;
    jobject uri = intent_uri(e, act);
    if (!uri) return ROM_NOT_FOUND;
    jclass cC = (*e)->FindClass(e, "android/content/Context");
    jobject cr = (*e)->CallObjectMethod(e, act, (*e)->GetMethodID(e, cC, "getContentResolver", "()Landroid/content/ContentResolver;"));
    if (jx(e) || !cr) return ROM_NOT_FOUND;
    jclass cR = (*e)->FindClass(e, "android/content/ContentResolver");
    jobject in = (*e)->CallObjectMethod(e, cr, (*e)->GetMethodID(e, cR, "openInputStream", "(Landroid/net/Uri;)Ljava/io/InputStream;"), uri);
    if (jx(e) || !in) return ROM_UNREADABLE;
    jclass cIS = (*e)->FindClass(e, "java/io/InputStream");
    jmethodID rd = (*e)->GetMethodID(e, cIS, "read", "([BII)I");
    size_t cap = 1 << 16, n = 0;
    uint8_t *d = malloc(cap);
    jbyteArray arr = (*e)->NewByteArray(e, 65536);
    int r = ROM_NOT_FOUND, failed = 0;
    while (d && arr) {
        jint g = (*e)->CallIntMethod(e, in, rd, arr, 0, 65536);
        if (jx(e)) { failed = 1; break; }
        if (g < 0) break;
        if (n + (size_t)g > ROM_MAX_FILE) { failed = 1; break; }
        if (n + (size_t)g > cap) { while (n + (size_t)g > cap) cap *= 2; uint8_t *nd = realloc(d, cap); if (!nd) { failed = 1; break; } d = nd; }
        (*e)->GetByteArrayRegion(e, arr, 0, g, (jbyte *)(d + n));
        n += (size_t)g;
    }
    (*e)->CallVoidMethod(e, in, (*e)->GetMethodID(e, cIS, "close", "()V")); jx(e);
    if (d && !failed) r = rom_check(d, n, out);
    else if (failed) r = ROM_UNREADABLE;
    free(d);
    if (arr) (*e)->DeleteLocalRef(e, arr);
    return r;
}

int rom_have_storage_permission(JavaVM *vm, jobject act) {
    JNIEnv *e = env_of(vm); if (!e) return 0;
    jclass cC = (*e)->FindClass(e, "android/content/Context");
    jstring p = (*e)->NewStringUTF(e, "android.permission.READ_EXTERNAL_STORAGE");
    jint r = (*e)->CallIntMethod(e, act, (*e)->GetMethodID(e, cC, "checkSelfPermission", "(Ljava/lang/String;)I"), p);
    if (jx(e)) return 0;
    return r == 0;
}
void rom_request_storage_permission(JavaVM *vm, jobject act) {
    JNIEnv *e = env_of(vm); if (!e) return;
    jclass cS = (*e)->FindClass(e, "java/lang/String");
    jobjectArray a = (*e)->NewObjectArray(e, 1, cS, (*e)->NewStringUTF(e, "android.permission.READ_EXTERNAL_STORAGE"));
    jclass cA = (*e)->FindClass(e, "android/app/Activity");
    (*e)->CallVoidMethod(e, act, (*e)->GetMethodID(e, cA, "requestPermissions", "([Ljava/lang/String;I)V"), a, 1);
    jx(e);
}

static void start(JNIEnv *e, jobject act, jobject it) {
    jclass cA = (*e)->FindClass(e, "android/app/Activity");
    (*e)->CallVoidMethod(e, act, (*e)->GetMethodID(e, cA, "startActivity", "(Landroid/content/Intent;)V"), it);
    jx(e);
}
static jobject new_intent(JNIEnv *e, const char *action) {
    jclass cI = (*e)->FindClass(e, "android/content/Intent");
    jstring a = (*e)->NewStringUTF(e, action);
    jobject it = (*e)->NewObject(e, cI, (*e)->GetMethodID(e, cI, "<init>", "(Ljava/lang/String;)V"), a);
    if (jx(e)) return NULL;
    (*e)->CallObjectMethod(e, it, (*e)->GetMethodID(e, cI, "addFlags", "(I)Landroid/content/Intent;"), 0x10000000); jx(e);   /* NEW_TASK */
    return it;
}
void rom_open_files_app(JavaVM *vm, jobject act) {
    JNIEnv *e = env_of(vm); if (!e) return;
    jobject it = new_intent(e, "android.intent.action.VIEW_DOWNLOADS");
    if (it) start(e, act, it);
}
/* after importing in an "Open with" instance: bring up the normal app */
void rom_launch_main(JavaVM *vm, jobject act) {
    JNIEnv *e = env_of(vm); if (!e) return;
    jobject it = new_intent(e, "android.intent.action.MAIN");
    if (!it) return;
    jclass cI = (*e)->FindClass(e, "android/content/Intent");
    jstring cat = (*e)->NewStringUTF(e, "android.intent.category.LAUNCHER");
    (*e)->CallObjectMethod(e, it, (*e)->GetMethodID(e, cI, "addCategory", "(Ljava/lang/String;)Landroid/content/Intent;"), cat); jx(e);
    jclass cC = (*e)->FindClass(e, "android/content/Context");
    jstring pkg = (jstring)(*e)->CallObjectMethod(e, act, (*e)->GetMethodID(e, cC, "getPackageName", "()Ljava/lang/String;"));
    if (jx(e) || !pkg) return;
    jstring cls = (*e)->NewStringUTF(e, "android.app.NativeActivity");
    (*e)->CallObjectMethod(e, it, (*e)->GetMethodID(e, cI, "setClassName", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;"), pkg, cls); jx(e);
    start(e, act, it);
}
#endif
