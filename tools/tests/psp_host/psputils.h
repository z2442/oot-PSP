#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
typedef CC_MD5_CTX SceKernelUtilsMd5Context;
#define HOST_MD5_INIT CC_MD5_Init
#define HOST_MD5_UPDATE CC_MD5_Update
#define HOST_MD5_FINAL CC_MD5_Final
#else
#include <openssl/md5.h>
typedef MD5_CTX SceKernelUtilsMd5Context;
#define HOST_MD5_INIT MD5_Init
#define HOST_MD5_UPDATE MD5_Update
#define HOST_MD5_FINAL MD5_Final
#endif
static inline int sceKernelUtilsMd5BlockInit(SceKernelUtilsMd5Context* ctx) {
    return HOST_MD5_INIT(ctx) == 1 ? 0 : -1;
}
static inline int sceKernelUtilsMd5BlockUpdate(SceKernelUtilsMd5Context* ctx, const void* data, unsigned int size) {
    return HOST_MD5_UPDATE(ctx, data, size) == 1 ? 0 : -1;
}
static inline int sceKernelUtilsMd5BlockResult(SceKernelUtilsMd5Context* ctx, unsigned char* digest) {
    return HOST_MD5_FINAL(digest, ctx) == 1 ? 0 : -1;
}
