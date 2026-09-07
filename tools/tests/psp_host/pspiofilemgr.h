#ifndef PSP_HOST_IO_H
#define PSP_HOST_IO_H
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
typedef int SceUID;
typedef off_t SceOff;
#define PSP_O_RDONLY O_RDONLY
#define PSP_O_WRONLY O_WRONLY
#define PSP_O_CREAT O_CREAT
#define PSP_O_TRUNC O_TRUNC
#define PSP_SEEK_SET SEEK_SET
#define PSP_SEEK_END SEEK_END
#define sceIoClose close
#define sceIoLseek32 lseek
static inline ssize_t sceIoRead(int fd, void* data, size_t size) { return read(fd, data, size); }
#define sceIoWrite write
#define sceIoRemove remove
#define sceIoRename rename
#define sceIoMkdir mkdir
extern const char* hostRomPath;
static inline int sceIoOpen(const char* path, int flags, int mode) {
    if (hostRomPath && strstr(path, "data/basrom.z64")) path = hostRomPath;
    return open(path, flags, mode);
}
typedef struct { struct stat d_stat; char d_name[256]; } SceIoDirent;
#define FIO_S_ISDIR S_ISDIR
static DIR* hostDirectory;
static inline int sceIoDopen(const char* path) { hostDirectory = opendir(path); return hostDirectory ? 1 : -1; }
static inline int sceIoDread(int fd, SceIoDirent* output) {
    (void)fd;
    struct dirent* entry = readdir(hostDirectory);
    if (!entry) return 0;
    snprintf(output->d_name, sizeof(output->d_name), "%s", entry->d_name);
    return fstatat(dirfd(hostDirectory), entry->d_name, &output->d_stat, 0) == 0 ? 1 : -1;
}
static inline int sceIoDclose(int fd) { (void)fd; return closedir(hostDirectory); }
#endif
