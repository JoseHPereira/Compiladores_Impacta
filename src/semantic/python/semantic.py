#!/usr/bin/env python3
"""
Analisador semântico do MiniC.

Uso:
    python semantic.py programa.c

Fluxo: lexer (../../lexer/lexer_v2.py) -> parser (../../lexer/parser.py)
-> análise semântica. O parser original não é modificado: aqui ele é
estendido (subclasse) apenas para registrar posição e texto-fonte de cada nó,
necessários para as mensagens de erro.

Regras (apostila da Aula 15 / tests/semantic/GABARITO.md):
  * escopo léxico com sombreamento; blocos abrem escopo;
  * assinaturas de todas as funções são coletadas antes dos corpos
    (recursão e chamada antes da definição);
  * compatibilidade EXATA de tipos, exceto a promoção int -> float;
  * condições de if/while devem ser bool;
  * índice de vetor deve ser int; vetores só são passados como vetor;
  * chamadas: aridade e tipo de cada argumento;
  * return compatível com o tipo da função; função não-void não pode
    "cair" no fim do corpo;
  * chamada void só pode ser usada como comando;
  * destino de atribuição deve ser variável ou elemento de vetor.

Catálogo de códigos
  SEM001 identificador não declarado       SEM002 declaração duplicada
  SEM003 atribuição/inicialização inválida SEM004 operador com operandos inválidos
  SEM005 condição não booleana             SEM006 índice de vetor inválido
  SEM007 aridade incorreta                 SEM008 tipo de argumento incompatível
  SEM009 retorno incompatível              SEM010 return sem valor / valor em void
  SEM011 função não-void pode não retornar SEM012 chamada void usada como valor
  SEM013 destino não atribuível            SEM014 variável/parâmetro void
  SEM015 comando fora de função            SEM016 indexação de não-vetor
  SEM017 função usada como variável        SEM018 chamada de não-função
  SEM019 tamanho de vetor inválido

Saída: um diagnóstico por linha, em ordem de origem, e a linha-resumo.
Código de saída: 0 = programa aceito, 1 = rejeitado ou erro sintático.
"""

from __future__ import annotations

import functools
import importlib.util
import os
import re
import sys
from dataclasses import dataclass, field

# ============================================================
# 0. Acesso ao lexer / parser do projeto
# ============================================================

_AQUI = os.path.dirname(os.path.abspath(__file__))
_LEXER_DIR = os.path.normpath(os.path.join(_AQUI, "..", "..", "lexer"))


def _carregar(nome: str, arquivo: str):
    """Carrega um módulo pelo caminho (evita conflito com módulos da stdlib,
    como o antigo `parser`)."""
    if _LEXER_DIR not in sys.path:
        sys.path.insert(0, _LEXER_DIR)          # parser.py faz `from lexer_v2 import`
    spec = importlib.util.spec_from_file_location(nome, os.path.join(_LEXER_DIR, arquivo))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[nome] = mod
    spec.loader.exec_module(mod)
    return mod


_lexer_mod = _carregar("lexer_v2", "lexer_v2.py")
_parser_mod = _carregar("minic_parser", "parser.py")

Lexer = _lexer_mod.Lexer
P = _parser_mod                                  # nós da AST: P.Binary, P.Id, ...
ErroSintatico = _parser_mod.ErroSintatico


# ============================================================
# 1. Parser com posições
# ============================================================

class Param(tuple):
    """(tipo, nome) como no parser original, mais posição e flag de vetor."""

    def __new__(cls, tipo, nome, ini, vetor):
        obj = super().__new__(cls, (tipo, nome))
        obj.ini = ini            # índice do token do tipo; o nome vem logo depois
        obj.vetor = vetor
        return obj


def _com_span(metodo):
    """Marca o nó devolvido com o intervalo de tokens [ini, fim] que consumiu
    (só na primeira vez: um nó parentetizado mantém o intervalo interno)."""
    @functools.wraps(metodo)
    def wrapper(self, *args, **kwargs):
        ini = self.pos
        no = metodo(self, *args, **kwargs)
        if isinstance(no, P.No) and not hasattr(no, "ini"):
            no.ini, no.fim = ini, max(self.pos - 1, ini)
        return no
    return wrapper


class ParserPos(P.Parser):
    """Parser do projeto + intervalo de tokens em cada nó (`ini`, `fim`)."""

    def _marca(self, no, ini):
        no.ini, no.fim = ini, max(self.pos - 1, ini)
        return no

    # `int v[]` é válido em parâmetros; o parser original exige expressão.
    def parse_parametro(self):
        ini = self.pos
        tipo = self.parse_tipo()
        nome = self.casar("IDENT", "identificador").lexema
        vetor = False
        if self.checar("LBRACKET"):
            vetor = True
            self.avancar()
            if not self.checar("RBRACKET"):
                self.parse_expr()
            self.casar("RBRACKET")
        return Param(tipo, nome, ini, vetor)

    # Nós intermediários de cadeias binárias/postfix precisam de posição própria.
    def _bin_esquerda(self, proximo_nivel, operadores):
        ini = self.pos
        esq = proximo_nivel()
        while self.atual().tipo in operadores:
            op = self.avancar().lexema
            dire = proximo_nivel()
            esq = self._marca(P.Binary(op, esq, dire), ini)
        return esq

    def parse_postfix(self):
        ini = self.pos
        expr = self.parse_primary()
        while True:
            if self.checar("LBRACKET"):
                self.avancar()
                indice = self.parse_expr()
                self.casar("RBRACKET")
                expr = self._marca(P.Index(expr, indice), ini)
            elif self.checar("LPAREN"):
                self.avancar()
                args = []
                if not self.checar("RPAREN"):
                    args.append(self.parse_expr())
                    while self.checar("COMMA"):
                        self.avancar()
                        args.append(self.parse_expr())
                self.casar("RPAREN", "argumento ou ')'")
                expr = self._marca(P.Call(expr, args), ini)
            else:
                break
        return expr

    # print(...) / read(...) viram ExprStmt(Call(Id(...))) sem posição própria.
    def _builtin(self, original):
        ini = self.pos
        no = original()
        for sub in (no, no.expr, no.expr.callee):
            self._marca(sub, ini)
        return no

    def parse_print(self):
        return self._builtin(super().parse_print)

    def parse_read(self):
        return self._builtin(super().parse_read)


for _nome in ("parse_assign", "parse_unary", "parse_primary", "parse_stmt",
              "parse_block", "parse_declaracao_topo"):
    setattr(ParserPos, _nome, _com_span(getattr(P.Parser, _nome)))


# ============================================================
# 2. Tipos
# ============================================================

@dataclass(frozen=True)
class Tipo:
    base: str
    vetor: bool = False
    tamanho: int | None = None

    def __str__(self):
        return self.base + ("[]" if self.vetor else "")

    def descr(self):
        if self.vetor and self.tamanho is not None:
            return f"{self.base}[{self.tamanho}]"
        return str(self)


ERRO = Tipo("erro")            # "poison": evita erros em cascata
INT, FLOAT, BOOL, CHAR = Tipo("int"), Tipo("float"), Tipo("bool"), Tipo("char")
STRING, VOID = Tipo("string"), Tipo("void")

TIPOS_NOMEADOS = {"int", "float", "bool", "char", "void"}

# Conversões implícitas permitidas (origem, destino). Pela apostila, só int -> float.
CONVERSOES = {("int", "float")}


def eh_erro(t):
    return t.base == "erro"


def eh_numerico(t):
    """int ou float (operandos aritméticos)."""
    return not t.vetor and t.base in ("int", "float")


def atribuivel(destino: Tipo, origem: Tipo) -> bool:
    if eh_erro(destino) or eh_erro(origem):
        return True
    if destino.base in ("void", "string") or origem.base == "void":
        return False
    if destino.vetor or origem.vetor:
        return destino.vetor and origem.vetor and destino.base == origem.base
    return destino.base == origem.base or (origem.base, destino.base) in CONVERSOES


LITERAL_TIPO = {"int": INT, "real": FLOAT, "char": CHAR, "string": STRING, "bool": BOOL}
LITERAL_DESCRICAO = {"int": "o literal inteiro", "real": "o literal real",
                     "char": "o literal de caractere", "string": "o literal de cadeia",
                     "bool": "o literal booleano"}


# ============================================================
# 3. Símbolos e diagnósticos
# ============================================================

@dataclass
class Simbolo:
    nome: str
    categoria: str                 # "var" | "param" | "func"
    tipo: Tipo                     # tipo da variável ou retorno da função
    linha: int = 0
    coluna: int = 0
    parametros: list = field(default_factory=list)       # [Tipo]


@dataclass
class Diagnostico:
    codigo: str
    mensagem: str
    linha: int
    coluna: int

    def __str__(self):
        return f"{self.codigo} — linha {self.linha}, coluna {self.coluna}: {self.mensagem}"


# ============================================================
# 4. Analisador
# ============================================================

class AnalisadorSemantico:

    def __init__(self, codigo: str, tokens: list):
        self.codigo = codigo
        self.tokens = tokens
        self.inicio_linha = [0]
        for i, c in enumerate(codigo):
            if c == "\n":
                self.inicio_linha.append(i + 1)
        self.diagnosticos: list[Diagnostico] = []
        self.escopos: list[dict] = []
        self.funcao: Simbolo | None = None

    # ---------------- infraestrutura ----------------

    def pos(self, no):
        tok = self.tokens[no.ini]
        return tok.linha, tok.coluna

    def pos_nome(self, no):
        """Posição do identificador de uma declaração (token logo após o tipo)."""
        tok = self.tokens[no.ini + 1]
        return tok.linha, tok.coluna

    def texto(self, no):
        ini, fim = self.tokens[no.ini], self.tokens[no.fim]
        a = self.inicio_linha[ini.linha - 1] + ini.coluna - 1
        b = self.inicio_linha[fim.linha - 1] + fim.coluna - 1 + len(fim.lexema)
        return re.sub(r"\s*\n\s*", " ", self.codigo[a:b])

    def erro(self, codigo, mensagem, posicao):
        if not isinstance(posicao, tuple):
            posicao = self.pos(posicao)
        self.diagnosticos.append(Diagnostico(codigo, mensagem, *posicao))

    def abrir(self):
        self.escopos.append({})

    def fechar(self):
        self.escopos.pop()

    def declarar(self, sim: Simbolo) -> bool:
        atual = self.escopos[-1]
        if sim.nome in atual:
            ant = atual[sim.nome]
            desc = (f"função com retorno {ant.tipo}" if ant.categoria == "func"
                    else f"tipo {ant.tipo.descr()}")
            self.erro("SEM002",
                      f"“{sim.nome}” já declarado neste escopo; declaração anterior "
                      f"na linha {ant.linha}, coluna {ant.coluna} ({desc}).",
                      (sim.linha, sim.coluna))
            return False
        atual[sim.nome] = sim
        return True

    def buscar(self, nome):
        for escopo in reversed(self.escopos):
            if nome in escopo:
                return escopo[nome]
        return None

    # ---------------- programa ----------------

    def analisar(self, programa) -> list[Diagnostico]:
        self.abrir()                                      # escopo global

        # Passada 1: assinaturas (recursão e uso antes da definição).
        assinaturas = {}
        for d in programa.declaracoes:
            if isinstance(d, P.Function):
                sim = self._assinatura(d)
                self.declarar(sim)
                assinaturas[id(d)] = sim

        # Passada 2: globais e corpos, na ordem do arquivo.
        for d in programa.declaracoes:
            if isinstance(d, P.Function):
                self._corpo_funcao(d, assinaturas[id(d)])
            elif isinstance(d, P.VarDecl):
                self._decl(d)
            else:
                self.erro("SEM015", "Apenas declarações são permitidas no escopo global; "
                                    "comandos devem estar dentro de uma função.", d)

        self.fechar()
        self.diagnosticos.sort(key=lambda x: (x.linha, x.coluna))   # estável
        return self.diagnosticos

    # ---------------- funções ----------------

    def _assinatura(self, d) -> Simbolo:
        linha, coluna = self.pos_nome(d)
        parametros = []
        for p in d.parametros:
            if p[0] == "void":
                self.erro("SEM014", f"O parâmetro “{p[1]}” não pode ter tipo void.",
                          self.pos_nome(p))
                parametros.append(ERRO)
            else:
                parametros.append(Tipo(p[0], p.vetor))
        return Simbolo(d.nome, "func", Tipo(d.tipo), linha, coluna, parametros)

    def _corpo_funcao(self, d, sim: Simbolo):
        self.funcao = sim
        self.abrir()
        for p, tp in zip(d.parametros, sim.parametros):
            linha, coluna = self.pos_nome(p)
            self.declarar(Simbolo(p[1], "param", tp, linha, coluna))

        queda = self._comandos(d.corpo.comandos)          # corpo compartilha o escopo dos parâmetros
        if queda is not None and sim.tipo.base != "void":
            motivo = queda or "o corpo alcança o fim sem executar um return"
            self.erro("SEM011",
                      f"A função “{sim.nome}” pode terminar sem retornar {sim.tipo}; {motivo}.",
                      d)

        self.fechar()
        self.funcao = None

    # ---------------- declarações ----------------

    def _decl(self, no):
        nome = no.nome
        posicao = self.pos_nome(no)

        if no.tipo == "void":
            self.erro("SEM014", f"A variável “{nome}” não pode ter tipo void.", posicao)
            tv = ERRO
        else:
            tv = Tipo(no.tipo)

        if no.tamanho is not None:
            self.valor(no.tamanho, "tamanho de vetor")
            n = self._constante_inteira(no.tamanho)
            if n is None or n <= 0:
                self.erro("SEM019",
                          f"O tamanho do vetor “{nome}” deve ser uma constante inteira "
                          f"positiva (expressão “{self.texto(no.tamanho)}”).", no.tamanho)
                n = None
            if not eh_erro(tv):
                tv = Tipo(tv.base, True, n)

        if no.inicializador is not None:
            ti = self.valor(no.inicializador, "inicializador de variável")
            if not atribuivel(tv, ti):
                self.erro("SEM003",
                          f"Não é possível atribuir {ti} a {tv.descr()} sem conversão permitida "
                          f"(destino “{nome}”; expressão “{self.texto(no.inicializador)}”).",
                          no.inicializador)

        # a declaração só vale depois do inicializador (int x = x; usa o x de fora)
        self.declarar(Simbolo(nome, "var", tv, *posicao))

    @staticmethod
    def _constante_inteira(no):
        if isinstance(no, P.Lit) and no.tipo == "int":
            try:
                return int(no.valor)
            except ValueError:
                return None
        if isinstance(no, P.Unary) and no.op == "-":
            v = AnalisadorSemantico._constante_inteira(no.expr)
            return -v if v is not None else None
        return None

    # ---------------- comandos ----------------
    # Cada comando devolve None se garante `return` em todos os caminhos;
    # senão devolve o motivo da "queda" ("" = motivo genérico).

    def _comandos(self, comandos):
        garante, motivo = False, ""
        for c in comandos:
            r = self.stmt(c)
            if r is None:
                garante = True
            elif r:
                motivo = r
        return None if garante else motivo

    def _ramo(self, no):
        if isinstance(no, P.Block):
            return self.stmt(no)
        self.abrir()
        r = self.stmt(no)
        self.fechar()
        return r

    def stmt(self, no):
        if isinstance(no, P.Block):
            self.abrir()
            r = self._comandos(no.comandos)
            self.fechar()
            return r
        if isinstance(no, P.VarDecl):
            self._decl(no)
            return ""
        if isinstance(no, P.If):
            return self._if(no)
        if isinstance(no, P.While):
            self.condicao(no.condicao, "while")
            self._ramo(no.corpo)
            return ""
        if isinstance(no, P.Return):
            self._return(no)
            return None
        if isinstance(no, P.ExprStmt):
            self.expr(no.expr)
            return ""
        return ""

    def _if(self, no):
        self.condicao(no.condicao, "if")
        cond = self.texto(no.condicao)
        r_entao = self._ramo(no.entao)
        if no.senao is None:
            return f"o ramo em que “{cond}” é falso alcança o fim do corpo"
        r_senao = self._ramo(no.senao)
        if r_entao is None and r_senao is None:
            return None
        if r_entao is not None:
            return r_entao or f"o ramo em que “{cond}” é verdadeiro alcança o fim do corpo"
        return r_senao or f"o ramo em que “{cond}” é falso alcança o fim do corpo"

    def _return(self, no):
        ret = self.funcao.tipo
        nome = self.funcao.nome
        if no.expr is None:
            if ret.base != "void":
                self.erro("SEM010",
                          f"A função “{nome}” deve retornar {ret}, mas o return não tem valor.", no)
            return
        t = self.valor(no.expr, "valor de retorno")
        if ret.base == "void":
            if not eh_erro(t):
                self.erro("SEM010",
                          f"A função void “{nome}” não pode retornar valor "
                          f"(expressão “{self.texto(no.expr)}” do tipo {t}).", no.expr)
        elif not atribuivel(ret, t):
            self.erro("SEM009",
                      f"Retorno {t} incompatível com o tipo {ret} da função “{nome}”; "
                      f"conversão implícita de {t} para {ret} não permitida.", no.expr)

    def condicao(self, no, palavra):
        t = self.valor(no, "condição")
        if not eh_erro(t) and t != BOOL:
            self.erro("SEM005",
                      f"Condição de {palavra} deve ter tipo bool; recebeu {t} "
                      f"(expressão “{self.texto(no)}”).", no)

    # ---------------- expressões ----------------

    def valor(self, no, contexto) -> Tipo:
        """Expressão que precisa produzir valor: chamada void é erro (SEM012)."""
        t = self.expr(no)
        if t.base == "void":
            nome = no.callee.nome if isinstance(no, P.Call) else self.texto(no)
            self.erro("SEM012",
                      f"Função “{nome}” não produz valor (retorno void) e não pode ser "
                      f"usada como {contexto}.", no)
            return ERRO
        return t

    def expr(self, no) -> Tipo:
        return getattr(self, "e_" + type(no).__name__)(no)

    def e_Lit(self, no):
        return LITERAL_TIPO[no.tipo]

    def e_Id(self, no):
        s = self.buscar(no.nome)
        if s is None:
            self.erro("SEM001", f"Identificador “{no.nome}” não declarado neste escopo.", no)
            return ERRO
        if s.categoria == "func":
            self.erro("SEM017", f"“{no.nome}” é uma função e não pode ser usada como variável.", no)
            return ERRO
        return s.tipo

    def e_Index(self, no):
        tb = self.expr(no.base)
        ti = self.valor(no.indice, "índice de vetor")
        nome = self.texto(no.base)
        if not eh_erro(tb) and not tb.vetor:
            self.erro("SEM016", f"“{nome}” é do tipo {tb} e não pode ser indexado; "
                                "apenas vetores aceitam índice.", no.base)
            tb = ERRO
        if not eh_erro(ti) and ti != INT:
            self.erro("SEM006",
                      f"Índice do vetor “{nome}” deve ser int; recebeu {ti} "
                      f"(expressão “{self.texto(no.indice)}”).", no.indice)
        return ERRO if eh_erro(tb) else Tipo(tb.base)

    def e_Call(self, no):
        if not isinstance(no.callee, P.Id):
            self.expr(no.callee)
            self.erro("SEM018", f"A expressão “{self.texto(no.callee)}” não é uma função "
                                "e não pode ser chamada.", no.callee)
            for a in no.args:
                self.valor(a, "argumento de chamada")
            return ERRO

        nome = no.callee.nome
        if nome in ("print", "read"):
            return self._builtin(no, nome)

        s = self.buscar(nome)
        tipos = [self.valor(a, "argumento de chamada") for a in no.args]   # sempre analisa
        if s is None:
            self.erro("SEM001", f"Identificador “{nome}” não declarado neste escopo.", no.callee)
            return ERRO
        if s.categoria != "func":
            self.erro("SEM018", f"“{nome}” não é uma função e não pode ser chamado.", no.callee)
            return ERRO

        esperados = s.parametros
        if len(tipos) != len(esperados):
            e, r = len(esperados), len(tipos)
            self.erro("SEM007",
                      f"“{nome}” espera {e} argumento{'s' if e != 1 else ''}, "
                      f"mas recebeu {r}.", no)
        else:
            for i, (ta, tp, a) in enumerate(zip(tipos, esperados, no.args), 1):
                if not atribuivel(tp, ta):
                    self.erro("SEM008",
                              f"Argumento {i} de “{nome}”: esperado {tp}, recebido {ta} "
                              f"(expressão “{self.texto(a)}”).", a)
        return s.tipo

    def _builtin(self, no, nome):
        for i, a in enumerate(no.args, 1):
            if nome == "print":
                t = self.valor(a, "argumento de print")
                if t.vetor:
                    self.erro("SEM008", f"Argumento {i} de “print”: não é possível imprimir "
                                        f"um vetor inteiro (expressão “{self.texto(a)}”).", a)
            else:
                self.alvo(a, "leitura")
        return VOID

    def e_Unary(self, no):
        t = self.valor(no.expr, "operando")
        if eh_erro(t):
            return BOOL if no.op == "!" else ERRO
        if no.op == "!":
            if t != BOOL:
                self.erro("SEM004", f"Operador “!” exige operando bool; recebeu {t} "
                                    f"(expressão “{self.texto(no)}”).", no)
                return ERRO
            return BOOL
        if not eh_numerico(t):
            self.erro("SEM004", f"Operador “{no.op}” exige operando numérico (int ou float); "
                                f"recebeu {t} (expressão “{self.texto(no)}”).", no)
            return ERRO
        return t

    def e_Binary(self, no):
        op = no.op
        ta = self.valor(no.esq, "operando")
        tb = self.valor(no.dire, "operando")
        arit = op in ("+", "-", "*", "/", "%")
        resultado = ERRO if arit else BOOL
        if eh_erro(ta) or eh_erro(tb):
            return resultado

        if arit:
            ok = (ta == INT and tb == INT) if op == "%" else (eh_numerico(ta) and eh_numerico(tb))
            exigido = "int" if op == "%" else "numéricos (int ou float)"
        elif op in ("<", ">", "<=", ">="):
            ok = (eh_numerico(ta) and eh_numerico(tb)) or (ta == CHAR and tb == CHAR)
            exigido = "numéricos (int ou float) ou ambos char"
        elif op in ("==", "!="):
            ok = (eh_numerico(ta) and eh_numerico(tb)) or (
                not ta.vetor and not tb.vetor and ta.base == tb.base
                and ta.base in ("bool", "char"))
            exigido = "de tipos compatíveis"
        else:  # && ||
            ok = ta == BOOL and tb == BOOL
            exigido = "bool"

        if not ok:
            self.erro("SEM004", f"Operador “{op}” exige operandos {exigido}; recebeu {ta} e {tb} "
                                f"(expressão “{self.texto(no)}”).", no)
            return resultado
        if arit:
            return FLOAT if FLOAT in (ta, tb) else INT
        return BOOL

    def e_Assign(self, no):
        tv = self.valor(no.valor, "expressão de atribuição")
        ta = self.alvo(no.alvo, "atribuição")
        if eh_erro(ta) or eh_erro(tv):
            return ta
        if not atribuivel(ta, tv):
            self.erro("SEM003",
                      f"Não é possível atribuir {tv} a {ta} sem conversão permitida "
                      f"(destino “{self.texto(no.alvo)}”; expressão “{self.texto(no.valor)}”).",
                      no.valor)
        return ta

    def alvo(self, no, contexto) -> Tipo:
        """Destino de atribuição/leitura: variável escalar ou elemento de vetor."""
        prefixo = f"Destino de {contexto} não é atribuível; "
        fim = " não designa uma variável ou elemento de vetor."
        if isinstance(no, P.Index):
            return self.e_Index(no)
        if isinstance(no, P.Id):
            s = self.buscar(no.nome)
            if s is None:
                self.erro("SEM001", f"Identificador “{no.nome}” não declarado neste escopo.", no)
                return ERRO
            if s.categoria == "func":
                self.erro("SEM013", f"{prefixo}a função “{no.nome}”{fim}", no)
                return ERRO
            if s.tipo.vetor:
                self.erro("SEM013", f"{prefixo}o vetor “{no.nome}” não pode ser tratado como um "
                                    "todo; use um elemento (por exemplo, "
                                    f"“{no.nome}[0]”).", no)
                return ERRO
            return s.tipo
        desc = LITERAL_DESCRICAO.get(no.tipo, "a expressão") if isinstance(no, P.Lit) \
            else "a expressão"
        self.expr(no)                                   # ainda reporta erros internos
        self.erro("SEM013", f"{prefixo}{desc} “{self.texto(no)}”{fim}", no)
        return ERRO


# ============================================================
# 5. Linha de comando
# ============================================================

def formatar_saida(diags) -> str:
    linhas = [str(d) for d in diags]
    n = len(diags)
    if n == 0:
        linhas.append("Análise semântica concluída: 0 erros; programa aceito.")
    else:
        linhas.append(f"Análise semântica concluída: {n} erro{'s' if n != 1 else ''}; "
                      "programa rejeitado.")
    # Formato dos gabaritos: linhas separadas por CRLF e sem quebra final.
    return "\r\n".join(linhas)


def principal(argv) -> int:
    if len(argv) != 2:
        print("Uso: python semantic.py programa.c", file=sys.stderr)
        return 2
    try:
        with open(argv[1], "r", encoding="utf-8") as f:
            codigo = f.read()
    except OSError as e:
        print(f"Erro ao abrir arquivo: {e}", file=sys.stderr)
        return 2

    tokens = Lexer(codigo).analisar()
    try:
        programa = ParserPos(tokens).parse_program()
    except ErroSintatico as e:
        print(f"Erro sintático: {e}", file=sys.stderr)
        return 1

    diags = AnalisadorSemantico(codigo, tokens).analisar(programa)
    sys.stdout.buffer.write(formatar_saida(diags).encode("utf-8"))
    sys.stdout.buffer.flush()
    return 1 if diags else 0


if __name__ == "__main__":
    sys.exit(principal(sys.argv))