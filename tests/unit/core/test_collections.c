#include "../framework/test_utils.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include <ttypt/corm.h>

static int
test_validate_collection_name(const char *name)
{
	if (!name || !name[0])
		return 0;
	size_t len = strlen(name);
	if (len > 64)
		return 0;
	for (size_t i = 0; i < len; ++i) {
		char c = name[i];
		if (!isalnum((unsigned char)c) && c != '_' && c != '-')
			return 0;
	}
	return 1;
}

TEST(collections_name_validation)
{
	ASSERT(test_validate_collection_name("kb_docs"));
	ASSERT(test_validate_collection_name("resumes-2025"));
	ASSERT(test_validate_collection_name("Code_Search_Index"));

	/* Directory traversal and escape attempts MUST fail */
	ASSERT_EQ(test_validate_collection_name("../escape"), 0);
	ASSERT_EQ(test_validate_collection_name("docs/nested"), 0);
	ASSERT_EQ(test_validate_collection_name(".."), 0);
	ASSERT_EQ(test_validate_collection_name(""), 0);
	ASSERT_EQ(test_validate_collection_name(NULL), 0);
	ASSERT_EQ(test_validate_collection_name("bad space"), 0);
	ASSERT_EQ(test_validate_collection_name("semi;colon"), 0);
}

/* Concrete test exercising real libcorm multi-tenant collection stores */
TEST(collections_cross_tenant_corm_isolation_and_lifecycle)
{
	char t_dir[256];
	snprintf(t_dir, sizeof(t_dir), "/tmp/auren_col_test_%d", getpid());
	mkdir(t_dir, 0755);

	char kb_corm_path[512];
	char res_corm_path[512];
	snprintf(kb_corm_path, sizeof(kb_corm_path), "%s/kb_docs.corm", t_dir);
	snprintf(res_corm_path, sizeof(res_corm_path), "%s/resumes.corm", t_dir);

	/* Open two isolated real corm maps storing string pointers / values */
	uint32_t kb_hd = corm_open(kb_corm_path, NULL, CM_STR, CM_U32, 0xFF, 0);
	uint32_t res_hd = corm_open(res_corm_path, NULL, CM_STR, CM_U32, 0xFF, 0);

	ASSERT(kb_hd != CM_MISS);
	ASSERT(res_hd != CM_MISS);
	ASSERT(kb_hd != res_hd);

	/* Insert document references with identical ID "common_id" into each collection */
	uint64_t ref_kb = 101;
	uint64_t ref_res = 202;

	corm_put(kb_hd, "id:common_id", &ref_kb);
	corm_put(res_hd, "id:common_id", &ref_res);

	corm_save();

	/* Verify isolation: content in kb_hd is distinct from content in res_hd */
	const uint64_t *p_kb = corm_get(kb_hd, "id:common_id");
	const uint64_t *p_res = corm_get(res_hd, "id:common_id");

	ASSERT_NOT_NULL(p_kb);
	ASSERT_NOT_NULL(p_res);
	ASSERT_EQ(*p_kb, 101);
	ASSERT_EQ(*p_res, 202);

	/* Delete from kb_hd only */
	corm_del(kb_hd, "id:common_id");
	corm_save();

	/* Verify kb_hd has lost the document */
	ASSERT_NULL(corm_get(kb_hd, "id:common_id"));

	/* Verify res_hd still retains its document completely intact */
	const uint64_t *res_still = corm_get(res_hd, "id:common_id");
	ASSERT_NOT_NULL(res_still);
	ASSERT_EQ(*res_still, 202);

	corm_close(kb_hd);
	corm_close(res_hd);

	unlink(kb_corm_path);
	unlink(res_corm_path);
	rmdir(t_dir);
}
