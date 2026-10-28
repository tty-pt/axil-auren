#ifndef AUREN_COLLECTIONS_H
#define AUREN_COLLECTIONS_H

/*
 * Multi-tenant collections subsystem for axil-auren.
 *
 * Provides partitioned vector and document management using libsepal and libcorm:
 *   - POST   /v1/collections/{name}/documents
 *   - GET    /v1/collections/{name}/search?q=...&limit=10
 *   - DELETE /v1/collections/{name}/documents/{id}
 *
 * Strict isolation:
 *   - Collection names validated against ^[a-zA-Z0-9_-]{1,64}$ (no traversal, no slashes/dots).
 *   - Each collection maintains separate stores under var/collections/{name}/.
 *   - Cross-collection search and delete isolation is strictly maintained.
 */

int auren_collections_init(void);
void auren_collections_shutdown(void);

#endif /* AUREN_COLLECTIONS_H */
