#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Tipo {
    char base[20];       
    bool vetor;
    int tamanho;         
} Tipo;

typedef struct No {
    char kind[30];
    struct No** args;    
    int num_args;
    char* atom;          
    int linha;
    int coluna;
} No;

typedef struct Diagnostico {
    char nivel[10];      
    char codigo[50];
    char mensagem[256];
    int linha;
    int coluna;
} Diagnostico;

bool eh_erro(Tipo t) {
    return strcmp(t.base, "erro") == 0;
}

bool numerico(Tipo t) {
    return !t.vetor && (strcmp(t.base, "char") == 0 || 
                        strcmp(t.base, "int") == 0 || 
                        strcmp(t.base, "float") == 0);
}

bool inteiro(Tipo t) {
    return !t.vetor && (strcmp(t.base, "char") == 0 || 
                        strcmp(t.base, "int") == 0);
}

bool escalar(Tipo t) {
    return !t.vetor && (numerico(t) || strcmp(t.base, "bool") == 0);
}

bool atribuivel(Tipo destino, Tipo origem) {
    if (eh_erro(destino) || eh_erro(origem)) return true;
    if (strcmp(destino.base, "void") == 0 || strcmp(origem.base, "void") == 0) return false;
    
    if (destino.vetor || origem.vetor) {
        return destino.vetor && origem.vetor && (strcmp(destino.base, origem.base) == 0);
    }
    if (strcmp(destino.base, origem.base) == 0) {
        return strcmp(destino.base, "string") != 0;
    }
    
    if (strcmp(origem.base, "char") == 0 && (strcmp(destino.base, "int") == 0 || strcmp(destino.base, "float") == 0)) return true;
    if (strcmp(origem.base, "int") == 0 && strcmp(destino.base, "float") == 0) return true;
    if (strcmp(origem.base, "bool") == 0 && strcmp(destino.base, "int") == 0) return true;
    
    return false;
}

void imprimir_diagnostico(Diagnostico* d) {
    char* rotulo = strcmp(d->nivel, "erro") == 0 ? "Erro" : "Aviso";
    if (d->linha != 0) {
        printf("%s semântico na linha %d, coluna %d: %s\n", 
               rotulo, d->linha, d->coluna, d->mensagem);
    } else {
        printf("%s semântico: %s\n", rotulo, d->mensagem);
    }
}

// TODO: Implementar a resolução completa da AST e main(), conforme lógica de escopo discutida.
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    // O restante do código que já está aí...
    return 0;

}
