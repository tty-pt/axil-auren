#include <node_api.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <ttypt/auren.h>
#include <ttypt/sepal.h>

static bool extract_float_vec(napi_env env, napi_value val, const float **out_ptr, size_t *out_len, float **out_owned) {
	*out_ptr = NULL;
	*out_len = 0;
	*out_owned = NULL;

	bool is_ta = false;
	napi_is_typedarray(env, val, &is_ta);
	if (is_ta) {
		napi_typedarray_type ta_type;
		size_t elem_len = 0;
		void *data = NULL;
		napi_get_typedarray_info(env, val, &ta_type, &elem_len, &data, NULL, NULL);
		if (ta_type == napi_float32_array) {
			*out_ptr = (const float *)data;
			*out_len = elem_len;
			return true;
		}
	}

	uint32_t arr_len = 0;
	if (napi_get_array_length(env, val, &arr_len) != napi_ok)
		return false;
	if (arr_len == 0)
		return true;

	float *buf = (float *)malloc((size_t)arr_len * sizeof(float));
	if (!buf)
		return false;
	for (uint32_t i = 0; i < arr_len; i++) {
		napi_value el;
		napi_get_element(env, val, i, &el);
		double d = 0.0;
		napi_get_value_double(env, el, &d);
		buf[i] = (float)d;
	}
	*out_ptr = buf;
	*out_len = (size_t)arr_len;
	*out_owned = buf;
	return true;
}

/* cosine(vecA, vecB) -> number */
static napi_value AurenCosine(napi_env env, napi_callback_info info) {
	size_t argc = 2;
	napi_value args[2];
	napi_get_cb_info(env, info, &argc, args, NULL, NULL);
	if (argc < 2) {
		napi_throw_type_error(env, NULL, "Expected 2 arguments: (vecA, vecB)");
		return NULL;
	}

	const float *vA = NULL, *vB = NULL;
	size_t lenA = 0, lenB = 0;
	float *ownA = NULL, *ownB = NULL;
	if (!extract_float_vec(env, args[0], &vA, &lenA, &ownA) ||
	    !extract_float_vec(env, args[1], &vB, &lenB, &ownB)) {
		free(ownA);
		free(ownB);
		napi_throw_error(env, NULL, "Invalid float vector arguments");
		return NULL;
	}

	float sim = 0.0f;
	if (lenA > 0 && lenA == lenB)
		sim = sepal_cosine(vA, vB, lenA);

	free(ownA);
	free(ownB);

	napi_value res;
	napi_create_double(env, (double)sim, &res);
	return res;
}

/* hasNativeModule() -> boolean (anchors DT_NEEDED for libaxil-auren) */
static napi_value AurenHasNativeModule(napi_env env, napi_callback_info info) {
	(void)info;
	int dummy = auren_n_embd(NULL);
	napi_value res;
	napi_get_boolean(env, dummy == 0, &res);
	return res;
}

static napi_value Init(napi_env env, napi_value exports) {
	napi_property_descriptor props[] = {
		{ "cosine", NULL, AurenCosine, NULL, NULL, NULL, napi_default, NULL },
		{ "hasNativeModule", NULL, AurenHasNativeModule, NULL, NULL, NULL, napi_default, NULL },
	};
	napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
	return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
