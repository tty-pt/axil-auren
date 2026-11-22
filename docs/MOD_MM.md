# ren `ren-mm`: Pure-C Multi-Project Memory & Codebase Intelligence

`ren-mm.so` is a dynamically loaded Xylem extension for `ren` (installed from `external/libmm`). It connects `ren` to `libmm.so`, giving the LLM agent access to persistent memory lore, multi-project code search, AST-bound symbol inspection, call-graph tracing, git diff blast-radius analysis, and zero-JSON distillation.

---

## 1. Available Tools

`ren-mm` registers two high-density, low-overhead tools with `ren`: `recall` and `record`.

### `recall`
Universal retrieval tool supporting multiple specialized query formats:

| Query Format | Action | Engine API |
| :--- | :--- | :--- |
| `recall <query>` | Hybrid lexical + semantic persistent memory search | `mm_recall()` |
| `recall projects` | Lists all discovered code repositories | `mm_project_list()` |
| `recall [project:<name>] code:<query>` | Hybrid code symbol and signature search | `mm_project_code_search()` |
| `recall [project:<name>] symbol:<name>` | Full source AST read + callers/callees | `mm_project_code_symbol()` |
| `recall [project:<name>] trace:<name>` | Bidirectional multi-hop call graph trace | `mm_project_code_trace()` |
| `recall [project:<name>] diff` | Worktree git diff impact & blast radius | `mm_code_diff_impact()` |
| `recall guardrail:<intent>` | Pre-flight counterfactual safety check | `mm_evaluate_guardrail()` |
| `recall session:<query>` | Cross-session transcript search | `mm_session_recall()` |

- **Multi-Project Resolution**: `project:<name>` is optional. If omitted on code queries, `libmm` searches across all registered repositories and prefixes hits with `[project_name]`.
- **Automatic Mtime Freshness**: Edits in registered projects are detected via `mtime`, automatically triggering incremental re-indexing before serving the query.

### `record`
Memory storage and distillation tool:

1. **Simple Key-Value Format**:
   ```
   record <problem>::<findings>
   ```
2. **Zero-JSON Line-Delimited Distillation Format**:
   ```text
   record PROBLEM: Fix socket leak on client disconnect
   TYPE: episode
   STANDING: false
   PROJECT: axil-auren
   SUBSYSTEM: net
   APPROACH: Close fd on exit | worked | Prevents FD exhaustion
   FINDING: Socket remained open after client reset
   CONSTRAINT: Never exit connection loop without closing client FD
   ```

---

## 2. Usage with `ren`

Load `ren-mm.so` when starting `ren`:

```bash
# Direct CLI run
ren -m ren-mm -p "Search for aec_process in libjarvis"

# Background socket listener (used by Jarvis)
ren --session jarvis \
    --listen /tmp/ren-jarvis-$USER.sock \
    --max-turns 16 \
    -m ren-jarvis:ren-reflex:ren-mm
```
