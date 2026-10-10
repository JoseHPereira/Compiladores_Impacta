#include "lexer_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    N_PROGRAM,
    N_FUNCTION,
    N_BLOCK,
    N_VARDECL,
    N_IF,
    N_WHILE,
    N_RETURN,
    N_EXPRSTMT,
    N_ASSIGN,
    N_BINARY,
    N_UNARY,
    N_CALL,
    N_INDEX,
    N_ID,
    N_LIT
} NodeKind;

typedef struct Node Node;
typedef struct NodeList NodeList;
typedef struct Param Param;

struct NodeList {
    Node *node;
    NodeList *next;
};

struct Param {
    char *type;
    char *name;
    int line;
    int column;
    int is_array;
    Param *next;
};

struct Node {
    NodeKind kind;
    char *a;
    char *b;
    Node *left;
    Node *right;
    Node *third;
    NodeList *items;
    Param *params;
    int token_start;
    int token_end;
};

typedef struct {
    TokenList tokens;
    int pos;
    int failed;
} Parser;

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static Node *new_node(NodeKind kind) {
    Node *node = (Node *)calloc(1, sizeof(Node));
    if (node != NULL) node->kind = kind;
    return node;
}

static Node *new_text_node(NodeKind kind, const char *a) {
    Node *node = new_node(kind);
    if (node != NULL) node->a = copy_text(a);
    return node;
}

static Node *new_binary(NodeKind kind, const char *op, Node *left, Node *right) {
    Node *node = new_node(kind);
    if (node != NULL) {
        node->a = copy_text(op);
        node->left = left;
        node->right = right;
    }
    return node;
}

static Node *set_span(Node *node, int token_start, int token_end) {
    if (node != NULL) {
        node->token_start = token_start;
        node->token_end = token_end;
    }
    return node;
}

static NodeList *append_node(NodeList **head, NodeList **tail, Node *node) {
    NodeList *item = (NodeList *)malloc(sizeof(NodeList));
    if (item == NULL) return NULL;
    item->node = node;
    item->next = NULL;
    if (*tail != NULL) (*tail)->next = item;
    else *head = item;
    *tail = item;
    return item;
}

static Param *append_param(Param **head, Param **tail, char *type, char *name,
                           int line, int column, int is_array) {
    Param *param = (Param *)malloc(sizeof(Param));
    if (param == NULL) return NULL;
    param->type = type;
    param->name = name;
    param->line = line;
    param->column = column;
    param->is_array = is_array;
    param->next = NULL;
    if (*tail != NULL) (*tail)->next = param;
    else *head = param;
    *tail = param;
    return param;
}

static Token *current(Parser *parser) {
    return &parser->tokens.itens[parser->pos];
}

static int check(Parser *parser, const char *type) {
    return strcmp(current(parser)->tipo, type) == 0;
}

static Token *advance_token(Parser *parser) {
    Token *token = current(parser);
    if (parser->pos < parser->tokens.quantidade - 1) parser->pos++;
    return token;
}

static const char *friendly_name(const char *type) {
    if (strcmp(type, "SEMICOLON") == 0) return "';'";
    if (strcmp(type, "COMMA") == 0) return "','";
    if (strcmp(type, "LPAREN") == 0) return "'('";
    if (strcmp(type, "RPAREN") == 0) return "')'";
    if (strcmp(type, "LBRACE") == 0) return "'{'";
    if (strcmp(type, "RBRACE") == 0) return "'}'";
    if (strcmp(type, "LBRACKET") == 0) return "'['";
    if (strcmp(type, "RBRACKET") == 0) return "']'";
    if (strcmp(type, "ASSIGN") == 0) return "'='";
    if (strcmp(type, "IDENT") == 0) return "identificador";
    if (strcmp(type, "EOF") == 0) return "fim de arquivo";
    return type;
}

static void syntax_error(Parser *parser, const char *expected) {
    Token *token = current(parser);
    const char *lexeme = token->lexema[0] != '\0' ? token->lexema : "<fim de arquivo>";
    fprintf(stderr, "Erro sintático: esperado %s, encontrado '%s' (%s) na linha %d, coluna %d\n",
            expected, lexeme, token->tipo, token->linha, token->coluna);
    parser->failed = 1;
}

static Token *expect(Parser *parser, const char *type, const char *description) {
    if (parser->failed) return current(parser);
    if (check(parser, type)) return advance_token(parser);
    syntax_error(parser, description != NULL ? description : friendly_name(type));
    return current(parser);
}

static int is_type(const char *type) {
    return strcmp(type, "INT") == 0 || strcmp(type, "FLOAT") == 0 ||
           strcmp(type, "BOOL") == 0 || strcmp(type, "CHAR") == 0 ||
           strcmp(type, "VOID") == 0;
}

static char *parse_type(Parser *parser);
static Node *parse_expr(Parser *parser);
static Node *parse_stmt(Parser *parser);
static Node *parse_block(Parser *parser);

static char *parse_type(Parser *parser) {
    if (is_type(current(parser)->tipo)) return copy_text(advance_token(parser)->lexema);
    syntax_error(parser, "tipo (int, float, bool, char ou void)");
    return copy_text("");
}

static Node *parse_var_rest(Parser *parser, char *type, char *name) {
    int token_start = parser->pos - 2;
    Node *node = new_node(N_VARDECL);
    if (node == NULL) return NULL;
    node->a = type;
    node->b = name;
    if (check(parser, "LBRACKET")) {
        advance_token(parser);
        node->left = parse_expr(parser);
        expect(parser, "RBRACKET", NULL);
    }
    if (check(parser, "ASSIGN")) {
        advance_token(parser);
        node->right = parse_expr(parser);
    }
    expect(parser, "SEMICOLON", NULL);
    return set_span(node, token_start, parser->pos);
}

static Param *parse_param(Parser *parser) {
    char *type = parse_type(parser);
    Token *name_token = current(parser);
    char *name = copy_text(expect(parser, "IDENT", "identificador")->lexema);
    int is_array = 0;
    if (check(parser, "LBRACKET")) {
        is_array = 1;
        advance_token(parser);
        if (!check(parser, "RBRACKET")) parse_expr(parser);
        expect(parser, "RBRACKET", NULL);
    }
    Param *param = (Param *)malloc(sizeof(Param));
    if (param != NULL) {
        param->type = type;
        param->name = name;
        param->line = name_token->linha;
        param->column = name_token->coluna;
        param->is_array = is_array;
        param->next = NULL;
    }
    return param;
}

static Node *parse_function(Parser *parser, char *type, char *name) {
    int token_start = parser->pos - 2;
    Node *node = new_node(N_FUNCTION);
    Param *head = NULL;
    Param *tail = NULL;
    expect(parser, "LPAREN", NULL);
    if (!check(parser, "RPAREN")) {
        Param *param = parse_param(parser);
        if (param != NULL) append_param(&head, &tail, param->type, param->name,
                        param->line, param->column, param->is_array);
        free(param);
        while (check(parser, "COMMA")) {
            advance_token(parser);
            param = parse_param(parser);
            if (param != NULL) append_param(&head, &tail, param->type, param->name,
                                            param->line, param->column, param->is_array);
            free(param);
        }
    }
    expect(parser, "RPAREN", "tipo de parâmetro ou ')'" );
    if (node != NULL) {
        node->a = type;
        node->b = name;
        node->params = head;
        node->left = parse_block(parser);
    } else {
        parse_block(parser);
    }
    return set_span(node, token_start, parser->pos);
}

static Node *parse_top_declaration(Parser *parser) {
    if (is_type(current(parser)->tipo)) {
        char *type = parse_type(parser);
        char *name = copy_text(expect(parser, "IDENT", "identificador")->lexema);
        if (check(parser, "LPAREN")) return parse_function(parser, type, name);
        return parse_var_rest(parser, type, name);
    }
    int token_start = parser->pos;
    Node *expr = parse_expr(parser);
    expect(parser, "SEMICOLON", NULL);
    Node *node = new_node(N_EXPRSTMT);
    if (node != NULL) node->left = expr;
    return set_span(node, token_start, parser->pos);
}

static Node *parse_print_or_read(Parser *parser, const char *token_type, const char *name) {
    int token_start = parser->pos;
    Node *node = new_node(N_EXPRSTMT);
    Node *call = new_node(N_CALL);
    NodeList *head = NULL;
    NodeList *tail = NULL;
    expect(parser, token_type, NULL);
    expect(parser, "LPAREN", NULL);
    if (!check(parser, "RPAREN")) {
        append_node(&head, &tail, parse_expr(parser));
        while (check(parser, "COMMA")) {
            advance_token(parser);
            append_node(&head, &tail, parse_expr(parser));
        }
    }
    expect(parser, "RPAREN", "argumento ou ')' ");
    expect(parser, "SEMICOLON", NULL);
    if (call != NULL) {
        call->left = new_text_node(N_ID, name);
        call->items = head;
        set_span(call->left, token_start, token_start + 1);
        set_span(call, token_start, parser->pos);
    }
    if (node != NULL) node->left = call;
    return set_span(node, token_start, parser->pos);
}

static Node *parse_if(Parser *parser) {
    int token_start = parser->pos;
    Node *node = new_node(N_IF);
    expect(parser, "IF", NULL);
    expect(parser, "LPAREN", NULL);
    Node *condition = parse_expr(parser);
    expect(parser, "RPAREN", NULL);
    Node *then_node = parse_stmt(parser);
    Node *else_node = NULL;
    if (check(parser, "ELSE")) {
        advance_token(parser);
        else_node = parse_stmt(parser);
    }
    if (node != NULL) {
        node->left = condition;
        node->right = then_node;
        node->third = else_node;
    }
    return set_span(node, token_start, parser->pos);
}

static Node *parse_while(Parser *parser) {
    int token_start = parser->pos;
    Node *node = new_node(N_WHILE);
    expect(parser, "WHILE", NULL);
    expect(parser, "LPAREN", NULL);
    Node *condition = parse_expr(parser);
    expect(parser, "RPAREN", NULL);
    if (node != NULL) {
        node->left = condition;
        node->right = parse_stmt(parser);
    } else {
        parse_stmt(parser);
    }
    return set_span(node, token_start, parser->pos);
}

static Node *parse_return(Parser *parser) {
    int token_start = parser->pos;
    Node *node = new_node(N_RETURN);
    expect(parser, "RETURN", NULL);
    if (!check(parser, "SEMICOLON")) {
        if (node != NULL) node->left = parse_expr(parser);
        else parse_expr(parser);
    }
    expect(parser, "SEMICOLON", NULL);
    return set_span(node, token_start, parser->pos);
}

static Node *parse_stmt(Parser *parser) {
    if (check(parser, "LBRACE")) return parse_block(parser);
    if (check(parser, "IF")) return parse_if(parser);
    if (check(parser, "WHILE")) return parse_while(parser);
    if (check(parser, "RETURN")) return parse_return(parser);
    if (check(parser, "PRINT")) return parse_print_or_read(parser, "PRINT", "print");
    if (check(parser, "READ")) return parse_print_or_read(parser, "READ", "read");
    if (is_type(current(parser)->tipo)) {
        char *type = parse_type(parser);
        char *name = copy_text(expect(parser, "IDENT", "identificador")->lexema);
        return parse_var_rest(parser, type, name);
    }
    int token_start = parser->pos;
    Node *node = new_node(N_EXPRSTMT);
    if (node != NULL) node->left = parse_expr(parser);
    else parse_expr(parser);
    expect(parser, "SEMICOLON", NULL);
    return set_span(node, token_start, parser->pos);
}

static Node *parse_block(Parser *parser) {
    int token_start = parser->pos;
    Node *node = new_node(N_BLOCK);
    NodeList *head = NULL;
    NodeList *tail = NULL;
    expect(parser, "LBRACE", NULL);
    while (!check(parser, "RBRACE") && !check(parser, "EOF") && !parser->failed) {
        append_node(&head, &tail, parse_stmt(parser));
    }
    expect(parser, "RBRACE", NULL);
    if (node != NULL) node->items = head;
    return set_span(node, token_start, parser->pos);
}

static Node *parse_assign(Parser *parser);

static Node *parse_binary(Parser *parser, Node *(*next)(Parser *), const char *const *operators, int count) {
    Node *left = next(parser);
    while (!parser->failed) {
        int found = 0;
        for (int i = 0; i < count; i++) {
            if (check(parser, operators[i])) {
                found = 1;
                break;
            }
        }
        if (!found) break;
        char *op = copy_text(advance_token(parser)->lexema);
        Node *right = next(parser);
        int token_start = left != NULL ? left->token_start : parser->pos;
        int token_end = parser->pos;
        left = set_span(new_binary(N_BINARY, op, left, right), token_start, token_end);
        free(op);
    }
    return left;
}

static Node *parse_or(Parser *parser);
static Node *parse_and(Parser *parser);
static Node *parse_equality(Parser *parser);
static Node *parse_relational(Parser *parser);
static Node *parse_additive(Parser *parser);
static Node *parse_mult(Parser *parser);
static Node *parse_unary(Parser *parser);
static Node *parse_postfix(Parser *parser);
static Node *parse_primary(Parser *parser);

static Node *parse_or(Parser *parser) {
    static const char *ops[] = {"OR"};
    return parse_binary(parser, parse_and, ops, 1);
}

static Node *parse_and(Parser *parser) {
    static const char *ops[] = {"AND"};
    return parse_binary(parser, parse_equality, ops, 1);
}

static Node *parse_equality(Parser *parser) {
    static const char *ops[] = {"EQ", "NE"};
    return parse_binary(parser, parse_relational, ops, 2);
}

static Node *parse_relational(Parser *parser) {
    static const char *ops[] = {"LT", "GT", "LE", "GE"};
    return parse_binary(parser, parse_additive, ops, 4);
}

static Node *parse_additive(Parser *parser) {
    static const char *ops[] = {"PLUS", "MINUS"};
    return parse_binary(parser, parse_mult, ops, 2);
}

static Node *parse_mult(Parser *parser) {
    static const char *ops[] = {"STAR", "SLASH", "PERCENT"};
    return parse_binary(parser, parse_unary, ops, 3);
}

static Node *parse_unary(Parser *parser) {
    if (check(parser, "MINUS") || check(parser, "NOT")) {
        int token_start = parser->pos;
        char *op = copy_text(advance_token(parser)->lexema);
        Node *node = new_binary(N_UNARY, op, parse_unary(parser), NULL);
        free(op);
        return set_span(node, token_start, parser->pos);
    }
    return parse_postfix(parser);
}

static Node *parse_postfix(Parser *parser) {
    Node *expr = parse_primary(parser);
    while (!parser->failed) {
        if (check(parser, "LBRACKET")) {
            int token_start = expr != NULL ? expr->token_start : parser->pos;
            advance_token(parser);
            Node *index = parse_expr(parser);
            expect(parser, "RBRACKET", NULL);
            expr = set_span(new_binary(N_INDEX, "", expr, index), token_start, parser->pos);
        } else if (check(parser, "LPAREN")) {
            int token_start = expr != NULL ? expr->token_start : parser->pos;
            NodeList *head = NULL;
            NodeList *tail = NULL;
            advance_token(parser);
            if (!check(parser, "RPAREN")) {
                append_node(&head, &tail, parse_expr(parser));
                while (check(parser, "COMMA")) {
                    advance_token(parser);
                    append_node(&head, &tail, parse_expr(parser));
                }
            }
            expect(parser, "RPAREN", "argumento ou ')' ");
            Node *call = new_node(N_CALL);
            if (call != NULL) {
                call->left = expr;
                call->items = head;
            }
            expr = set_span(call, token_start, parser->pos);
        } else {
            break;
        }
    }
    return expr;
}

static Node *parse_primary(Parser *parser) {
    int token_start = parser->pos;
    Token *token = current(parser);
    if (strcmp(token->tipo, "INT_LIT") == 0) {
        advance_token(parser);
        Node *node = new_node(N_LIT);
        if (node != NULL) { node->a = copy_text("int"); node->b = copy_text(token->lexema); }
        return set_span(node, token_start, parser->pos);
    }
    if (strcmp(token->tipo, "FLOAT_LIT") == 0) {
        advance_token(parser);
        Node *node = new_node(N_LIT);
        if (node != NULL) { node->a = copy_text("real"); node->b = copy_text(token->lexema); }
        return set_span(node, token_start, parser->pos);
    }
    if (strcmp(token->tipo, "CHAR_LIT") == 0) {
        advance_token(parser);
        Node *node = new_node(N_LIT);
        if (node != NULL) { node->a = copy_text("char"); node->b = copy_text(token->atributo); }
        return set_span(node, token_start, parser->pos);
    }
    if (strcmp(token->tipo, "STRING_LIT") == 0) {
        advance_token(parser);
        Node *node = new_node(N_LIT);
        if (node != NULL) { node->a = copy_text("string"); node->b = copy_text(token->atributo); }
        return set_span(node, token_start, parser->pos);
    }
    if (strcmp(token->tipo, "TRUE") == 0 || strcmp(token->tipo, "FALSE") == 0) {
        advance_token(parser);
        Node *node = new_node(N_LIT);
        if (node != NULL) { node->a = copy_text("bool"); node->b = copy_text(token->tipo[0] == 'T' ? "true" : "false"); }
        return set_span(node, token_start, parser->pos);
    }
    if (strcmp(token->tipo, "IDENT") == 0) {
        advance_token(parser);
        return set_span(new_text_node(N_ID, token->lexema), token_start, parser->pos);
    }
    if (strcmp(token->tipo, "LPAREN") == 0) {
        advance_token(parser);
        Node *expr = parse_expr(parser);
        expect(parser, "RPAREN", NULL);
        return expr;
    }
    syntax_error(parser, "expressão (identificador, literal, '(' ou operador unário)");
    return new_node(N_ID);
}

static Node *parse_assign(Parser *parser) {
    Node *left = parse_or(parser);
    if (check(parser, "ASSIGN")) {
        advance_token(parser);
        Node *right = parse_assign(parser);
        int token_start = left != NULL ? left->token_start : parser->pos;
        return set_span(new_binary(N_ASSIGN, "", left, right), token_start, parser->pos);
    }
    return left;
}

static Node *parse_expr(Parser *parser) {
    return parse_assign(parser);
}

static Node *parse_program(Parser *parser) {
    int token_start = parser->pos;
    Node *node = new_node(N_PROGRAM);
    NodeList *head = NULL;
    NodeList *tail = NULL;
    while (!check(parser, "EOF") && !parser->failed) {
        append_node(&head, &tail, parse_top_declaration(parser));
    }
    if (node != NULL) node->items = head;
    return set_span(node, token_start, parser->pos);
}

static void print_list(NodeList *items, const char *separator);
static void print_node(const Node *node);

static void print_node(const Node *node) {
    if (node == NULL) return;
    switch (node->kind) {
        case N_PROGRAM:
            printf("Program("); print_list(node->items, ", "); printf(")"); break;
        case N_FUNCTION: {
            printf("Function(%s %s(", node->a, node->b);
            for (Param *param = node->params; param != NULL; param = param->next) {
                if (param != node->params) printf(",");
                printf("%s %s", param->type, param->name);
            }
            printf(") "); print_node(node->left); printf(")"); break;
        }
        case N_BLOCK: printf("Block("); print_list(node->items, ", "); printf(")"); break;
        case N_VARDECL:
            if (node->left != NULL) { printf("VarDecl(%s %s size=", node->a, node->b); print_node(node->left); printf(")"); }
            else if (node->right != NULL) { printf("VarDecl(%s %s=", node->a, node->b); print_node(node->right); printf(")"); }
            else printf("VarDecl(%s %s)", node->a, node->b);
            break;
        case N_IF: printf("If("); print_node(node->left); printf(","); print_node(node->right); printf(","); if (node->third) print_node(node->third); else printf("NULL"); printf(")"); break;
        case N_WHILE: printf("While("); print_node(node->left); printf(","); print_node(node->right); printf(")"); break;
        case N_RETURN: printf("Return("); if (node->left) print_node(node->left); else printf("NULL"); printf(")"); break;
        case N_EXPRSTMT: printf("ExprStmt("); print_node(node->left); printf(")"); break;
        case N_ASSIGN: printf("Assign("); print_node(node->left); printf(","); print_node(node->right); printf(")"); break;
        case N_BINARY: printf("Binary(%s,", node->a); print_node(node->left); printf(","); print_node(node->right); printf(")"); break;
        case N_UNARY: printf("Unary(%s,", node->a); print_node(node->left); printf(")"); break;
        case N_CALL: printf("Call("); print_node(node->left); if (node->items) printf(","); print_list(node->items, ","); printf(")"); break;
        case N_INDEX: printf("Index("); print_node(node->left); printf(","); print_node(node->right); printf(")"); break;
        case N_ID: printf("Id(%s)", node->a); break;
        case N_LIT: printf("Lit(%s,%s)", node->a, node->b); break;
    }
}

static void print_list(NodeList *items, const char *separator) {
    for (NodeList *item = items; item != NULL; item = item->next) {
        if (item != items) printf("%s", separator);
        print_node(item->node);
    }
}

static char *read_file(const char *path, int *size) {
    FILE *file = fopen(path, "rb");
    char *buffer;
    long length;
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    buffer = (char *)malloc((size_t)length + 1);
    if (buffer == NULL) { fclose(file); return NULL; }
    if (fread(buffer, 1, (size_t)length, file) != (size_t)length) { free(buffer); fclose(file); return NULL; }
    buffer[length] = '\0';
    fclose(file);
    *size = (int)length;
    return buffer;
}

#ifndef PARSER_SEMANTIC_LIBRARY
int main(int argc, char **argv) {
    char *code;
    int size;
    Parser parser;
    Node *program;
    if (argc < 2) {
        fprintf(stderr, "uso: ./parser <arquivo.c>\n");
        return 1;
    }
    code = read_file(argv[1], &size);
    if (code == NULL) {
        fprintf(stderr, "erro ao abrir arquivo: %s\n", argv[1]);
        return 1;
    }
    parser.tokens = lexer_tokenizar(code, size);
    parser.pos = 0;
    parser.failed = 0;
    program = parse_program(&parser);
    if (parser.failed) {
        token_list_liberar(&parser.tokens);
        free(code);
        return 1;
    }
    print_node(program);
    putchar('\n');
    token_list_liberar(&parser.tokens);
    free(code);
    return 0;
}
#endif
