#ifndef STELE_H
#define STELE_H

enum {
	STL_LOG_ERR,
	STL_LOG_WARNING,
	STL_LOG_NOTICE,
	STL_LOG_INFO,
	STL_LOG_DEBUG
};

static inline void stl_openlog(const char *name) { (void)name; }
static inline void stl_syslog(int level, const char *fmt, ...) { (void)level; (void)fmt; }

#endif
