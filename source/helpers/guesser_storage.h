/**
 * Furi Storage bridge: turns a File* into a Tq_Reader, and persists settings,
 * stats and the learned-word overlay. This is the only file in helpers/ that
 * is allowed to include Furi headers.
 */
#pragma once

#include <furi.h>
#include <storage/storage.h>

#include "guesser_engine.h"
#include "guesser_learned.h"

#define GUESSER_SETTINGS_MAGIC   0x51533032UL /* "20SQ" */
#define GUESSER_STATS_MAGIC      0x54533032UL /* "20ST" */
#define GUESSER_SETTINGS_VERSION 1
#define GUESSER_STATS_VERSION    1

typedef struct {
    Storage* storage;
    File* file;
    Tq_Reader reader;
    bool open;
} GuesserStorage;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t question_limit; /* 15, 20 or 25 */
    uint8_t ask_category; /* 1 = show the category screen */
    uint8_t sound;
    uint8_t vibro;
    uint8_t default_answer_yes; /* 1 = start on Yes, 0 = start on "?" */
    uint8_t reserved;
} GuesserSettings;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t games;
    uint16_t wins;
    uint16_t best_game; /* fewest questions to a correct first guess */
    uint32_t questions_to_win; /* running total, for the average */
    uint16_t win_games; /* games counted in questions_to_win */
    uint16_t reserved;
} GuesserStats;

/** Opens APP_ASSETS_PATH("guesser.dat"). Always call close, even on failure. */
Tq_Error guesser_storage_open_dataset(GuesserStorage* st, Storage* storage, Tq_Dataset* ds);
void guesser_storage_close_dataset(GuesserStorage* st, Tq_Dataset* ds);

void guesser_settings_load(Storage* storage, GuesserSettings* out);
void guesser_settings_save(Storage* storage, const GuesserSettings* settings);
void guesser_stats_load(Storage* storage, GuesserStats* out);
void guesser_stats_save(Storage* storage, const GuesserStats* stats);

/** Loads learned.dat; renames it to .stale if it belongs to another dataset. */
void guesser_learned_load_file(Storage* storage, Tq_Learned* learned, uint32_t base_crc);
bool guesser_learned_save_file(Storage* storage, const Tq_Learned* learned);
bool guesser_learned_delete_file(Storage* storage);
