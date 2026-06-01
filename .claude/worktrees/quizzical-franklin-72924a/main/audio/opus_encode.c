#include "audio/opus_encode.h"
#include "memory/psram_alloc.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "opus.h"

static const char *TAG = "opus_enc";

/* Opus frame size: 960 samples at 48kHz = 20ms.
 * At 16kHz, that's 320 samples per frame (20ms). */
#define OPUS_FRAME_SAMPLES_16K  320
#define OPUS_MAX_PACKET         1500

/* ── Minimal OGG page writer ──────────────────────────────────── */
/* We only need to write valid OGG pages — no random access, no seeking.
 * OGG spec: https://xiph.org/ogg/doc/rfc3533.txt */

/* CRC-32 lookup table for OGG (polynomial 0x04C11DB7) */
static uint32_t ogg_crc32(const uint8_t *data, size_t len)
{
    static const uint32_t crc_table[256] = {
        0x00000000,0x04C11DB7,0x09823B6E,0x0D4326D9,0x130476DC,0x17C56B6B,0x1A864DB2,0x1E475005,
        0x2608EDB8,0x22C9F00F,0x2F8AD6D6,0x2B4BCB61,0x350C9B64,0x31CD86D3,0x3C8EA00A,0x384FBDBD,
        0x4C11DB70,0x48D0C6C7,0x4593E01E,0x4152FDA9,0x5F15ADAC,0x5BD4B01B,0x569796C2,0x52568B75,
        0x6A1936C8,0x6ED82B7F,0x639B0DA6,0x675A1011,0x791D4014,0x7DDC5DA3,0x709F7B7A,0x745E66CD,
        0x9823B6E0,0x9CE2AB57,0x91A18D8E,0x95609039,0x8B27C03C,0x8FE6DD8B,0x82A5FB52,0x8664E6E5,
        0xBE2B5B58,0xBAEA46EF,0xB7A96036,0xB3687D81,0xAD2F2D84,0xA9EE3033,0xA4AD16EA,0xA06C0B5D,
        0xD4326D90,0xD0F37027,0xDDB056FE,0xD9714B49,0xC7361B4C,0xC3F706FB,0xCEB42022,0xCA753D95,
        0xF23A8028,0xF6FB9D9F,0xFBB8BB46,0xFF79A6F1,0xE13EF6F4,0xE5FFEB43,0xE8BCCD9A,0xEC7DD02D,
        0x34867077,0x30476DC0,0x3D044B19,0x39C556AE,0x278206AB,0x23431B1C,0x2E003DC5,0x2AC12072,
        0x128E9DCF,0x164F8078,0x1B0CA6A1,0x1FCDBB16,0x018AEB13,0x054BF6A4,0x0808D07D,0x0CC9CDCA,
        0x7897AB07,0x7C56B6B0,0x71159069,0x75D48DDE,0x6B93DDDB,0x6F52C06C,0x6211E6B5,0x66D0FB02,
        0x5E9F46BF,0x5A5E5B08,0x571D7DD1,0x53DC6066,0x4D9B3063,0x495A2DD4,0x44190B0D,0x40D816BA,
        0xACA5C697,0xA864DB20,0xA527FDF9,0xA1E6E04E,0xBFA1B04B,0xBB60ADFC,0xB6238B25,0xB2E29692,
        0x8AAD2B2F,0x8E6C3698,0x832F1041,0x87EE0DF6,0x99A95DF3,0x9D684044,0x902B669D,0x94EA7B2A,
        0xE0B41DE7,0xE4750050,0xE9362689,0xEDF73B3E,0xF3B06B3B,0xF771768C,0xFA325055,0xFEF34DE2,
        0xC6BCF05F,0xC27DEDE8,0xCF3ECB31,0xCBFFD686,0xD5B88683,0xD1799B34,0xDC3ABDED,0xD8FBA05A,
        0x690CE0EE,0x6DCDFD59,0x608EDB80,0x644FC637,0x7A089632,0x7EC98B85,0x738AAD5C,0x774BB0EB,
        0x4F040D56,0x4BC510E1,0x46863638,0x42472B8F,0x5C007B8A,0x58C1663D,0x558240E4,0x51435D53,
        0x251D3B9E,0x21DC2629,0x2C9F00F0,0x285E1D47,0x36194D42,0x32D850F5,0x3F9B762C,0x3B5A6B9B,
        0x0315D626,0x07D4CB91,0x0A97ED48,0x0E56F0FF,0x1011A0FA,0x14D0BD4D,0x19939B94,0x1D528623,
        0xF12F560E,0xF5EE4BB9,0xF8AD6D60,0xFC6C70D7,0xE22B20D2,0xE6EA3D65,0xEBA91BBC,0xEF68060B,
        0xD727BBB6,0xD3E6A601,0xDEA580D8,0xDA649D6F,0xC423CD6A,0xC0E2D0DD,0xCDA1F604,0xC960EBB3,
        0xBD3E8D7E,0xB9FF90C9,0xB4BCB610,0xB07DABA7,0xAE3AFBA2,0xAAFBE615,0xA7B8C0CC,0xA379DD7B,
        0x9B3660C6,0x9FF77D71,0x92B45BA8,0x9675461F,0x8832161A,0x8CF30BAD,0x81B02D74,0x857130C3,
        0x5D8A9099,0x594B8D2E,0x5408ABF7,0x50C9B640,0x4E8EE645,0x4A4FFBF2,0x470CDD2B,0x43CDC09C,
        0x7B827D21,0x7F436096,0x7200464F,0x76C15BF8,0x68860BFD,0x6C47164A,0x61043093,0x65C52D24,
        0x119B4BE9,0x155A565E,0x18197087,0x1CD86D30,0x029F3D35,0x065E2082,0x0B1D065B,0x0FDC1BEC,
        0x3793A651,0x3352BBE6,0x3E119D3F,0x3AD08088,0x2497D08D,0x2056CD3A,0x2D15EBE3,0x29D4F654,
        0xC5A92679,0xC1683BCE,0xCC2B1D17,0xC8EA00A0,0xD6AD50A5,0xD26C4D12,0xDF2F6BCB,0xDBEE767C,
        0xE3A1CBC1,0xE760D676,0xEA23F0AF,0xEEE2ED18,0xF0A5BD1D,0xF464A0AA,0xF9278673,0xFDE69BC4,
        0x89B8FD09,0x8D79E0BE,0x8038C667,0x84F9DBD0,0x9ABE8BD5,0x9E7F9662,0x933CB0BB,0x97FDAD0C,
        0xAFB210B1,0xAB730D06,0xA6302BDF,0xA2F13668,0xBCB6666D,0xB8777BDA,0xB5345D03,0xB1F540B4,
    };
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ crc_table[((crc >> 24) ^ data[i]) & 0xFF];
    }
    return crc;
}

/* Write an OGG page to output buffer. Returns bytes written. */
static size_t write_ogg_page(uint8_t *dst, size_t dst_cap,
                              uint32_t serial, uint32_t page_seq,
                              uint64_t granule_pos,
                              uint8_t header_type,
                              const uint8_t *payload, size_t payload_len)
{
    /* Calculate segment table */
    int n_segments = (payload_len + 254) / 255;
    if (n_segments == 0) n_segments = 1;
    size_t header_size = 27 + n_segments;
    size_t total = header_size + payload_len;
    if (total > dst_cap) return 0;

    memset(dst, 0, header_size);

    /* Capture pattern */
    dst[0] = 'O'; dst[1] = 'g'; dst[2] = 'g'; dst[3] = 'S';
    /* Version */
    dst[4] = 0;
    /* Header type */
    dst[5] = header_type;
    /* Granule position (64-bit LE) */
    for (int i = 0; i < 8; i++) dst[6 + i] = (granule_pos >> (8 * i)) & 0xFF;
    /* Serial number (32-bit LE) */
    for (int i = 0; i < 4; i++) dst[14 + i] = (serial >> (8 * i)) & 0xFF;
    /* Page sequence (32-bit LE) */
    for (int i = 0; i < 4; i++) dst[18 + i] = (page_seq >> (8 * i)) & 0xFF;
    /* CRC placeholder (filled below) */
    dst[22] = dst[23] = dst[24] = dst[25] = 0;
    /* Number of segments */
    dst[26] = (uint8_t)n_segments;
    /* Segment table */
    size_t remaining = payload_len;
    for (int i = 0; i < n_segments; i++) {
        if (remaining >= 255) {
            dst[27 + i] = 255;
            remaining -= 255;
        } else {
            dst[27 + i] = (uint8_t)remaining;
            remaining = 0;
        }
    }

    /* Copy payload */
    memcpy(dst + header_size, payload, payload_len);

    /* Compute and fill CRC */
    uint32_t crc = ogg_crc32(dst, total);
    dst[22] = (crc >>  0) & 0xFF;
    dst[23] = (crc >>  8) & 0xFF;
    dst[24] = (crc >> 16) & 0xFF;
    dst[25] = (crc >> 24) & 0xFF;

    return total;
}

/* ── Public API ────────────────────────────────────────────────── */

esp_err_t opus_encode_pcm_to_ogg(const int16_t *pcm_data, size_t pcm_bytes,
                                  uint32_t sample_rate,
                                  uint8_t **out_data, size_t *out_size)
{
    if (!pcm_data || pcm_bytes < 2 || !out_data || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t num_samples = pcm_bytes / sizeof(int16_t);
    int frame_size = (sample_rate == 16000) ? OPUS_FRAME_SAMPLES_16K : (int)(sample_rate / 50);

    /* Create Opus encoder */
    int opus_err;
    OpusEncoder *enc = opus_encoder_create((opus_int32)sample_rate, 1,
                                            OPUS_APPLICATION_VOIP, &opus_err);
    if (!enc || opus_err != OPUS_OK) {
        ESP_LOGE(TAG, "Opus encoder create failed: %d", opus_err);
        return ESP_ERR_NO_MEM;
    }

    /* Optimize for speech: low bitrate, complexity 5 (balanced for ESP32) */
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(16000));       /* 16 kbps */
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    /* Allocate output buffer in PSRAM — worst case roughly same as input */
    size_t out_cap = pcm_bytes / 2 + 8192;  /* Opus should compress well; add OGG overhead */
    uint8_t *ogg_buf = ps_malloc(out_cap);
    if (!ogg_buf) {
        opus_encoder_destroy(enc);
        ESP_LOGE(TAG, "OGG buffer alloc failed (%u bytes)", (unsigned)out_cap);
        return ESP_ERR_NO_MEM;
    }

    size_t ogg_pos = 0;
    uint32_t serial = 0x4C414E47;  /* "LANG" */
    uint32_t page_seq = 0;

    /* ── OGG page 0: OpusHead header ── */
    uint8_t opus_head[19] = {
        'O','p','u','s','H','e','a','d',  /* magic */
        1,                                  /* version */
        1,                                  /* channel count */
        0, 0,                              /* pre-skip (LE) — 0 for simplicity */
        0, 0, 0, 0,                        /* input sample rate (LE) */
        0, 0,                              /* output gain (LE) */
        0,                                 /* channel mapping family */
    };
    /* Fill sample rate */
    opus_head[12] = (sample_rate >>  0) & 0xFF;
    opus_head[13] = (sample_rate >>  8) & 0xFF;
    opus_head[14] = (sample_rate >> 16) & 0xFF;
    opus_head[15] = (sample_rate >> 24) & 0xFF;

    size_t n = write_ogg_page(ogg_buf + ogg_pos, out_cap - ogg_pos,
                               serial, page_seq++, 0,
                               0x02,  /* BOS (beginning of stream) */
                               opus_head, sizeof(opus_head));
    if (n == 0) goto fail;
    ogg_pos += n;

    /* ── OGG page 1: OpusTags header ── */
    const char *vendor = "Langoustine";
    uint32_t vendor_len = strlen(vendor);
    size_t tags_size = 8 + 4 + vendor_len + 4;
    uint8_t tags_buf[64];
    memcpy(tags_buf, "OpusTags", 8);
    tags_buf[8]  = (vendor_len >>  0) & 0xFF;
    tags_buf[9]  = (vendor_len >>  8) & 0xFF;
    tags_buf[10] = (vendor_len >> 16) & 0xFF;
    tags_buf[11] = (vendor_len >> 24) & 0xFF;
    memcpy(tags_buf + 12, vendor, vendor_len);
    /* User comment count = 0 */
    tags_buf[12 + vendor_len] = 0;
    tags_buf[13 + vendor_len] = 0;
    tags_buf[14 + vendor_len] = 0;
    tags_buf[15 + vendor_len] = 0;

    n = write_ogg_page(ogg_buf + ogg_pos, out_cap - ogg_pos,
                        serial, page_seq++, 0,
                        0x00,  /* continuation page */
                        tags_buf, tags_size);
    if (n == 0) goto fail;
    ogg_pos += n;

    /* ── Encode PCM frames into OGG audio pages ── */
    {
        uint8_t opus_pkt[OPUS_MAX_PACKET];
        uint64_t granule = 0;
        size_t sample_idx = 0;

        while (sample_idx + frame_size <= num_samples) {
            int pkt_len = opus_encode(enc, pcm_data + sample_idx, frame_size,
                                       opus_pkt, sizeof(opus_pkt));
            if (pkt_len < 0) {
                ESP_LOGE(TAG, "Opus encode error: %d", pkt_len);
                break;
            }

            sample_idx += frame_size;
            granule += frame_size;

            /* Check if this is the last frame */
            uint8_t hdr_type = (sample_idx + frame_size > num_samples) ? 0x04 : 0x00;

            n = write_ogg_page(ogg_buf + ogg_pos, out_cap - ogg_pos,
                                serial, page_seq++, granule,
                                hdr_type,
                                opus_pkt, (size_t)pkt_len);
            if (n == 0) {
                ESP_LOGW(TAG, "OGG buffer full at %u bytes", (unsigned)ogg_pos);
                break;
            }
            ogg_pos += n;
        }

        /* Handle remaining samples (pad with zeros to fill a frame) */
        if (sample_idx < num_samples && sample_idx + frame_size > num_samples) {
            int16_t *padded = calloc(frame_size, sizeof(int16_t));
            if (padded) {
                size_t remaining = num_samples - sample_idx;
                memcpy(padded, pcm_data + sample_idx, remaining * sizeof(int16_t));
                int pkt_len = opus_encode(enc, padded, frame_size,
                                           opus_pkt, sizeof(opus_pkt));
                free(padded);
                if (pkt_len > 0) {
                    granule += frame_size;
                    n = write_ogg_page(ogg_buf + ogg_pos, out_cap - ogg_pos,
                                        serial, page_seq++, granule,
                                        0x04,  /* EOS */
                                        opus_pkt, (size_t)pkt_len);
                    if (n > 0) ogg_pos += n;
                }
            }
        }
    }

    opus_encoder_destroy(enc);

    float ratio = pcm_bytes > 0 ? (float)ogg_pos / pcm_bytes * 100.0f : 0;
    ESP_LOGI(TAG, "Opus encode: %u PCM bytes → %u OGG bytes (%.1f%%)",
             (unsigned)pcm_bytes, (unsigned)ogg_pos, (double)ratio);

    *out_data = ogg_buf;
    *out_size = ogg_pos;
    return ESP_OK;

fail:
    opus_encoder_destroy(enc);
    free(ogg_buf);
    ESP_LOGE(TAG, "OGG page write failed");
    return ESP_ERR_NO_MEM;
}
