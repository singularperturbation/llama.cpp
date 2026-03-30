# Handoff: Pythonic Tool Calling for Llama 3.2 3B

This document summarizes the changes introduced to enable and robustly support Python-style tool calling for Llama 3.2 (specifically the 1B and 3B Instruct models) within `llama.cpp` and `llama-server`.

## Background & Architecture

LLaMA 3.2 Instruct models are optimized for zero-shot tool calling using a Pythonic list format (e.g., `[func(arg='val')]`) rather than the nested JSON structures common in larger models. 

Existing `llama.cpp` infrastructure uses a **Differential Autoparser**. This system renders a template multiple times with dummy tools to "guess" the tool-call markers (prefixes, suffixes, etc.). However, Llama 3.2's format is significantly different from the tag-based or JSON-based formats the analyzer was designed to detect, often leading to "polluted" grammars containing analyzer placeholders (like `AA_ARG_FST_AA`).

### The Solution: Forced Pythonic Mode
Instead of relying on heuristic detection, we introduced a mechanism to **force** the parser into a dedicated Pythonic mode via a new CLI argument. This bypasses the uncertain analyzer state and engages a clean, high-performance PEG (Parsing Expression Grammar) generator.

---

## Key Changes

### 1. CLI & Server Integration
- **Flag:** Added `--chat-template-tool-format` (`auto`, `json`, `pythonic`) to `llama-cli` and `llama-server`.
- **Propagation:** The flag is threaded through `common_params` into the `autoparser::generation_params`, ensuring the server respects this setting during every API request.

### 2. Specialized Pythonic PEG Generator
- **Grammar Isolation:** In `common/chat-auto-parser-generator.cpp`, when `pythonic` is requested, the system now explicitly clears any state leftover from the differential analysis.
- **Robust Grammar:** 
    - **Naked Calls:** Updated the PEG rules in `common/chat-peg-parser.cpp` to support both the standard list format `[func(args)]` and "naked" function calls `func(args)`.
    - **Strict Enforcement:** The grammar is now non-lazy for Pythonic mode. It forces the model into the tool-calling syntax immediately after the assistant header, preventing the model from drifting into natural language or JSON.
    - **Trigger Logic:** Transition markers now trigger on either a `[` or the specific name of any available tool, ensuring the grammar kicks in reliably.

### 3. Jinja Engine Enhancements
- **Built-in `format` Filter:** Implemented a C++ version of the Jinja `format` filter in `common/jinja/value.cpp`. This was a critical missing piece required by the official Llama 3.2 Pythonic templates (e.g., `{{ "%s" | format(val) }}`).
- **Type Safety:** The filter includes safety checks to prevent crashes if there's a type mismatch between the format string and the argument (e.g., formatting an integer as a string).

### 4. PEG Mapper Logic
- The existing `common_chat_peg_mapper` was verified and confirmed to correctly map the Pythonic AST nodes (`TOOL_NAME`, `TOOL_ARG_NAME`, `TOOL_ARG_VALUE`) back into standard OpenAI-compatible JSON tool-call objects. This allows the API to remain JSON-native while the model generates Python.

---

## Testing & Verification

### Unit Tests
Two primary test suites cover these changes:

1.  **`tests/test-chat-pythonic.cpp` (New):**
    *   Verifies that the GBNF grammar generated for Pythonic mode contains the correct markers (`[`, `]`, function names, argument keys).
    *   Verifies end-to-end parsing of Pythonic strings (including double quotes and naked calls) into `common_chat_tool_call` objects.
2.  **`tests/test-jinja.cpp` (Updated):**
    *   Added a suite of tests for the `format` filter covering strings, integers, and floats.
    *   Includes safety tests for "type-punned" formatting (e.g., `%s` used on a numeric value).

**To run these tests:**
```bash
# Build the test binaries
cmake --build build --config Release -j 10

# Execute the specific suites
./build/bin/test-chat-pythonic
./build/bin/test-jinja
```

### End-to-End Integration Test
To verify the feature in the server environment:

1.  **Start the server:**
    ```bash
    ./build/bin/llama-server \
      --jinja \
      -m Llama-3.2-3B-Instruct-Q4_K_M.gguf \
      --chat-template-file path/to/llama3.2_pythonic.jinja \
      --chat-template-tool-format pythonic \
      --port 8080
    ```
2.  **Make a request:** Use the `/v1/chat/completions` endpoint with `tools` provided. The model should return a valid JSON response containing tool calls, having been internally constrained to Pythonic generation.

---

## Assumptions & Limitations
- **`tool_choice` Objects:** The server currently only supports string values for `tool_choice` (`auto`, `required`, `none`). Passing an OpenAI-style object to force a specific function is currently parsed as `auto`.
- **Parallel Calls:** The Pythonic parser supports multiple calls in a single turn (e.g., `[func1(), func2()]`), provided `parallel_tool_calls` is enabled in the request.
