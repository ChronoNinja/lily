#ifndef LILY_MSGBUF_H
# define LILY_MSGBUF_H

# include <stdarg.h>
# include <stdint.h>

/* Don't include lily_core_types.h when this is all that's needed from it. */
struct lily_type_t;

/* This is shared by different modules of the interpreter for different reasons.
   Raiser uses it for formatting error messages, and debug uses it for holding
   literals for printing. */
typedef struct {
    /* The message being stored. */
    char *message;
    /* The size that the message currently takes. */
    uint16_t pos;
    /* The buffer space allocated for the message. */
    uint16_t size;
    /* 0 by default, 1 if a function could not expand the buffer. If 1, then
       future functions will safely do nothing. Thus it is not necessary for a
       caller to check a msgbuf before adding to it. */
    uint32_t truncated;
} lily_msgbuf;

void lily_free_msgbuf(lily_msgbuf *);
lily_msgbuf *lily_new_msgbuf(void);
void lily_msgbuf_add(lily_msgbuf *, char *);
void lily_msgbuf_add_char(lily_msgbuf *, char);
void lily_msgbuf_add_text_range(lily_msgbuf *, char *, int, int);
void lily_msgbuf_add_int(lily_msgbuf *, int);
void lily_msgbuf_add_double(lily_msgbuf *, double);
void lily_msgbuf_add_type(lily_msgbuf *, struct lily_type_t *);
void lily_msgbuf_add_fmt(lily_msgbuf *, char *, ...);
void lily_msgbuf_add_fmt_va(lily_msgbuf *, char *, va_list);
void lily_msgbuf_escaped_add_str(lily_msgbuf *, char *);
void lily_msgbuf_flush(lily_msgbuf *);
void lily_msgbuf_reset(lily_msgbuf *);

#endif
