#ifndef DS4_EVAL_COMMON_H
#define DS4_EVAL_COMMON_H

#include <stdbool.h>
#include <stddef.h>

#define EVAL_MAX_CHOICES 10
#define EVAL_ANSWER_MAX 32
#define EVAL_MAX_CONTEXT 1000000

typedef struct {
    const char *source;
    const char *id;
    const char *domain;
    const char *title;
    const char *question;
    const char *choice[EVAL_MAX_CHOICES];
    const char *answer;
} eval_case;

extern const eval_case eval_cases[];
extern const int eval_case_count;

int eval_case_nchoices(const eval_case *tc);
bool eval_case_is_multiple_choice(const eval_case *tc);
bool eval_case_is_compsec(const eval_case *tc);
const char *eval_system_prompt(void);
char *build_question_prompt(const eval_case *tc);
void find_case_answer(const eval_case *tc, const char *generated,
                      char *dst, size_t dstlen);
bool answer_matches(const eval_case *tc, const char *got);

#endif
