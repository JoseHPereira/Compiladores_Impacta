#!/usr/bin/env python3
"""
Analisador semântico do MiniC.

Entrada : AST do programa (ver README_SEMANTICO.md para o contrato de nós).
Saída   : lista de diagnósticos (erros e avisos) com linha/coluna.

Uso como biblioteca:
    from semantic import AnalisadorSemantico
    diagnosticos = AnalisadorSemantico().analisar(raiz)

Uso pela linha de comando (AST em S-expressão):
    python semantic.py arvore.sexp [--json] [--strict]

Código de saída: 0 = sem erros, 1 = há erros (ou avisos, com --strict).
"""

from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass, field


# ============================================================
# 1. AST
# ============================================================

class ErroAST(Exception):
    """AST malformada (problema do produtor da AST, não do programa)."""


@dataclass
class No:
    kind: str
    args: list = field(default_factory=list)   # No ou str (átomo)
    linha: int = 0
    coluna: int = 0


_TOKEN = re.compile(r'\(|\)|"(?:\\.|[^"\\])*"|[^\s()]+')
_CABECA = re.compile(r'([^@]+)(?:@(\d+):(\d+))?')


def ler_sexpr(texto: str) -> No:
    """
    Lê uma AST em S-expressão. A posição de um nó é opcional e vem
    colada ao nome:  (binary@3:7 + (id a) (int 1))
    """
    tokens = _TOKEN.findall(texto)
    pos = 0

    def ler():
        nonlocal pos
        if pos >= len(tokens):
            raise ErroAST("fim inesperado da S-expressão")
        tok = tokens[pos]
        pos += 1

        if tok == ")":
            raise ErroAST("')' inesperado")

        if tok == "(":
            if pos >= len(tokens) or tokens[pos] in ("(", ")"):
                raise ErroAST("nó sem nome após '('")
            m = _CABECA.fullmatch(tokens[pos])
            pos += 1
            no = No(m.group(1), [], int(m.group(2) or 0), int(m.group(3) or 0))
            while pos < len(tokens) and tokens[pos] != ")":
                no.args.append(ler())
            if pos >= len(tokens):
                raise ErroAST("faltou ')'")
            pos += 1
            return no

        if tok.startswith('"'):
            return (tok[1:-1].replace('\\"', '"').replace("\\n", "\n")
                    .replace("\\t", "\t").replace("\\\\", "\\"))
        return tok

    raiz = ler()
    if pos != len(tokens):
        raise ErroAST("texto sobrando após a AST")
    if not isinstance(raiz, No):
        raise ErroAST("a AST deve ser uma lista")
    return raiz


# ============================================================
# 2. TIPOS
# ============================================================

@dataclass(frozen=True)
class Tipo:
    base: str
    vetor: bool = False
    tamanho: int | None = None

    def __str__(self):
        return self.base + ("[]" if self.vetor else "")


ERRO = Tipo("erro")        # tipo "poison": evita erros em cascata
INT, FLOAT, BOOL, CHAR = Tipo("int"), Tipo("float"), Tipo("bool"), Tipo("char")
STRING, VOID = Tipo("string"), Tipo("void")

TIPOS_DECL = {"int", "float", "bool", "char"}
NUMERICOS = {"char", "int", "float"}
INTEIROS = {"char", "int"}

# Conversões implícitas permitidas (origem -> destino). Ajuste aqui
# para tornar a linguagem mais (ou menos) permissiva.
CONVERSOES = {
    ("char", "int"), ("char", "float"),
    ("int", "float"),
    ("bool", "int"),
}


def eh_erro(t: Tipo) -> bool:
    return t.base == "erro"


def numerico(t: Tipo) -> bool:
    return not t.vetor and t.base in NUMERICOS


def inteiro(t: Tipo) -> bool:
    return not t.vetor and t.base in INTEIROS


def escalar(t: Tipo) -> bool:
    """Pode ser usado como condição (bool ou numérico)."""
    return not t.vetor and (t.base in NUMERICOS or t.base == "bool")


def atribuivel(destino: Tipo, origem: Tipo) -> bool:
    if eh_erro(destino) or eh_erro(origem):
        return True
    if destino.base == "void" or origem.base == "void":
        return False
    if destino.vetor or origem.vetor:
        return destino.vetor and origem.vetor and destino.base == origem.base
    if destino.base == origem.base:
        return destino.base != "string"
    return (origem.base, destino.base) in CONVERSOES


# ============================================================
# 3. SÍMBOLOS E DIAGNÓSTICOS
# ============================================================

@dataclass
class Simbolo:
    nome: str
    categoria: str                 # "var" | "param" | "func"
    tipo: Tipo                     # tipo da variável ou retorno da função
    linha: int = 0
    coluna: int = 0
    parametros: list = field(default_factory=list)   # [(nome, Tipo, No)]
    inicializado: bool = False
    usado: bool = False

    @property
    def tipos_parametros(self):
        return [t for _, t, _ in self.parametros]


@dataclass
class Diagnostico:
    nivel: str          # "erro" | "aviso"
    codigo: str
    mensagem: str
    linha: int = 0
    coluna: int = 0

    def __str__(self):
        rotulo = "Erro" if self.nivel == "erro" else "Aviso"
        local = f" na linha {self.linha}, coluna {self.coluna}" if self.linha else ""
        return f"{rotulo} semântico{local}: {self.mensagem}"

    def para_dict(self):
        return {"level": self.nivel, "code": self.codigo,
                "message": self.mensagem, "line": self.linha, "column": self.coluna}


EXPR_KINDS = {"int", "float", "bool", "char", "str", "id", "index",
              "call", "unary", "binary", "assign"}
OP_ARIT = {"+", "-", "*", "/", "%"}
OP_REL = {"<", ">", "<=", ">="}
OP_IGUAL = {"==", "!="}
OP_LOG = {"&&", "||"}


def valor_constante(no):
    """Valor inteiro de um literal (aceita sinal), ou None."""
    if not isinstance(no, No):
        return None
    if no.kind == "int":
        try:
            return int(no.args[0])
        except (ValueError, IndexError):
            return None
    if no.kind == "unary" and len(no.args) == 2 and no.args[0] in ("-", "+"):
        v = valor_constante(no.args[1])
        if v is not None:
            return -v if no.args[0] == "-" else v
    return None


# ============================================================
# 4. ANALISADOR
# ============================================================

class AnalisadorSemantico:

    def __init__(self, avisos: bool = True):
        self.avisos = avisos
        self.diagnosticos: list[Diagnostico] = []
        self.escopos: list[dict[str, Simbolo]] = []
        self.funcao: Simbolo | None = None
        self.lacos = 0

    # ---------------- infraestrutura ----------------

    def _diag(self, nivel, codigo, msg, no):
        self.diagnosticos.append(Diagnostico(
            nivel, codigo, msg,
            getattr(no, "linha", 0), getattr(no, "coluna", 0)))

    def erro(self, codigo, msg, no):
        self._diag("erro", codigo, msg, no)

    def aviso(self, codigo, msg, no):
        if self.avisos:
            self._diag("aviso", codigo, msg, no)

    def abrir(self):
        self.escopos.append({})

    def fechar(self):
        escopo = self.escopos.pop()
        if self.escopos:                       # não avisa no escopo global
            for s in escopo.values():
                if s.categoria == "var" and not s.usado:
                    self.aviso("nao_usada",
                               f"variável '{s.nome}' declarada mas nunca usada",
                               No("?", [], s.linha, s.coluna))

    def declarar(self, sim: Simbolo, no) -> bool:
        atual = self.escopos[-1]
        if sim.nome in atual:
            ant = atual[sim.nome]
            self.erro("redeclaracao",
                      f"'{sim.nome}' já foi declarado neste escopo (linha {ant.linha})", no)
            return False
        atual[sim.nome] = sim
        return True

    def buscar(self, nome: str) -> Simbolo | None:
        for escopo in reversed(self.escopos):
            if nome in escopo:
                return escopo[nome]
        return None

    # ---------------- programa ----------------

    def analisar(self, raiz) -> list[Diagnostico]:
        self.diagnosticos = []
        self.escopos = []
        self.funcao = None
        self.lacos = 0

        if not isinstance(raiz, No) or raiz.kind != "program":
            self.erro("ast_malformada", "a raiz da AST deve ser 'program'", raiz)
            return self.diagnosticos

        self.abrir()                                  # escopo global

        # Passada 1: registra todas as funções (permite chamada antes da definição
        # e recursão mútua).
        assinaturas = {}
        for d in raiz.args:
            if isinstance(d, No) and d.kind == "func":
                sim = self._assinatura(d)
                self.declarar(sim, d)
                assinaturas[id(d)] = sim

        # Passada 2: declarações globais e corpos, na ordem do arquivo.
        for d in raiz.args:
            if not isinstance(d, No):
                self.erro("ast_malformada", "átomo solto no nível do programa", raiz)
            elif d.kind == "func":
                self._corpo_funcao(d, assinaturas[id(d)])
            elif d.kind in ("var", "array"):
                self._decl(d, global_=True)
            else:
                self.erro("declaracao_invalida",
                          "apenas declarações são permitidas no escopo global", d)

        self._verificar_main(raiz)
        self.fechar()

        self.diagnosticos.sort(key=lambda d: (d.linha, d.coluna))
        return self.diagnosticos

    def _verificar_main(self, raiz):
        main = self.escopos[0].get("main")
        if main is None or main.categoria != "func":
            self.erro("main_ausente", "função 'main' não definida", raiz)
            return
        if main.tipo.base != "int":
            self.erro("main_tipo",
                      f"'main' deve retornar int, mas retorna {main.tipo}",
                      No("?", [], main.linha, main.coluna))
        if main.parametros:
            self.erro("main_parametros", "'main' não deve ter parâmetros",
                      No("?", [], main.linha, main.coluna))

    # ---------------- funções ----------------

    def _assinatura(self, d: No) -> Simbolo:
        tipo, nome, params = d.args[0], d.args[1], d.args[2]
        if tipo in TIPOS_DECL or tipo == "void":
            tret = Tipo(tipo)
        else:
            self.erro("tipo_invalido", f"tipo de retorno desconhecido '{tipo}'", d)
            tret = ERRO

        parametros = []
        for p in params.args:
            base = p.args[0]
            if base not in TIPOS_DECL:
                self.erro("parametro_invalido",
                          f"parâmetro '{p.args[1]}' não pode ter tipo '{base}'", p)
                t = ERRO
            else:
                t = Tipo(base, vetor=(p.kind == "param-array"))
            parametros.append((p.args[1], t, p))

        return Simbolo(nome, "func", tret, d.linha, d.coluna,
                       parametros=parametros, inicializado=True, usado=True)

    def _corpo_funcao(self, d: No, sim: Simbolo):
        self.funcao = sim
        self.abrir()
        for nome, t, pno in sim.parametros:
            self.declarar(Simbolo(nome, "param", t, pno.linha, pno.coluna,
                                  inicializado=True, usado=True), pno)

        retorna = self._bloco(d.args[3], novo_escopo=False)
        if not retorna and sim.tipo.base not in ("void", "erro"):
            self.aviso("retorno_ausente",
                       f"função '{sim.nome}' pode terminar sem retornar um valor", d)

        self.fechar()
        self.funcao = None

    # ---------------- declarações ----------------

    def _decl(self, no: No, global_: bool = False):
        tipo, nome = no.args[0], no.args[1]

        if tipo in TIPOS_DECL:
            base = Tipo(tipo)
        else:
            motivo = "void" if tipo == "void" else f"'{tipo}'"
            self.erro("variavel_void" if tipo == "void" else "tipo_invalido",
                      f"variável '{nome}' não pode ter tipo {motivo}", no)
            base = ERRO

        if no.kind == "var":
            tv = base
            ini = no.args[2] if len(no.args) > 2 else None
            if ini is not None:
                ti = self.valor(ini)
                if not atribuivel(tv, ti):
                    self.erro("atribuicao_incompativel",
                              f"não é possível inicializar '{nome}' ({tv}) com valor do tipo {ti}",
                              no)
            inicializado = ini is not None or global_

        else:  # array
            tamanho = valor_constante(no.args[2])
            if tamanho is None or tamanho <= 0:
                self.erro("tamanho_invalido",
                          f"tamanho do vetor '{nome}' deve ser uma constante inteira positiva", no)
                tamanho = None
            tv = Tipo(base.base, True, tamanho) if not eh_erro(base) else ERRO
            lista = no.args[3] if len(no.args) > 3 else None
            if lista is not None:
                if lista.kind != "list":
                    self.erro("inicializador_invalido",
                              f"vetor '{nome}' só pode ser inicializado com uma lista", lista)
                else:
                    for e in lista.args:
                        te = self.valor(e)
                        if not atribuivel(base, te):
                            self.erro("atribuicao_incompativel",
                                      f"elemento do tipo {te} em vetor de {base}", e)
                    if tamanho is not None and len(lista.args) > tamanho:
                        self.erro("inicializador_excede",
                                  f"{len(lista.args)} valores para vetor de tamanho {tamanho}",
                                  lista)
            inicializado = True

        # a declaração vale depois do inicializador (int x = x; olha o x de fora)
        self.declarar(Simbolo(nome, "var", tv, no.linha, no.coluna,
                              inicializado=inicializado), no)

    # ---------------- comandos ----------------
    # Cada comando devolve True se garante retorno em todos os caminhos.

    def _bloco(self, no: No, novo_escopo: bool = True) -> bool:
        if novo_escopo:
            self.abrir()
        retorna = False
        for s in no.args:
            if self.stmt(s):
                retorna = True
        if novo_escopo:
            self.fechar()
        return retorna

    def _ramo(self, no) -> bool:
        if isinstance(no, No) and no.kind == "block":
            return self._bloco(no)
        self.abrir()
        r = self.stmt(no)
        self.fechar()
        return r

    def stmt(self, no) -> bool:
        if not isinstance(no, No):
            self.erro("ast_malformada", f"átomo '{no}' onde era esperado um comando", None)
            return False
        try:
            k = no.kind
            if k == "block":
                return self._bloco(no)
            if k in ("var", "array"):
                self._decl(no)
                return False
            if k == "if":
                return self._if(no)
            if k == "while":
                self.condicao(no.args[0])
                self.lacos += 1
                self._ramo(no.args[1])
                self.lacos -= 1
                return False
            if k == "for":
                self._for(no)
                return False
            if k == "return":
                self._return(no)
                return True
            if k in ("break", "continue"):
                if self.lacos == 0:
                    self.erro(f"{k}_fora_de_laco",
                              f"'{k}' só pode ser usado dentro de um laço", no)
                return False
            if k == "print":
                self._print(no)
                return False
            if k == "read":
                self._read(no)
                return False
            if k == "expr":
                self.expr(no.args[0])
                return False
            if k == "nil":
                return False
            if k in EXPR_KINDS:
                self.expr(no)
                return False
            self.erro("no_desconhecido", f"comando desconhecido '{k}'", no)
            return False
        except (IndexError, AttributeError, TypeError):
            self.erro("ast_malformada", f"nó '{no.kind}' com estrutura inválida", no)
            return False

    def _if(self, no: No) -> bool:
        self.condicao(no.args[0])
        r1 = self._ramo(no.args[1])
        r2 = self._ramo(no.args[2]) if len(no.args) > 2 else False
        return r1 and r2

    def _for(self, no: No):
        ini, cond, passo, corpo = no.args
        self.abrir()
        if ini.kind in ("var", "array"):
            self._decl(ini)
        elif ini.kind != "nil":
            self.expr(ini)
        if cond.kind != "nil":
            self.condicao(cond)
        if passo.kind != "nil":
            self.expr(passo)
        self.lacos += 1
        self._ramo(corpo)
        self.lacos -= 1
        self.fechar()

    def _return(self, no: No):
        ret = self.funcao.tipo
        if not no.args:
            if ret.base not in ("void", "erro"):
                self.erro("retorno_sem_valor",
                          f"função '{self.funcao.nome}' deve retornar {ret}", no)
            return
        t = self.valor(no.args[0])
        if ret.base == "void":
            self.erro("retorno_valor_em_void",
                      f"função void '{self.funcao.nome}' não pode retornar valor", no)
        elif not atribuivel(ret, t):
            self.erro("retorno_incompativel",
                      f"retorno do tipo {t} em função que retorna {ret}", no)

    def _print(self, no: No):
        for e in no.args:
            t = self.valor(e)
            if t.vetor:
                self.erro("print_vetor", "não é possível imprimir um vetor inteiro", e)

    def _read(self, no: No):
        t = self.lvalue(no.args[0])
        if t.base == "string":
            self.erro("lvalue_invalido", "alvo de read inválido", no)

    def condicao(self, no):
        t = self.valor(no)
        if not eh_erro(t) and not escalar(t):
            self.erro("condicao_invalida",
                      f"condição deve ser bool ou numérica, encontrado {t}", no)

    # ---------------- expressões ----------------

    def valor(self, no) -> Tipo:
        """Expressão que precisa produzir valor (void não serve)."""
        t = self.expr(no)
        if t.base == "void" and not t.vetor:
            self.erro("uso_de_void", "expressão do tipo void não produz valor", no)
            return ERRO
        return t

    def expr(self, no) -> Tipo:
        if not isinstance(no, No):
            self.erro("ast_malformada", f"átomo '{no}' onde era esperada uma expressão", None)
            return ERRO
        handler = getattr(self, "e_" + no.kind, None)
        if handler is None or no.kind not in EXPR_KINDS:
            self.erro("no_desconhecido", f"expressão desconhecida '{no.kind}'", no)
            return ERRO
        try:
            return handler(no)
        except (IndexError, AttributeError, TypeError, ValueError):
            self.erro("ast_malformada", f"nó '{no.kind}' com estrutura inválida", no)
            return ERRO

    def e_int(self, no):    return INT
    def e_float(self, no):  return FLOAT
    def e_bool(self, no):   return BOOL
    def e_char(self, no):   return CHAR
    def e_str(self, no):    return STRING

    def e_id(self, no):
        nome = no.args[0]
        s = self.buscar(nome)
        if s is None:
            self.erro("nao_declarado", f"'{nome}' não foi declarado", no)
            return ERRO
        s.usado = True
        if s.categoria == "func":
            self.erro("funcao_como_variavel",
                      f"'{nome}' é uma função e não pode ser usada como valor", no)
            return ERRO
        if s.categoria == "var" and not s.inicializado:
            self.aviso("nao_inicializada",
                       f"'{nome}' pode ser usada sem ter sido inicializada", no)
            s.inicializado = True        # um aviso por variável
        return s.tipo

    def e_index(self, no):
        tb = self.expr(no.args[0])
        ti = self.valor(no.args[1])
        if not eh_erro(ti) and not inteiro(ti):
            self.erro("indice_nao_inteiro", f"índice deve ser inteiro, encontrado {ti}", no.args[1])
        if eh_erro(tb):
            return ERRO
        if not tb.vetor:
            self.erro("indexacao_invalida", f"valor do tipo {tb} não pode ser indexado", no)
            return ERRO
        v = valor_constante(no.args[1])
        if v is not None and tb.tamanho is not None and not 0 <= v < tb.tamanho:
            self.erro("indice_fora_limite",
                      f"índice {v} fora dos limites do vetor (tamanho {tb.tamanho})", no)
        return Tipo(tb.base)

    def e_call(self, no):
        nome = no.args[0]
        argumentos = no.args[1:]
        tipos = [self.valor(a) for a in argumentos]       # sempre analisa os argumentos

        s = self.buscar(nome)
        if s is None:
            self.erro("funcao_nao_declarada", f"função '{nome}' não foi declarada", no)
            return ERRO
        if s.categoria != "func":
            self.erro("nao_e_funcao", f"'{nome}' não é uma função", no)
            return ERRO
        s.usado = True

        esperados = s.tipos_parametros
        if len(tipos) != len(esperados):
            self.erro("aridade",
                      f"'{nome}' espera {len(esperados)} argumento(s), recebeu {len(tipos)}", no)
        else:
            for i, (ta, tp) in enumerate(zip(tipos, esperados), 1):
                if not atribuivel(tp, ta):
                    self.erro("argumento_incompativel",
                              f"argumento {i} de '{nome}': esperado {tp}, encontrado {ta}",
                              argumentos[i - 1])
        return s.tipo

    def e_unary(self, no):
        op, e = no.args
        t = self.valor(e)
        if eh_erro(t):
            return ERRO
        if t.vetor:
            self.erro("operando_vetor", f"operador '{op}' não se aplica a vetores", no)
            return ERRO
        if op == "!":
            if not escalar(t):
                self.erro("operando_invalido", f"'!' exige bool ou numérico, encontrado {t}", no)
                return ERRO
            return BOOL
        if op in ("-", "+"):
            if not numerico(t):
                self.erro("operando_invalido", f"'{op}' unário exige numérico, encontrado {t}", no)
                return ERRO
            return INT if t.base == "char" else t
        self.erro("operador_desconhecido", f"operador unário desconhecido '{op}'", no)
        return ERRO

    def e_binary(self, no):
        op, a, b = no.args
        ta, tb = self.valor(a), self.valor(b)
        if eh_erro(ta) or eh_erro(tb):
            return ERRO
        if ta.vetor or tb.vetor:
            self.erro("operando_vetor", f"operador '{op}' não se aplica a vetores", no)
            return ERRO

        if op in OP_ARIT:
            if not (numerico(ta) and numerico(tb)):
                self.erro("operando_invalido",
                          f"'{op}' exige operandos numéricos, encontrado {ta} e {tb}", no)
                return ERRO
            if op == "%" and not (inteiro(ta) and inteiro(tb)):
                self.erro("operando_invalido",
                          f"'%' exige operandos inteiros, encontrado {ta} e {tb}", no)
                return ERRO
            if op in ("/", "%") and self._eh_zero(b):
                self.erro("divisao_por_zero", "divisão por zero", no)
            return FLOAT if "float" in (ta.base, tb.base) else INT

        if op in OP_REL:
            if not (numerico(ta) and numerico(tb)):
                self.erro("operando_invalido",
                          f"'{op}' exige operandos numéricos, encontrado {ta} e {tb}", no)
                return ERRO
            return BOOL

        if op in OP_IGUAL:
            if (numerico(ta) and numerico(tb)) or ta.base == tb.base == "bool":
                return BOOL
            self.erro("comparacao_incompativel",
                      f"não é possível comparar {ta} com {tb}", no)
            return ERRO

        if op in OP_LOG:
            if not (escalar(ta) and escalar(tb)):
                self.erro("operando_invalido",
                          f"'{op}' exige operandos bool ou numéricos, encontrado {ta} e {tb}", no)
                return ERRO
            return BOOL

        self.erro("operador_desconhecido", f"operador desconhecido '{op}'", no)
        return ERRO

    @staticmethod
    def _eh_zero(no) -> bool:
        if valor_constante(no) == 0:
            return True
        if isinstance(no, No) and no.kind == "float":
            try:
                return float(no.args[0]) == 0.0
            except ValueError:
                return False
        return False

    def e_assign(self, no):
        alvo, valor = no.args
        tv = self.valor(valor)           # valor primeiro: x = x + 1 usa o x antigo
        ta = self.lvalue(alvo)
        if not atribuivel(ta, tv):
            self.erro("atribuicao_incompativel",
                      f"não é possível atribuir {tv} a {ta}", no)
        return ta

    def lvalue(self, no) -> Tipo:
        """Alvo de atribuição/read: variável escalar ou elemento de vetor."""
        if not isinstance(no, No):
            self.erro("lvalue_invalido", "alvo de atribuição inválido", None)
            return ERRO
        if no.kind == "index":
            return self.e_index(no)
        if no.kind != "id":
            self.erro("lvalue_invalido", "o alvo da atribuição deve ser variável ou elemento de vetor", no)
            return ERRO
        nome = no.args[0]
        s = self.buscar(nome)
        if s is None:
            self.erro("nao_declarado", f"'{nome}' não foi declarado", no)
            return ERRO
        if s.categoria == "func":
            self.erro("atribuicao_a_funcao", f"'{nome}' é uma função", no)
            return ERRO
        if s.tipo.vetor:
            self.erro("atribuicao_a_vetor",
                      f"'{nome}' é um vetor; atribua/leia elemento a elemento", no)
            return ERRO
        s.inicializado = True
        return s.tipo


# ============================================================
# 5. LINHA DE COMANDO
# ============================================================

def principal(argv) -> int:
    opcoes = {a for a in argv[1:] if a.startswith("--")}
    arquivos = [a for a in argv[1:] if not a.startswith("--")]
    if len(arquivos) != 1:
        print("Uso: python semantic.py arvore.sexp [--json] [--strict]", file=sys.stderr)
        return 2

    try:
        with open(arquivos[0], encoding="utf-8") as f:
            raiz = ler_sexpr(f.read())
    except (OSError, ErroAST) as e:
        print(f"Erro ao ler a AST: {e}", file=sys.stderr)
        return 2

    diags = AnalisadorSemantico().analisar(raiz)

    if "--json" in opcoes:
        for d in diags:
            print(json.dumps(d.para_dict(), ensure_ascii=False))
    else:
        for d in diags:
            print(d)
        if not diags:
            print("Análise semântica concluída sem problemas.")

    strict = "--strict" in opcoes
    falhou = any(d.nivel == "erro" or (strict and d.nivel == "aviso") for d in diags)
    return 1 if falhou else 0


if __name__ == "__main__":
    sys.exit(principal(sys.argv))
