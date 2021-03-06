#include <string.h>

#include "lily_impl.h"
#include "lily_parser.h"
#include "lily_parser_tok_table.h"
#include "lily_keyword_table.h"
#include "lily_pkg_sys.h"
#include "lily_value.h"
#include "lily_membuf.h"

/** Parser is responsible for:
    * Creating all other major structures (ast pool, emitter, lexer, etc.)
    * Ensuring that all other major structures are deleted.
    * Holding the startup functions (lily_parse_file and others).

    Notes:
    * Parser uses a type stack to hold types while processing complex
      var information. This is used to keep parser from leaking memory, since
      parser functions often call lily_raise.
    * Parser checks for proper form, but does not verify call argument counts,
      proper types for assignment, etc. AST handles argument counts, and
      emitter checks types.
    * 'Forward token' is extremely important to parser. This means that
      caller functions will call lily_lexer to get the token ready before
      calling other parser functions. This allows parser to do token lookaheads
      without a penalty: Since a calling function has to get the token ready,
      it can check for a certain value and call lily_lexer again if it needs to
      do so.
**/

/* These flags are for collect_var_type. */

/* Expect a name with every class given. Create a var for each class+name pair.
   This is suitable for collecting the args of a function. */
#define CV_MAKE_VARS    0x1

/* This is set if the variable is not inside another variable. This is suitable
   for collecting a function that may have named arguments. */
#define CV_TOPLEVEL     0x2

/* This is for collecting the opening part of a class declaration. */
#define CV_CLASS_INIT   0x4

/* This is for collecting the type of a variant. This is because a variant
   needs to have a result that includes only the generics that were seen within
   the parentheses. */
#define CV_VARIANT_FUNC 0x10

#define NEED_NEXT_TOK(expected) \
lily_lexer(lex); \
if (lex->token != expected) \
    lily_raise(parser->raiser, lily_SyntaxError, "Expected '%s', not %s.\n", \
               tokname(expected), tokname(lex->token));

#define NEED_CURRENT_TOK(expected) \
if (lex->token != expected) \
    lily_raise(parser->raiser, lily_SyntaxError, "Expected '%s', not %s.\n", \
               tokname(expected), tokname(lex->token));

static char *exception_bootstrap =
"class Exception(string message) {\n"
"    string @message = message\n"
"    list[tuple[string, integer]] @traceback = []\n"
"}\n"
"class DivisionByZeroError (string message) < Exception(message) {}\n"
"class IndexError          (string message) < Exception(message) {}\n"
"class BadTypecastError    (string message) < Exception(message) {}\n"
"class NoReturnError       (string message) < Exception(message) {}\n"
"class ValueError          (string message) < Exception(message) {}\n"
"class RecursionError      (string message) < Exception(message) {}\n"
"class KeyError            (string message) < Exception(message) {}\n"
"class FormatError         (string message) < Exception(message) {}\n";

/*****************************************************************************/
/* Parser creation and teardown                                              */
/*****************************************************************************/

lily_parse_state *lily_new_parse_state(void *data, int argc, char **argv)
{
    lily_parse_state *parser = lily_malloc(sizeof(lily_parse_state));
    lily_raiser *raiser = lily_new_raiser();

    if (parser == NULL)
        return NULL;

    /* This ensures that runners always have a valid parser mode when trying to
       figure out how to show an error. */
    parser->mode = pm_init;
    parser->type_stack_pos = 0;
    parser->type_stack_size = 4;
    parser->class_depth = 0;
    parser->next_lambda_id = 0;
    parser->raiser = raiser;
    parser->type_stack = lily_malloc(4 * sizeof(lily_type *));
    parser->ast_pool = lily_new_ast_pool(raiser, 8);
    parser->symtab = lily_new_symtab(raiser);
    parser->emit = lily_new_emit_state(raiser);
    parser->lex = lily_new_lex_state(raiser, data);
    parser->vm = lily_new_vm_state(raiser, data);
    parser->membuf = lily_membuf_new(raiser);

    if (parser->raiser == NULL || parser->type_stack == NULL ||
        parser->lex == NULL || parser->emit == NULL || parser->symtab == NULL ||
        parser->ast_pool == NULL || parser->vm == NULL ||
        parser->membuf == NULL ||
        lily_emit_try_enter_main(parser->emit,
                                 parser->symtab->main_var) == 0) {
        lily_free_parse_state(parser);

        return NULL;
    }

    parser->vm->main = parser->symtab->main_var;
    parser->vm->symtab = parser->symtab;

    parser->symtab->lex_linenum = &parser->lex->line_num;

    parser->ast_pool->lex_linenum = &parser->lex->line_num;

    parser->emit->lex_linenum = &parser->lex->line_num;
    parser->emit->symtab = parser->symtab;
    parser->emit->ast_membuf = parser->ast_pool->ast_membuf;
    parser->emit->parser = parser;

    parser->lex->symtab = parser->symtab;

    /* When declaring a new function, initially give it the same type as
       __main__. This ensures that, should building the proper type fail, the
       symtab will still see the function as a function and destroy the
       contents. */
    parser->default_call_type = parser->vm->main->type;

    /* This creates a new var, so it has to be done after symtab's lex_linenum
       is set. */
    if (lily_pkg_sys_init(parser->symtab, argc, argv) == 0) {
        lily_free_parse_state(parser);
        return NULL;
    }

    return parser;
}

void lily_free_parse_state(lily_parse_state *parser)
{
    if (parser->raiser)
        lily_free_raiser(parser->raiser);

    if (parser->ast_pool)
        lily_free_ast_pool(parser->ast_pool);

    /* Lily saves some global info in vars, and also in literals as well as the
       registers. Vars and literals are linked lists, while the registers are
       one really large block. Splitting things up like this is weird, but it
       allows Lily to create exactly the amount of register info all at once and
       without guessing at it with malloc + reallocs.
       The downside is that the vm and symtab need to be torn down in a rather
       specific order. Start off by blasting the registers, because those came
       after the symtab's literals and vars. */
    if (parser->vm)
        lily_vm_free_registers(parser->vm);

    /* The symtab's literals and vars go next. This includes __main__, builtins,
       and the like. Past this point, nothing is callable. */
    if (parser->symtab)
        lily_free_symtab_lits_and_vars(parser->symtab);

    /* The vm gets freed next. This will invoke the gc and clear out any
       circular references as well as gc entries on stuff. Past this point, no
       values should be alive. */
    if (parser->vm)
        lily_free_vm_state(parser->vm);

    /* Finally, tear down the symtab. This clears out classes and types, so
       it's VERY important that this go later on, because classes and type
       info is so crucial. */
    if (parser->symtab)
        lily_free_symtab(parser->symtab);

    /* Order doesn't matter for the rest of this. */

    if (parser->lex)
        lily_free_lex_state(parser->lex);

    if (parser->emit)
        lily_free_emit_state(parser->emit);

    lily_membuf_free(parser->membuf);
    lily_free(parser->type_stack);
    lily_free(parser);
}

/*****************************************************************************/
/* Shared code                                                               */
/*****************************************************************************/

/*  shorthash_for_name
    Copied from symtab for keyword_by_name. This gives (up to) the first 8
    bytes of the name as an int for doing fast comparisons. */
static uint64_t shorthash_for_name(const char *name)
{
    const char *ch = &name[0];
    int i, shift;
    uint64_t ret;
    for (i = 0, shift = 0, ret = 0;
         *ch != '\0' && i != 8;
         ch++, i++, shift += 8) {
        ret |= ((uint64_t)*ch) << shift;
    }
    return ret;
}

/*  keyword_by_name
    Do a fast lookup through the keyword table to see if the name given is a
    keyword. Returns -1 if not found, or something higher than that if the name
    is a keyword. */
static int keyword_by_name(char *name)
{
    int i;
    uint64_t shorthash = shorthash_for_name(name);

    for (i = 0;i <= KEY_LAST_ID;i++) {
        if (keywords[i].shorthash == shorthash &&
            strcmp(keywords[i].name, name) == 0)
            return i;
        else if (keywords[i].shorthash > shorthash)
            break;
    }

    return -1;
}

/*  count_unresolved_generics
    This is a helper function for lambda resolution. The purpose of this
    function is to help determine if one of the arguments passed to a lambda
    has an incomplete type. */
static int count_unresolved_generics(lily_emit_state *emit)
{
    int count = 0, top = emit->type_stack_pos + emit->current_generic_adjust;
    int i;
    for (i = emit->type_stack_pos;i < top;i++) {
        if (emit->type_stack[i] == NULL)
            count++;
    }

    return count;
}

/*  get_named_var
    Attempt to create a var with the given type. This will call lexer to
    get the name, as well as ensuring that the given var is unique. */
static lily_var *get_named_var(lily_parse_state *parser, lily_type *var_type,
        int flags)
{
    lily_lex_state *lex = parser->lex;
    lily_var *var;

    var = lily_var_by_name(parser->symtab, lex->label);
    if (var != NULL)
        lily_raise(parser->raiser, lily_SyntaxError,
                   "%s has already been declared.\n", lex->label);

    /* Since class methods and class properties are both accessed the same way,
       prevent them from having the same name. */
    if ((flags & VAR_IS_READONLY) && parser->class_depth) {
        lily_class *current_class = parser->emit->block->self->type->cls;
        lily_prop_entry *entry = lily_find_property(parser->symtab,
                current_class, lex->label);

        if (entry)
            lily_raise(parser->raiser, lily_SyntaxError,
                "A property in class '%s' already has the name '%s'.\n",
                current_class->name, lex->label);
    }

    var = lily_try_new_var(parser->symtab, var_type, lex->label, flags);
    if (var == NULL)
        lily_raise_nomem(parser->raiser);

    lily_lexer(lex);
    return var;
}

/*  get_named_property
    The same thing as get_named_var, but with a property instead. */
static lily_prop_entry *get_named_property(lily_parse_state *parser,
        lily_type *prop_type, int flags)
{
    char *name = parser->lex->label;
    lily_class *current_class = parser->emit->block->self->type->cls;

    lily_prop_entry *prop = lily_find_property(parser->symtab,
            current_class, name);

    if (prop != NULL)
        lily_raise(parser->raiser, lily_SyntaxError,
                "Property %s already exists in class %s.\n", name,
                current_class->name);

    /* Like with get_named_var, prevent properties from having the same name as
       what will become a class method. This is because they are both accessed
       in the same manner outside the class. */
    lily_var *function_var = parser->emit->block->function_var;
    lily_var *lookup_var = lily_var_by_name(parser->symtab, name);

    /* The second check works because register spots for declared functions are
       given out in a linear order. So the lookup var's spot is only going to
       be higher if it was declared after the class block. */
    if (lookup_var && lookup_var->reg_spot > function_var->reg_spot)
        lily_raise(parser->raiser, lily_SyntaxError,
                "A method in class '%s' already has the name '%s'.\n",
                current_class->name, name);

    prop = lily_add_class_property(current_class, prop_type, name, 0);
    if (prop == NULL)
        lily_raise_nomem(parser->raiser);

    lily_lexer(parser->lex);
    return prop;
}

/*  bad_decl_token
    This is a function called when parse_decl is expecting a var name and gets
    a property name, or vice versa. For either case, give the user a more
    useful error message.
    This is particularly important for classes: A new user may expect that
    class properties don't have an @ starter. This gives a useful error message
    in that case. */
static void bad_decl_token(lily_parse_state *parser)
{
    char *message;

    if (parser->lex->token == tk_word)
        message = "Class properties must start with @.\n";
    else
        message = "Cannot use a class property outside of a constructor.\n";

    lily_raise(parser->raiser, lily_SyntaxError, message);
}

/*  grow_type_stack
    Make the stack holding type information bigger for more types. */
static void grow_type_stack(lily_parse_state *parser)
{
    parser->type_stack_size *= 2;

    lily_type **new_type_stack = lily_realloc(parser->type_stack,
        sizeof(lily_type *) * parser->type_stack_size);

    if (new_type_stack == NULL)
        lily_raise_nomem(parser->raiser);

    parser->type_stack = new_type_stack;
}

/*  calculate_generics_used
    This recurses through a given type, marking down what positions are seen
    in the "map" of generics. Additionally, generic_max is set to the highest
    generic position seen + 1. If A is the highest, it's 1, B = 2, C = 3, etc.
    Since Lily's generics are from A to Z (literally), generic_map only needs
    that many slots available. */
static void calculate_generics_used(lily_type *type, int *generic_map,
        int *generic_max)
{
    if (type == NULL)
        return;
    else if (type->cls->id == SYM_CLASS_TEMPLATE) {
        int generic_pos = type->template_pos;
        generic_map[generic_pos] = 1;
        if ((generic_pos + 1) > *generic_max)
            *generic_max = generic_pos + 1;
    }
    else if (type->subtypes) {
        int i;
        for (i = 0;i < type->subtype_count;i++)
            calculate_generics_used(type->subtypes[i], generic_map,
                    generic_max);
    }
}

/*  calculate_variant_return
    This function is called by inner_type_collector to determine what the
    resulting type of a variant 'function' is.
    Ex: For 'enum class Option[A] { Some(A), ...}', a function is created with
        the type 'function [A](A => Some[A])'.

    This is important because the variant may not use all the generics of the
    enum class. In such a situation, the emitter can use the lack of explicit
    information to better calculate type information. */
static lily_type *calculate_variant_return(lily_parse_state *parser,
        lily_class *variant_class, int stack_start, int stack_top)
{
    int generic_map[32];
    int i, j, k, generic_max = 0;

    memset(generic_map, 0, sizeof(generic_map));

    for (i = stack_start;i < stack_top;i++) {
        lily_type *t = parser->type_stack[i];
        calculate_generics_used(t, generic_map, &generic_max);
    }

    if (stack_top + generic_max > parser->type_stack_size)
        grow_type_stack(parser);

    /* The symtab puts each of the generic signatures together, from A onward.
       What this does is to add the generics that are seen. */
    lily_type *generic_iter = parser->symtab->template_type_start;
    for (i = 0, j = stack_top + 1, k = 0;
         i < generic_max;
         i++, generic_iter = generic_iter->next) {
        if (generic_map[i]) {
            parser->type_stack[j] = generic_iter;
            j++;
            k++;
        }
    }

    lily_type *variant_return = lily_build_ensure_type(parser->symtab,
            variant_class, 0, parser->type_stack,
            stack_top + 1, k);

    /* Since the true number of generics that the class takes is known, set
       that directly on the class from here. Note that, for variants, the
       number of generics is not necessarily the highest generic's ID, since
       the variant could, for example, use only A and C. */
    variant_class->template_count = k;
    return variant_return;
}

/*****************************************************************************/
/* Type collection                                                           */
/*****************************************************************************/

static lily_type *collect_var_type(lily_parse_state *parser, lily_class *cls,
        int flags);

#define TC_DEMAND_VALUE  1
#define TC_WANT_VALUE    2
/* In this case, an operator is => or , or ... */
#define TC_WANT_OPERATOR 3
#define TC_BAD_TOKEN     4
#define TC_DONE          5

/*  inner_type_collector
    Given a class that takes inner types (like list, hash, function, etc.),
    collect those inner types. A valid, unique type is returned. */
static lily_type *inner_type_collector(lily_parse_state *parser, lily_class *cls,
        int flags)
{
    int i;
    int state = TC_WANT_VALUE, stack_start = parser->type_stack_pos;
    int type_flags = 0, have_arrow = 0, have_dots = 0;
    lily_token end_token;
    lily_class *variant_class = NULL;

    if (flags & CV_VARIANT_FUNC) {
        variant_class = cls;
        cls = lily_class_by_id(parser->symtab, SYM_CLASS_FUNCTION);
    }

    if (cls->id == SYM_CLASS_FUNCTION) {
        /* Functions have their return as the first type, so leave a hole. */
        if ((parser->type_stack_pos + 2) == parser->type_stack_size)
            grow_type_stack(parser);

        if (flags & CV_CLASS_INIT)
            /* This is a constructor, so use the most recent type declared
               since it's the right one (lily_set_class_generics makes sure of
               it). */
            parser->type_stack[parser->type_stack_pos] =
                    parser->symtab->root_type;
        else
            parser->type_stack[parser->type_stack_pos] = NULL;

        parser->type_stack_pos++;
        end_token = tk_right_parenth;
        i = 1;

        /* Add an implicit 'self' for class functions (except for any nested
           classes). */
        if (flags & CV_TOPLEVEL && parser->class_depth &&
            (flags & CV_CLASS_INIT) == 0) {
            parser->type_stack[parser->type_stack_pos] =
                parser->emit->block->self->type;
            parser->type_stack_pos++;
            i++;
        }
    }
    else {
        end_token = tk_right_bracket;
        i = 0;
    }

    if (flags & CV_TOPLEVEL) {
        flags |= CV_MAKE_VARS;
        flags &= ~CV_TOPLEVEL;
    }
    else
        flags &= ~CV_MAKE_VARS;

    lily_lex_state *lex = parser->lex;
    while (1) {
        if (lex->token == tk_word) {
            if (parser->type_stack_pos == parser->type_stack_size)
                grow_type_stack(parser);

            if (have_arrow)
                flags &= ~(CV_MAKE_VARS);

            lily_type *type = collect_var_type(parser, NULL, flags);
            if (have_arrow == 0) {
                parser->type_stack[parser->type_stack_pos] = type;
                parser->type_stack_pos++;
                i++;
            }
            else
                parser->type_stack[stack_start] = type;

            state = TC_WANT_OPERATOR;
            continue;
        }
        else if (lex->token == tk_comma) {
            if (have_arrow || have_dots ||
                state != TC_WANT_OPERATOR)
                state = TC_BAD_TOKEN;
            else
                state = TC_DEMAND_VALUE;
        }
        else if (lex->token == tk_arrow) {
            if (state == TC_DEMAND_VALUE || have_arrow ||
                end_token == tk_right_bracket ||
                flags & CV_CLASS_INIT)
                state = TC_BAD_TOKEN;
            else if (state == TC_WANT_VALUE || state == TC_WANT_OPERATOR)
                state = TC_DEMAND_VALUE;

            have_arrow = 1;
        }
        else if (lex->token == end_token) {
            /* If there are no args, bump i anyway so that the type will have
               NULL at [1] to indicate no args. */
            if (state == TC_DEMAND_VALUE)
                state = TC_BAD_TOKEN;
            else
                state = TC_DONE;
        }
        else if (lex->token == tk_three_dots) {
            if (have_dots || end_token == tk_right_bracket ||
                state != TC_WANT_OPERATOR)
                state = TC_BAD_TOKEN;
            else {
                lily_type *last_type;
                last_type = parser->type_stack[parser->type_stack_pos - 1];
                if (last_type->cls->id != SYM_CLASS_LIST)
                    lily_raise(parser->raiser, lily_SyntaxError,
                        "A list is required for variable arguments (...).\n");

                have_dots = 1;
                type_flags |= TYPE_IS_VARARGS;
                state = TC_WANT_OPERATOR;
            }
        }
        else
            state = TC_BAD_TOKEN;

        if (state == TC_DONE)
            break;
        else if (state == TC_BAD_TOKEN)
            lily_raise(parser->raiser, lily_SyntaxError,
                    "Unexpected token '%s'.\n", tokname(lex->token));
        else
            lily_lexer(lex);
    }

    if (parser->type_stack_pos - stack_start != cls->template_count &&
        cls->template_count != -1) {
        lily_raise(parser->raiser, lily_SyntaxError,
                "Class %s expects %d type(s), but got %d type(s).\n",
                cls->name, cls->template_count,
                parser->type_stack_pos - stack_start);
    }

    if (cls->id == SYM_CLASS_HASH) {
        /* For hash, make sure that the key (the first type) is valid. */
        lily_type *check_type = parser->type_stack[stack_start];
        if ((check_type->cls->flags & CLS_VALID_HASH_KEY) == 0) {
            lily_raise(parser->raiser, lily_SyntaxError,
                    "'^T' is not a valid hash key.\n", check_type);
        }
    }

    if (flags & CV_VARIANT_FUNC) {
        lily_type *variant_return = calculate_variant_return(parser,
                variant_class, stack_start, i);
        parser->type_stack[stack_start] = variant_return;
    }

    lily_type *result = lily_build_ensure_type(parser->symtab, cls,
            type_flags, parser->type_stack, stack_start, i);
    parser->type_stack_pos = stack_start;
    return result;
}

static int collect_generics(lily_parse_state *parser)
{
    char name[] = "A";
    char ch = name[0];
    lily_lex_state *lex = parser->lex;

    while (1) {
        NEED_NEXT_TOK(tk_word)
        if (lex->label[0] != ch || lex->label[1] != '\0') {
            name[0] = ch;
            lily_raise(parser->raiser, lily_SyntaxError,
                    "Invalid generic name (wanted %s, got %s).\n",
                    name, lex->label);
        }

        ch++;
        lily_lexer(lex);
        if (lex->token == tk_right_bracket) {
            lily_lexer(lex);
            break;
        }
        else if (lex->token != tk_comma)
            lily_raise(parser->raiser, lily_SyntaxError,
                    "Expected either ',' or ']', not '%s'.\n",
                    tokname(lex->token));
    }

    return ch - 'A';
}

/*  collect_var_type
    This is the outer part of type collection. This takes flags (CV_* defines)
    which tell it how to act. Additionally, if the parser has already scanned
    the class info, then 'cls' should be the scanned class. Otherwise, 'cls' 
    will be NULL. This is so parser can check if it's 'sometype T' or
    'sometype::member' without rewinding. */
static lily_type *collect_var_type(lily_parse_state *parser, lily_class *cls,
        int flags)
{
    lily_lex_state *lex = parser->lex;
    if (cls == NULL) {
        NEED_CURRENT_TOK(tk_word)
        cls = lily_class_by_name(parser->symtab, lex->label);
        if (cls == NULL)
            lily_raise(parser->raiser, lily_SyntaxError,
                       "unknown class name %s.\n", lex->label);

        lily_lexer(lex);
    }

    lily_type *result;

    if (cls->flags & CLS_VARIANT_CLASS)
        lily_raise(parser->raiser, lily_SyntaxError,
                "Variant types not allowed in a declaration.\n");

    if (cls->template_count == 0) {
        result = cls->type;
        if (flags & CV_MAKE_VARS)
            get_named_var(parser, cls->type, 0);
    }
    else if (cls->template_count != 0 &&
             cls->id != SYM_CLASS_FUNCTION) {
        NEED_CURRENT_TOK(tk_left_bracket)
        lily_lexer(lex);
        result = inner_type_collector(parser, cls, flags);

        lily_lexer(lex);
        if (flags & CV_MAKE_VARS)
            get_named_var(parser, result, 0);
    }
    else if (cls->id == SYM_CLASS_FUNCTION) {
        /* This is a dummy until the real type is known. */
        lily_type *call_type = parser->default_call_type;
        lily_var *call_var;

        if (flags & CV_MAKE_VARS)
            call_var = get_named_var(parser, call_type, 0);
        else
            call_var = NULL;

        NEED_CURRENT_TOK(tk_left_parenth)
        lily_lexer(lex);
        call_type = inner_type_collector(parser, cls, flags);

        if (flags & CV_MAKE_VARS)
            call_var->type = call_type;

        result = call_type;
        lily_lexer(lex);
    }
    else
        result = NULL;

    return result;
}

static lily_var *parse_prototype(lily_parse_state *parser, lily_class *cls,
        lily_foreign_func foreign_func)
{
    lily_var *save_var_top = parser->symtab->var_chain;
    lily_lex_state *lex = parser->lex;
    /* Skip the 'function' part, going straight for the name. Since this is
       from a builtin source, assume that the identifier is unique. */
    NEED_CURRENT_TOK(tk_word)
    lily_lexer(lex);

    lily_type *call_type = parser->default_call_type;
    int save_generics = parser->emit->block->generic_count;
    int generics_used;

    char *class_name = NULL;
    if (cls)
        class_name = cls->name;

    /* Assume that builtin things are smart enough to not redeclare things and
       just declare the var. */
    lily_var *call_var = lily_try_new_var(parser->symtab, call_type, lex->label,
            VAR_IS_READONLY);
    if (call_var == NULL)
        lily_raise_nomem(parser->raiser);

    call_var->parent = cls;
    call_var->value.function = lily_try_new_foreign_function_val(foreign_func,
            class_name, call_var->name);

    if (call_var->value.function == NULL)
        lily_raise_nomem(parser->raiser);

    call_var->flags &= ~VAL_IS_NIL;

    lily_lexer(lex);

    if (parser->lex->token == tk_left_bracket)
        generics_used = collect_generics(parser);
    else
        generics_used = 0;

    lily_class *function_cls = lily_class_by_id(parser->symtab, SYM_CLASS_FUNCTION);

    NEED_CURRENT_TOK(tk_left_parenth)
    lily_lexer(lex);

    lily_update_symtab_generics(parser->symtab, NULL, generics_used);
    call_type = inner_type_collector(parser, function_cls, 0);
    call_var->type = call_type;
    lily_update_symtab_generics(parser->symtab, NULL, save_generics);
    lily_lexer(lex);

    if (cls) {
        if (cls->call_start == NULL) {
            cls->call_start = call_var;
            cls->call_top = call_var;
        }
        else {
            cls->call_top->next = call_var;
            cls->call_top = cls->call_top->next;
        }
        call_var->next = NULL;
        parser->symtab->var_chain = save_var_top;
    }

    return call_var;
}

/*  parse_function
    This is called to parse class declarations (which are just functions that
    become a class) and toplevel functions (functions not a parameter inside
    something else). */
static void parse_function(lily_parse_state *parser, lily_class *decl_class)
{
    lily_lex_state *lex = parser->lex;
    lily_type *call_type = parser->default_call_type;
    lily_var *call_var;
    int block_type, generics_used;
    int flags = CV_MAKE_VARS | CV_TOPLEVEL;

    lily_class *function_cls = lily_class_by_id(parser->symtab,
            SYM_CLASS_FUNCTION);

    if (decl_class != NULL) {
        call_var = lily_try_new_var(parser->symtab, call_type, "new",
                VAR_IS_READONLY);
        if (call_var == NULL)
            lily_raise_nomem(parser->raiser);

        block_type = BLOCK_FUNCTION | BLOCK_CLASS;
        flags |= CV_CLASS_INIT;
        lily_lexer(lex);
    }
    else {
        call_var = get_named_var(parser, call_type, VAR_IS_READONLY);
        call_var->parent = parser->emit->current_class;
        block_type = BLOCK_FUNCTION;
    }

    if (lex->token == tk_left_bracket)
        generics_used = collect_generics(parser);
    else
        generics_used = parser->emit->block->generic_count;

    lily_emit_enter_block(parser->emit, block_type);
    lily_update_symtab_generics(parser->symtab, decl_class, generics_used);

    if (decl_class != NULL)
        lily_make_constructor_return_type(parser->symtab);
    else if (parser->class_depth && decl_class == NULL) {
        /* Functions of a class get a (self) of that class for the first
           parameter. */
        lily_var *v = lily_try_new_var(parser->symtab,
                parser->emit->block->self->type, "(self)", 0);
        if (v == NULL)
            lily_raise_nomem(parser->raiser);

        parser->emit->block->self = (lily_storage *)v;
    }

    NEED_CURRENT_TOK(tk_left_parenth)
    lily_lexer(lex);

    call_type = inner_type_collector(parser, function_cls, flags);
    call_var->type = call_type;

    lily_emit_update_function_block(parser->emit, decl_class,
            generics_used, call_type->subtypes[0]);

    lily_lexer(lex);
}

/*****************************************************************************/
/* Expression handling                                                       */
/*****************************************************************************/

/* I need a value to work with. */
#define ST_DEMAND_VALUE         1
/* A binary op or an operation (dot call, call, subscript), or a close. */
#define ST_WANT_OPERATOR        2
/* A value is nice, but not required (ex: call arguments). */
#define ST_WANT_VALUE           3
/* This is a special value that's passed to expression, but never set by
   expression internally. If this is initially passed to expression, then ')'
   can finish the expression.
   This is needed because otherwise...
   class Bird(...) > Animal(...)[0]
                                ^^^
                                This is allowed. */
#define ST_MAYBE_END_ON_PARENTH 4

#define ST_DONE                 5
#define ST_BAD_TOKEN            6

/*  expression_static_call
    This handles expressions like `<type>::member`. */
static void expression_static_call(lily_parse_state *parser, lily_class *cls)
{
    lily_lex_state *lex = parser->lex;
    NEED_NEXT_TOK(tk_word)

    lily_var *v = lily_find_class_callable(parser->symtab, cls, lex->label);
    if (v == NULL)
        v = lily_parser_dynamic_load(parser, cls, lex->label);

    if (v)
        lily_ast_push_defined_func(parser->ast_pool, v);
    else {
        lily_class *variant_cls = NULL;
        if (cls->flags & CLS_ENUM_CLASS)
            variant_cls = lily_find_scoped_variant(cls, lex->label);

        if (variant_cls == NULL) {
            lily_raise(parser->raiser, lily_SyntaxError,
                    "%s::%s does not exist.\n", cls->name, lex->label);
        }
        else
            lily_ast_push_variant(parser->ast_pool, variant_cls);
    }
}

/*  parse_special_keyword
    This handles all the simple keywords that map to a string/integer value. */
static lily_literal *parse_special_keyword(lily_parse_state *parser, int key_id)
{
    lily_symtab *symtab = parser->symtab;
    lily_literal *ret;

    /* So far, these are the only keywords that map to literals.
       Additionally, these literal fetching routines are guaranteed to either
       return a literal with the given value, or raise nomem. */
    if (key_id == KEY__LINE__)
        ret = lily_get_integer_literal(symtab, parser->lex->line_num);
    else if (key_id == KEY__FILE__)
        ret = lily_get_string_literal(symtab, parser->lex->filename);
    else if (key_id == KEY__FUNCTION__)
        ret = lily_get_string_literal(symtab, parser->emit->top_var->name);
    else
        ret = NULL;

    return ret;
}

/*  expression_package
    This handles x::y kinds of expressions wherein 'x' is a package.
    * A check for :: is forced so that an inner var can be collected, instead
      of letting packages be assignable. This was done so that package accesses
      can be effectively computed at emit time (given that packages are
      initialized through parser and not assignable or able to be put in lists).
    * This does not check for packages in packages, because those don't
      currently exist. It doesn't check for a callable inner var for the same
      reason.
    * This enters a package tree to stay consistent with all non-(binary/unary)
      trees.
    * This enters tree_package to be consistent with how other things
      (subscripts, list building, typecasts, etc.) all create enterable trees
      to handle things. */
static void expression_package(lily_parse_state *parser, lily_var *package_var)
{
    lily_ast_pool *ap = parser->ast_pool;
    lily_lex_state *lex = parser->lex;
    lily_var *scope = package_var->value.package->vars[0];

    lily_ast_enter_tree(ap, tree_package);

    lily_ast_push_global_var(ap, package_var);
    lily_ast_collect_arg(ap);

    NEED_NEXT_TOK(tk_colon_colon)
    NEED_NEXT_TOK(tk_word)

    lily_var *inner_var = lily_scoped_var_by_name(parser->symtab, scope,
            lex->label);
    if (inner_var == NULL) {
        lily_raise(parser->raiser, lily_SyntaxError,
                "Package %s has no member %s.\n",
                package_var->name, lex->label);
    }

    lily_ast_push_global_var(ap, inner_var);
    lily_ast_collect_arg(ap);
    lily_ast_leave_tree(ap);
}

/*  expression_variant
    This is called when expression_word hits a label that's a class that's
    marked as a variant class. They're used like a function, sometimes. Not
    actually a function though. */
static void expression_variant(lily_parse_state *parser,
        lily_class *variant_cls)
{
    lily_ast_push_variant(parser->ast_pool, variant_cls);
}

/*  expression_word
    This is a helper function that handles words in expressions. These are
    sort of complicated. :( */
static void expression_word(lily_parse_state *parser, int *state)
{
    lily_symtab *symtab = parser->symtab;
    lily_lex_state *lex = parser->lex;
    lily_var *var = lily_var_by_name(symtab, lex->label);

    if (var) {
        if (var->flags & SYM_NOT_INITIALIZED)
            lily_raise(parser->raiser, lily_SyntaxError,
                    "Attempt to use uninitialized value '%s'.\n",
                    var->name);

        if (var->function_depth == 1) {
            /* It's in __main__ as a global. */
            if (var->type->cls->id == SYM_CLASS_PACKAGE)
                expression_package(parser, var);
            else
                lily_ast_push_global_var(parser->ast_pool, var);
        }
        else if (var->function_depth == parser->emit->function_depth)
            /* In this current scope? Load as a local var. */
            lily_ast_push_local_var(parser->ast_pool, var);
        else if (var->function_depth == -1)
            /* This is a function created through 'define'. */
            lily_ast_push_defined_func(parser->ast_pool, var);
        else
            /* todo: Handle upvalues later, maybe. */
            lily_raise(parser->raiser, lily_SyntaxError,
                       "Attempt to use %s, which is not in the current scope.\n",
                       var->name);

        *state = ST_WANT_OPERATOR;
    }
    else {
        int key_id = keyword_by_name(lex->label);
        if (key_id != -1) {
            lily_literal *lit = parse_special_keyword(parser, key_id);
            lily_ast_push_literal(parser->ast_pool, lit);
            *state = ST_WANT_OPERATOR;
        }
        else {
            lily_class *cls = lily_class_by_name(parser->symtab, lex->label);

            if (cls != NULL) {
                if (cls->flags & CLS_VARIANT_CLASS) {
                    expression_variant(parser, cls);
                    *state = ST_WANT_OPERATOR;
                }
                else {
                    lily_lexer(lex);
                    expression_static_call(parser, cls);
                    *state = ST_WANT_OPERATOR;
                }
            }
            else
                lily_raise(parser->raiser, lily_SyntaxError,
                       "%s has not been declared.\n", lex->label);
        }
    }
}

/*  expression_property
    Within a class declaration, the properties of the class are referred to
    by using a @ in front of the name.

    Example:
        class Point(integer inX, integer inY) { @x = inX    @y = inY }
        Point p = Point::new(1, 2)
        # @x now availble as 'p.x', @y as 'p.y'.

    Similar to expression_word, minus the complexity. */
static void expression_property(lily_parse_state *parser, int *state)
{
    if (parser->emit->current_class == NULL)
        lily_raise(parser->raiser, lily_SyntaxError,
                "Properties cannot be used outside of a class constructor.\n");

    char *name = parser->lex->label;
    lily_class *current_class = parser->emit->block->self->type->cls;

    lily_prop_entry *prop = lily_find_property(parser->symtab, current_class,
            name);
    if (prop == NULL)
        lily_raise(parser->raiser, lily_SyntaxError,
                "Property %s is not in class %s.\n", name, current_class->name);

    lily_ast_push_property(parser->ast_pool, prop);
    *state = ST_WANT_OPERATOR;
}

/*  check_valid_close_tok
    This is a helper function that makes sure blocks get the right close token.
    It prevents things like 'abc(1, 2, 3]>' and '[1, 2, 3)'. */
static void check_valid_close_tok(lily_parse_state *parser)
{
    lily_token token = parser->lex->token;
    lily_ast *ast = lily_ast_get_saved_tree(parser->ast_pool);
    lily_tree_type tt = ast->tree_type;
    lily_token expect;

    if (tt == tree_call || tt == tree_parenth || tt == tree_typecast)
        expect = tk_right_parenth;
    else if (tt == tree_tuple)
        expect = tk_tuple_close;
    else
        expect = tk_right_bracket;

    if (token != expect)
        lily_raise(parser->raiser, lily_SyntaxError,
                "Expected closing token '%s', not '%s'.\n", tokname(expect),
                tokname(token));
}

/*  maybe_digit_fixup
    Sometimes 1+1 should actually be 1 + 1 instead of 1 +1. This will either
    split it into two things or it won't if it can't. */
static void maybe_digit_fixup(lily_parse_state *parser, int *did_fixup)
{
    /* The lexer records where the last digit scan started. So check if it
       started with '+' or '-'. */
    lily_lex_state *lex = parser->lex;
    char ch = lex->input_buffer[lex->last_digit_start];

    if (ch == '-' || ch == '+') {
        int expr_op;

        if (ch == '-')
            expr_op = parser_tok_table[tk_minus].expr_op;
        else
            expr_op = parser_tok_table[tk_plus].expr_op;

        lily_ast_push_binary_op(parser->ast_pool, (lily_expr_op)expr_op);
        /* Call this to force a rescan from the proper starting point, yielding
           a proper new token. */
        lily_lexer_digit_rescan(lex);

        lily_ast_push_literal(parser->ast_pool, lex->last_literal);
        *did_fixup = 1;
    }
    else
        *did_fixup = 0;
}

/*  expression_literal
    This handles all literals: integer, double, and string. */
static void expression_literal(lily_parse_state *parser, int *state)
{
    lily_lex_state *lex = parser->lex;
    lily_token token = parser->lex->token;

    if (*state == ST_WANT_OPERATOR && token != tk_double_quote) {
        int did_fixup;
        maybe_digit_fixup(parser, &did_fixup);
        if (did_fixup == 0)
            *state = ST_DONE;
    }
    else {
        lily_ast_push_literal(parser->ast_pool, lex->last_literal);
        *state = ST_WANT_OPERATOR;
    }
}

/*  expression_comma_arrow
    This handles commas and arrows. The & 0x1 is nothing magical: a proper
    hash will always have pairs of values. The values to the left side of
    the arrow will always be odd, and the right ones will be even. */
static void expression_comma_arrow(lily_parse_state *parser, int *state)
{
    lily_lex_state *lex = parser->lex;

    if (parser->ast_pool->active == NULL)
        lily_raise(parser->raiser, lily_SyntaxError,
                    "Expected a value, not ','.\n");

    lily_ast *last_tree = lily_ast_get_saved_tree(parser->ast_pool);
    if (lex->token == tk_comma) {
        if (last_tree->tree_type == tree_hash &&
            (last_tree->args_collected & 0x1) == 0)
            lily_raise(parser->raiser, lily_SyntaxError,
                    "Expected a key => value pair before ','.\n");
    }
    else if (lex->token == tk_arrow) {
        if (last_tree->tree_type == tree_list) {
            if (last_tree->args_collected == 0)
                last_tree->tree_type = tree_hash;
            else
                lily_raise(parser->raiser, lily_SyntaxError,
                        "Unexpected token '%s'.\n", tokname(tk_arrow));
        }
        else if (last_tree->tree_type != tree_hash ||
                 (last_tree->args_collected & 0x1) == 1)
                lily_raise(parser->raiser, lily_SyntaxError,
                        "Unexpected token '%s'.\n", tokname(tk_arrow));
    }

    lily_ast_collect_arg(parser->ast_pool);
    *state = ST_DEMAND_VALUE;
}

static void expression_unary(lily_parse_state *parser, int *state)
{
    if (*state == ST_WANT_OPERATOR)
        *state = ST_BAD_TOKEN;
    else {
        lily_token token = parser->lex->token;
        if (token == tk_minus)
            lily_ast_push_unary_op(parser->ast_pool, expr_unary_minus);
        else if (token == tk_not)
            lily_ast_push_unary_op(parser->ast_pool, expr_unary_not);

        *state = ST_DEMAND_VALUE;
    }
}

/*  expression_dot
    This handles "oo-style" accesses.
        Those can be either for a callable member:
            string x = "abc"
            abc.concat("def")
        Or for getting properties of a class:
            ValueError v = ValueError::new("test")
            show (v.message)
    It also handles typecasts: `abc.@(type)`. */
static void expression_dot(lily_parse_state *parser, int *state)
{
    lily_lex_state *lex = parser->lex;
    lily_lexer(lex);
    if (lex->token == tk_word) {
        /* Create a magic oo access tree and expect an operator. This allows
           the property to be called or not, important for implementing
           properties and callables through dot. */
        lily_ast_push_oo_access(parser->ast_pool, parser->lex->label);

        *state = ST_WANT_OPERATOR;
    }
    else if (lex->token == tk_typecast_parenth) {
        lily_lexer(lex);
        lily_type *new_type = collect_var_type(parser, NULL, 0);
        lily_ast_enter_typecast(parser->ast_pool, new_type);
        lily_ast_leave_tree(parser->ast_pool);
        *state = ST_WANT_OPERATOR;
    }
}

/*  expression_raw
    BEHOLD! This is the magic function that handles expressions. The expression
    state is viewed as being in one of a handful of states. The ast pool takes
    care of knowing how deep a current expression is.

    It is recommended that expression be used instead of this, unless the
    caller -really- needs to have a starting state that requires a word (yes,
    this does happen). */
static void expression_raw(lily_parse_state *parser, int state)
{
    lily_lex_state *lex = parser->lex;
    int maybe_end_on_parenth = 0;
    if (state == ST_MAYBE_END_ON_PARENTH) {
        maybe_end_on_parenth = 1;
        state = ST_WANT_VALUE;
    }

    while (1) {
        int expr_op = parser_tok_table[lex->token].expr_op;
        if (lex->token == tk_word) {
            if (state == ST_WANT_OPERATOR)
                state = ST_DONE;
            else
                expression_word(parser, &state);
        }
        else if (expr_op != -1) {
            if (state == ST_WANT_OPERATOR) {
                lily_ast_push_binary_op(parser->ast_pool, (lily_expr_op)expr_op);
                state = ST_DEMAND_VALUE;
            }
            else if (lex->token == tk_minus)
                expression_unary(parser, &state);
            else
                state = ST_BAD_TOKEN;
        }
        else if (lex->token == tk_left_parenth) {
            if (state == ST_WANT_VALUE || state == ST_DEMAND_VALUE) {
                lily_ast_enter_tree(parser->ast_pool, tree_parenth);
                state = ST_DEMAND_VALUE;
            }
            else if (state == ST_WANT_OPERATOR) {
                lily_ast_enter_tree(parser->ast_pool, tree_call);
                state = ST_WANT_VALUE;
            }
        }
        else if (lex->token == tk_left_bracket) {
            if (state == ST_WANT_VALUE || state == ST_DEMAND_VALUE) {
                lily_ast_enter_tree(parser->ast_pool, tree_list);
                state = ST_WANT_VALUE;
            }
            else if (state == ST_WANT_OPERATOR) {
                lily_ast_enter_tree(parser->ast_pool, tree_subscript);
                state = ST_DEMAND_VALUE;
            }
        }
        else if (lex->token == tk_prop_word) {
            if (state == ST_WANT_OPERATOR)
                state = ST_DONE;
            else
                expression_property(parser, &state);
        }
        else if (lex->token == tk_tuple_open) {
            if (state == ST_WANT_OPERATOR)
                state = ST_DONE;
            else {
                lily_ast_enter_tree(parser->ast_pool, tree_tuple);
                state = ST_WANT_VALUE;
            }
        }
        else if (lex->token == tk_right_parenth ||
                 lex->token == tk_right_bracket ||
                 lex->token == tk_tuple_close) {
            if (state == ST_DEMAND_VALUE ||
                parser->ast_pool->save_depth == 0) {
                state = ST_BAD_TOKEN;
            }
            else {
                check_valid_close_tok(parser);
                lily_ast_leave_tree(parser->ast_pool);
                if (maybe_end_on_parenth == 0 ||
                    lex->token != tk_right_parenth ||
                    parser->ast_pool->save_depth != 0)
                    state = ST_WANT_OPERATOR;
                else {
                    state = ST_DONE;
                }
            }
        }
        else if (lex->token == tk_integer || lex->token == tk_double ||
                 lex->token == tk_double_quote)
            expression_literal(parser, &state);
        else if (lex->token == tk_dot)
            expression_dot(parser, &state);
        else if (lex->token == tk_minus || lex->token == tk_not)
            expression_unary(parser, &state);
        else if (parser_tok_table[lex->token].val_or_end &&
                 parser->ast_pool->save_depth == 0 &&
                 state == ST_WANT_OPERATOR)
            state = ST_DONE;
        else if (lex->token == tk_comma || lex->token == tk_arrow)
            expression_comma_arrow(parser, &state);
        else if (lex->token == tk_lambda) {
            lily_ast_push_lambda(parser->ast_pool, parser->lex->lambda_start_line,
                     parser->lex->lambda_data);
            state = ST_WANT_OPERATOR;
        }
        else
            state = ST_BAD_TOKEN;

        if (state == ST_DONE)
            break;
        else if (state == ST_BAD_TOKEN)
            lily_raise(parser->raiser, lily_SyntaxError,
                    "Unexpected token '%s'.\n", tokname(lex->token));
        else
            lily_lexer(lex);
    }
}

/*  expression
    This calls expression_raw with a starting state of ST_DEMAND_VALUE. 99%
    of the time, that's what's needed.

    Calling this function is preferred, as there's no weird 'ST_DEMAND_VALUE'
    showing up everywhere. */
static void expression(lily_parse_state *parser)
{
    expression_raw(parser, ST_DEMAND_VALUE);
}

/* parse_decl
   This function takes a type and handles a declaration wherein each var name
   is separated by a comma. Ex:

   integer a, b, c
   double d
   list[integer] e

   This handles anything but function declarations.
   Expected token: A label (the first variable name). */
static void parse_decl(lily_parse_state *parser, lily_type *type)
{
    lily_lex_state *lex = parser->lex;
    lily_var *var = NULL;
    lily_prop_entry *prop = NULL;
    /* This prevents variables from being used to initialize themselves. */
    int flags = SYM_NOT_INITIALIZED;

    lily_token token, want_token, other_token;
    if (parser->emit->block->block_type & BLOCK_CLASS) {
        want_token = tk_prop_word;
        other_token = tk_word;
    }
    else {
        want_token = tk_word;
        other_token = tk_prop_word;
    }

    while (1) {
        /* For this special case, give a useful error message. */
        if (lex->token == other_token)
            bad_decl_token(parser);

        NEED_CURRENT_TOK(want_token)

        if (lex->token == tk_word)
            var = get_named_var(parser, type, flags);
        else
            prop = get_named_property(parser, type, flags);

        if (lex->token != tk_equal)
            lily_raise(parser->raiser, lily_SyntaxError,
                    "An initialization expression is required here.\n");

        if (var != NULL) {
            /* It's important to add locals and globals differently, because
               the emitter can't optimize stuff with globals. */
            if (parser->emit->function_depth == 1)
                lily_ast_push_global_var(parser->ast_pool, var);
            else
                lily_ast_push_local_var(parser->ast_pool, var);
        }
        else
            lily_ast_push_property(parser->ast_pool, prop);

        lily_ast_push_binary_op(parser->ast_pool, expr_assign);
        lily_lexer(lex);
        expression(parser);
        lily_emit_eval_expr(parser->emit, parser->ast_pool);

        token = lex->token;
        /* This is the start of the next statement (or, for 'var', only allow
           one decl at a time to discourage excessive use of 'var'). */
        if (token == tk_word || token == tk_prop_word || token == tk_end_tag ||
            token == tk_inner_eof || token == tk_right_curly ||
            token == tk_final_eof || type == NULL)
            break;
        else if (token != tk_comma) {
            lily_raise(parser->raiser, lily_SyntaxError,
                       "Expected ',' or ')', not %s.\n", tokname(lex->token));
        }
        /* else it's a comma, so make sure a word is next. */

        lily_lexer(lex);
    }
}

static lily_var *parse_for_range_value(lily_parse_state *parser, char *name)
{
    lily_ast_pool *ap = parser->ast_pool;
    expression(parser);

    /* Don't allow assigning expressions, since that just looks weird.
       ex: for i in a += 10..5
       Also, it makes no real sense to do that. */
    if (ap->root->tree_type == tree_binary &&
        ap->root->op >= expr_assign) {
        lily_raise(parser->raiser, lily_SyntaxError,
                   "For range value expression contains an assignment.");
    }

    lily_class *cls = lily_class_by_id(parser->symtab, SYM_CLASS_INTEGER);

    /* For loop values are created as vars so there's a name in case of a
       problem. This name doesn't have to be unique, since it will never be
       found by the user. */
    lily_var *var = lily_try_new_var(parser->symtab, cls->type, name, 0);
    if (var == NULL)
        lily_raise_nomem(parser->raiser);

    lily_emit_eval_expr_to_var(parser->emit, ap, var);

    return var;
}

/*****************************************************************************/
/* Statement handling                                                        */
/*****************************************************************************/

/* Every keyword has an associated handler, even if it's something rather
   simple. */
static void if_handler(lily_parse_state *, int);
static void do_handler(lily_parse_state *, int);
static void var_handler(lily_parse_state *, int);
static void for_handler(lily_parse_state *, int);
static void try_handler(lily_parse_state *, int);
static void case_handler(lily_parse_state *, int);
static void else_handler(lily_parse_state *, int);
static void elif_handler(lily_parse_state *, int);
static void enum_handler(lily_parse_state *, int);
static void while_handler(lily_parse_state *, int);
static void raise_handler(lily_parse_state *, int);
static void match_handler(lily_parse_state *, int);
static void break_handler(lily_parse_state *, int);
static void class_handler(lily_parse_state *, int);
static void define_handler(lily_parse_state *, int);
static void return_handler(lily_parse_state *, int);
static void except_handler(lily_parse_state *, int);
static void file_kw_handler(lily_parse_state *, int);
static void line_kw_handler(lily_parse_state *, int);
static void continue_handler(lily_parse_state *, int);
static void function_kw_handler(lily_parse_state *, int);

typedef void (keyword_handler)(lily_parse_state *, int);

/* This is setup so that handlers[key_id] is the handler for that keyword. */
static keyword_handler *handlers[] = {
    if_handler,
    do_handler,
    var_handler,
    for_handler,
    try_handler,
    case_handler,
    else_handler,
    elif_handler,
    enum_handler,
    while_handler,
    raise_handler,
    match_handler,
    break_handler,
    class_handler,
    define_handler,
    return_handler,
    except_handler,
    file_kw_handler,
    line_kw_handler,
    continue_handler,
    function_kw_handler
};

static void parse_multiline_block_body(lily_parse_state *, int);

/*  statement
    This is a magic function that handles keywords outside of expression,
    as well as getting declarations started.
    If multi is set, this function will do the above until it finds a starting
    token that isn't a label. */
static void statement(lily_parse_state *parser, int multi)
{
    int key_id;
    lily_class *lclass;
    lily_lex_state *lex = parser->lex;

    do {
        lily_token token = lex->token;

        if (token == tk_word) {
            key_id = keyword_by_name(lex->label);
            if (key_id != -1) {
                /* Ask the handler for this keyword what to do. */
                lily_lexer(lex);
                handlers[key_id](parser, multi);
            }
            else {
                lclass = lily_class_by_name(parser->symtab, lex->label);

                if (lclass != NULL) {
                    lily_lexer(lex);
                    if (lex->token == tk_colon_colon) {
                        expression_static_call(parser, lclass);
                        lily_lexer(lex);
                        expression_raw(parser, ST_WANT_OPERATOR);
                        lily_emit_eval_expr(parser->emit, parser->ast_pool);
                    }
                    else {
                        lily_type *cls_type = collect_var_type(parser, lclass, 0);
                        parse_decl(parser, cls_type);
                    }
                }
                else {
                    expression(parser);
                    lily_emit_eval_expr(parser->emit, parser->ast_pool);
                }
            }
        }
        else if (token == tk_integer || token == tk_double ||
                 token == tk_double_quote || token == tk_left_parenth ||
                 token == tk_left_bracket || token == tk_tuple_open ||
                 token == tk_prop_word) {
            expression(parser);
            lily_emit_eval_expr(parser->emit, parser->ast_pool);
        }
        /* The caller will be expecting '}' or maybe ?> / EOF if it's the main
           parse loop. */
        else if (multi)
            break;
        /* Single-line expressions need a value to prevent things like
           'if 1: }' and 'if 1: ?>'. */
        else
            lily_raise(parser->raiser, lily_SyntaxError,
                    "Expected a value, not '%s'.\n", tokname(token));
    } while (multi);
}

/*  parse_block_body
    This is a helper function for parsing the body of a simple (but multi-line)
    block. This is suitable for 'while', 'do while', and 'for...in'.

    This is called when the current token is the ':'. It will handle the '{',
    call statement, then check that '}' is found after the statement. Finally,
    it calls up the next token for the parent.

    for i in 1..10: { ... }
                  ^
    do: {  ... } while 1:
      ^
    while 1: { ... }
           ^
    if 1: { ... }
        ^
    */
static void parse_multiline_block_body(lily_parse_state *parser,
        int multi)
{
    lily_lex_state *lex = parser->lex;

    if (multi == 0)
        lily_raise(parser->raiser, lily_SyntaxError,
                   "Multi-line block within single-line block.\n");

    lily_lexer(lex);
    /* statement expects the token to be ready. */
    if (lex->token != tk_right_curly)
        statement(parser, 1);
    NEED_CURRENT_TOK(tk_right_curly)
    lily_lexer(lex);
}

/*  if_handler
    This handles parsing 'if'. There are two kinds of if blocks:
    (multi-line)  if expr { expr... }
    (single-line) if expr: expr

    'elif' and 'else' are multi-line if 'if' is multi-line. The 'if' is closed
    by a single '}', rather than by each 'elif'/'else' (like with C). */
static void if_handler(lily_parse_state *parser, int multi)
{
    lily_lex_state *lex = parser->lex;

    lily_emit_enter_block(parser->emit, BLOCK_IF);
    expression(parser);
    lily_emit_eval_condition(parser->emit, parser->ast_pool);
    NEED_CURRENT_TOK(tk_colon)

    lily_lexer(lex);
    if (lex->token == tk_left_curly)
        parse_multiline_block_body(parser, multi);
    else {
        statement(parser, 0);
        while (lex->token == tk_word) {
            int key_id = keyword_by_name(lex->label);

            /* Jump directly into elif/else. Doing it this way (instead of
               through statement) means that the 'if' block can be popped in a
               single place. */
            if (key_id == KEY_ELIF || key_id == KEY_ELSE) {
                lily_lexer(parser->lex);
                handlers[key_id](parser, 0);
            }
            else
                break;
        }
    }

    lily_emit_leave_block(parser->emit);
}

/*  elif_handler
    This handles elif. Both elif and else don't call the block because they're
    always called somehow through if_handler calling statement.
    The multi-line-ness has already been determined by the if block. */
static void elif_handler(lily_parse_state *parser, int multi)
{
    lily_lex_state *lex = parser->lex;
    lily_emit_change_block_to(parser->emit, BLOCK_IF_ELIF);
    expression(parser);
    lily_emit_eval_condition(parser->emit, parser->ast_pool);

    NEED_CURRENT_TOK(tk_colon)

    lily_lexer(lex);
    statement(parser, multi);
}

/*  else_handler
    This handles the else keyword. Doesn't get much easier. */
static void else_handler(lily_parse_state *parser, int multi)
{
    lily_lex_state *lex = parser->lex;

    lily_emit_change_block_to(parser->emit, BLOCK_IF_ELSE);
    NEED_CURRENT_TOK(tk_colon)
    lily_lexer(lex);

    statement(parser, multi);
}

/*  return_handler
    This handles the return keyword. It'll look up the current function to see
    if an expression is needed, or if just 'return' alone is fine. */
static void return_handler(lily_parse_state *parser, int multi)
{
    if (parser->emit->block->block_type & BLOCK_CLASS)
        lily_raise(parser->raiser, lily_SyntaxError,
                "'return' not allowed in a class constructor.\n");

    lily_type *ret_type = parser->emit->top_function_ret;
    lily_ast *ast;

    if (ret_type != NULL) {
        expression(parser);
        ast = parser->ast_pool->root;
    }
    else
        ast = NULL;

    lily_emit_return(parser->emit, ast);
    if (ast)
        lily_ast_reset_pool(parser->ast_pool);

    if (multi && parser->lex->token != tk_right_curly)
        lily_raise(parser->raiser, lily_SyntaxError,
                "'return' not at the end of a multi-line block.\n");
}

/*  while_handler
    Syntax:
        (multi-line)  while expr: { expr... }
        (single-line) while expr: expr
    */
static void while_handler(lily_parse_state *parser, int multi)
{
    lily_lex_state *lex = parser->lex;

    lily_emit_enter_block(parser->emit, BLOCK_WHILE);

    expression(parser);
    lily_emit_eval_condition(parser->emit, parser->ast_pool);

    NEED_CURRENT_TOK(tk_colon)
    lily_lexer(lex);
    if (lex->token == tk_left_curly)
        parse_multiline_block_body(parser, multi);
    else
        statement(parser, 0);

    lily_emit_leave_block(parser->emit);
}

/*  continue_handler
    This handles a 'continue' command. This just tells the emitter to insert a
    continue, nothing fancy. */
static void continue_handler(lily_parse_state *parser, int multi)
{
    lily_emit_continue(parser->emit);

    if (multi && parser->lex->token != tk_right_curly)
        lily_raise(parser->raiser, lily_SyntaxError,
                "'continue' not at the end of a multi-line block.\n");
}

/*  break_handler
    This handles the 'break' statement. Just a wrapper for emitter to call
    to emit a break. */
static void break_handler(lily_parse_state *parser, int multi)
{
    lily_emit_break(parser->emit);

    if (multi && parser->lex->token != tk_right_curly)
        lily_raise(parser->raiser, lily_SyntaxError,
                "'break' not at the end of a multi-line block.\n");
}

/*  do_keyword
    This handles simple keywords that can start expressions. It unifies common
    code in __line__, __file__, and __function__.

    key_id: The id of the keyword to handle. */
static void do_keyword(lily_parse_state *parser, int key_id)
{
    lily_literal *lit;
    lit = parse_special_keyword(parser, key_id);
    lily_ast_push_literal(parser->ast_pool, lit);

    expression_raw(parser, ST_WANT_OPERATOR);
    lily_emit_eval_expr(parser->emit, parser->ast_pool);
}

/*  line_kw_handler
    This handles __line__. This raises an error because it's not considered all
    that useful outside of an expression. */
static void line_kw_handler(lily_parse_state *parser, int multi)
{
    do_keyword(parser, KEY__LINE__);
}

/*  file_kw_handler
    This handles __file__. This raises an error because it's not considered all
    that useful outside of an expression. */
static void file_kw_handler(lily_parse_state *parser, int multi)
{
    do_keyword(parser, KEY__FILE__);
}

/*  function_kw_handler
    This handles __function__. This raises an error because it's not considered
    all that useful outside of an expression. */
static void function_kw_handler(lily_parse_state *parser, int multi)
{
    do_keyword(parser, KEY__FUNCTION__);
}

/*  for_handler
    Syntax:
        (multi-line)  for var in start..end: { expr... }
        (single-line) for var in start..end: expr

    This handles for..in statements. These only accept integers for var, start,
    and end. Additionally, start and end can be expressions, but may not
    contain any sort of assignment.

    (So this would be invalid: for i in a = 10..11: ...)
    (But this is okay: for i in 1+2..4*4: ...)

    If var does not exist, it is created as an integer, and falls out of scope
    when the loop exits.
    If var does exist, then it will exist after the loop. */
static void for_handler(lily_parse_state *parser, int multi)
{
    lily_lex_state *lex = parser->lex;
    lily_var *loop_var;

    NEED_CURRENT_TOK(tk_word)

    lily_emit_enter_block(parser->emit, BLOCK_FOR_IN);

    loop_var = lily_var_by_name(parser->symtab, lex->label);
    if (loop_var == NULL) {
        lily_class *cls = lily_class_by_id(parser->symtab, SYM_CLASS_INTEGER);
        loop_var = lily_try_new_var(parser->symtab, cls->type, lex->label, 0);
        if (loop_var == NULL)
            lily_raise_nomem(parser->raiser);
    }
    else if (loop_var->type->cls->id != SYM_CLASS_INTEGER) {
        lily_raise(parser->raiser, lily_SyntaxError,
                   "Loop var must be type integer, not type '^T'.\n",
                   loop_var->type);
    }

    NEED_NEXT_TOK(tk_word)
    if (strcmp(lex->label, "in") != 0)
        lily_raise(parser->raiser, lily_SyntaxError, "Expected 'in', not '%s'.\n",
                   lex->label);

    lily_lexer(lex);

    lily_var *for_start, *for_end, *for_step;

    for_start = parse_for_range_value(parser, "(for start)");

    NEED_CURRENT_TOK(tk_three_dots)
    lily_lexer(lex);

    for_end = parse_for_range_value(parser, "(for end)");

    if (lex->token == tk_word) {
        if (strcmp(lex->label, "by") != 0)
            lily_raise(parser->raiser, lily_SyntaxError,
                       "Expected 'by', not '%s'.\n", lex->label);

        lily_lexer(lex);
        for_step = parse_for_range_value(parser, "(for step)");
    }
    else
        for_step = NULL;

    lily_emit_finalize_for_in(parser->emit, loop_var, for_start, for_end,
                              for_step, parser->lex->line_num);

    NEED_CURRENT_TOK(tk_colon)
    lily_lexer(lex);
    if (lex->token == tk_left_curly)
        parse_multiline_block_body(parser, multi);
    else
        statement(parser, 0);

    lily_emit_leave_block(parser->emit);
}

/*  do_handler
    Syntax:
        (multi-line)  do: { expr... } while expr:
        (single-line) do: expr while expr:
    This is like while, except there's no check on entry and the while check
    jumps to the top if successful. */
static void do_handler(lily_parse_state *parser, int multi)
{
    lily_lex_state *lex = parser->lex;

    lily_emit_enter_block(parser->emit, BLOCK_DO_WHILE);

    NEED_CURRENT_TOK(tk_colon)
    lily_lexer(lex);
    if (lex->token == tk_left_curly)
        parse_multiline_block_body(parser, multi);
    else
        statement(parser, 0);

    NEED_CURRENT_TOK(tk_word)
    /* This could do a keyword scan, but there's only one correct answer
       so...nah. */
    if (strcmp(lex->label, "while") != 0)
        lily_raise(parser->raiser, lily_SyntaxError,
                   "Expected 'while', not '%s'.\n", lex->label);

    /* Now prep the token for expression. Save the resulting tree so that
       it can be eval'd specially. */
    lily_lexer(lex);
    expression(parser);
    lily_emit_eval_condition(parser->emit, parser->ast_pool);
    lily_emit_leave_block(parser->emit);
}

static void except_handler(lily_parse_state *parser, int multi)
{
    lily_lex_state *lex = parser->lex;

    NEED_CURRENT_TOK(tk_word)
    lily_class *exception_class = lily_class_by_name(parser->symtab, lex->label);
    if (exception_class == NULL)
        lily_raise(parser->raiser, lily_SyntaxError,
                "'%s' is not a class.\n", lex->label);

    /* Exception is likely to always be the base exception class. */
    lily_class *exception_base = lily_class_by_name(parser->symtab,
            "Exception");

    int is_valid = lily_check_right_inherits_or_is(exception_base,
            exception_class);
    if (is_valid == 0)
        lily_raise(parser->raiser, lily_SyntaxError,
                "'%s' is not a valid exception class.\n");

    lily_var *exception_var = NULL;

    lily_lexer(lex);
    if (lex->token == tk_word) {
        if (strcmp(parser->lex->label, "as") != 0)
            lily_raise(parser->raiser, lily_SyntaxError,
                "Expected 'as', not '%s'.\n", lex->label);

        NEED_NEXT_TOK(tk_word)
        exception_var = lily_var_by_name(parser->symtab, lex->label);
        if (exception_var != NULL)
            lily_raise(parser->raiser, lily_SyntaxError,
                "%s has already been declared.\n", exception_var->name);

        exception_var = lily_try_new_var(parser->symtab, exception_class->type,
                lex->label, 0);

        lily_lexer(lex);
    }

    NEED_CURRENT_TOK(tk_colon)
    lily_emit_change_block_to(parser->emit, BLOCK_TRY_EXCEPT);
    lily_emit_except(parser->emit, exception_class, exception_var,
            lex->line_num);

    lily_lexer(lex);
    if (lex->token != tk_right_curly)
        statement(parser, 1);
}

static void try_handler(lily_parse_state *parser, int multi)
{
    lily_lex_state *lex = parser->lex;

    lily_emit_enter_block(parser->emit, BLOCK_TRY);
    lily_emit_try(parser->emit, parser->lex->line_num);

    NEED_CURRENT_TOK(tk_colon)
    lily_lexer(lex);
    if (lex->token == tk_left_curly)
        parse_multiline_block_body(parser, multi);
    else {
        statement(parser, 0);
        while (lex->token == tk_word) {
            if (strcmp("except", lex->label) == 0) {
                lily_lexer(parser->lex);
                except_handler(parser, multi);
            }
            else
                break;
        }
    }

    /* The vm expects that the last except block will have a 'next' of 0 to
       indicate the end of the 'except' chain. Remove the patch that the last
       except block installed so it doesn't get patched. */
    parser->emit->patch_pos--;

    lily_emit_leave_block(parser->emit);
}

static void raise_handler(lily_parse_state *parser, int multi)
{
    expression(parser);
    lily_emit_raise(parser->emit, parser->ast_pool->root);
    lily_ast_reset_pool(parser->ast_pool);
}

static void ensure_valid_class(lily_parse_state *parser, char *name)
{
    if (name[1] == '\0')
        lily_raise(parser->raiser, lily_SyntaxError,
                "'%s' is not a valid class name (too short).\n", name);

    if ((parser->emit->block->block_type & BLOCK_CLASS) == 0 &&
        parser->emit->block->prev != NULL) {
        /* This could probably be worded better... */
        lily_raise(parser->raiser, lily_SyntaxError,
                "Attempt to declare a class within something that isn't another class.\n");
    }

    lily_class *lookup_class = lily_class_by_name(parser->symtab, name);
    if (lookup_class != NULL) {
        lily_raise(parser->raiser, lily_SyntaxError,
                "Class '%s' has already been declared.\n", name);
    }
}

/*  parse_inheritance
    Syntax: class Bird(args...) > Animal(args...) {
                       ^                 ^
                       Start             End

    This function is responsible making sure that the class to be inherited
    from is valid and inheritable. This also collects the arguments to call
    the ::new of the inherited class and executes the call. */
static void parse_inheritance(lily_parse_state *parser, lily_class *cls)
{
    lily_lex_state *lex = parser->lex;
    NEED_NEXT_TOK(tk_word)

    lily_class *super_class = lily_class_by_name(parser->symtab,
            lex->label);

    if (super_class == NULL)
        lily_raise(parser->raiser, lily_SyntaxError,
                "Class '%s' does not exist.\n", lex->label);
    else if (super_class == cls)
        lily_raise(parser->raiser, lily_SyntaxError,
                "A class cannot inherit from itself!\n");
    else if (super_class->id <= SYM_CLASS_TEMPLATE)
        lily_raise(parser->raiser, lily_SyntaxError,
                "Cannot inherit from builtin classes. Sorry.\n");
    else if (super_class->call_start == NULL)
        lily_raise(parser->raiser, lily_SyntaxError,
                "A class cannot inherit from an incomplete class.\n");

    lily_var *class_new = lily_find_class_callable(parser->symtab,
            super_class, "new");

    /* I don't think this is possible, but I'm not entirely sure. */
    if (class_new == NULL)
        lily_raise(parser->raiser, lily_SyntaxError,
                "Inherited class does not have a constructor?\n");

    NEED_NEXT_TOK(tk_left_parenth)

    /* There's a small problem here. The idea of being able to pass expressions
       as well as values is great. However, expression cannot be trusted to
       collect what's inside of the parentheses because it may allow a subscript
       afterward.
       Ex: class Point(integer value) > Parent(value)[0].
       This is avoided by passing a special flag to expression and calling it
       directly. */

    lily_ast_pool *ap = parser->ast_pool;
    lily_ast_enter_tree(ap, tree_call);
    lily_ast_push_inherited_new(ap, class_new);
    lily_ast_collect_arg(ap);

    /* Since the call was already entered, skip the first '(' or the parser
       will attempt to enter it again. */
    lily_lexer(lex);

    expression_raw(parser, ST_MAYBE_END_ON_PARENTH);
    lily_emit_eval_expr(parser->emit, ap);
    lily_change_parent_class(super_class, cls);

    /* The caller will sure that this is '{'. */
    lily_lexer(lex);
}

static void class_handler(lily_parse_state *parser, int multi)
{
    lily_lex_state *lex = parser->lex;
    NEED_CURRENT_TOK(tk_word);
    ensure_valid_class(parser, lex->label);

    lily_class *created_class = lily_new_class(parser->symtab, lex->label);

    parse_function(parser, created_class);

    if (lex->token == tk_lt)
        parse_inheritance(parser, created_class);

    NEED_CURRENT_TOK(tk_left_curly)

    parser->class_depth++;
    parse_multiline_block_body(parser, multi);
    parser->class_depth--;

    lily_finish_class(parser->symtab, created_class);

    lily_emit_leave_block(parser->emit);
}

static void var_handler(lily_parse_state *parser, int multi)
{
    parse_decl(parser, NULL);
}

static void enum_handler(lily_parse_state *parser, int multi)
{
    lily_lex_state *lex = parser->lex;

    NEED_CURRENT_TOK(tk_word)
    if (strcmp(lex->label, "class") != 0) {
        lily_raise(parser->raiser, lily_SyntaxError,
                "Expected 'class', not '%s'.\n", lex->label);
    }

    NEED_NEXT_TOK(tk_word)
    ensure_valid_class(parser, lex->label);

    lily_class *enum_class = lily_new_class(parser->symtab, lex->label);

    lily_lexer(lex);
    int save_generics = parser->emit->block->generic_count;
    int generics_used;
    if (lex->token == tk_left_bracket)
        generics_used = collect_generics(parser);
    else
        generics_used = 0;

    lily_update_symtab_generics(parser->symtab, enum_class, generics_used);
    lily_make_constructor_return_type(parser->symtab);
    lily_type *result_type = parser->symtab->root_type;

    NEED_CURRENT_TOK(tk_left_curly)
    lily_lexer(lex);

    int inner_class_count = 0;
    int is_scoped = (lex->token == tk_colon_colon);

    while (1) {
        if (is_scoped) {
            NEED_CURRENT_TOK(tk_colon_colon)
            lily_lexer(lex);
        }

        NEED_CURRENT_TOK(tk_word)
        lily_class *variant_class = lily_class_by_name(parser->symtab, lex->label);
        if (variant_class != NULL)
            lily_raise(parser->raiser, lily_SyntaxError,
                    "A class with the name '%s' already exists.\n",
                    variant_class->name);

        variant_class = lily_new_variant_class(parser->symtab, enum_class,
                lex->label);
        lily_type *variant_type;

        lily_lexer(lex);
        if (lex->token == tk_left_parenth) {
            lily_lexer(lex);
            if (lex->token == tk_right_parenth)
                lily_raise(parser->raiser, lily_SyntaxError,
                        "Variant class cannot take empty ().\n");

            variant_type = inner_type_collector(parser, variant_class,
                    CV_VARIANT_FUNC);

            /* Skip the closing ')'. */
            lily_lexer(lex);
        }
        else
            variant_type = NULL;

        lily_finish_variant_class(parser->symtab, variant_class, variant_type);

        inner_class_count++;

        if (lex->token == tk_comma)
            lily_lexer(lex);
        else if (lex->token == tk_right_curly)
            break;
    }

    if (inner_class_count < 2) {
        lily_raise(parser->raiser, lily_SyntaxError,
                "An enum class must have at least two variants.\n");
    }

    lily_finish_enum_class(parser->symtab, enum_class, is_scoped, result_type);
    lily_update_symtab_generics(parser->symtab, NULL, save_generics);
    lily_lexer(lex);
}

/*  match_handler
    Syntax:
        'match <expr>: { ... }'

    The match block is an outlier compared to other blocks because it must
    always have the { and } after it. This is so that the inner case entries
    can automagically be multi-line. */
static void match_handler(lily_parse_state *parser, int multi)
{
    if (multi == 0)
        lily_raise(parser->raiser, lily_SyntaxError,
                "Match block cannot be in a single-line block.\n");

    lily_lex_state *lex = parser->lex;

    lily_emit_enter_block(parser->emit, BLOCK_MATCH);

    expression(parser);
    lily_emit_eval_match_expr(parser->emit, parser->ast_pool);

    NEED_CURRENT_TOK(tk_colon)
    NEED_NEXT_TOK(tk_left_curly)

    parse_multiline_block_body(parser, multi);

    lily_emit_leave_block(parser->emit);
}

/*  calculate_decompose_type
    This is used to determine what type that variables declared as part of
    a enum class decomposition will get. I'm very, very unhappy to say that I
    copied this directly from the vm (resolve_property_type). */
static lily_type *calculate_decompose_type(lily_parse_state *parser,
        lily_type *match_type, lily_type *input_type, int stack_offset)
{
    lily_type *result_type;

    if (input_type->cls->id == SYM_CLASS_TEMPLATE)
        result_type = match_type->subtypes[input_type->template_pos];
    else if (input_type->cls->template_count == 0)
        result_type = input_type;
    else {
        int types_needed = input_type->subtype_count;

        if ((stack_offset + types_needed) > parser->type_stack_size) {
            lily_type **new_types = lily_realloc(parser->type_stack,
                    sizeof(lily_type *) *
                    (stack_offset + types_needed));

            if (new_types == NULL)
                lily_raise_nomem(parser->raiser);

            parser->type_stack = new_types;
            parser->type_stack_size = (stack_offset + types_needed);
        }

        int i;
        lily_type *inner_type;
        for (i = 0;i < input_type->subtype_count;i++) {
            inner_type = input_type->subtypes[i];
            inner_type = calculate_decompose_type(parser, input_type,
                    inner_type, stack_offset + i);

            parser->type_stack[stack_offset + i] = inner_type;
        }

        int flags = (input_type->flags & TYPE_IS_VARARGS);
        result_type = lily_build_ensure_type(parser->symtab, input_type->cls,
                flags, parser->type_stack, stack_offset, i);
    }

    return result_type;
}

/*  case_handler
    Syntax:
        For variants that do not take values:
            'case <variant class>: ...'

        For those that do:
            'case <variant class>(<var name>, <var name>...):'

    Each case in a match block is multi-line, so that users don't have to put
    { and } around a lot of cases (because that would probably be annoying).

    The emitter will check that, within a match block, each variant is seen
    exactly once (lily_emit_check_match_case).

    Some variants may have inner values. For those, parser will collect the
    appropriate number of identifiers and determine what the type of those
    identifiers should be! The variant's values are then decomposed to those
    identifiers.

    Checking for incomplete match blocks is done within emitter when the match
    block closes.

    The section for a case is done when the next case is seen. */
static void case_handler(lily_parse_state *parser, int multi)
{
    lily_block *block = parser->emit->block;
    if (block->block_type != BLOCK_MATCH)
        lily_raise(parser->raiser, lily_SyntaxError,
                "'case' not allowed outside of 'match'.\n");

    lily_type *match_input_type = block->match_sym->type;
    lily_class *match_class = match_input_type->cls;
    lily_lex_state *lex = parser->lex;
    lily_class *case_class = NULL;

    NEED_CURRENT_TOK(tk_word)

    int i;
    for (i = 0;i < match_class->variant_size;i++) {
        if (strcmp(lex->label, match_class->variant_members[i]->name) == 0) {
            case_class = match_class->variant_members[i];
            break;
        }
    }

    if (i == match_class->variant_size)
        lily_raise(parser->raiser, lily_SyntaxError,
                "%s is not a member of enum class %s.\n", lex->label,
                match_class->name);

    if (lily_emit_add_match_case(parser->emit, i) == 0)
        lily_raise(parser->raiser, lily_SyntaxError,
                "Already have a case for variant %s.\n", lex->label);

    lily_type *variant_type = case_class->variant_type;
    if (variant_type->subtype_count != 0) {
        NEED_NEXT_TOK(tk_left_parenth)
        /* There should be as many identifiers as there are arguments to this
           variant's creation type.
           Also, start at 1 so that the return at [0] is skipped. */
        NEED_NEXT_TOK(tk_word)

        for (i = 1;i < variant_type->subtype_count;i++) {
            lily_type *var_type = calculate_decompose_type(parser,
                    match_input_type, variant_type->subtypes[i],
                    parser->type_stack_pos);

            /* It doesn't matter what the var is, only that it's unique. The
               emitter will grab the vars it needs from the symtab when writing
               the decompose.
               This function also calls up the next token. */
            get_named_var(parser, var_type, 0);
            if (i != variant_type->subtype_count - 1) {
                NEED_CURRENT_TOK(tk_comma)
            }
        }
        NEED_CURRENT_TOK(tk_right_parenth)

        lily_emit_variant_decompose(parser->emit, variant_type);
    }
    /* else the variant does not take arguments, and cannot decompose because
       there is nothing inside to decompose. */

    NEED_NEXT_TOK(tk_colon)
    lily_lexer(lex);
}

static void define_handler(lily_parse_state *parser, int multi)
{
    lily_lex_state *lex = parser->lex;
    parse_function(parser, NULL);
    NEED_CURRENT_TOK(tk_left_curly)
    parse_multiline_block_body(parser, multi);
    lily_emit_leave_block(parser->emit);
}

static void do_bootstrap(lily_parse_state *parser)
{
    lily_lex_state *lex = parser->lex;
    const lily_func_seed *global_seed = lily_get_global_seed_chain();
    while (global_seed) {
        lily_load_str(lex, "[builtin]", lm_no_tags,
                global_seed->func_definition);
        lily_lexer(lex);
        parse_prototype(parser, NULL, global_seed->func);
        global_seed = global_seed->next;
    }

    lily_lex_entry *first_entry = parser->lex->entry;
    lily_load_str(lex, "[builtin]", lm_no_tags, exception_bootstrap);
    lily_lexer(lex);
    do {
        statement(parser, 1);
    } while (parser->lex->entry != first_entry);
}

/*  parser_loop
    This is the main parsing function. This is called by a lily_parse_*
    function which will set the raiser and give the lexer a file before calling
    this function. */
static void parser_loop(lily_parse_state *parser)
{
    if (parser->mode == pm_init)
        do_bootstrap(parser);

    /* Must do this first, in the rare case this next call fails. */
    parser->mode = pm_parse;

    lily_lex_state *lex = parser->lex;
    lily_lexer(lex);

    while (1) {
        if (lex->token == tk_word)
            statement(parser, 1);
        else if (lex->token == tk_right_curly) {
            lily_emit_leave_block(parser->emit);
            lily_lexer(lex);
        }
        else if (lex->token == tk_end_tag ||
                 (lex->token == tk_final_eof && lex->mode == lm_no_tags)) {
            if (parser->emit->block->prev != NULL) {
                lily_raise(parser->raiser, lily_SyntaxError,
                           "Unterminated block(s) at end of parsing.\n");
            }
            lily_emit_vm_return(parser->emit);

            lily_vm_prep(parser->vm, parser->symtab);
            parser->mode = pm_execute;
            lily_vm_execute(parser->vm);
            parser->mode = pm_parse;

            /* Clear __main__ for the next pass. */
            lily_reset_main(parser->emit);

            if (lex->token == tk_end_tag) {
                lily_lexer_handle_page_data(parser->lex);
                if (lex->token == tk_final_eof)
                    break;
                else
                    lily_lexer(lex);
            }
            else
                break;
        }
        /* This makes it possible to have expressions that don't start with a
           var. This may be useful later for building a repl. */
        else if (lex->token == tk_integer || lex->token == tk_double ||
                 lex->token == tk_double_quote ||
                 lex->token == tk_left_parenth ||
                 lex->token == tk_left_bracket ||
                 lex->token == tk_tuple_open) {
            expression(parser);
            lily_emit_eval_expr(parser->emit, parser->ast_pool);
        }
        else if (lex->token == tk_inner_eof)
            /* TODO: Eventually, this case should be used to make sure that a
                     file hasn't exited in the middle of a function and various
                     other things.
                     For now, don't bother because there's no importing. */
            lily_lexer(lex);
        else
            lily_raise(parser->raiser, lily_SyntaxError, "Unexpected token %s.\n",
                       tokname(lex->token));
    }
}

/*****************************************************************************/
/* Exported API                                                              */
/*****************************************************************************/

/*  lily_parser_lambda_eval
    This function is called by the emitter to process the body of a lambda. The
    type that the emitter expects is given so that the types of the
    lambda's arguments can be inferred. */
lily_var *lily_parser_lambda_eval(lily_parse_state *parser,
        int lambda_start_line, char *lambda_body, lily_type *expect_type,
        int did_resolve)
{
    lily_lex_state *lex = parser->lex;
    int args_collected = 0, resolved_any_args = 0;
    lily_type *root_result;

    /* Process the lambda as if it were a file with a slightly adjusted
       starting line number. The line number is patched so that multi-line
       lambdas show the right line number for errors.
       Additionally, lambda_body is a shallow copy of data within the ast's
       string pool. A deep copy MUST be made because expressions within this
       lambda may cause the ast's string pool to be resized. */
    lily_load_copy_string(lex, "[lambda]", lm_no_tags, lambda_body);
    lex->line_num = lambda_start_line;

    char lambda_name[32];
    snprintf(lambda_name, 32, "*lambda_%d", parser->next_lambda_id);
    parser->next_lambda_id++;

    /* Block entry assumes that the most recent var added is the var to bind
       the function to. For the type of the lambda, use the default call
       type (a function with no args and no output) because expect_type may
       be NULL if the emitter doesn't know what it wants. */
    lily_var *lambda_var = lily_try_new_var(parser->symtab,
            parser->default_call_type, lambda_name, VAR_IS_READONLY);

    /* From here on, vars created will be in the scope of the lambda. Also,
       this binds a function value to lambda_var. */
    lily_emit_enter_block(parser->emit, BLOCK_LAMBDA | BLOCK_FUNCTION);

    lily_lexer(lex);
    /* Emitter ensures that the given type is either NULL or a function
       type.
       Collect arguments if expecting a function and the function takes at
       least one argument. */
    if (expect_type && expect_type->subtype_count > 1) {
        if (lex->token == tk_logical_or)
            lily_raise(parser->raiser, lily_SyntaxError,
                    "Lambda expected %d args, but got 0.\n",
                    expect_type->subtype_count - 1);

        /* -1 because the return isn't an arg. */
        int num_args = expect_type->subtype_count - 1;
        int originally_unresolved = -1;
        lily_token wanted_token = tk_comma;
        if (did_resolve == 0)
            originally_unresolved = count_unresolved_generics(parser->emit);

        while (1) {
            NEED_NEXT_TOK(tk_word)
            lily_type *arg_type = expect_type->subtypes[args_collected + 1];
            if (did_resolve == 0) {
                arg_type = lily_resolve_type(parser->emit, arg_type);
                int num_unresolved = count_unresolved_generics(parser->emit);
                /* lily_resolve_type likes to fill in unresolved generics with
                   type 'any' if it doesn't have type information. However, a
                   lambda should have full type info for each arg. */
                if (num_unresolved != originally_unresolved) {
                    lily_raise(parser->raiser, lily_SyntaxError,
                            "Cannot infer type of '%s'.\n", lex->label);
                }

                resolved_any_args = 1;
            }

            get_named_var(parser, arg_type, 0);
            args_collected++;
            if (args_collected == num_args)
                wanted_token = tk_bitwise_or;

            NEED_CURRENT_TOK(wanted_token)
            if (wanted_token == tk_bitwise_or)
                break;
        }
    }
    else if (lex->token == tk_bitwise_or) {
        NEED_NEXT_TOK(tk_bitwise_or)
    }
    else if (lex->token != tk_logical_or)
        lily_raise(parser->raiser, lily_SyntaxError, "Unexpected token '%s'.\n",
                lex->token);

    lily_lexer(lex);

    /* If the emitter knows what the lambda's result should be, then use that
       to do some type inference on the result of the expression. */
    lily_type *result_wanted = NULL;
    if (expect_type)
        result_wanted = expect_type->subtypes[0];

    /* It's time to process the body of the lambda. Before this is done, freeze
       the ast pool's state so that the save depth is 0 and such. This allows
       the expression function to ensure that the body of the lambda is valid. */
    lily_ast_freeze_state(parser->ast_pool);
    expression(parser);
    lily_emit_eval_lambda_body(parser->emit, parser->ast_pool, result_wanted,
            did_resolve);
    if (parser->ast_pool->root->result)
        /* Save this before state thaw wipes it out. It can't be gotten (easily)
           later. */
        root_result = parser->ast_pool->root->result->type;
    else
        /* It's possible that the body of the lambda is a function that doesn't
           return a value. */
        root_result = NULL;

    lily_ast_thaw_state(parser->ast_pool);

    NEED_CURRENT_TOK(tk_right_curly)
    lily_lexer(lex);

    if (resolved_any_args || root_result != result_wanted) {
        /* The type passed does not accurately describe the lambda. Build
           one that does, because the emitter may use this returned type in
           further type inference. */
        int types_needed = args_collected + 1;
        int flags = 0, end = parser->type_stack_pos + types_needed;
        int i;
        lily_class *function_cls = lily_class_by_id(parser->symtab,
                SYM_CLASS_FUNCTION);
        lily_var *var_iter = parser->symtab->var_chain;
        if (parser->type_stack_pos + types_needed > parser->type_stack_size)
            grow_type_stack(parser);

        if (expect_type && expect_type->cls->id == SYM_CLASS_FUNCTION &&
            expect_type->flags & TYPE_IS_VARARGS)
            flags = TYPE_IS_VARARGS;

        parser->type_stack[parser->type_stack_pos] = root_result;
        /* Symtab puts the most recent var on top, and goes to the oldest.
           That's the reverse order of the arguments so apply backward. */
        for (i = 1;i < types_needed;i++, var_iter = var_iter->next)
            parser->type_stack[end - i] = var_iter->type;

        lily_type *new_type = lily_build_ensure_type(parser->symtab, function_cls,
                flags, parser->type_stack, parser->type_stack_pos, types_needed);
        lambda_var->type = new_type;
    }
    else if (expect_type)
        lambda_var->type = expect_type;
    else
        lambda_var->type = parser->default_call_type;

    lily_emit_leave_block(parser->emit);

    return lambda_var;
}

lily_var *lily_parser_dynamic_load(lily_parse_state *parser, lily_class *cls,
        char *name)
{
    lily_lex_state *lex = parser->lex;
    const lily_func_seed *seed = lily_find_class_call_seed(parser->symtab,
            cls, name);

    lily_var *ret;

    if (seed != NULL) {
        lily_load_str(lex, "[builtin]", lm_no_tags, seed->func_definition);
        lily_lexer(lex);
        ret = parse_prototype(parser, cls, seed->func);
    }
    else
        ret = NULL;

    return ret;
}

/*  lily_parse_file
    This function starts parsing from a file indicated by the given filename.
    The file is opened through fopen, and is automatically destroyed when the
    parser is free'd.

    parser:  The parser that will be used to parse and run the data.
    mode:    This determines if <?lily ?> tags are parsed or not.
    str:     The string to parse.

    Returns 1 if successful, or 0 if an error was raised. */
int lily_parse_file(lily_parse_state *parser, lily_lex_mode mode, char *filename)
{
    if (setjmp(parser->raiser->jumps[parser->raiser->jump_pos]) == 0) {
        parser->raiser->jump_pos++;
        lily_load_file(parser->lex, mode, filename);
        if (parser->lex->token != tk_final_eof)
            parser_loop(parser);

        return 1;
    }

    return 0;
}

/*  lily_parse_string
    This function starts parsing from a source that is a string passed. The caller
    is responsible for destroying the string if it needs to be destroyed.

    parser:  The parser that will be used to parse and run the data.
    name:    The name for this file, for when trace is printed.
    mode:    This determines if <?lily ?> tags are parsed or not.
    str:     The string to parse.

    Returns 1 if successful, or 0 if some error occured. */
int lily_parse_string(lily_parse_state *parser, char *name, lily_lex_mode mode,
        char *str)
{
    if (setjmp(parser->raiser->jumps[parser->raiser->jump_pos]) == 0) {
        parser->raiser->jump_pos++;
        lily_load_str(parser->lex, name, mode, str);
        parser_loop(parser);
        return 1;
    }

    return 0;
}

/*  lily_parse_special
    This function starts parsing from a source given by the runner. Use this if
    a given source isn't a file or a str.

    parser:       The parser that will be used to parse and run the data.
    mode:         This determines if <?lily ?> tags are parsed or not.
    source:       The source providing text for the lexer to read.
    filename:     A filename for this source.
    read_line_fn: A function for the lexer to call to read a line from the
                  source.
    close_fn:     A function for the lexer to call to close the data source. If
                  the source does not need to be closed, this should be a no-op
                  function, not NULL. */
int lily_parse_special(lily_parse_state *parser, lily_lex_mode mode,
    void *source, char *filename, lily_reader_fn read_line_fn,
    lily_close_fn close_fn)
{
    if (setjmp(parser->raiser->jumps[parser->raiser->jump_pos]) == 0) {
        parser->raiser->jump_pos++;
        lily_load_special(parser->lex, mode, source, filename, read_line_fn,
                close_fn);
        parser_loop(parser);
        return 1;
    }

    return 0;
}