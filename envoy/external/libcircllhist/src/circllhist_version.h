#ifndef CIRCLLHIST_VERSION_H
#ifndef CIRCLLHIST_BRANCH
#define CIRCLLHIST_BRANCH "branches/master"
#endif
#ifndef CIRCLLHIST_VERSION
#define CIRCLLHIST_VERSION "80fdcce6dbe2be1e50cfea3741670a0a6c766988.1546244678"
#endif

#include <stdio.h>

static inline int noit_build_version(char *buff, int len) {
  const char *start = CIRCLLHIST_BRANCH;
  if(!strncmp(start, "branches/", 9)) 
    return snprintf(buff, len, "%s.%s", start+9, CIRCLLHIST_VERSION);
  if(!strncmp(start, "tags/", 5)) 
    return snprintf(buff, len, "%s.%s", start+5, CIRCLLHIST_VERSION);
  return snprintf(buff, len, "%s.%s", CIRCLLHIST_BRANCH, CIRCLLHIST_VERSION);
}

#endif
