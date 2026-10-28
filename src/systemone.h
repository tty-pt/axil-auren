#ifndef SYSTEMONE_H
#define SYSTEMONE_H

/*
 * DEPRECATED: System One decision engine is deprecated in favor of fast,
 * deterministic embedding-based classification (POST /v1/classify).
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((deprecated("System One decision endpoint is deprecated; use POST /v1/classify instead")))
#endif
int systemone_init(void);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((deprecated("System One decision endpoint is deprecated; use POST /v1/classify instead")))
#endif
void systemone_shutdown(void);

#endif
