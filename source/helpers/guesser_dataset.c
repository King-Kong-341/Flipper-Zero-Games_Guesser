#include "guesser_dataset.h"

#include <stdlib.h>
#include <string.h>

#define TQ_IO_CHUNK 512

static uint32_t tq_rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t tq_rd_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static float tq_rd_f32(const uint8_t* p) {
    /* The file is little-endian IEEE-754 and so is the M4F; memcpy avoids
     * both strict-aliasing UB and unaligned-load surprises. */
    uint32_t bits = tq_rd_u32(p);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

/* Nibble-wise CRC-32 (IEEE). 16-entry table -- 64 bytes of .rodata instead of
 * 1 KB, and no transcendentals. ~2x slower than a byte table, irrelevant here. */
static const uint32_t tq_crc_tab[16] = {
    0x00000000,
    0x1DB71064,
    0x3B6E20C8,
    0x26D930AC,
    0x76DC4190,
    0x6B6B51F4,
    0x4DB26158,
    0x5005713C,
    0xEDB88320,
    0xF00F9344,
    0xD6D6A3E8,
    0xCB61B38C,
    0x9B64C2B0,
    0x86D3D2D4,
    0xA00AE278,
    0xBDBDF21C,
};

static uint32_t tq_crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = tq_crc_tab[crc & 0x0Fu] ^ (crc >> 4);
        crc = tq_crc_tab[crc & 0x0Fu] ^ (crc >> 4);
    }
    return crc;
}

const char* tq_error_str(Tq_Error err) {
    switch(err) {
    case TqOk:
        return "ok";
    case TqErrIo:
        return "read failed";
    case TqErrMagic:
        return "not a 20q data file";
    case TqErrVersion:
        return "unsupported version";
    case TqErrRange:
        return "unsupported data file";
    case TqErrCrc:
        return "data file corrupt";
    default:
        return "unknown";
    }
}

static bool tq_read(const Tq_Dataset* ds, uint32_t off, void* buf, size_t len) {
    if(len == 0) return true;
    if(ds->reader.size && (uint64_t)off + len > (uint64_t)ds->reader.size) return false;
    return ds->reader.read_at(ds->reader.ctx, off, buf, len);
}

bool tq_dataset_verify(Tq_Dataset* ds) {
    if(ds->reader.size <= TQ_HEADER_SIZE) return false;
    uint8_t chunk[TQ_IO_CHUNK];
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t off = TQ_HEADER_SIZE;
    while(off < ds->reader.size) {
        uint32_t want = ds->reader.size - off;
        if(want > TQ_IO_CHUNK) want = TQ_IO_CHUNK;
        if(!tq_read(ds, off, chunk, want)) return false;
        crc = tq_crc32_update(crc, chunk, want);
        off += want;
    }
    return (crc ^ 0xFFFFFFFFUL) == ds->crc32;
}

/* u16[count] block -> heap array, read in chunks. */
static uint16_t* tq_load_u16_block(const Tq_Dataset* ds, uint32_t off, uint32_t count) {
    uint16_t* out = malloc(sizeof(uint16_t) * count);
    uint8_t chunk[TQ_IO_CHUNK];
    uint32_t done = 0;
    while(done < count) {
        uint32_t want = count - done;
        if(want > TQ_IO_CHUNK / 2) want = TQ_IO_CHUNK / 2;
        if(!tq_read(ds, off + done * 2, chunk, want * 2)) {
            free(out);
            return NULL;
        }
        for(uint32_t k = 0; k < want; k++)
            out[done + k] = tq_rd_u16(chunk + k * 2);
        done += want;
    }
    return out;
}

static uint8_t*
    tq_load_u8_block(const Tq_Dataset* ds, uint32_t off, uint32_t count, uint8_t fill) {
    uint8_t* out = malloc(count);
    if(off == 0) {
        memset(out, fill, count);
        return out;
    }
    if(!tq_read(ds, off, out, count)) {
        free(out);
        return NULL;
    }
    return out;
}

Tq_Error tq_dataset_open(Tq_Dataset* ds, Tq_Reader reader, bool verify_crc) {
    memset(ds, 0, sizeof(*ds));
    ds->reader = reader;

    uint8_t h[TQ_HEADER_SIZE];
    if(!tq_read(ds, 0, h, sizeof(h))) return TqErrIo;
    if(tq_rd_u32(h + 0x00) != TQ_MAGIC) return TqErrMagic;
    uint16_t version = tq_rd_u16(h + 0x04);
    if(version < 1 || version > TQ_FORMAT_VERSION) return TqErrVersion;

    ds->flags = tq_rd_u16(h + 0x06);
    ds->entity_count = tq_rd_u32(h + 0x08);
    ds->question_count = tq_rd_u32(h + 0x0C);
    ds->matrix_off = tq_rd_u32(h + 0x10);
    ds->ent_index_off = tq_rd_u32(h + 0x14);
    ds->ent_text_off = tq_rd_u32(h + 0x18);
    ds->qst_index_off = tq_rd_u32(h + 0x1C);
    ds->qst_text_off = tq_rd_u32(h + 0x20);
    ds->prior_off = tq_rd_u32(h + 0x24);
    ds->category_off = tq_rd_u32(h + 0x28);
    ds->qrank_off = tq_rd_u32(h + 0x2C);
    for(int i = 0; i < TQ_LUT_SIZE; i++)
        ds->lut[i] = tq_rd_f32(h + 0x30 + i * 4);
    ds->crc32 = tq_rd_u32(h + 0x70);
    ds->build_epoch = tq_rd_u32(h + 0x74);
    memcpy(ds->dataset_id, h + 0x78, 32);
    ds->dataset_id[32] = '\0';
    ds->family_off = tq_rd_u32(h + 0x98);
    if(version >= 2) {
        if(ds->flags & TQ_FLAG_HAS_ROWS) {
            ds->row_off = tq_rd_u32(h + 0x9C);
            ds->row_stride = tq_rd_u32(h + 0xA0);
        }
        if(ds->flags & TQ_FLAG_HAS_CATMEAN) ds->catmean_off = tq_rd_u32(h + 0xA4);
        if(ds->flags & TQ_FLAG_HAS_DOMAINS) {
            ds->domain_off = tq_rd_u32(h + 0xA8);
            ds->qdomain_off = tq_rd_u32(h + 0xAC);
        }
    }

    /* SPEC 5.6 guard rails -- a clean error, never a crash. */
    if(ds->entity_count == 0 || ds->entity_count > TQ_MAX_ENTITIES) return TqErrRange;
    if(ds->question_count == 0 || ds->question_count > TQ_MAX_QUESTIONS) return TqErrRange;
    ds->stride = (ds->entity_count + 1u) / 2u;
    if((uint64_t)ds->stride * ds->question_count > 0x7FFFFFFFULL) return TqErrRange;
    if(ds->matrix_off < TQ_HEADER_SIZE || ds->ent_index_off < TQ_HEADER_SIZE ||
       ds->qst_index_off < TQ_HEADER_SIZE)
        return TqErrRange;
    if(reader.size &&
       (uint64_t)ds->matrix_off + (uint64_t)ds->stride * ds->question_count > reader.size)
        return TqErrRange;
    for(int i = 0; i < TQ_LUT_SIZE; i++) {
        if(!(ds->lut[i] >= 0.0f && ds->lut[i] <= 1.0f)) return TqErrRange;
    }
    if(ds->row_off) {
        if(ds->row_stride != (ds->question_count + 1u) / 2u || ds->row_off < TQ_HEADER_SIZE)
            return TqErrRange;
        if(reader.size &&
           (uint64_t)ds->row_off + (uint64_t)ds->row_stride * ds->entity_count > reader.size)
            return TqErrRange;
    }
    if(ds->catmean_off && reader.size &&
       (uint64_t)ds->catmean_off + 4u * ds->question_count > reader.size)
        return TqErrRange;
    if(ds->domain_off && reader.size &&
       ((uint64_t)ds->domain_off + ds->entity_count > reader.size ||
        (uint64_t)ds->qdomain_off + 2u * ds->question_count > reader.size))
        return TqErrRange;

    if(verify_crc && !tq_dataset_verify(ds)) return TqErrCrc;

    uint32_t q = ds->question_count;
    if(ds->qrank_off) {
        ds->qrank = tq_load_u16_block(ds, ds->qrank_off, q);
        if(!ds->qrank) return TqErrIo;
        for(uint32_t k = 0; k < q; k++) {
            if(ds->qrank[k] >= q) { /* corrupt ranking: fall back to identity */
                for(uint32_t m = 0; m < q; m++)
                    ds->qrank[m] = (uint16_t)m;
                break;
            }
        }
    } else {
        ds->qrank = malloc(sizeof(uint16_t) * q);
        for(uint32_t k = 0; k < q; k++)
            ds->qrank[k] = (uint16_t)k;
    }
    ds->qrank_pos = malloc(sizeof(uint16_t) * q);
    for(uint32_t k = 0; k < q; k++)
        ds->qrank_pos[k] = (uint16_t)k; /* safe default */
    for(uint32_t k = 0; k < q; k++)
        ds->qrank_pos[ds->qrank[k]] = (uint16_t)k;

    ds->family =
        tq_load_u8_block(ds, (ds->flags & TQ_FLAG_HAS_FAMILIES) ? ds->family_off : 0, q, 0);
    if(!ds->family) {
        tq_dataset_close(ds);
        return TqErrIo;
    }
    /* Categories, packed 4 per byte: 1 KB per 4096 entities instead of 4 KB. */
    uint32_t n = ds->entity_count;
    ds->category = malloc((n + 3u) / 4u);
    memset(ds->category, 0xFF, (n + 3u) / 4u); /* 3 = "other" when absent */
    if((ds->flags & TQ_FLAG_HAS_CATEGORIES) && ds->category_off) {
        uint8_t chunk[TQ_IO_CHUNK];
        for(uint32_t done = 0; done < n;) {
            uint32_t want = n - done;
            if(want > TQ_IO_CHUNK) want = TQ_IO_CHUNK;
            if(!tq_read(ds, ds->category_off + done, chunk, want)) {
                tq_dataset_close(ds);
                return TqErrIo;
            }
            for(uint32_t k = 0; k < want; k++) {
                uint32_t i = done + k;
                uint8_t cat = chunk[k] < TQ_CATEGORY_COUNT ? chunk[k] : TQ_CATEGORY_COUNT - 1;
                uint8_t shift = (uint8_t)((i & 3u) * 2u);
                ds->category[i >> 2] =
                    (uint8_t)((ds->category[i >> 2] & ~(3u << shift)) | (cat << shift));
            }
            done += want;
        }
    }

    /* Topic domains, packed 2 per byte; the question masks as they are. */
    if(ds->domain_off && ds->qdomain_off) {
        ds->domain = malloc((n + 1u) / 2u);
        memset(ds->domain, 0, (n + 1u) / 2u);
        uint8_t chunk[TQ_IO_CHUNK];
        ds->qdomain = tq_load_u16_block(ds, ds->qdomain_off, q);
        bool ok = ds->qdomain != NULL;
        for(uint32_t done = 0; ok && done < n;) {
            uint32_t want = n - done;
            if(want > TQ_IO_CHUNK) want = TQ_IO_CHUNK;
            ok = tq_read(ds, ds->domain_off + done, chunk, want);
            for(uint32_t k = 0; ok && k < want; k++) {
                uint32_t i = done + k;
                uint8_t d = chunk[k] < TQ_DOMAIN_COUNT ? chunk[k] : TQ_DOMAIN_ANY;
                ds->domain[i >> 1] |= (uint8_t)(d << ((i & 1u) * 4u));
            }
            done += want;
        }
        if(!ok) {
            tq_dataset_close(ds);
            return TqErrIo;
        }
    }

    /* Category means: read them when the file has them (v2), otherwise
     * compute them once here by streaming every column (v1). */
    ds->catmean = malloc(4u * q);
    if(ds->catmean_off) {
        if(!tq_read(ds, ds->catmean_off, ds->catmean, 4u * q)) {
            tq_dataset_close(ds);
            return TqErrIo;
        }
    } else {
        uint8_t* col = malloc(ds->stride);
        uint32_t counts[TQ_CATEGORY_COUNT] = {0};
        for(uint32_t i = 0; i < ds->entity_count; i++)
            counts[tq_cat(ds, i)]++;
        for(uint32_t j = 0; j < q; j++) {
            float sums[TQ_CATEGORY_COUNT] = {0.0f};
            if(!tq_dataset_read_column(ds, j, col)) {
                free(col);
                tq_dataset_close(ds);
                return TqErrIo;
            }
            for(uint32_t i = 0; i < ds->entity_count; i++)
                sums[tq_cat(ds, i)] += tq_pyes(ds, col, i);
            for(uint8_t c = 0; c < TQ_CATEGORY_COUNT; c++) {
                float m = counts[c] ? sums[c] / (float)counts[c] : 0.5f;
                ds->catmean[j * 4u + c] = (uint8_t)(m * 255.0f + 0.5f);
            }
        }
        free(col);
    }

    return TqOk;
}

void tq_dataset_close(Tq_Dataset* ds) {
    free(ds->qrank);
    free(ds->qrank_pos);
    free(ds->family);
    free(ds->category);
    free(ds->catmean);
    ds->catmean = NULL;
    free(ds->domain);
    ds->domain = NULL;
    free(ds->qdomain);
    ds->qdomain = NULL;
    ds->qrank = NULL;
    ds->qrank_pos = NULL;
    ds->family = NULL;
    ds->category = NULL;
}

bool tq_dataset_read_column(const Tq_Dataset* ds, uint32_t j, uint8_t* colbuf) {
    if(j >= ds->question_count) return false;
    return tq_read(ds, ds->matrix_off + j * ds->stride, colbuf, ds->stride);
}

bool tq_dataset_read_row(const Tq_Dataset* ds, uint32_t i, uint8_t* rowbuf) {
    if(!ds->row_off || i >= ds->entity_count) return false;
    return tq_read(ds, ds->row_off + i * ds->row_stride, rowbuf, ds->row_stride);
}

/* Text blocks are not NUL-terminated: length = index[i+1] - index[i]. */
static bool tq_text_span(
    const Tq_Dataset* ds,
    uint32_t index_off,
    uint32_t text_off,
    uint32_t i,
    uint32_t* start,
    uint32_t* len) {
    uint8_t raw[8];
    if(!tq_read(ds, index_off + i * 4, raw, 8)) return false;
    uint32_t a = tq_rd_u32(raw);
    uint32_t b = tq_rd_u32(raw + 4);
    if(b < a) return false;
    *start = text_off + a;
    *len = b - a;
    return true;
}

static bool tq_text_get(
    const Tq_Dataset* ds,
    uint32_t index_off,
    uint32_t text_off,
    uint32_t i,
    uint32_t limit,
    char* out,
    size_t out_size) {
    if(out_size == 0) return false;
    out[0] = '\0';
    if(i >= limit) return false;
    uint32_t start, len;
    if(!tq_text_span(ds, index_off, text_off, i, &start, &len)) return false;
    if(len >= out_size) len = (uint32_t)out_size - 1;
    if(!tq_read(ds, start, out, len)) return false;
    out[len] = '\0';
    return true;
}

bool tq_dataset_entity_name(const Tq_Dataset* ds, uint32_t i, char* out, size_t out_size) {
    return tq_text_get(
        ds, ds->ent_index_off, ds->ent_text_off, i, ds->entity_count, out, out_size);
}

bool tq_dataset_question_text(const Tq_Dataset* ds, uint32_t j, char* out, size_t out_size) {
    return tq_text_get(
        ds, ds->qst_index_off, ds->qst_text_off, j, ds->question_count, out, out_size);
}

static char tq_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int tq_stricmp(const char* a, const char* b) {
    while(*a && *b) {
        char ca = tq_lower(*a++), cb = tq_lower(*b++);
        if(ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
    }
    return (int)(unsigned char)tq_lower(*a) - (int)(unsigned char)tq_lower(*b);
}

bool tq_dataset_find_entity(const Tq_Dataset* ds, const char* name, uint32_t* out_index) {
    /* pack.py guarantees the name block is sorted (SPEC 8.6). */
    uint32_t lo = 0, hi = ds->entity_count;
    char buf[TQ_MAX_NAME + 1];
    while(lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if(!tq_dataset_entity_name(ds, mid, buf, sizeof(buf))) return false;
        int cmp = tq_stricmp(buf, name);
        if(cmp == 0) {
            if(out_index) *out_index = mid;
            return true;
        }
        if(cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

bool tq_dataset_read_priors(const Tq_Dataset* ds, float* out) {
    uint32_t n = ds->entity_count;
    if(!(ds->flags & TQ_FLAG_HAS_PRIORS) || ds->prior_off == 0) {
        for(uint32_t i = 0; i < n; i++)
            out[i] = 1.0f;
        return true;
    }
    uint8_t chunk[TQ_IO_CHUNK];
    uint32_t done = 0;
    while(done < n) {
        uint32_t want = n - done;
        if(want > TQ_IO_CHUNK / 2) want = TQ_IO_CHUNK / 2;
        if(!tq_read(ds, ds->prior_off + done * 2, chunk, want * 2)) return false;
        for(uint32_t k = 0; k < want; k++) {
            uint16_t v = tq_rd_u16(chunk + k * 2);
            out[done + k] = (v == 0) ? (1.0f / 65535.0f) : ((float)v / 65535.0f);
        }
        done += want;
    }
    return true;
}

float tq_dataset_prior(const Tq_Dataset* ds, uint32_t i) {
    if(i >= ds->entity_count || !(ds->flags & TQ_FLAG_HAS_PRIORS) || !ds->prior_off) return 1.0f;
    uint8_t raw[2];
    if(!tq_read(ds, ds->prior_off + i * 2, raw, 2)) return 1.0f;
    uint16_t v = tq_rd_u16(raw);
    return (v == 0) ? (1.0f / 65535.0f) : ((float)v / 65535.0f);
}

uint8_t tq_dataset_category(const Tq_Dataset* ds, uint32_t i) {
    if(i >= ds->entity_count || !ds->category) return TQ_CATEGORY_COUNT - 1;
    return tq_cat(ds, i);
}

uint8_t tq_dataset_family(const Tq_Dataset* ds, uint32_t j) {
    if(j >= ds->question_count || !ds->family) return 0;
    return ds->family[j];
}
