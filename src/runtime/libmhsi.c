#include "mhseval.h"
#include "mhsi.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

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

struct compiler_status_packet {
    int32_t rc;
    uint32_t err_len;
};

static int
write_full(int fd, const void *buf, size_t len)
{
    const char *ptr = buf;
    while (len > 0) {
        ssize_t written = write(fd, ptr, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        ptr += written;
        len -= (size_t)written;
    }
    return 0;
}

static int
read_full(int fd, void *buf, size_t len)
{
    char *ptr = buf;
    while (len > 0) {
        ssize_t rd = read(fd, ptr, len);
        if (rd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rd == 0) {
            return -1;
        }
        ptr += rd;
        len -= (size_t)rd;
    }
    return 0;
}

static int
write_status_packet(int fd, int32_t rc, const char *msg)
{
    struct compiler_status_packet packet;
    packet.rc = rc;
    if (msg && msg[0] != '\0') {
        size_t len = strlen(msg);
        packet.err_len = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
    } else {
        packet.err_len = 0;
    }
    if (write_full(fd, &packet, sizeof(packet)) != 0) {
        return -1;
    }
    if (packet.err_len > 0 && write_full(fd, msg, packet.err_len) != 0) {
        return -1;
    }
    return 0;
}

static int
read_status_packet(int fd, int32_t *rc_out, char **msg_out)
{
    struct compiler_status_packet packet;
    if (read_full(fd, &packet, sizeof(packet)) != 0) {
        return -1;
    }
    if (rc_out) {
        *rc_out = packet.rc;
    }
    if (!msg_out) {
        if (packet.err_len) {
            char discard[256];
            size_t remaining = packet.err_len;
            while (remaining > 0) {
                size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : remaining;
                if (read_full(fd, discard, chunk) != 0) {
                    return -1;
                }
                remaining -= chunk;
            }
        }
        return 0;
    }
    if (packet.err_len == 0) {
        *msg_out = NULL;
        return 0;
    }
    char *msg = malloc(packet.err_len + 1);
    if (!msg) {
        return -1;
    }
    if (read_full(fd, msg, packet.err_len) != 0) {
        free(msg);
        return -1;
    }
    msg[packet.err_len] = '\0';
    *msg_out = msg;
    return 0;
}

static int
collect_child_output(int fd,
                     char **comb_data,
                     size_t *comb_len,
                     char *err_buf,
                     size_t err_buf_len)
{
    FILE *capture = tmpfile();
    if (!capture) {
        set_error(err_buf, err_buf_len, "Failed to create capture stream");
        return -1;
    }

    for (;;) {
        char buf[4096];
        ssize_t rd = read(fd, buf, sizeof(buf));
        if (rd < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(err_buf, err_buf_len, "Failed to read compiler output: %s",
                      strerror(errno));
            fclose(capture);
            return -1;
        }
        if (rd == 0) {
            break;
        }
        if (fwrite(buf, 1, (size_t)rd, capture) != (size_t)rd) {
            set_error(err_buf, err_buf_len, "Failed to buffer compiler output");
            fclose(capture);
            return -1;
        }
    }

    if (fflush(capture) != 0 || fseek(capture, 0, SEEK_END) != 0) {
        set_error(err_buf, err_buf_len, "Failed to read compiler output");
        fclose(capture);
        return -1;
    }
    long len = ftell(capture);
    if (len < 0) {
        set_error(err_buf, err_buf_len, "Failed to size compiler output");
        fclose(capture);
        return -1;
    }
    if (fseek(capture, 0, SEEK_SET) != 0) {
        set_error(err_buf, err_buf_len, "Failed to rewind compiler output");
        fclose(capture);
        return -1;
    }

    char *data = NULL;
    if (len > 0) {
        data = malloc((size_t)len);
        if (!data) {
            set_error(err_buf, err_buf_len, "Failed to allocate output buffer");
            fclose(capture);
            return -1;
        }
        size_t read_len = fread(data, 1, (size_t)len, capture);
        if (read_len != (size_t)len) {
            free(data);
            set_error(err_buf, err_buf_len, "Failed to read compiler output");
            fclose(capture);
            return -1;
        }
    }
    fclose(capture);

    if (comb_data) {
        *comb_data = data;
    } else if (data) {
        free(data);
    }
    if (comb_len) {
        *comb_len = (size_t)len;
    }
    return 0;
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
    int data_pipe[2] = { -1, -1 };
    int status_pipe[2] = { -1, -1 };
    if (pipe(data_pipe) != 0 || pipe(status_pipe) != 0) {
        set_error(err_buf, err_buf_len, "Failed to create compiler pipes: %s",
                  strerror(errno));
        if (data_pipe[0] != -1) {
            close(data_pipe[0]);
            close(data_pipe[1]);
        }
        if (status_pipe[0] != -1) {
            close(status_pipe[0]);
            close(status_pipe[1]);
        }
        return -1;
    }

    pid_t child = fork();
    if (child < 0) {
        set_error(err_buf, err_buf_len, "Failed to fork compiler: %s",
                  strerror(errno));
        close(data_pipe[0]);
        close(data_pipe[1]);
        close(status_pipe[0]);
        close(status_pipe[1]);
        return -1;
    }

    if (child == 0) {
        close(data_pipe[0]);
        close(status_pipe[0]);

        char child_err[1024];
        child_err[0] = '\0';

        MhsContextPtr ctx = mhs_init_context();
        if (!ctx) {
            set_error(child_err, sizeof(child_err),
                      "Failed to initialize MicroHs runtime");
            write_status_packet(status_pipe[1], -1, child_err);
            _exit(1);
        }

        char *src_copy = malloc(source_len + 1);
        if (!src_copy) {
            set_error(child_err, sizeof(child_err),
                      "Failed to allocate source buffer");
            mhs_free_context(ctx);
            write_status_packet(status_pipe[1], -1, child_err);
            _exit(1);
        }
        memcpy(src_copy, source, source_len);
        src_copy[source_len] = '\0';

        const char *argv_local[] = {
            "mhs",
            "-O",
            "-e",
            src_copy
        };

        if (mhs_context_set_args(ctx, argv_local,
                                 sizeof(argv_local) / sizeof(argv_local[0])) != 0) {
            set_error(child_err, sizeof(child_err),
                      "Unable to configure compiler arguments");
            free(src_copy);
            mhs_free_context(ctx);
            write_status_packet(status_pipe[1], -1, child_err);
            _exit(1);
        }

        const struct ffi_entry *prev_imports = xffi_table;
        struct ffe_entry *prev_exports = xffe_table;
        xffi_table = mhsi_compiler_imp_table;
        xffe_table = mhsi_compiler_exp_table;

        if (dup2(data_pipe[1], fileno(stdout)) < 0) {
            set_error(child_err, sizeof(child_err),
                      "Failed to redirect compiler output: %s",
                      strerror(errno));
            xffi_table = prev_imports;
            xffe_table = prev_exports;
            free(src_copy);
            mhs_free_context(ctx);
            write_status_packet(status_pipe[1], -1, child_err);
            _exit(1);
        }
        close(data_pipe[1]);
        fflush(stdout);

        int rc = mhs_run_zstring(ctx, (const char *)mhsi_compiler_combexpr,
                                 mhsi_compiler_combexprlen);
        fflush(stdout);

        xffi_table = prev_imports;
        xffe_table = prev_exports;

        if (rc != 0) {
            const char *msg = mhs_get_error(ctx);
            if (!msg || !*msg) {
                msg = "Embedded compiler failed";
            }
            set_error(child_err, sizeof(child_err), "%s", msg);
            free(src_copy);
            mhs_free_context(ctx);
            write_status_packet(status_pipe[1], -1, child_err);
            _exit(1);
        }

        free(src_copy);
        mhs_free_context(ctx);
        write_status_packet(status_pipe[1], 0, NULL);
        _exit(0);
    }

    close(data_pipe[1]);
    close(status_pipe[1]);

    char *compiled = NULL;
    size_t compiled_len = 0;
    int output_rc = collect_child_output(data_pipe[0], &compiled, &compiled_len,
                                         err_buf, err_buf_len);
    close(data_pipe[0]);

    int32_t status_code = -1;
    char *child_msg = NULL;
    int status_rc = read_status_packet(status_pipe[0], &status_code, &child_msg);
    if (status_rc != 0 && err_buf && err_buf_len > 0) {
        set_error(err_buf, err_buf_len,
                  "Failed to read compiler status: %s", strerror(errno));
    }
    close(status_pipe[0]);

    int wait_status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &wait_status, 0);
    } while (waited == -1 && errno == EINTR);
    if (waited == -1) {
        set_error(err_buf, err_buf_len, "Failed to reap compiler: %s",
                  strerror(errno));
        free(compiled);
        free(child_msg);
        return -1;
    }

    if (output_rc != 0) {
        free(compiled);
        free(child_msg);
        return -1;
    }
    if (status_rc != 0) {
        free(compiled);
        free(child_msg);
        return -1;
    }

    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0 || status_code != 0) {
        const char *msg = child_msg && *child_msg ? child_msg :
                          "Embedded compiler failed";
        set_error(err_buf, err_buf_len, "%s", msg);
        free(compiled);
        free(child_msg);
        return -1;
    }

    free(child_msg);
    if (comb_data) {
        *comb_data = compiled;
    } else {
        free(compiled);
    }
    if (comb_len) {
        *comb_len = compiled_len;
    }
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
