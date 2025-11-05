#ifndef MHS_H
#define MHS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mhsi_interpreter mhsi_interpreter;

mhsi_interpreter *mhsi_new(void);
void mhsi_free(mhsi_interpreter *interp);

const char *mhsi_last_error(const mhsi_interpreter *interp);

int mhsi_run(mhsi_interpreter *interp,
             const char *source,
             size_t length);

/* Evaluates a pure expression and returns its rendered result string.
 * On success the caller takes ownership of *result and must free it with mhs_free_result(). */
int mhsi_eval(mhsi_interpreter *interp,
              const char *source,
              size_t length,
              char **result,
              size_t *result_len);

/* NOTE: libmhsi is not thread-safe. Callers must serialize access if used
 * from multiple threads.
 */

#ifdef __cplusplus
}
#endif

#endif /* MHS_H */
