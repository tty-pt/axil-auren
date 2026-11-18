#include "../../src/auren-engine.h"

int auren_engine_next_job_id(void)
{
	static int id = 100;
	return id++;
}

int auren_engine_push_external_result(const struct auren_engine_result *r)
{
	(void)r;
	return 0;
}
