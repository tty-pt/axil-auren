/**
 * @file qllm.h
 * @brief Backward compatibility forwarding header for libauren (formerly libqllm).
 * @see <ttypt/auren.h>
 */

#ifndef QLLM_H
#define QLLM_H

#include <ttypt/auren.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Backward compatibility type aliases */
typedef struct auren_config  qllm_config_t;
#define qllm_config          auren_config
#define qllm_context         auren_context
#define qllm_message         auren_message
#define qllm_token_cb        auren_token_cb
#define qllm_token_full_cb   auren_token_full_cb

/* Backward compatibility function aliases */
#define qllm_create              auren_create
#define qllm_free                auren_free
#define qllm_generate            auren_generate
#define qllm_generate_stream     auren_generate_stream
#define qllm_generate_stream_full auren_generate_stream_full
#define qllm_embed               auren_embed
#define qllm_n_embd              auren_n_embd
#define qllm_prime               auren_prime
#define qllm_prime_block         auren_prime_block
#define qllm_n_ctx               auren_n_ctx
#define qllm_set_seq             auren_set_seq
#define qllm_set_grammar         auren_set_grammar
#define qllm_sampler_create      auren_sampler_create
#define qllm_sampler_add_grammar auren_sampler_add_grammar
#define qllm_sampler_free        auren_sampler_free
#define qllm_next                auren_next
#define qllm_chat                auren_chat
#define qllm_render              auren_render
#define qllm_reset               auren_reset
#define qllm_anchor_start        auren_anchor_start
#define qllm_anchor_end          auren_anchor_end
#define qllm_compress            auren_compress
#define qllm_set_eos_bias        auren_set_eos_bias
#define qllm_systemzero_eval     auren_systemzero_eval

#ifdef __cplusplus
}
#endif

#endif /* QLLM_H */
