#include "game.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    char id[GAME_CLIENT_ID_MAX + 1];
    int vote;
} voter_t;

static const game_question_t QUESTIONS[] = {
    {
        .question = "Sutskever, Vinyals, and Le's 2014 seq2seq paper got a large optimization gain by changing the source sequence how?",
        .options = {
            "Reversing source word order before encoding",
            "Replacing LSTMs with convolution only",
            "Training only on one-word outputs",
            "Removing the decoder recurrence",
        },
        .option_count = 4,
        .correct_index = 0,
    },
    {
        .question = "AlexNet's 2012 ImageNet result was most directly enabled by which systems-level choice?",
        .options = {
            "Training a large CNN efficiently on GPUs",
            "Using a symbolic expert system",
            "Avoiding labeled image data",
            "Deploying a transformer encoder",
        },
        .option_count = 4,
        .correct_index = 0,
    },
    {
        .question = "Kaplan-style neural scaling laws say language model loss changes with model size, data, and compute in what broad form?",
        .options = {
            "Smooth power laws over many orders of magnitude",
            "A hard threshold followed by no improvement",
            "A linear function only of parameter count",
            "Mostly random variation across training runs",
        },
        .option_count = 4,
        .correct_index = 0,
    },
    {
        .question = "Leopold Aschenbrenner's Situational Awareness essay centers on which claim about late-2020s AI progress?",
        .options = {
            "Automated AI R&D could sharply accelerate capabilities",
            "Model scaling has already permanently stopped",
            "Open-source models remove all security concerns",
            "Robotics is the only path to advanced AI",
        },
        .option_count = 4,
        .correct_index = 0,
    },
    {
        .question = "In AI alignment, why is scalable oversight considered important for superhuman systems?",
        .options = {
            "Humans may not directly judge every task the model can do",
            "It makes models smaller without retraining",
            "It removes the need for evaluation data",
            "It prevents all distribution shift by default",
        },
        .option_count = 4,
        .correct_index = 0,
    },
    {
        .question = "Why does a Chinchilla-style compute-optimal argument matter for frontier model training?",
        .options = {
            "It changes the optimal balance between parameters and tokens",
            "It proves small models always beat large models",
            "It says data quality is irrelevant",
            "It eliminates the need for pretraining compute",
        },
        .option_count = 4,
        .correct_index = 0,
    },
};

static SemaphoreHandle_t s_mutex;
static game_phase_t s_phase = GAME_PHASE_IDLE;
static int s_session_id;
static int s_question_index = -1;
static int s_votes[GAME_MAX_OPTIONS];
static voter_t s_voters[GAME_MAX_CLIENTS];
static int s_voter_count;

static void lock(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static void clear_votes(void)
{
    memset(s_votes, 0, sizeof(s_votes));
    memset(s_voters, 0, sizeof(s_voters));
    s_voter_count = 0;
}

void game_init(void)
{
    lock();
    s_phase = GAME_PHASE_IDLE;
    s_session_id = 0;
    s_question_index = -1;
    clear_votes();
    unlock();
}

esp_err_t game_start_next(void)
{
    lock();
    const int question_count = (int)(sizeof(QUESTIONS) / sizeof(QUESTIONS[0]));

    if (s_phase == GAME_PHASE_OPEN) {
        unlock();
        return ESP_OK;
    }

    if (s_phase == GAME_PHASE_IDLE || s_phase == GAME_PHASE_DONE) {
        s_question_index = 0;
    } else if (s_question_index + 1 < question_count) {
        s_question_index++;
    } else {
        s_phase = GAME_PHASE_DONE;
        unlock();
        return ESP_OK;
    }

    s_session_id++;
    s_phase = GAME_PHASE_OPEN;
    clear_votes();
    unlock();
    return ESP_OK;
}

esp_err_t game_reveal(void)
{
    lock();
    if (s_phase == GAME_PHASE_OPEN) {
        s_phase = GAME_PHASE_REVEALED;
        unlock();
        return ESP_OK;
    }
    unlock();
    return ESP_ERR_INVALID_STATE;
}

void game_reset(void)
{
    lock();
    s_phase = GAME_PHASE_IDLE;
    s_question_index = -1;
    clear_votes();
    unlock();
}

esp_err_t game_vote(const char *client_id, int option_index)
{
    if (client_id == NULL || client_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    if (s_phase != GAME_PHASE_OPEN || s_question_index < 0) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }

    const game_question_t *q = &QUESTIONS[s_question_index];
    if (option_index < 0 || option_index >= q->option_count) {
        unlock();
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < s_voter_count; i++) {
        if (strcmp(s_voters[i].id, client_id) == 0) {
            int previous = s_voters[i].vote;
            if (previous >= 0 && previous < GAME_MAX_OPTIONS) {
                s_votes[previous]--;
            }
            s_voters[i].vote = option_index;
            s_votes[option_index]++;
            unlock();
            return ESP_OK;
        }
    }

    if (s_voter_count >= GAME_MAX_CLIENTS) {
        unlock();
        return ESP_ERR_NO_MEM;
    }

    strlcpy(s_voters[s_voter_count].id, client_id, sizeof(s_voters[s_voter_count].id));
    s_voters[s_voter_count].vote = option_index;
    s_voter_count++;
    s_votes[option_index]++;
    unlock();
    return ESP_OK;
}

void game_get_snapshot(game_snapshot_t *out)
{
    lock();
    memset(out, 0, sizeof(*out));
    out->phase = s_phase;
    out->session_id = s_session_id;
    out->question_index = s_question_index;
    out->question_count = (int)(sizeof(QUESTIONS) / sizeof(QUESTIONS[0]));
    out->unique_clients = s_voter_count;

    if (s_question_index >= 0 && s_question_index < out->question_count) {
        out->question = QUESTIONS[s_question_index];
    }

    for (int i = 0; i < GAME_MAX_OPTIONS; i++) {
        out->votes[i] = s_votes[i];
        out->total_votes += s_votes[i];
    }
    unlock();
}

const char *game_phase_name(game_phase_t phase)
{
    switch (phase) {
        case GAME_PHASE_IDLE:     return "idle";
        case GAME_PHASE_OPEN:     return "open";
        case GAME_PHASE_REVEALED: return "revealed";
        case GAME_PHASE_DONE:     return "done";
        default:                  return "unknown";
    }
}
