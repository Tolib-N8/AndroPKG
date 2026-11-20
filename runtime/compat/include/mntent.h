// andropkg: no mount table on iOS; setmntent() returns NULL (ENOENT).
#pragma once
#include <stdio.h>
struct mntent {
  char* mnt_fsname;
  char* mnt_dir;
  char* mnt_type;
  char* mnt_opts;
  int mnt_freq;
  int mnt_passno;
};
#ifdef __cplusplus
extern "C" {
#endif
FILE* setmntent(const char* filename, const char* type);
struct mntent* getmntent(FILE* stream);
struct mntent* getmntent_r(FILE* stream, struct mntent* mntbuf, char* buf, int buflen);
int endmntent(FILE* stream);
#ifdef __cplusplus
}
#endif
