#ifndef QSYS_H
#define QSYS_H

#define QLOG_ERR 3
#define QLOG_WARN 4
#define QLOG_INFO 6
#define QLOG_DEBUG 7

static inline void qsys_openlog(const char *name) { (void)name; }
static inline void qsyslog(int level, const char *fmt, ...) { (void)level; (void)fmt; }

#endif
