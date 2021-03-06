#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "lily_parser.h"

/* cliexec_main.c :
   This starts an interpreter with a string given as a command-line argument.
   This is a nice tool to quickly check a piece of code, or to toy with the
   language without editing a file. */

void lily_impl_puts(void *data, char *text)
{
    fputs(text, stdout);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fputs("Usage : lily_cliexec <str>\n", stderr);
        exit(EXIT_FAILURE);
    }

    lily_parse_state *parser = lily_new_parse_state(NULL, argc, argv);
    if (parser == NULL) {
        fputs("NoMemoryError: No memory to alloc interpreter.\n", stderr);
        exit(EXIT_FAILURE);
    }

    if (lily_parse_string(parser, lm_no_tags, argv[1]) == 0) {
        lily_raiser *raiser = parser->raiser;
        fprintf(stderr, "%s", lily_name_for_error(raiser->error_code));
        if (raiser->msgbuf->message[0] != '\0')
            fprintf(stderr, ": %s", raiser->msgbuf->message);
        else
            fputc('\n', stderr);

        if (parser->mode == pm_parse) {
            int line_num;
            if (raiser->line_adjust == 0)
                line_num = parser->lex->line_num;
            else
                line_num = raiser->line_adjust;

            fprintf(stderr, "Where: File \"%s\" at line %d\n",
                    parser->lex->filename, line_num);
        }
        else if (parser->mode == pm_execute) {
            lily_vm_stack_entry **vm_stack;
            lily_vm_stack_entry *entry;
            int i;

            vm_stack = parser->vm->function_stack;
            fprintf(stderr, "Traceback:\n");

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
                    fprintf(stderr, "    Function %s%s%s [builtin]\n",
                            class_name, separator,
                            entry->function->trace_name);
                else
                    fprintf(stderr, "    Function %s%s%s at line %d\n",
                            class_name, separator,
                            entry->function->trace_name, entry->line_num);
            }
        }
        exit(EXIT_FAILURE);
    }

    lily_free_parse_state(parser);
    exit(EXIT_SUCCESS);
}
