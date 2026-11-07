/* SPDX-License-Identifier: MIT */
#ifndef KK_FRAME_CAPTURE_REQUEST_H
#define KK_FRAME_CAPTURE_REQUEST_H

#include <stdbool.h>
#include <sys/stat.h>
#include <time.h>

static inline struct timespec
kk_frame_capture_request_mtime(const struct stat *request)
{
#ifdef __APPLE__
   return request->st_mtimespec;
#else
   return request->st_mtim;
#endif
}

static inline struct timespec
kk_frame_capture_request_ctime(const struct stat *request)
{
#ifdef __APPLE__
   return request->st_ctimespec;
#else
   return request->st_ctim;
#endif
}

static inline bool
kk_frame_capture_request_matches(const struct stat *current,
                                 const struct stat *captured)
{
   const struct timespec a = kk_frame_capture_request_mtime(current);
   const struct timespec b = kk_frame_capture_request_mtime(captured);
   const struct timespec ca = kk_frame_capture_request_ctime(current);
   const struct timespec cb = kk_frame_capture_request_ctime(captured);
   return current->st_dev == captured->st_dev &&
          current->st_ino == captured->st_ino &&
          current->st_size == captured->st_size &&
          a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec &&
          ca.tv_sec == cb.tv_sec && ca.tv_nsec == cb.tv_nsec;
}

#endif
