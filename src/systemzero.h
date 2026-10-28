/* systemzero.h — High-efficiency binary decision endpoint for axil-auren. */

#ifndef SYSTEMZERO_H
#define SYSTEMZERO_H

/*
 * DEPRECATED: System Zero decision engine is deprecated in favor of fast,
 * deterministic embedding-based classification (POST /v1/classify).
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((deprecated("System Zero decision endpoint is deprecated; use POST /v1/classify instead")))
#endif
int systemzero_init(void);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((deprecated("System Zero decision endpoint is deprecated; use POST /v1/classify instead")))
#endif
void systemzero_shutdown(void);

#endif /* SYSTEMZERO_H */
