#include "guesser_storage.h"

#include <furi_hal.h>

#define TAG "GuesserStorage"

#define DATASET_PATH       APP_ASSETS_PATH("guesser.dat")
#define LEARNED_PATH       APP_DATA_PATH("learned.dat")
#define LEARNED_STALE_PATH APP_DATA_PATH("learned.dat.stale")
#define SETTINGS_PATH      APP_DATA_PATH("settings.bin")
#define STATS_PATH         APP_DATA_PATH("stats.bin")
#define VERIFIED_PATH      APP_DATA_PATH("verified.bin")

/* The v2 data file is a few MB: checking its CRC streams all of it, which
 * takes seconds on the Flipper's SD bus. Do it once per data file and
 * remember the result (file size + CRC from the header). */
typedef struct {
    uint32_t magic;
    uint32_t crc32;
    uint32_t size;
} GuesserVerified;
#define GUESSER_VERIFIED_MAGIC 0x56533032UL /* "20SV" */

#define LEARNED_MAX_BYTES (TQ_LEARN_MAX_RECORDS * 128u)

static bool read_record(Storage* storage, const char* path, void* out, size_t len);
static bool write_record(Storage* storage, const char* path, const void* data, size_t len);

static bool guesser_read_at(void* ctx, uint32_t offset, void* buf, size_t len) {
    File* file = (File*)ctx;
    if(!storage_file_seek(file, offset, true)) return false;
    /* A short read means the card went away mid-game (SPEC 9). */
    return storage_file_read(file, buf, (uint16_t)len) == len;
}

Tq_Error guesser_storage_open_dataset(GuesserStorage* st, Storage* storage, Tq_Dataset* ds) {
    st->storage = storage;
    st->file = storage_file_alloc(storage);
    st->open = false;

    if(!storage_file_open(st->file, DATASET_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "cannot open %s", DATASET_PATH);
        return TqErrIo;
    }
    st->open = true;

    uint64_t size = storage_file_size(st->file);
    if(size < TQ_HEADER_SIZE || size > 0x7FFFFFFFULL) return TqErrRange;

    st->reader.read_at = guesser_read_at;
    st->reader.ctx = st->file;
    st->reader.size = (uint32_t)size;

    uint32_t start = furi_get_tick();
    Tq_Error err = tq_dataset_open(ds, st->reader, false);
    FURI_LOG_I(TAG, "dataset open: %s in %lums", tq_error_str(err), furi_get_tick() - start);
    return err;
}

typedef struct {
    File* file;
    volatile bool* cancel;
    uint32_t reads;
} GuesserVerifyCtx;

static bool guesser_verify_read_at(void* ctx, uint32_t offset, void* buf, size_t len) {
    GuesserVerifyCtx* v = ctx;
    if(*v->cancel) return false;
    /* leave the SD card to the game now and then */
    if((++v->reads & 15u) == 0) furi_delay_tick(1);
    return guesser_read_at(v->file, offset, buf, len);
}

bool guesser_storage_verify_dataset(Storage* storage, const Tq_Dataset* ds, volatile bool* cancel) {
    GuesserVerified seen;
    if(read_record(storage, VERIFIED_PATH, &seen, sizeof(seen)) &&
       seen.magic == GUESSER_VERIFIED_MAGIC && seen.crc32 == ds->crc32 &&
       seen.size == ds->reader.size) {
        return true;
    }

    GuesserVerifyCtx v = {storage_file_alloc(storage), cancel, 0};
    bool ok = true;
    if(storage_file_open(v.file, DATASET_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint32_t start = furi_get_tick();
        /* only reader and crc32 are used: a shallow copy is enough */
        Tq_Dataset probe = *ds;
        probe.reader.read_at = guesser_verify_read_at;
        probe.reader.ctx = &v;
        ok = tq_dataset_verify(&probe);
        if(*cancel) {
            ok = true; /* not finished: check again next time */
        } else if(ok) {
            GuesserVerified done = {GUESSER_VERIFIED_MAGIC, ds->crc32, ds->reader.size};
            write_record(storage, VERIFIED_PATH, &done, sizeof(done));
        }
        FURI_LOG_I(
            TAG,
            "dataset check: %s in %lums",
            *cancel ? "stopped" : (ok ? "ok" : "BAD"),
            furi_get_tick() - start);
    } else {
        ok = false;
    }
    storage_file_close(v.file);
    storage_file_free(v.file);
    return ok;
}

void guesser_storage_close_dataset(GuesserStorage* st, Tq_Dataset* ds) {
    if(ds) tq_dataset_close(ds);
    if(st->file) {
        /* The Storage API wants close() even when open() failed. */
        storage_file_close(st->file);
        storage_file_free(st->file);
        st->file = NULL;
    }
    st->open = false;
}

/* --- small fixed-size records ------------------------------------------ */

static bool read_record(Storage* storage, const char* path, void* out, size_t len) {
    File* f = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        ok = storage_file_read(f, out, (uint16_t)len) == len;
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

static bool write_record(Storage* storage, const char* path, const void* data, size_t len) {
    File* f = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = storage_file_write(f, data, (uint16_t)len) == len;
        if(ok) ok = storage_file_sync(f);
    }
    storage_file_close(f);
    storage_file_free(f);
    if(!ok) FURI_LOG_W(TAG, "write failed: %s", path);
    return ok;
}

/* Guesser 1.0 settings, kept to carry them over */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t question_limit;
    uint8_t ask_category;
    uint8_t sound;
    uint8_t vibro;
    uint8_t default_answer_yes;
    uint8_t reserved;
} GuesserSettingsV1;

void guesser_settings_load(Storage* storage, GuesserSettings* out) {
    /* A corrupt or missing file is silently replaced by defaults (SPEC 9). */
    GuesserSettings def = {
        .magic = GUESSER_SETTINGS_MAGIC,
        .version = GUESSER_SETTINGS_VERSION,
        .question_limit = 20,
        .volume = 7,
        .sound = 1,
        .vibro = 1,
        .default_answer_yes = 1,
        .led = 1,
        .clicks = 1,
    };
    if(read_record(storage, SETTINGS_PATH, out, sizeof(*out)) &&
       out->magic == GUESSER_SETTINGS_MAGIC && out->version == GUESSER_SETTINGS_VERSION &&
       (out->question_limit == 15 || out->question_limit == 20 || out->question_limit == 25) &&
       out->volume <= 10) {
        return;
    }
    *out = def;
    GuesserSettingsV1 old;
    if(read_record(storage, SETTINGS_PATH, &old, sizeof(old)) &&
       old.magic == GUESSER_SETTINGS_MAGIC && old.version == 1) {
        if(old.question_limit == 15 || old.question_limit == 25)
            out->question_limit = old.question_limit;
        out->sound = old.sound ? 1 : 0;
        out->vibro = old.vibro ? 1 : 0;
        out->default_answer_yes = old.default_answer_yes ? 1 : 0;
    }
    guesser_settings_save(storage, out);
}

void guesser_settings_save(Storage* storage, const GuesserSettings* settings) {
    write_record(storage, SETTINGS_PATH, settings, sizeof(*settings));
}

void guesser_stats_load(Storage* storage, GuesserStats* out) {
    if(!read_record(storage, STATS_PATH, out, sizeof(*out)) || out->magic != GUESSER_STATS_MAGIC ||
       out->version != GUESSER_STATS_VERSION) {
        memset(out, 0, sizeof(*out));
        out->magic = GUESSER_STATS_MAGIC;
        out->version = GUESSER_STATS_VERSION;
        out->best_game = 0;
    }
}

void guesser_stats_save(Storage* storage, const GuesserStats* stats) {
    write_record(storage, STATS_PATH, stats, sizeof(*stats));
}

/* --- learned overlay ---------------------------------------------------- */

void guesser_learned_load_file(Storage* storage, Tq_Learned* learned, uint32_t base_crc) {
    tq_learned_init(learned, base_crc);
    File* f = storage_file_alloc(storage);
    bool loaded = false;
    bool exists = false;
    if(storage_file_open(f, LEARNED_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        exists = true;
        uint64_t size = storage_file_size(f);
        if(size >= 12 && size <= LEARNED_MAX_BYTES) {
            uint8_t* buf = malloc((size_t)size);
            if(storage_file_read(f, buf, (uint16_t)size) == size) {
                loaded = tq_learned_load(learned, buf, (size_t)size, base_crc);
            }
            free(buf);
        }
    }
    storage_file_close(f);
    storage_file_free(f);

    if(exists && !loaded) {
        /* Built against a different guesser.dat: question ids no longer mean the
         * same thing, so park it rather than misinterpret it (SPEC 9). */
        FURI_LOG_W(TAG, "learned.dat does not match this dataset, renaming");
        storage_common_remove(storage, LEARNED_STALE_PATH);
        storage_common_rename(storage, LEARNED_PATH, LEARNED_STALE_PATH);
        tq_learned_clear(learned);
        learned->base_crc32 = base_crc;
    }
    FURI_LOG_I(TAG, "learned words: %u", learned->count);
}

bool guesser_learned_save_file(Storage* storage, const Tq_Learned* learned) {
    size_t len = 0;
    uint8_t* blob = tq_learned_serialize(learned, &len);
    bool ok = write_record(storage, LEARNED_PATH, blob, len);
    free(blob);
    return ok;
}

bool guesser_learned_delete_file(Storage* storage) {
    return storage_common_remove(storage, LEARNED_PATH) == FSE_OK;
}
