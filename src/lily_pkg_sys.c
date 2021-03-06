#include <string.h>

#include "lily_impl.h"
#include "lily_symtab.h"

static void bind_stringlist(lily_symtab *symtab, int stringlist_size,
        char **stringlist, int *ok)
{
    *ok = 0;

    const int ids[] = {SYM_CLASS_LIST, SYM_CLASS_STRING};

    lily_type *list_string_type = lily_try_type_from_ids(symtab, ids);
    if (list_string_type == NULL)
        return;

    lily_type *string_type = list_string_type->subtypes[0];
    lily_var *bound_var = lily_try_new_var(symtab, list_string_type, "argv", 0);
    if (bound_var == NULL)
        return;

    lily_list_val *lv = lily_malloc(sizeof(lily_list_val));
    lily_value **values = lily_malloc(stringlist_size * sizeof(lily_value));
    if (lv == NULL || values == NULL) {
        lily_free(lv);
        lily_free(values);
        return;
    }

    lv->gc_entry = NULL;
    lv->elems = values;
    lv->num_values = stringlist_size;
    lv->refcount = 1;
    lv->elems = values;
    lv->visited = 0;
    bound_var->value.list = lv;
    bound_var->flags &= ~VAL_IS_NIL;

    int i, err;
    err = 0;
    for (i = 0;i < stringlist_size;i++) {
        values[i] = lily_malloc(sizeof(lily_value));
        if (values[i] == NULL) {
            lv->num_values = i;
            err = 1;
            break;
        }
        values[i]->type = string_type;
        values[i]->flags = VAL_IS_NIL;
    }

    if (err == 0) {
        for (i = 0;i < stringlist_size;i++) {
            lily_string_val *sv = lily_malloc(sizeof(lily_string_val));
            char *raw_string = lily_malloc(strlen(stringlist[i]) + 1);

            if (sv == NULL || raw_string == NULL) {
                lily_free(sv);
                lily_free(raw_string);
                err = 1;
                break;
            }
            strcpy(raw_string, stringlist[i]);
            sv->size = strlen(stringlist[i]);
            sv->refcount = 1;
            sv->string = raw_string;
            values[i]->flags = 0;
	        values[i]->value.string = sv;
        }
    }

    *ok = !err;
}

int lily_pkg_sys_init(lily_symtab *symtab, int argc, char **argv)
{
    if (symtab == NULL)
        return 0;

    int ok = 0;
    lily_class *package_cls = lily_class_by_id(symtab, SYM_CLASS_PACKAGE);
    lily_type *package_type = lily_try_type_for_class(symtab, package_cls);
    lily_var *bound_var = lily_try_new_var(symtab, package_type, "sys", 0);

    if (bound_var) {
        lily_var *save_chain = symtab->var_chain;
        int save_spot = symtab->next_register_spot;

        bind_stringlist(symtab, argc, argv, &ok);

        if (ok) {
            lily_package_val *pval = lily_malloc(sizeof(lily_package_val));
            lily_var **package_vars = lily_malloc(1 * sizeof(lily_var *));
            if (pval == NULL || package_vars == NULL) {
                lily_free(pval);
                lily_free(package_vars);
                ok = 0;
            }
            else {
                int i = 0;
                lily_var *var_iter = symtab->var_chain;
                while (var_iter != save_chain) {
                    package_vars[i] = var_iter;
                    i++;
                    var_iter = var_iter->next;
                }
                symtab->var_chain = save_chain;
                symtab->next_register_spot = save_spot;

                pval->refcount = 1;
                pval->name = bound_var->name;
                pval->gc_entry = NULL;
                pval->var_count = i;
                pval->vars = package_vars;
                bound_var->flags &= ~VAL_IS_NIL;
                bound_var->value.package = pval;
            }
        }
    }

    return ok;
}