#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Tool: memory_read
 * Returns the current contents of MEMORY.md so the agent can
 * introspect, deduplicate, or reference its long-term memory.
 */
esp_err_t tool_memory_read_execute(const char *input_json, char *output, size_t output_size);
