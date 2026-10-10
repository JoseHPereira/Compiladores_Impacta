#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEXER_SEM_MAIN
#include "../../lexer/lexer_v2.c"
#define PARSER_SEMANTIC_LIBRARY
#include "../../lexer/parser.c"

typedef struct {
    char base[20];
    bool array;
    int size;
} SemType;

typedef enum { SYMBOL_VARIABLE, SYMBOL_PARAMETER, SYMBOL_FUNCTION } SymbolKind;

typedef struct Symbol Symbol;
typedef struct Scope Scope;

struct Symbol {
    char *name;
    SymbolKind kind;
    SemType type;
    SemType *parameters;
    int parameter_count;
    int line;
    int column;
    Symbol *next;
};

struct Scope {
    Symbol *symbols;
    Scope *parent;
};

typedef struct {
    char *code;
    char *message;
    int line;
    int column;
    int sequence;
} Diagnostic;

typedef struct {
    const char *source;
    int source_size;
    TokenList *tokens;
    int *line_offsets;
    int line_count;
    Scope *scope;
    Symbol *function;
    Diagnostic *diagnostics;
    int diagnostic_count;
    int diagnostic_capacity;
    int next_sequence;
} Semantic;

static char *copy_string(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = (char *)malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

static char *format_string(const char *format, ...) {
    va_list args;
    va_list copy;
    int length;
    char *text;
    va_start(args, format);
    va_copy(copy, args);
    length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) {
        va_end(args);
        return copy_string("");
    }
    text = (char *)malloc((size_t)length + 1);
    if (text != NULL) vsnprintf(text, (size_t)length + 1, format, args);
    va_end(args);
    return text;
}

static SemType make_type(const char *base, bool array, int size) {
    SemType type;
    snprintf(type.base, sizeof(type.base), "%s", base);
    type.array = array;
    type.size = size;
    return type;
}

static SemType error_type(void) { return make_type("erro", false, 0); }
static SemType void_type(void) { return make_type("void", false, 0); }
static SemType bool_type(void) { return make_type("bool", false, 0); }
static SemType int_type(void) { return make_type("int", false, 0); }
static SemType float_type(void) { return make_type("float", false, 0); }

static bool is_error(SemType type) { return strcmp(type.base, "erro") == 0; }
static bool is_void(SemType type) { return !type.array && strcmp(type.base, "void") == 0; }
static bool is_numeric(SemType type) {
    return !type.array && (strcmp(type.base, "int") == 0 || strcmp(type.base, "float") == 0);
}

static char *type_name(SemType type) {
    if (type.array) return format_string("%s[]", type.base);
    return copy_string(type.base);
}

static char *type_description(SemType type) {
    if (type.array && type.size > 0) return format_string("%s[%d]", type.base, type.size);
    return type_name(type);
}

static bool assignable(SemType destination, SemType source) {
    if (is_error(destination) || is_error(source)) return true;
    if (strcmp(destination.base, "void") == 0 || strcmp(destination.base, "string") == 0 ||
        strcmp(source.base, "void") == 0) return false;
    if (destination.array || source.array) {
        return destination.array && source.array && strcmp(destination.base, source.base) == 0;
    }
    return strcmp(destination.base, source.base) == 0 ||
           (strcmp(source.base, "int") == 0 && strcmp(destination.base, "float") == 0);
}

static void open_scope(Semantic *semantic) {
    Scope *scope = (Scope *)calloc(1, sizeof(Scope));
    if (scope == NULL) return;
    scope->parent = semantic->scope;
    semantic->scope = scope;
}

static void close_scope(Semantic *semantic) {
    if (semantic->scope != NULL) semantic->scope = semantic->scope->parent;
}

static Symbol *lookup_symbol(Semantic *semantic, const char *name) {
    for (Scope *scope = semantic->scope; scope != NULL; scope = scope->parent) {
        for (Symbol *symbol = scope->symbols; symbol != NULL; symbol = symbol->next) {
            if (strcmp(symbol->name, name) == 0) return symbol;
        }
    }
    return NULL;
}

static void add_diagnostic(Semantic *semantic, const char *code, int line, int column,
                          const char *format, ...) {
    va_list args;
    va_list copy;
    int length;
    char *message;
    if (semantic->diagnostic_count == semantic->diagnostic_capacity) {
        int capacity = semantic->diagnostic_capacity == 0 ? 16 : semantic->diagnostic_capacity * 2;
        Diagnostic *items = (Diagnostic *)realloc(semantic->diagnostics,
                                                   (size_t)capacity * sizeof(Diagnostic));
        if (items == NULL) return;
        semantic->diagnostics = items;
        semantic->diagnostic_capacity = capacity;
    }
    va_start(args, format);
    va_copy(copy, args);
    length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) {
        va_end(args);
        return;
    }
    message = (char *)malloc((size_t)length + 1);
    if (message == NULL) {
        va_end(args);
        return;
    }
    vsnprintf(message, (size_t)length + 1, format, args);
    va_end(args);
    Diagnostic *diagnostic = &semantic->diagnostics[semantic->diagnostic_count++];
    diagnostic->code = copy_string(code);
    diagnostic->message = message;
    diagnostic->line = line;
    diagnostic->column = column;
    diagnostic->sequence = semantic->next_sequence++;
}

static Token *node_token(Semantic *semantic, const Node *node) {
    if (node == NULL || node->token_start < 0 || node->token_start >= semantic->tokens->quantidade)
        return NULL;
    return &semantic->tokens->itens[node->token_start];
}

static void node_position(Semantic *semantic, const Node *node, int *line, int *column) {
    Token *token = node_token(semantic, node);
    *line = token != NULL ? token->linha : 1;
    *column = token != NULL ? token->coluna : 1;
}

static void name_position(Semantic *semantic, const Node *node, int *line, int *column) {
    int index = node != NULL ? node->token_start + 1 : -1;
    if (index >= 0 && index < semantic->tokens->quantidade) {
        *line = semantic->tokens->itens[index].linha;
        *column = semantic->tokens->itens[index].coluna;
    } else {
        node_position(semantic, node, line, column);
    }
}

static char *node_text(Semantic *semantic, const Node *node) {
    Token *first;
    Token *last;
    size_t begin;
    size_t end;
    size_t length;
    char *text;
    if (node == NULL || node->token_start < 0 || node->token_end <= node->token_start ||
        node->token_end > semantic->tokens->quantidade) return copy_string("");
    first = &semantic->tokens->itens[node->token_start];
    last = &semantic->tokens->itens[node->token_end - 1];
    if (first->linha < 1 || first->linha > semantic->line_count ||
        last->linha < 1 || last->linha > semantic->line_count) return copy_string("");
    begin = (size_t)semantic->line_offsets[first->linha - 1] + (size_t)(first->coluna - 1);
    end = (size_t)semantic->line_offsets[last->linha - 1] + (size_t)(last->coluna - 1) +
          strlen(last->lexema);
    if (begin > (size_t)semantic->source_size || end > (size_t)semantic->source_size || end < begin)
        return copy_string("");
    length = end - begin;
    text = (char *)malloc(length + 1);
    if (text != NULL) {
        memcpy(text, semantic->source + begin, length);
        text[length] = '\0';
    }
    return text != NULL ? text : copy_string("");
}

static bool declare_symbol(Semantic *semantic, Symbol *symbol) {
    Symbol *previous;
    char *description;
    if (semantic->scope == NULL || symbol == NULL) return false;
    for (previous = semantic->scope->symbols; previous != NULL; previous = previous->next) {
        if (strcmp(previous->name, symbol->name) == 0) {
            description = previous->kind == SYMBOL_FUNCTION
                ? format_string("função com retorno %s", previous->type.base)
                : format_string("tipo %s", type_description(previous->type));
            add_diagnostic(semantic, "SEM002", symbol->line, symbol->column,
                           "“%s” já declarado neste escopo; declaração anterior na linha %d, coluna %d (%s).",
                           symbol->name, previous->line, previous->column, description);
            return false;
        }
    }
    symbol->next = semantic->scope->symbols;
    semantic->scope->symbols = symbol;
    return true;
}

static Symbol *new_symbol(const char *name, SymbolKind kind, SemType type, int line, int column) {
    Symbol *symbol = (Symbol *)calloc(1, sizeof(Symbol));
    if (symbol == NULL) return NULL;
    symbol->name = copy_string(name);
    symbol->kind = kind;
    symbol->type = type;
    symbol->line = line;
    symbol->column = column;
    return symbol;
}

static SemType semantic_expr(Semantic *semantic, Node *node);
static SemType semantic_value(Semantic *semantic, Node *node, const char *context);
static SemType semantic_target(Semantic *semantic, Node *node, const char *context);
static bool semantic_stmt(Semantic *semantic, Node *node, char **fall_reason);

static SemType semantic_index(Semantic *semantic, Node *node) {
    SemType base_type = semantic_expr(semantic, node->left);
    SemType index_type = semantic_value(semantic, node->right, "índice de vetor");
    char *name = node_text(semantic, node->left);
    int line;
    int column;
    if (!is_error(base_type) && !base_type.array) {
        node_position(semantic, node->left, &line, &column);
        add_diagnostic(semantic, "SEM016", line, column,
                       "“%s” é do tipo %s e não pode ser indexado; apenas vetores aceitam índice.",
                       name, type_name(base_type));
        base_type = error_type();
    }
    if (!is_error(index_type) && strcmp(index_type.base, "int") != 0) {
        node_position(semantic, node->right, &line, &column);
        add_diagnostic(semantic, "SEM006", line, column,
                       "Índice do vetor “%s” deve ser int; recebeu %s (expressão “%s”).",
                       name, type_name(index_type), node_text(semantic, node->right));
    }
    return is_error(base_type) ? error_type() : make_type(base_type.base, false, 0);
}

static SemType semantic_call(Semantic *semantic, Node *node) {
    Node *callee = node->left;
    int actual_count = 0;
    int line;
    int column;
    SemType *actual_types;
    Symbol *symbol;
    if (callee == NULL || callee->kind != N_ID) {
        if (callee != NULL) semantic_expr(semantic, callee);
        node_position(semantic, callee, &line, &column);
        add_diagnostic(semantic, "SEM018", line, column,
                       "A expressão “%s” não é uma função e não pode ser chamada.",
                       node_text(semantic, callee));
        for (NodeList *item = node->items; item != NULL; item = item->next)
            semantic_value(semantic, item->node, "argumento de chamada");
        return error_type();
    }
    for (NodeList *item = node->items; item != NULL; item = item->next) actual_count++;
    actual_types = actual_count > 0 ? (SemType *)calloc((size_t)actual_count, sizeof(SemType)) : NULL;
    int index = 0;
    for (NodeList *item = node->items; item != NULL; item = item->next)
        actual_types[index++] = semantic_value(semantic, item->node, "argumento de chamada");

    const char *name = callee->a != NULL ? callee->a : "";
    if (strcmp(name, "print") == 0 || strcmp(name, "read") == 0) {
        index = 0;
        for (NodeList *item = node->items; item != NULL; item = item->next) {
            index++;
            if (strcmp(name, "print") == 0) {
                SemType type = actual_types[index - 1];
                if (type.array) {
                    node_position(semantic, item->node, &line, &column);
                    add_diagnostic(semantic, "SEM008", line, column,
                                   "Argumento %d de “print”: não é possível imprimir um vetor inteiro (expressão “%s”).",
                                   index, node_text(semantic, item->node));
                }
            } else {
                semantic_target(semantic, item->node, "leitura");
            }
        }
        free(actual_types);
        return void_type();
    }

    symbol = lookup_symbol(semantic, name);
    if (symbol == NULL) {
        node_position(semantic, callee, &line, &column);
        add_diagnostic(semantic, "SEM001", line, column,
                       "Identificador “%s” não declarado neste escopo.", name);
        free(actual_types);
        return error_type();
    }
    if (symbol->kind != SYMBOL_FUNCTION) {
        node_position(semantic, callee, &line, &column);
        add_diagnostic(semantic, "SEM018", line, column,
                       "“%s” não é uma função e não pode ser chamado.", name);
        free(actual_types);
        return error_type();
    }
    if (actual_count != symbol->parameter_count) {
        node_position(semantic, node, &line, &column);
        add_diagnostic(semantic, "SEM007", line, column,
                       "“%s” espera %d argumento%s, mas recebeu %d.", name,
                       symbol->parameter_count, symbol->parameter_count == 1 ? "" : "s", actual_count);
    } else {
        index = 0;
        for (NodeList *item = node->items; item != NULL; item = item->next) {
            SemType expected = symbol->parameters[index];
            SemType received = actual_types[index];
            if (!assignable(expected, received)) {
                node_position(semantic, item->node, &line, &column);
                add_diagnostic(semantic, "SEM008", line, column,
                               "Argumento %d de “%s”: esperado %s, recebido %s (expressão “%s”).",
                               index + 1, name, type_name(expected), type_name(received),
                               node_text(semantic, item->node));
            }
            index++;
        }
    }
    free(actual_types);
    return symbol->type;
}

static SemType semantic_value(Semantic *semantic, Node *node, const char *context) {
    SemType type = semantic_expr(semantic, node);
    if (is_void(type)) {
        int line;
        int column;
        const char *name = node != NULL && node->kind == N_CALL && node->left != NULL &&
                           node->left->kind == N_ID ? node->left->a : NULL;
        char *expression = name == NULL ? node_text(semantic, node) : NULL;
        node_position(semantic, node, &line, &column);
        add_diagnostic(semantic, "SEM012", line, column,
                       "Função “%s” não produz valor (retorno void) e não pode ser usada como %s.",
                       name != NULL ? name : expression, context);
        return error_type();
    }
    return type;
}

static SemType semantic_expr(Semantic *semantic, Node *node) {
    int line;
    int column;
    if (node == NULL) return error_type();
    switch (node->kind) {
        case N_LIT:
            if (strcmp(node->a, "int") == 0) return int_type();
            if (strcmp(node->a, "real") == 0) return float_type();
            if (strcmp(node->a, "bool") == 0) return bool_type();
            return make_type(node->a, false, 0);
        case N_ID: {
            Symbol *symbol = lookup_symbol(semantic, node->a);
            if (symbol == NULL) {
                node_position(semantic, node, &line, &column);
                add_diagnostic(semantic, "SEM001", line, column,
                               "Identificador “%s” não declarado neste escopo.", node->a);
                return error_type();
            }
            if (symbol->kind == SYMBOL_FUNCTION) {
                node_position(semantic, node, &line, &column);
                add_diagnostic(semantic, "SEM017", line, column,
                               "“%s” é uma função e não pode ser usada como variável.", node->a);
                return error_type();
            }
            return symbol->type;
        }
        case N_INDEX:
            return semantic_index(semantic, node);
        case N_CALL:
            return semantic_call(semantic, node);
        case N_ASSIGN: {
            SemType value_type = semantic_value(semantic, node->right, "expressão de atribuição");
            SemType target_type = semantic_target(semantic, node->left, "atribuição");
            if (!is_error(target_type) && !is_error(value_type) && !assignable(target_type, value_type)) {
                node_position(semantic, node->right, &line, &column);
                add_diagnostic(semantic, "SEM003", line, column,
                               "Não é possível atribuir %s a %s sem conversão permitida (destino “%s”; expressão “%s”).",
                               type_name(value_type), type_description(target_type),
                               node_text(semantic, node->left), node_text(semantic, node->right));
            }
            return target_type;
        }
        case N_UNARY: {
            SemType operand = semantic_value(semantic, node->left, "operando");
            if (is_error(operand)) return strcmp(node->a, "!") == 0 ? bool_type() : error_type();
            if (strcmp(node->a, "!") == 0) {
                if (strcmp(operand.base, "bool") != 0 || operand.array) {
                    node_position(semantic, node, &line, &column);
                    add_diagnostic(semantic, "SEM004", line, column,
                                   "Operador “!” exige operando bool; recebeu %s (expressão “%s”).",
                                   type_name(operand), node_text(semantic, node));
                    return error_type();
                }
                return bool_type();
            }
            if (!is_numeric(operand)) {
                node_position(semantic, node, &line, &column);
                add_diagnostic(semantic, "SEM004", line, column,
                               "Operador “%s” exige operando numérico (int ou float); recebeu %s (expressão “%s”).",
                               node->a, type_name(operand), node_text(semantic, node));
                return error_type();
            }
            return operand;
        }
        case N_BINARY: {
            SemType left = semantic_value(semantic, node->left, "operando");
            SemType right = semantic_value(semantic, node->right, "operando");
            const char *op = node->a;
            bool arithmetic = strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                              strcmp(op, "*") == 0 || strcmp(op, "/") == 0 || strcmp(op, "%") == 0;
            bool valid;
            const char *expected;
            SemType result = arithmetic ? error_type() : bool_type();
            if (is_error(left) || is_error(right)) return result;
            if (arithmetic) {
                valid = strcmp(op, "%") == 0
                    ? strcmp(left.base, "int") == 0 && strcmp(right.base, "int") == 0 && !left.array && !right.array
                    : is_numeric(left) && is_numeric(right);
                expected = strcmp(op, "%") == 0 ? "int" : "numéricos (int ou float)";
            } else if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
                       strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
                valid = (is_numeric(left) && is_numeric(right)) ||
                        (!left.array && !right.array && strcmp(left.base, "char") == 0 && strcmp(right.base, "char") == 0);
                expected = "numéricos (int ou float) ou ambos char";
            } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
                valid = (is_numeric(left) && is_numeric(right)) ||
                        (!left.array && !right.array && strcmp(left.base, right.base) == 0 &&
                         (strcmp(left.base, "bool") == 0 || strcmp(left.base, "char") == 0));
                expected = "de tipos compatíveis";
            } else {
                valid = strcmp(left.base, "bool") == 0 && strcmp(right.base, "bool") == 0 &&
                        !left.array && !right.array;
                expected = "bool";
            }
            if (!valid) {
                node_position(semantic, node, &line, &column);
                add_diagnostic(semantic, "SEM004", line, column,
                               "Operador “%s” exige operandos %s; recebeu %s e %s (expressão “%s”).",
                               op, expected, type_name(left), type_name(right), node_text(semantic, node));
                return result;
            }
            if (arithmetic) return strcmp(left.base, "float") == 0 || strcmp(right.base, "float") == 0
                ? float_type() : int_type();
            return bool_type();
        }
        case N_EXPRSTMT:
            return semantic_expr(semantic, node->left);
        default:
            return error_type();
    }
}

static SemType semantic_target(Semantic *semantic, Node *node, const char *context) {
    int line;
    int column;
    if (node != NULL && node->kind == N_INDEX) return semantic_index(semantic, node);
    if (node != NULL && node->kind == N_ID) {
        Symbol *symbol = lookup_symbol(semantic, node->a);
        if (symbol == NULL) {
            node_position(semantic, node, &line, &column);
            add_diagnostic(semantic, "SEM001", line, column,
                           "Identificador “%s” não declarado neste escopo.", node->a);
            return error_type();
        }
        if (symbol->kind == SYMBOL_FUNCTION) {
            node_position(semantic, node, &line, &column);
            add_diagnostic(semantic, "SEM013", line, column,
                           "Destino de %s não é atribuível; a função “%s” não designa uma variável ou elemento de vetor.",
                           context, node->a);
            return error_type();
        }
        if (symbol->type.array) {
            node_position(semantic, node, &line, &column);
            add_diagnostic(semantic, "SEM013", line, column,
                           "Destino de %s não é atribuível; o vetor “%s” não pode ser tratado como um todo; use um elemento (por exemplo, “%s[0]”).",
                           context, node->a, node->a);
            return error_type();
        }
        return symbol->type;
    }
    SemType type = semantic_expr(semantic, node);
    const char *description = "a expressão";
    if (node != NULL && node->kind == N_LIT) {
        if (strcmp(node->a, "int") == 0) description = "o literal inteiro";
        else if (strcmp(node->a, "real") == 0) description = "o literal real";
        else if (strcmp(node->a, "char") == 0) description = "o literal de caractere";
        else if (strcmp(node->a, "string") == 0) description = "o literal de cadeia";
        else if (strcmp(node->a, "bool") == 0) description = "o literal booleano";
    }
    node_position(semantic, node, &line, &column);
    add_diagnostic(semantic, "SEM013", line, column,
                   "Destino de %s não é atribuível; %s “%s” não designa uma variável ou elemento de vetor.",
                   context, description, node_text(semantic, node));
    (void)type;
    return error_type();
}

static long constant_integer(Node *node, bool *valid) {
    char *end;
    long value;
    if (node == NULL) {
        *valid = false;
        return 0;
    }
    if (node->kind == N_LIT && strcmp(node->a, "int") == 0) {
        value = strtol(node->b, &end, 10);
        *valid = *end == '\0';
        return value;
    }
    if (node->kind == N_UNARY && strcmp(node->a, "-") == 0) {
        value = constant_integer(node->left, valid);
        return *valid ? -value : 0;
    }
    *valid = false;
    return 0;
}

static void semantic_decl(Semantic *semantic, Node *node) {
    int line;
    int column;
    SemType type = make_type(node->a, false, 0);
    name_position(semantic, node, &line, &column);
    if (strcmp(node->a, "void") == 0) {
        add_diagnostic(semantic, "SEM014", line, column,
                       "A variável “%s” não pode ter tipo void.", node->b);
        type = error_type();
    }
    if (node->left != NULL) {
        semantic_value(semantic, node->left, "tamanho de vetor");
        bool valid;
        long size = constant_integer(node->left, &valid);
        if (!valid || size <= 0) {
            node_position(semantic, node->left, &line, &column);
            add_diagnostic(semantic, "SEM019", line, column,
                           "O tamanho do vetor “%s” deve ser uma constante inteira positiva (expressão “%s”).",
                           node->b, node_text(semantic, node->left));
        } else if (!is_error(type)) {
            type.array = true;
            type.size = size > 2147483647L ? 0 : (int)size;
        }
    }
    if (node->right != NULL) {
        SemType initializer = semantic_value(semantic, node->right, "inicializador de variável");
        if (!assignable(type, initializer)) {
            node_position(semantic, node->right, &line, &column);
            add_diagnostic(semantic, "SEM003", line, column,
                           "Não é possível atribuir %s a %s sem conversão permitida (destino “%s”; expressão “%s”).",
                           type_name(initializer), type_description(type), node->b,
                           node_text(semantic, node->right));
        }
    }
    declare_symbol(semantic, new_symbol(node->b, SYMBOL_VARIABLE, type, line, column));
}

static void semantic_condition(Semantic *semantic, Node *node, const char *keyword) {
    SemType type = semantic_value(semantic, node, "condição");
    int line;
    int column;
    if (!is_error(type) && (strcmp(type.base, "bool") != 0 || type.array)) {
        node_position(semantic, node, &line, &column);
        add_diagnostic(semantic, "SEM005", line, column,
                       "Condição de %s deve ter tipo bool; recebeu %s (expressão “%s”).",
                       keyword, type_name(type), node_text(semantic, node));
    }
}

static void semantic_return(Semantic *semantic, Node *node) {
    SemType return_type = semantic->function->type;
    if (node->left == NULL) {
        if (strcmp(return_type.base, "void") != 0) {
            int line;
            int column;
            node_position(semantic, node, &line, &column);
            add_diagnostic(semantic, "SEM010", line, column,
                           "A função “%s” deve retornar %s, mas o return não tem valor.",
                           semantic->function->name, type_name(return_type));
        }
        return;
    }
    SemType actual = semantic_value(semantic, node->left, "valor de retorno");
    int line;
    int column;
    if (strcmp(return_type.base, "void") == 0) {
        if (!is_error(actual)) {
            node_position(semantic, node->left, &line, &column);
            add_diagnostic(semantic, "SEM010", line, column,
                           "A função void “%s” não pode retornar valor (expressão “%s” do tipo %s).",
                           semantic->function->name, node_text(semantic, node->left), type_name(actual));
        }
    } else if (!assignable(return_type, actual)) {
        node_position(semantic, node->left, &line, &column);
        add_diagnostic(semantic, "SEM009", line, column,
                       "Retorno %s incompatível com o tipo %s da função “%s”; conversão implícita de %s para %s não permitida.",
                       type_name(actual), type_name(return_type), semantic->function->name,
                       type_name(actual), type_name(return_type));
    }
}

static bool semantic_branch(Semantic *semantic, Node *node, char **fall_reason) {
    if (node != NULL && node->kind == N_BLOCK) return semantic_stmt(semantic, node, fall_reason);
    open_scope(semantic);
    bool returns = semantic_stmt(semantic, node, fall_reason);
    close_scope(semantic);
    return returns;
}

static bool semantic_if(Semantic *semantic, Node *node, char **fall_reason) {
    char *condition_text;
    char *then_reason = NULL;
    char *else_reason = NULL;
    bool then_returns;
    bool else_returns;
    semantic_condition(semantic, node->left, "if");
    condition_text = node_text(semantic, node->left);
    then_returns = semantic_branch(semantic, node->right, &then_reason);
    if (node->third == NULL) {
        *fall_reason = format_string("o ramo em que “%s” é falso alcança o fim do corpo", condition_text);
        return false;
    }
    else_returns = semantic_branch(semantic, node->third, &else_reason);
    if (then_returns && else_returns) return true;
    if (!then_returns) {
        *fall_reason = then_reason != NULL && then_reason[0] != '\0' ? then_reason :
            format_string("o ramo em que “%s” é verdadeiro alcança o fim do corpo", condition_text);
    } else {
        *fall_reason = else_reason != NULL && else_reason[0] != '\0' ? else_reason :
            format_string("o ramo em que “%s” é falso alcança o fim do corpo", condition_text);
    }
    return false;
}

static bool semantic_stmt(Semantic *semantic, Node *node, char **fall_reason) {
    if (fall_reason != NULL) *fall_reason = copy_string("");
    if (node == NULL) return false;
    switch (node->kind) {
        case N_BLOCK: {
            open_scope(semantic);
            bool returns = false;
            char *reason = copy_string("");
            for (NodeList *item = node->items; item != NULL; item = item->next) {
                char *next_reason = NULL;
                if (semantic_stmt(semantic, item->node, &next_reason)) returns = true;
                else if (next_reason != NULL && next_reason[0] != '\0') reason = next_reason;
            }
            close_scope(semantic);
            if (fall_reason != NULL) *fall_reason = reason;
            return returns;
        }
        case N_VARDECL:
            semantic_decl(semantic, node);
            return false;
        case N_IF:
            return semantic_if(semantic, node, fall_reason);
        case N_WHILE:
            semantic_condition(semantic, node->left, "while");
            semantic_branch(semantic, node->right, NULL);
            return false;
        case N_RETURN:
            semantic_return(semantic, node);
            return true;
        case N_EXPRSTMT:
            semantic_expr(semantic, node->left);
            return false;
        default:
            return false;
    }
}

static int compare_diagnostics(const void *left_pointer, const void *right_pointer) {
    const Diagnostic *left = (const Diagnostic *)left_pointer;
    const Diagnostic *right = (const Diagnostic *)right_pointer;
    if (left->line != right->line) return left->line < right->line ? -1 : 1;
    if (left->column != right->column) return left->column < right->column ? -1 : 1;
    return left->sequence - right->sequence;
}

static void collect_function_signature(Semantic *semantic, Node *node) {
    int line;
    int column;
    int count = 0;
    for (Param *parameter = node->params; parameter != NULL; parameter = parameter->next) count++;
    name_position(semantic, node, &line, &column);
    Symbol *symbol = new_symbol(node->b, SYMBOL_FUNCTION, make_type(node->a, false, 0), line, column);
    if (symbol == NULL) return;
    symbol->parameter_count = count;
    if (count > 0) symbol->parameters = (SemType *)calloc((size_t)count, sizeof(SemType));
    int index = 0;
    for (Param *parameter = node->params; parameter != NULL; parameter = parameter->next) {
        SemType type = make_type(parameter->type, parameter->is_array != 0, 0);
        if (strcmp(parameter->type, "void") == 0) {
            add_diagnostic(semantic, "SEM014", parameter->line, parameter->column,
                           "O parâmetro “%s” não pode ter tipo void.", parameter->name);
            type = error_type();
        }
        if (symbol->parameters != NULL) symbol->parameters[index] = type;
        index++;
    }
    declare_symbol(semantic, symbol);
}

static void analyze_function(Semantic *semantic, Node *node) {
    Symbol *symbol = lookup_symbol(semantic, node->b);
    if (symbol == NULL || symbol->kind != SYMBOL_FUNCTION) return;
    semantic->function = symbol;
    open_scope(semantic);
    Param *parameter = node->params;
    int index = 0;
    while (parameter != NULL) {
        SemType type = symbol->parameters != NULL ? symbol->parameters[index] : error_type();
        declare_symbol(semantic, new_symbol(parameter->name, SYMBOL_PARAMETER, type,
                                             parameter->line, parameter->column));
        parameter = parameter->next;
        index++;
    }
    char *fall_reason = NULL;
    bool returns = true;
    if (node->left != NULL) {
        returns = false;
        for (NodeList *item = node->left->items; item != NULL; item = item->next) {
            char *next_reason = NULL;
            if (semantic_stmt(semantic, item->node, &next_reason)) returns = true;
            else if (next_reason != NULL && next_reason[0] != '\0') fall_reason = next_reason;
        }
    }
    if (!returns && strcmp(symbol->type.base, "void") != 0) {
        int line;
        int column;
        node_position(semantic, node, &line, &column);
        const char *reason = fall_reason != NULL && fall_reason[0] != '\0'
            ? fall_reason : "o corpo alcança o fim sem executar um return";
        add_diagnostic(semantic, "SEM011", line, column,
                       "A função “%s” pode terminar sem retornar %s; %s.",
                       symbol->name, type_name(symbol->type), reason);
    }
    close_scope(semantic);
    semantic->function = NULL;
}

static void analyze_program(Semantic *semantic, Node *program) {
    open_scope(semantic);
    for (NodeList *item = program->items; item != NULL; item = item->next) {
        if (item->node->kind == N_FUNCTION) collect_function_signature(semantic, item->node);
    }
    for (NodeList *item = program->items; item != NULL; item = item->next) {
        Node *node = item->node;
        if (node->kind == N_FUNCTION) {
            analyze_function(semantic, node);
        } else if (node->kind == N_VARDECL) {
            semantic_decl(semantic, node);
        } else {
            int line;
            int column;
            node_position(semantic, node, &line, &column);
            add_diagnostic(semantic, "SEM015", line, column,
                           "Apenas declarações são permitidas no escopo global; comandos devem estar dentro de uma função.");
        }
    }
    close_scope(semantic);
    qsort(semantic->diagnostics, (size_t)semantic->diagnostic_count,
          sizeof(Diagnostic), compare_diagnostics);
}

static int *make_line_offsets(const char *source, int size, int *line_count) {
    int count = 1;
    for (int index = 0; index < size; index++) if (source[index] == '\n') count++;
    int *offsets = (int *)malloc((size_t)count * sizeof(int));
    if (offsets == NULL) return NULL;
    offsets[0] = 0;
    int line = 1;
    for (int index = 0; index < size; index++) {
        if (source[index] == '\n' && line < count) offsets[line++] = index + 1;
    }
    *line_count = count;
    return offsets;
}

static void print_diagnostics(Semantic *semantic) {
    for (int index = 0; index < semantic->diagnostic_count; index++) {
        Diagnostic *diagnostic = &semantic->diagnostics[index];
        printf("%s — linha %d, coluna %d: %s\n", diagnostic->code,
               diagnostic->line, diagnostic->column, diagnostic->message);
    }
    if (semantic->diagnostic_count == 0) {
        printf("Análise semântica concluída: 0 erros; programa aceito.");
    } else {
        printf("Análise semântica concluída: %d erro%s; programa rejeitado.",
               semantic->diagnostic_count, semantic->diagnostic_count == 1 ? "" : "s");
    }
}

int main(int argc, char **argv) {
    int source_size;
    char *source;
    Parser parser = {0};
    Node *program;
    Semantic semantic = {0};
    if (argc != 2) {
        fprintf(stderr, "Uso: minic <arquivo.c>\n");
        return 2;
    }
    source = read_file(argv[1], &source_size);
    if (source == NULL) {
        fprintf(stderr, "Erro ao abrir arquivo: %s\n", argv[1]);
        return 2;
    }
    parser.tokens = lexer_tokenizar(source, source_size);
    program = parse_program(&parser);
    if (parser.failed) {
        token_list_liberar(&parser.tokens);
        free(source);
        return 1;
    }
    semantic.source = source;
    semantic.source_size = source_size;
    semantic.tokens = &parser.tokens;
    semantic.line_offsets = make_line_offsets(source, source_size, &semantic.line_count);
    analyze_program(&semantic, program);
    print_diagnostics(&semantic);
    int result = semantic.diagnostic_count == 0 ? 0 : 1;
    token_list_liberar(&parser.tokens);
    free(semantic.line_offsets);
    free(semantic.diagnostics);
    free(source);
    return result;
}
