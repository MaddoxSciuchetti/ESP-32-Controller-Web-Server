#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAME_MAX_OPTIONS 4
#define GAME_MAX_CLIENTS 48
#define GAME_CLIENT_ID_MAX 24
#define GAME_QUESTION_MAX 192
#define GAME_OPTION_MAX 96

typedef enum {
    GAME_PHASE_IDLE,
    GAME_PHASE_OPEN,
    GAME_PHASE_REVEALED,
    GAME_PHASE_DONE,
} game_phase_t;

typedef struct {
    const char *question;
    const char *options[GAME_MAX_OPTIONS];
    int option_count;
    int correct_index;
} game_question_t;

typedef struct {
    game_phase_t phase;
    int session_id;
    int question_index;
    int question_count;
    game_question_t question;
    int votes[GAME_MAX_OPTIONS];
    int total_votes;
    int unique_clients;
} game_snapshot_t;

void game_init(void);
esp_err_t game_start_next(void);
esp_err_t game_reveal(void);
void game_reset(void);
esp_err_t game_vote(const char *client_id, int option_index);
void game_get_snapshot(game_snapshot_t *out);
const char *game_phase_name(game_phase_t phase);

#ifdef __cplusplus
}
#endif
