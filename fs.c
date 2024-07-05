#include "aiwn.h"
#include <dirent.h>
#include <stdio.h>
// clang-format off
#include <sys/types.h>
#include <sys/stat.h>
// clang-format on
#include <unistd.h>
#if defined(_WIN32) || defined(WIN32)
  #include <windows.h>
  #include <libloaderapi.h>
  #include <processthreadsapi.h>
  #include <synchapi.h>
  #define stat _stati64
static void MakePathSane(char *ptr) {
  char *ptr2 = ptr;
enter:
  while (*ptr) {
    if (*ptr == '/' && ptr[1] == '/') {
      *ptr2++ = *ptr++;
      while (*ptr == '/')
        ptr++;
      goto enter;
    }
    if (ptr != ptr2)
      *ptr2++ = *ptr;
    else
      ptr2++;
    if (*ptr)
      ptr++;
  }
  *ptr2 = 0;
}
#endif
static int __FExists(char *path) {
#if defined(_WIN32) || defined(WIN32)
  return PathFileExistsA(path);
#else
  return access(path, F_OK) == 0;
#endif
}
static int __FIsDir(char *path) {
#if defined(_WIN32) || defined(WIN32)
  return PathIsDirectoryA(path);
#else
  struct stat s;
  stat(path, &s);
  return (s.st_mode & S_IFMT) == S_IFDIR;
#endif
}

int64_t FileExists(char *name) {
  return access(name, F_OK) == 0;
}
void FileWrite(char *fn, char *data, int64_t sz) {
  FILE *f = fopen(fn, "wb");
  if (!f)
    return;
  fwrite(data, sz, 1, f);
  fclose(f);
}
char *FileRead(char *fn, int64_t *sz) {
  int64_t s, e;
  FILE *f = fopen(fn, "rb");
  char *ret;
  if (!f) {
    if (sz)
      *sz = 0;
    return A_CALLOC(1, NULL);
  }
  fseek(f, 0, SEEK_END);
  e = ftell(f);
  fseek(f, 0, SEEK_SET);
  s   = e - ftell(f);
  ret = A_MALLOC(s + 1, NULL);
  fread(ret, 1, s, f);
  ret[s] = 0;
  fclose(f);
  if (sz)
    *sz = s;
  return ret;
}

_Thread_local char thrd_pwd[1024];
_Thread_local char thrd_drv;

void VFsThrdInit() {
  strcpy(thrd_pwd, "/");
  thrd_drv = 'T';
}
void VFsSetDrv(char d) {
  if (!isalpha(d))
    return;
  thrd_drv = toupper(d);
}
int VFsCd(char *to, int make) {
  to = __VFsFileNameAbs(to);
  if (__FExists(to) && __FIsDir(to)) {
    A_FREE(to);
    return 1;
  } else if (make) {
#if defined(_WIN32) || defined(WIN32)
    mkdir(to);
#else
    mkdir(to, 0700);
#endif
    A_FREE(to);
    return 1;
  }
  return 0;
}
static void DelDir(char *p) {
  DIR *d = opendir(p);
  struct dirent *d2;
  char od[2048];
  while (d2 = readdir(d)) {
    if (!strcmp(".", d2->d_name) || !strcmp("..", d2->d_name))
      continue;
    strcpy(od, p);
    strcat(od, "/");
    strcat(od, d2->d_name);
    if (__FIsDir(od)) {
      DelDir(od);
    } else {
      remove(od);
    }
  }
  closedir(d);
  rmdir(p);
}

int64_t VFsDel(char *p) {
  int r;
  p = __VFsFileNameAbs(p);
  if (!__FExists(p)) {
    A_FREE(p);
    return 0;
  }
  if (__FIsDir(p)) {
    DelDir(p);
  } else
    remove(p);
  r = !__FExists(p);
  A_FREE(p);
  return r;
}

static char *mount_points['z' - 'a' + 1];

static char *stpcpy2(char *dst, char const *src) { // mingw doesnt have stpcpy
  size_t sz = strlen(src);
  return memcpy(dst, src, sz + 1) + sz;
}

char *__VFsFileNameAbs(char *name) {
  char ret[0x400], *cur;
  cur = stpcpy2(stpcpy2(ret, mount_points[toupper(thrd_drv) - 'A']), thrd_pwd);
  if (strlen(name ?: ""))
    stpcpy2(stpcpy2(cur, "/"), name);
  return A_STRDUP(ret, NULL);
}

int64_t VFsUnixTime(char *name) {
  char *fn = __VFsFileNameAbs(name);
  struct stat s;
  stat(fn, &s);
  A_FREE(fn);
  return s.st_mtime;
}
#if defined(_WIN32) || defined(WIN32)

int64_t VFsFSize(char *name) {
  char *fn = __VFsFileNameAbs(name), *delim;
  int64_t s64;
  int32_t h32;
  if (!fn)
    return 0;
  if (!__FExists(fn)) {
    A_FREE(fn);
    return 0;
  }
  if (__FIsDir(fn)) {
    WIN32_FIND_DATAA data;
    HANDLE dh;
    char buffer[strlen(fn) + 4];
    strcpy(buffer, fn);
    strcat(buffer, "/*");
    MakePathSane(buffer);
    while (delim = strchr(buffer, '/'))
      *delim = '\\';
    s64 = 0;
    dh  = FindFirstFileA(buffer, &data);
    do
      s64++;
    while (FindNextFileA(dh, &data));
    A_FREE(fn);
    // https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstfilea
    if (dh != INVALID_HANDLE_VALUE)
      FindClose(dh);
    return s64;
  }
  HANDLE fh = CreateFileA(fn, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS, NULL);
  s64       = GetFileSize(fh, &h32);
  s64 |= (int64_t)h32 << 32;
  A_FREE(fn);
  CloseHandle(fh);
  return s64;
}

char **VFsDir(char *name) {
  char *fn = __VFsFileNameAbs(""), **ret = NULL, *delim;
  if (!fn)
    return 0;
  if (!__FExists(fn) || !__FIsDir(fn)) {
    A_FREE(fn);
    return 0;
  }
  int64_t sz = VFsFSize("");
  if (sz) {
  #if defined(WIN32) || defined(_WIN32)
    //+1 for "."
    ret = A_CALLOC((sz + 1 + 1) * 8, NULL);
  #else
    ret = A_CALLOC((sz + 1) * 8, NULL);
  #endif
    WIN32_FIND_DATAA data;
    HANDLE dh;
    char buffer[strlen(fn) + 4];
    strcpy(buffer, fn);
    strcat(buffer, "/*");
    MakePathSane(buffer);
    while (delim = strchr(buffer, '/'))
      *delim = '\\';
    int64_t s64 = 0;
    dh          = FindFirstFileA(buffer, &data);
    while (FindNextFileA(dh, &data)) {
      // CDIR_FILENAME_LEN  is 38(includes nul terminator)
      if (strlen(data.cFileName) <= 37)
        ret[s64++] = A_STRDUP(data.cFileName, NULL);
    }
  #if defined(WIN32) || defined(_WIN32)
    ret[s64++] = A_STRDUP(".", NULL);
  #endif
    A_FREE(fn);
    // https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstfilea
    if (dh != INVALID_HANDLE_VALUE)
      FindClose(dh);
  }
  return ret;
}

#else

char **VFsDir(char *fn) {
  int64_t sz;
  char **ret;
  fn = __VFsFileNameAbs("");
  while (strlen(fn) && fn[strlen(fn) - 1] == '/')
    fn[strlen(fn) - 1] = 0;
  DIR *dir = opendir(fn);
  if (!dir) {
    A_FREE(fn);
    return NULL;
  }
  struct dirent *ent;
  sz = 0;
  while (ent = readdir(dir))
    sz++;
  rewinddir(dir);
  ret = A_MALLOC((sz + 1) * sizeof(char *), NULL);
  sz = 0;
  while (ent = readdir(dir)) {
    // CDIR_FILENAME_LEN  is 38(includes nul terminator)
    if (strlen(ent->d_name) <= 37)
      ret[sz++] = A_STRDUP(ent->d_name, NULL);
  }
  ret[sz] = 0;
  A_FREE(fn);
  closedir(dir);
  return ret;
}

int64_t VFsFSize(char *name) {
  struct stat s;
  long cnt;
  DIR *d;
  struct dirent *de;
  char *fn = __VFsFileNameAbs(name);
  if (!__FExists(fn)) {
    A_FREE(fn);
    return -1;
  } else if (__FIsDir(fn)) {
    d = opendir(fn);
    cnt = 0;
    while (de = readdir(d))
      if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
        cnt++;
    closedir(d);
    A_FREE(fn);
    return cnt;
  }
  stat(fn, &s);
  A_FREE(fn);
  return s.st_size;
}
#endif
int64_t VFsFileWrite(char *name, char *data, int64_t len) {
  FILE *f;
  name = __VFsFileNameAbs(name);
  if (name) {
    f = fopen(name, "wb");
    if (f) {
      fwrite(data, 1, len, f);
      fclose(f);
    }
  }
  A_FREE(name);
  return !!name;
}
int64_t VFsIsDir(char *name) {
  int64_t ret;
  name = __VFsFileNameAbs(name);
  if (!name)
    return 0;
  ret = __FIsDir(name);
  A_FREE(name);
  return ret;
}
int64_t VFsFileRead(char *name, int64_t *len) {
  if (len)
    *len = 0;
  FILE *f;
  int64_t s, e;
  void *data = NULL;
  name       = __VFsFileNameAbs(name);
  if (!name)
    goto end;
  if (__FExists(name))
    if (!__FIsDir(name)) {
      f = fopen(name, "rb");
      if (!f)
        goto end;
      s = ftell(f);
      fseek(f, 0, SEEK_END);
      e = ftell(f);
      fseek(f, 0, SEEK_SET);
      fread(data = A_MALLOC(e - s + 1, NULL), 1, e - s, f);
      fclose(f);
      if (len)
        *len = e - s;
      ((char *)data)[e - s] = 0;
    }
end:
  A_FREE(name);
  if (!data)
    data = A_CALLOC(1, NULL);
  return (int64_t)data;
}
int VFsFileExists(char *path) {
  if (!path)
    return 0;
  path  = __VFsFileNameAbs(path);
  int e = __FExists(path);
  A_FREE(path);
  return e;
}

int VFsMountDrive(char let, char *path) {
  int idx = toupper(let) - 'A';
  if (mount_points[idx])
    A_FREE(mount_points[idx]);
  mount_points[idx] = A_MALLOC(strlen(path) + 2, NULL);
  strcpy(mount_points[idx], path);
  strcat(mount_points[idx], "/");
}
FILE *VFsFOpen(char *path, char *m) {
  path    = __VFsFileNameAbs(path);
  FILE *f = fopen(path, m);
  A_FREE(path);
  return f;
}

int64_t VFsFClose(FILE *f) {
  fclose(f);
  return 0;
}
int64_t VFsFBlkRead(void *d, int64_t n, int64_t sz, FILE *f) {
  fflush(f);
  return 0 != fread(d, n, sz, f);
}
int64_t VFsFBlkWrite(void *d, int64_t n, int64_t sz, FILE *f) {
  fflush(f);
  int64_t rc = n * sz != fwrite(d, 1, n * sz, f);
  fflush(f);
  return rc;
}
int64_t VFsFSeek(int64_t off, FILE *f) {
  fflush(f);
  if (off == -1)
    return 0 != fseek(f, 0, SEEK_END);
  return 0 != fseek(f, off, SEEK_SET);
}

int64_t VFsTrunc(char *fn, int64_t sz) {
  fn = __VFsFileNameAbs(fn);
  if (fn) {
    truncate(fn, sz);
    A_FREE(fn);
  }
  return 0;
}
void VFsSetPwd(char *pwd) {
  if (!pwd)
    pwd = "/";
  strcpy(thrd_pwd, pwd);
}

FILE *VFsFOpenW(char *f) {
  char *path = __VFsFileNameAbs(f);
  FILE *r    = fopen(path, "w+b");
  A_FREE(path);
  return r;
}

FILE *VFsFOpenR(char *f) {
  char *path = __VFsFileNameAbs(f);
  FILE *r    = fopen(path, "rb");
  A_FREE(path);
  return r;
}

int64_t VFsDirMk(char *f) {
  return VFsCd(f, 1);
}

// Creates a virtual drive by a template
static void CopyDir(char *dst, char *src) {
#if defined(WIN32) || defined(_WIN32)
  char delim = '\\';
#else
  char delim = '/';
#endif
  if (!__FExists(dst)) {
#if defined(_WIN32) || defined(WIN32)
    mkdir(dst);
#else
    mkdir(dst, 0700);
#endif
  }
  char buf[1024], sbuf[1024], *s, buffer[0x10000];
  int64_t root, sz, sroot, r;
  strcpy(buf, dst);
  buf[root = strlen(buf)] = delim;
  buf[++root]             = 0;

  strcpy(sbuf, src);
  sbuf[sroot = strlen(sbuf)] = delim;
  sbuf[++sroot]              = 0;

  DIR *d = opendir(src);
  struct dirent *ent;
  while (ent = readdir(d)) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;
    buf[root]   = 0;
    sbuf[sroot] = 0;
    strcat(buf, ent->d_name);
    strcat(sbuf, ent->d_name);
    if (__FIsDir(sbuf)) {
      CopyDir(buf, sbuf);
    } else {
      FILE *read = fopen(sbuf, "rb"), *write = fopen(buf, "wb");
      if (!read || !write) {
        if (read)
          fclose(read);
        if (write)
          fclose(write);
        continue;
      }
      while (r = fread(buffer, 1, sizeof(buffer), read)) {
        if (r < 0)
          break;
        fwrite(buffer, 1, r, write);
      }
      fclose(read);
      fclose(write);
    }
  }
  closedir(d);
}

static int __FIsNewer(char *fn, char *fn2) {
#if !(defined(_WIN32) || defined(WIN32))
  struct stat s, s2;
  stat(fn, &s), stat(fn2, &s2);
  int64_t r  = mktime(localtime(&s.st_ctime)),
          r2 = mktime(localtime(&s2.st_ctime));
  if (r > r2)
    return 1;
  else
    return 0;
#else
  int32_t h32;
  int64_t s64, s64_2;
  FILETIME t;
  HANDLE fh = CreateFileA(fn, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL),
         fh2 = CreateFileA(fn2, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  GetFileTime(fh, NULL, NULL, &t);
  s64 = t.dwLowDateTime | ((int64_t)t.dwHighDateTime << 32);
  GetFileTime(fh2, NULL, NULL, &t);
  s64_2 = t.dwLowDateTime | ((int64_t)t.dwHighDateTime << 32);
  CloseHandle(fh), CloseHandle(fh2);
  return s64 > s64_2;
#endif
}
#define DUMB_MESSAGE(FMT,...) \
	{ \
	  int64_t l=snprintf(NULL,0,FMT,__VA_ARGS__); \
	  char buffer3[l]; \
	  snprintf(buffer3,l,FMT,__VA_ARGS__); \
	  if(!IsCmdLineMode()) { \
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,"Aiwnios",buffer3,NULL); \
	  } else { \
		fprintf(AIWNIOS_OSTREAM,"%s",buffer3); \
	  } \
    }
int CreateTemplateBootDrv(char *to, char *template) {
  char buffer[1024], drvl[16], buffer2[1024];
  if (!__FExists(template)) {
	  DUMB_MESSAGE("Template directory %s doesn't exist. You probably didnt install "
            "Aiwnios\n",
            template);
    return 0;
  }
  if (__FExists(to)) {
    int64_t _try;
    for (_try = 0; _try != 0x10000; _try++) {
      sprintf(buffer, "%s_BAKCUP.%ld", to, _try);
      if (!__FExists(buffer)) {
	  DUMB_MESSAGE("Newer Template drive found,backing up old drive to \"%s\".\n",buffer);
// Rename the old boot drive to something else
#if defined(_WIN32) || defined(WIN32)
        MoveFile(to, buffer);
#else
        rename(to, buffer);
#endif
        break;
      }
    }
  }
#if defined(_WIN32) || defined(WIN32)
  strcpy(buffer2, template);
  strcat(buffer2, "\\");
#else
  strcpy(buffer2, template);
#endif
  if (!__FExists(buffer2)) {
    fprintf(AIWNIOS_OSTREAM,
            "Use \"./aiwnios -t T\" to specify the T drive.\n");
    return 0;
  }
  if(!__FExists(to)) {
	  char *next,*old;
	  char delim;
	  strcpy(buffer,to);
	  old=buffer;
#if defined(_WIN32) || defined(WIN32)
	const char fdelim='\\';
#else
	const char fdelim='/';
#endif
	  while(next=strchr(old,fdelim)) {
		  delim=*next;
		  *next=0;
#if defined(_WIN32) || defined(WIN32)
		  mkdir(buffer);
#else
          mkdir(buffer, 0700);
#endif
		  *next=delim;
		  old=next+1;
	  }
	  if(!__FExists(buffer)) {
	  #if defined(_WIN32) || defined(WIN32)
		  mkdir(buffer);
#else
          mkdir(buffer, 0700);
#endif
	 }
    CopyDir(to, buffer2);
    return 1;
  }
  return 0;
}

const char *ResolveBootDir(char *use, int make_new_dir) {
  if (__FExists("HCRT2.BIN")) {
    return ".";
  }
  if (__FExists("T/HCRT2.BIN")) {
    return "T";
  }
  if(__FExists(use)&&!make_new_dir) {
	  return strdup(use);
  }
  //CreateTemplateBootDrv will return existing boot dir if missing
#if !defined(_WIN32) && !defined(WIN32)
  if (!CreateTemplateBootDrv(use, AIWNIOS_TEMPLATE_DIR)) {
#else
  char exe_name[0x10000];
  int64_t len;
  GetModuleFileNameA(NULL, exe_name, sizeof(exe_name));
  PathRemoveFileSpecA(exe_name); // Remove aiwnios.exe
  PathRemoveFileSpecA(exe_name); // Remove /bin
  len = strlen(exe_name);
  sprintf(exe_name + len, "\\T");
  if (!CreateTemplateBootDrv(use, exe_name)) {
#endif
  fail:
    fprintf(AIWNIOS_OSTREAM, "I don't know where the HCRT2.BIN is!!!\n");
    fprintf(AIWNIOS_OSTREAM, "Use \"aiwnios -b\" in the root of the source "
                             "directory to build a boot binary.\n");
    fprintf(AIWNIOS_OSTREAM, "Or Use \"aiwnios -n\" to make a new boot "
                             "drive(if installed on your system).\n");
    exit(-1);
  }
  return use;
}
