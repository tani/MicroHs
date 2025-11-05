#include "mhseval.h"
#include "mhsi.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#else
#include <unistd.h>
#endif

struct ffi_entry;
struct ffe_entry;

extern const struct ffi_entry *xffi_table;
extern struct ffe_entry *xffe_table;

#define data mhsi_compiler_data
#define combexpr mhsi_compiler_combexpr
#define combexprlen mhsi_compiler_combexprlen
#define xffi_table mhsi_compiler_xffi_table
#define xffe_table mhsi_compiler_xffe_table
#define imp_table mhsi_compiler_imp_table
#define exp_table mhsi_compiler_exp_table
#include "../../generated/mhs.c"
#undef data
#undef combexpr
#undef combexprlen
#undef xffi_table
#undef xffe_table
#undef imp_table
#undef exp_table

struct mhsi_interpreter {
    MhsContextPtr ctx;
    char last_error[1024];
};

static void
set_error(char *err_buf, size_t err_len, const char *fmt, ...)
{
    if (!err_buf || err_len == 0 || !fmt) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vsnprintf(err_buf, err_len, fmt, args);
    va_end(args);
}

static int run_embedded_mhs_child(const char *source,
                                  size_t source_len,
                                  char **comb_data,
                                  size_t *comb_len,
                                  char *err_buf,
                                  size_t err_buf_len) {
    if (comb_data) {
        *comb_data = NULL;
    }
    if (comb_len) {
        *comb_len = 0;
    }
    MhsContextPtr ctx = mhs_init_context();
    if (!ctx) {
        set_error(err_buf, err_buf_len, "Failed to initialize MicroHs runtime");
        return -1;
    }

    char *src_copy = malloc(source_len + 1);
    if (!src_copy) {
        set_error(err_buf, err_buf_len, "Failed to allocate source buffer");
        mhs_free_context(ctx);
        return -1;
    }
    memcpy(src_copy, source, source_len);
    src_copy[source_len] = '\0';

    const char *argv_local[] = {
        "mhs",
        "-O",
        "-e",
        src_copy
    };

    if (mhs_context_set_args(ctx, argv_local, sizeof(argv_local) / sizeof(argv_local[0])) != 0) {
        set_error(err_buf, err_buf_len, "Unable to configure compiler arguments");
        free(src_copy);
        mhs_free_context(ctx);
        return -1;
    }

    const struct ffi_entry *prev_imports = xffi_table;
    struct ffe_entry *prev_exports = xffe_table;
    xffi_table = mhsi_compiler_imp_table;
    xffe_table = mhsi_compiler_exp_table;

    FILE *capture = tmpfile();
    if (!capture) {
        set_error(err_buf, err_buf_len, "Failed to create capture stream");
        free(src_copy);
        mhs_free_context(ctx);
        return -1;
    }

    int stdout_fd = dup(fileno(stdout));
    if (stdout_fd < 0) {
        set_error(err_buf, err_buf_len, "Failed to duplicate stdout");
        fclose(capture);
        free(src_copy);
        mhs_free_context(ctx);
        return -1;
    }

    fflush(stdout);
    if (dup2(fileno(capture), fileno(stdout)) < 0) {
        set_error(err_buf, err_buf_len, "Failed to redirect stdout");
        close(stdout_fd);
        fclose(capture);
        free(src_copy);
        mhs_free_context(ctx);
        return -1;
    }

    int rc = mhs_run_zstring(ctx, (const char *)mhsi_compiler_combexpr, mhsi_compiler_combexprlen);

    fflush(stdout);
    dup2(stdout_fd, fileno(stdout));
    close(stdout_fd);

    if (rc != 0) {
        const char *msg = mhs_get_error(ctx);
        if (!msg || !*msg) {
            msg = "Embedded compiler failed";
        }
        set_error(err_buf, err_buf_len, "%s", msg);
        fclose(capture);
        free(src_copy);
        mhs_free_context(ctx);
        return -1;
    }

    if (fflush(capture) != 0 || fseek(capture, 0, SEEK_END) != 0) {
        set_error(err_buf, err_buf_len, "Failed to read compiler output");
        fclose(capture);
        free(src_copy);
        mhs_free_context(ctx);
        return -1;
    }
    long len = ftell(capture);
    if (len < 0) {
        set_error(err_buf, err_buf_len, "Failed to size compiler output");
        fclose(capture);
        free(src_copy);
        mhs_free_context(ctx);
        return -1;
    }
    if (fseek(capture, 0, SEEK_SET) != 0) {
        set_error(err_buf, err_buf_len, "Failed to rewind compiler output");
        fclose(capture);
        free(src_copy);
        mhs_free_context(ctx);
        return -1;
    }

    char *data = malloc((size_t)len);
    if (!data) {
        set_error(err_buf, err_buf_len, "Failed to allocate output buffer");
        fclose(capture);
        free(src_copy);
        mhs_free_context(ctx);
        return -1;
    }
    size_t read_len = fread(data, 1, (size_t)len, capture);
    fclose(capture);
    if (read_len != (size_t)len) {
        free(data);
        set_error(err_buf, err_buf_len, "Failed to read compiler output");
        free(src_copy);
        mhs_free_context(ctx);
        return -1;
    }

    *comb_data = data;
    if (comb_len) {
        *comb_len = (size_t)len;
    }

    xffi_table = prev_imports;
    xffe_table = prev_exports;
    mhs_free_context(ctx);
    free(src_copy);
    return 0;
}

static int compile_haskell_to_comb(const char *source,
                                   size_t length,
                                   char **comb_data,
                                   size_t *comb_len,
                                   char *err_buf,
                                   size_t err_buf_len) {
    if (err_buf && err_buf_len > 0) {
        err_buf[0] = '\0';
    }
    int rc = run_embedded_mhs_child(source, length, comb_data, comb_len, err_buf, err_buf_len);
    if (rc == 0 && getenv("MHS_DEBUG") && comb_len && *comb_len) {
        fprintf(stderr, "[libmhsi] compiled source to %zu-byte combinator stream\n", *comb_len);
    }
    return rc;
}

int mhsi_run(mhsi_interpreter *interp,
             const char *source,
             size_t length) {
    if (!interp || !source) {
        return -1;
    }
    if (interp->ctx) {
        mhs_free_context(interp->ctx);
        interp->ctx = NULL;
    }
    interp->last_error[0] = '\0';
    char *comb = NULL;
    size_t comb_len = 0;
    int rc = -1;

    if (compile_haskell_to_comb(source, length, &comb, &comb_len,
                                interp->last_error, sizeof(interp->last_error)) == 0) {
        interp->ctx = mhs_init_context();
        if (interp->ctx) {
            rc = mhs_run_string(interp->ctx, comb, comb_len);
        } else {
            snprintf(interp->last_error, sizeof(interp->last_error),
                     "Failed to initialize MicroHs runtime");
        }
    } else if (interp->last_error[0] == '\0') {
        snprintf(interp->last_error, sizeof(interp->last_error),
                 "Failed to compile source snippet");
    }

    if (interp->ctx) {
        mhs_free_context(interp->ctx);
        interp->ctx = NULL;
    }
    free(comb);
    return rc;
}

int mhsi_eval(mhsi_interpreter *interp,
              const char *source,
              size_t length,
              char **result,
              size_t *result_len) {
    if (!interp || !source || !result) {
        return -1;
    }
    *result = NULL;
    size_t tmp_len = 0;
    size_t *len_out = result_len ? result_len : &tmp_len;
    *len_out = 0;

    if (interp->ctx) {
        mhs_free_context(interp->ctx);
        interp->ctx = NULL;
    }
    interp->last_error[0] = '\0';

    char *comb = NULL;
    size_t comb_len = 0;
    int rc = -1;

    if (compile_haskell_to_comb(source, length, &comb, &comb_len,
                                interp->last_error, sizeof(interp->last_error)) == 0) {
        interp->ctx = mhs_init_context();
        if (!interp->ctx) {
            snprintf(interp->last_error, sizeof(interp->last_error),
                     "Failed to initialize MicroHs runtime");
        } else {
            char *eval_result = NULL;
            rc = mhs_eval_string(interp->ctx, comb, comb_len, &eval_result, len_out);
            if (rc == 0) {
                *result = eval_result;
            } else {
                if (eval_result) {
                    mhs_free_result(eval_result);
                }
                const char *msg = mhs_get_error(interp->ctx);
                if (msg && *msg) {
                    snprintf(interp->last_error, sizeof(interp->last_error),
                             "%s", msg);
                } else {
                    snprintf(interp->last_error, sizeof(interp->last_error),
                             "Evaluation failed");
                }
            }
        }
    } else if (interp->last_error[0] == '\0') {
        snprintf(interp->last_error, sizeof(interp->last_error),
                 "Failed to compile source snippet");
    }

    if (interp->ctx) {
        mhs_free_context(interp->ctx);
        interp->ctx = NULL;
    }
    free(comb);
    if (rc != 0) {
        *result = NULL;
        *len_out = 0;
    }
    return rc;
}

mhsi_interpreter *mhsi_new(void) {
    mhsi_interpreter *interp = malloc(sizeof(*interp));
    if (!interp) {
        return NULL;
    }
    interp->ctx = NULL;
    interp->last_error[0] = '\0';
    return interp;
}

void mhsi_free(mhsi_interpreter *interp) {
    if (!interp) {
        return;
    }
    if (interp->ctx) {
        mhs_free_context(interp->ctx);
    }
    free(interp);
}

const char *mhsi_last_error(const mhsi_interpreter *interp) {
    if (!interp) {
        return "Invalid interpreter";
    }
    if (interp->last_error[0] != '\0') {
        return interp->last_error;
    }
    if (!interp->ctx) {
        return "Interpreter not initialized";
    }
    return mhs_get_error(interp->ctx);
}
