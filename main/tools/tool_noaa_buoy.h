#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Fetch real-time surf/swell data from NOAA National Data Buoy Center.
 * Uses the NDBC public text API — no API key required.
 *
 * Input JSON fields:
 *   station  (string, optional)  — NDBC station ID, e.g. "46012" (Pt. Reyes/SF).
 *                                   Defaults to "46012" if omitted.
 *
 * Output: plain-text with wave height (ft), dominant period (s), swell direction,
 *         wind speed (mph), wind direction, water temp — ready for surf verdict.
 *
 * Default station 46012 = NOAA Buoy Point Reyes, offshore NW of SF/Pacifica.
 * Other useful stations:
 *   46026 = San Francisco Bar (closer inshore)
 *   46013 = Bodega Bay
 *   46214 = Point Arena
 */
esp_err_t tool_noaa_buoy_execute(const char *input_json, char *output, size_t output_size);
