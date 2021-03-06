#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lily_parser.h"

/*  lily_main.c
    This is THE main runner for Lily. */

void lily_impl_puts(void *data, char *text)
{
    fputs(text, stdout);
}

static void usage()
{
    fputs("Usage: lily [option] ...\n"
          "Options:\n"
          "-h        : Print this help and exit.\n"
          "-t        : Code is between <?lily ... ?> tags.\n"
          "            Everything else is printed to stdout.\n"
          "            By default, everything is treated as code.\n"
          "-s string : The program is a string (end of options).\n"
          "file      : The program is the given filename.\n", stderr);
    exit(EXIT_FAILURE);
}

int is_file;
int do_tags = 0;
char *to_process = NULL;

static void process_args(int argc, char **argv)
{
    int i;
    for (i = 1;i < argc;i++) {
        char *arg = argv[i];
        if (strcmp("-h", arg) == 0)
            usage();
        else if (strcmp("-t", arg) == 0)
            do_tags = 1;
        else if (strcmp("-s", arg) == 0) {
            i++;
            if (i == argc)
                usage();

            to_process = argv[i];
            if ((i + 1) != argc)
                usage();

            is_file = 0;
            break;
        }
        else {
            to_process = argv[i];
            if ((i + 1) != argc)
                usage();

            is_file = 1;
            break;
        }
    }
}

void traceback_to_file(lily_parse_state *parser, FILE *outfile)
{
    lily_raiser *raiser = parser->raiser;
    fprintf(outfile, "%s", lily_name_for_error(raiser->error_code));
    if (raiser->msgbuf->message[0] != '\0')
        fprintf(outfile, ": %s", raiser->msgbuf->message);
    else
        fputc('\n', outfile);

    if (parser->mode == pm_parse) {
        lily_lex_entry *iter = parser->lex->entry;

        int fixed_line_num = (raiser->line_adjust == 0 ?
                parser->lex->line_num : raiser->line_adjust);

        /* The parser handles lambda processing by putting entries with the
           name [lambda]. Don't show these. */
        while (strcmp(iter->filename, "[lambda]") == 0)
            iter = iter->prev;

        /* Since importing is not yet possible, simply show the top entry. This
           should be the actual file loaded. */
        iter->saved_line_num = fixed_line_num;
        fprintf(outfile, "Where: File \"%s\" at line %d\n", iter->filename,
                iter->saved_line_num);
    }
    else if (parser->mode == pm_execute) {
        lily_vm_stack_entry **vm_stack;
        lily_vm_stack_entry *entry;
        int i;

        vm_stack = parser->vm->function_stack;
        fprintf(outfile, "Traceback:\n");

        for (i = parser->vm->function_stack_pos-1;i >= 0;i--) {
            entry = vm_stack[i];
            char *class_name = entry->function->class_name;
            char *separator;
            if (class_name == NULL) {
                class_name = "";
                separator = "";
            }
            else
                separator = "::";

            if (entry->function->code == NULL)
                fprintf(outfile, "    Function %s%s%s [builtin]\n",
                        class_name, separator,
                        entry->function->trace_name);
            else
                fprintf(outfile, "    Function %s%s%s at line %d\n",
                        class_name, separator,
                        entry->function->trace_name, entry->line_num);
        }
    }
}

int main(int argc, char **argv)
{
    process_args(argc, argv);

    lily_parse_state *parser = lily_new_parse_state(NULL, argc, argv);
    if (parser == NULL) {
        fputs("NoMemoryError: No memory to alloc interpreter.\n", stderr);
        exit(EXIT_FAILURE);
    }

    lily_lex_mode mode = (do_tags ? lm_tags : lm_no_tags);

    int result;
    if (is_file == 1)
        result = lily_parse_file(parser, mode, to_process);
    else
        result = lily_parse_string(parser, "[cli]", mode, to_process);

    if (result == 0) {
        traceback_to_file(parser, stderr);
        exit(EXIT_FAILURE);
    }

    lily_free_parse_state(parser);
    exit(EXIT_SUCCESS);
}
