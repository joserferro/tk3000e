/****************************************************************************
*
*  APPLE //E EMULATOR FOR WINDOWS
*
*  Copyright (C) 1994-96, Michael O'Brien.  All rights reserved.
*
***/

/*  Emulador do computador TK3000 //e (Microdigital)
 *  por Fábio Belavenuto - Copyright (C) 2004
 *
 *  Adaptado do emulador Applewin por Michael O'Brien
 *  Part of code is Copyright (C) 2003-2004 Tom Charlesworth
 *
 *  Este arquivo é distribuido pela Licença Pública Geral GNU.
 *  Veja o arquivo Licenca.txt distribuido com este software.
 *
 *  ESTE SOFTWARE NÃO OFERECE NENHUMA GARANTIA
 *
 */

/*  Fita K7 (leitura e gravação em arquivos WAV) - versão 2.5
 *  por José R. Ferro - Copyright (C) 2026
 *
 *  Acrescentado por José R. Ferro:
 *  - Menu "Fita K7": LOAD, SAVE, Pausar/continuar, Rebobinar e Parar fita
 *    (funções TapeSelecionaLeitura, TapeSelecionaGravacao, TapePausaFita,
 *    TapeRebobinaFita e TapeParaFita).
 *  - Leitura e gravação de WAV: TapeCarregar, TapeGravar, TapeParar,
 *    TapeLeitura ($C060) e TapeEscrita ($C020).
 *  - Pausa manual (TapePausar) e pausa automática opcional por inatividade
 *    (menu Fita K7, desligada por padrão).
 *  - Indicador K7 da barra de status e contador estilo gravador
 *    (TapeLuz, TapeContador, TapeTextoEstado).
 *  - Conserto de falhas no tom piloto (ConsertaFalhas).
 *  - Inversão de polaridade do sinal (TapeAlternaInverte), para fitas
 *    rápidas gravadas com a fase oposta.
 *  - Leitura de WAV guardando o instante de cada transição (cruzamento por
 *    zero com interpolação entre amostras), para ler também fitas rápidas.
 */

// Emula um tape (gravador K7) usando arquivos WAV.
//
//  $C020 (saída, gravação): qualquer acesso (leitura ou escrita) inverte o
//        nível do sinal. A fita gravada é uma onda quadrada formada pelos
//        ciclos da CPU em que o programa acessou esse endereço.
//  $C060 (entrada, leitura): o bit 7 devolve o nível atual do sinal da fita.
//
// Leitura (LOAD): o arquivo WAV é carregado e convertido para níveis 0/1
// (com histerese). A fita começa a "rodar" no primeiro acesso a $C060 depois
// de selecionada, e a posição é calculada pelo contador de ciclos da CPU.
// Assim funciona igual em qualquer velocidade de emulação.
//
// Gravação (SAVE): as transições de $C020 ficam na memória (com o ciclo em
// que ocorreram) e, ao parar a fita, são renderizadas como onda quadrada em
// um arquivo WAV (8 bits, mono). O silêncio antes da 1a transição é cortado.

#include "stdhdr.h"
#include <vector>
#include "tape.h"
#include "tk3000e.h"
#include "memoria.h"
#include "janela.h"
#include "registro.h"
#include "disco.h"

// Definições
//#define DEBUGTAPE    /* Depurar atraves de um console */

#define TAPE_MAX_WAV        (256*1024*1024)	// Tamanho máximo do WAV lido
#define TAPE_TAXA_MIN       8000			// Taxa mínima aceita na leitura
#define TAPE_TAXA_SAVE      44100			// Taxa do WAV gerado
#define TAPE_NIVEL_ALTO     0xC0			// Amostra (8 bits) do nível alto
#define TAPE_NIVEL_BAIXO    0x40			// Amostra (8 bits) do nível baixo
#define TAPE_HISTERESE      0.15			// Fração do pico para trocar de nível
#define TAPE_MIN_PICO       200				// Pico mínimo (escala de 16 bits), abaixo disso é silêncio
#define TAPE_MAX_TRANSICOES (16*1000*1000)	// Limite de transições gravadas

// Estados
#define TAPE_OCIOSO         0	// Nada carregado
#define TAPE_ARMADO         1	// WAV carregado, aguardando o 1o acesso a $C060
#define TAPE_REPRODUZINDO   2	// Fita rodando
#define TAPE_FIM            3	// Fita chegou ao fim
#define TAPE_GRAVANDO       4	// Gravando transições de $C020
#define TAPE_PAUSADO        5	// Fita parada por inatividade (mantém a posição)
#define TAPE_PAUSA_MANUAL   6	// Fita parada pelo usuário (menu Pausar), só sai pelo menu

// Se a ROM ficar mais que isto sem ler $C060 a fita para sozinha, como se o
// usuário apertasse STOP no gravador. A espera de 3,5 s da rotina READ da ROM
// fica abaixo do limite e não é afetada.
#define TAPE_PAUSA_CICLOS   4600000		// ~4,5 s a 1,0227 MHz

// Variáveis
static int                              TapeEstado   = TAPE_OCIOSO;

// Leitura
static std::vector<double>              TapeTrans;			// Instantes das transições (em amostras, com fração)
static BYTE                             TapeNivelIni = 0;	// Nível antes da 1a transição
static double                           TapeTotal    = 0.0;	// Duração da fita, em amostras
static size_t                           TapeIdxCache = 0;	// Última posição achada em TapeTrans (acelera a busca)
static DWORD                            TapeTaxa     = 0;	// Taxa do WAV carregado
static unsigned __int64                 TapeCicloIni = 0;	// Ciclo em que a fita (re)começou a rodar
static unsigned __int64                 TapeUltAcesso = 0;	// Ciclo do último acesso a $C060
static double                           TapeBase     = 0.0;	// Posição (amostras) em TapeCicloIni
static int                              TapeLuzAtual = -1;	// Último estado mostrado no indicador
static int                              TapeContAtual = -1;	// Último valor do contador mostrado
static int                              TapeInverte  = 0;	// 1 = polaridade do sinal invertida (menu Fita K7)
static int                              TapePausaAuto = 0;	// 1 = a fita para sozinha quando fica sem ser lida
static int                              TapeAntesPausa = TAPE_ARMADO;	// Estado a retomar ao continuar
static unsigned __int64                 TapeCicloGrav = 0;	// Ciclo em que a gravação começou
static BYTE                             TapeUltimo   = 0;	// Último nível (depois do fim)

// Gravação
static std::vector<unsigned __int64>    TapeTransicoes;
static char                             TapeArqGravacao[MAX_PATH] = "";

// Funções Internas

//===========================================================================
// Posição da fita, em amostras, no ciclo dado
static double PosicaoFita(unsigned __int64 ciclo)
{
	return TapeBase + (double)(__int64)(ciclo - TapeCicloIni) * (double)TapeTaxa / CLK_6502;
}

//===========================================================================
// Nível do sinal da fita no instante x (em amostras): conta quantas transições
// já ocorreram. A busca parte da última posição achada, então leituras
// seguidas (que andam para a frente) custam O(1).
static BYTE NivelEm(double x)
{
	size_t n = TapeTrans.size();
	size_t i = TapeIdxCache;

	if (i > n)
		i = n;
	while (i > 0 && TapeTrans[i-1] > x)
		i--;
	while (i < n && TapeTrans[i] <= x)
		i++;
	TapeIdxCache = i;
	return (BYTE)(TapeNivelIni ^ (i & 1) ^ TapeInverte);
}

//===========================================================================
// Se a fita ficou sem ser lida por muito tempo, para na posição do último acesso
static void VerificaPausa()
{
	// A pausa automática vem desligada: um gravador de verdade não para
	// sozinho, e carregadores rápidos esperam a fita andar enquanto gravam o
	// disco (no Burgertime são 17 s de tom entre um bloco e outro).
	if (TapePausaAuto &&
		TapeEstado == TAPE_REPRODUZINDO &&
		(__int64)(g_nCumulativeCycles - TapeUltAcesso) > TAPE_PAUSA_CICLOS)
	{
		TapeBase   = PosicaoFita(TapeUltAcesso);
		TapeEstado = TAPE_PAUSADO;
	}
}

//===========================================================================
// Atualiza o indicador K7 da barra de status quando o estado ou o contador muda
static void AtualizaIndicador()
{
	int luz  = TapeLuz();
	int cont = TapeContador();

	if (luz != TapeLuzAtual || cont != TapeContAtual)
	{
		TapeLuzAtual  = luz;
		TapeContAtual = cont;
		FrameRefreshStatus(DRAW_BACKGROUND | DRAW_LEDS);
	}
}

//===========================================================================
static WORD Le16(const BYTE *p)
{
	return (WORD)(p[0] | (p[1] << 8));
}

//===========================================================================
static DWORD Le32(const BYTE *p)
{
	return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

//===========================================================================
static void Grava16(BYTE *p, DWORD v)
{
	p[0] = (BYTE)(v);
	p[1] = (BYTE)(v >> 8);
}

//===========================================================================
static void Grava32(BYTE *p, DWORD v)
{
	p[0] = (BYTE)(v);
	p[1] = (BYTE)(v >> 8);
	p[2] = (BYTE)(v >> 16);
	p[3] = (BYTE)(v >> 24);
}

//===========================================================================
// Decodificador de amostras do WAV (qualquer formato -> inteiro de 16 bits,
// com os canais misturados em um só)
typedef struct {
	const BYTE *dados;
	DWORD       frames;
	WORD        canais;
	WORD        bits;
	int         ponto_flutuante;
	DWORD       bytesamostra;
	DWORD       bytesframe;
} WAVDEC;

static int WavAmostra(const WAVDEC *w, const BYTE *p)
{
	switch (w->bits)
	{
	case 8:
		return ((int)p[0] - 128) << 8;
	case 16:
		return (short)Le16(p);
	case 24:
		return (short)Le16(p+1);
	default: // 32 bits
		if (w->ponto_flutuante)
		{
			float f;
			int   v;
			memcpy(&f, p, 4);
			v = (int)(f * 32767.0f);
			if (v >  32767) v =  32767;
			if (v < -32767) v = -32767;
			return v;
		}
		return (short)Le16(p+2);
	}
}

static int WavFrame(const WAVDEC *w, DWORD i)
{
	const BYTE *p = w->dados + (size_t)i * w->bytesframe;
	int soma = 0;
	int c;

	if (w->canais == 1)
		return WavAmostra(w, p);
	for (c = 0; c < w->canais; c++)
		soma += WavAmostra(w, p + c * w->bytesamostra);
	return soma / w->canais;
}

//===========================================================================
// Conserta falhas (dropouts) no tom piloto: um trecho mudo de 1,5 a 60 ms
// entre dois pulsos de tom piloto (~650 us) vira vários pulsos de 650 us. Sem
// isso a ROM acha que a falha é o sincronismo e lê lixo (ERR). Os bits de dados
// têm pulsos de 250/500 us (ou menos), então não são tocados.
static void ConsertaFalhas(std::vector<double> &t, DWORD taxa)
{
	std::vector<double> n;
	size_t r;
	double pmin = 0.5e-3 * taxa, pmax = 0.8e-3 * taxa;
	double gmin = 1.5e-3 * taxa, gmax = 60e-3 * taxa, pw = 0.65e-3 * taxa;

	if (t.size() < 4)
		return;
	n.reserve(t.size());
	for (r = 0; r < t.size(); r++)
	{
		n.push_back(t[r]);
		if (r >= 1 && r + 2 < t.size())
		{
			double ant  = t[r]   - t[r-1];
			double L    = t[r+1] - t[r];
			double prox = t[r+2] - t[r+1];
			if (L >= gmin && L <= gmax && ant >= pmin && ant <= pmax && prox >= pmin && prox <= pmax)
			{
				int k = (int)(L / pw + 0.5);
				int j;
				if (k < 1) k = 1;
				if (!(k & 1)) k++;			// Número ímpar: volta ao mesmo nível no fim
				for (j = 1; j < k; j++)
					n.push_back(t[r] + L * j / k);
			}
		}
	}
	t.swap(n);
}

//===========================================================================
// Lê o arquivo WAV e converte para níveis 0/1. Retorna 1 se OK.
static int CarregaWav(const char *arq, std::vector<double> &trans, BYTE &nivelIni, double &total, DWORD &taxa, char *erro)
{
	FILE *f;
	long  tam;
	std::vector<BYTE> buf;
	DWORD pos;
	int   achoufmt = 0, achoudata = 0;
	WORD  tag = 0, canais = 0, bits = 0;
	DWORD dataini = 0, datatam = 0;
	WAVDEC w;
	DWORD i;
	double soma, media, pico, histe;
	DWORD  hist[513];
	int    nivel;

	f = fopen(arq, "rb");
	if (!f)
	{
		strcpy(erro, "Não foi possível abrir o arquivo.");
		return 0;
	}
	fseek(f, 0, SEEK_END);
	tam = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (tam < 44 || tam > TAPE_MAX_WAV)
	{
		fclose(f);
		strcpy(erro, "Tamanho de arquivo inválido para um WAV (máximo 256 MB).");
		return 0;
	}
	buf.resize(tam);
	if (fread(&buf[0], 1, tam, f) != (size_t)tam)
	{
		fclose(f);
		strcpy(erro, "Erro ao ler o arquivo.");
		return 0;
	}
	fclose(f);

	if (memcmp(&buf[0], "RIFF", 4) || memcmp(&buf[8], "WAVE", 4))
	{
		strcpy(erro, "O arquivo não é um WAV (RIFF/WAVE).");
		return 0;
	}

	// Percorre os chunks procurando "fmt " e "data"
	pos = 12;
	while ((unsigned __int64)pos + 8 <= (unsigned __int64)tam)
	{
		DWORD sz    = Le32(&buf[pos+4]);
		DWORD corpo = pos + 8;
		unsigned __int64 prox = (unsigned __int64)corpo + sz + (sz & 1);

		if (!memcmp(&buf[pos], "fmt ", 4))
		{
			if (sz >= 16 && (unsigned __int64)corpo + 16 <= (unsigned __int64)tam)
			{
				tag    = Le16(&buf[corpo]);
				canais = Le16(&buf[corpo+2]);
				taxa   = Le32(&buf[corpo+4]);
				bits   = Le16(&buf[corpo+14]);
				if (tag == 0xFFFE && sz >= 40 && (unsigned __int64)corpo + 26 <= (unsigned __int64)tam)
					tag = Le16(&buf[corpo+24]);		// WAVE_FORMAT_EXTENSIBLE
				achoufmt = 1;
			}
		}
		else if (!memcmp(&buf[pos], "data", 4))
		{
			DWORD resto = (DWORD)tam - corpo;
			dataini  = corpo;
			datatam  = (sz == 0 || sz > resto) ? resto : sz;	// aceita tamanho 0/inválido (WAV em streaming)
			achoudata = 1;
			break;
		}
		if (prox > (unsigned __int64)tam)
			break;
		pos = (DWORD)prox;
	}
	if (!achoufmt || !achoudata)
	{
		strcpy(erro, "WAV inválido (faltam os blocos 'fmt ' ou 'data').");
		return 0;
	}
	if (!((tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
		  (tag == 3 && bits == 32)))
	{
		strcpy(erro, "Formato de WAV não suportado (use PCM de 8, 16, 24 ou 32 bits).");
		return 0;
	}
	if (canais < 1 || canais > 8)
	{
		strcpy(erro, "Número de canais inválido no WAV.");
		return 0;
	}
	if (taxa < TAPE_TAXA_MIN)
	{
		strcpy(erro, "Taxa de amostragem muito baixa (mínimo 8000 Hz).");
		return 0;
	}

	w.dados           = &buf[dataini];
	w.canais          = canais;
	w.bits            = bits;
	w.ponto_flutuante = (tag == 3);
	w.bytesamostra    = bits / 8;
	w.bytesframe      = w.bytesamostra * canais;
	w.frames          = datatam / w.bytesframe;
	if (w.frames == 0)
	{
		strcpy(erro, "O WAV não contém amostras.");
		return 0;
	}

	// Passo 1: nível DC (média)
	soma = 0.0;
	for (i = 0; i < w.frames; i++)
		soma += WavFrame(&w, i);
	media = soma / (double)w.frames;

	// Passo 2: pico do sinal (percentil 99, imune a cliques isolados)
	memset(hist, 0, sizeof(hist));
	for (i = 0; i < w.frames; i++)
	{
		double v = WavFrame(&w, i) - media;
		int    b;
		if (v < 0) v = -v;
		b = (int)(v / 64.0);
		if (b > 512) b = 512;
		hist[b]++;
	}
	{
		double alvo = (double)w.frames * 0.99;
		double acum = 0.0;
		int    b;
		for (b = 0; b <= 512; b++)
		{
			acum += hist[b];
			if (acum >= alvo)
				break;
		}
		pico = (double)(b + 1) * 64.0;
	}
	if (pico < TAPE_MIN_PICO)
	{
		strcpy(erro, "O WAV não tem sinal (arquivo mudo ou muito baixo).");
		return 0;
	}
	histe = pico * TAPE_HISTERESE;

	// Passo 3: gatilho Schmitt -> instantes das transições. O instante de cada
	// transição é refinado pelo cruzamento por zero (interpolação linear entre
	// duas amostras), como faz o comparador da entrada de cassete do Apple II.
	// Sem isso, pulsos curtos de WAVs rápidos (40 a 80 us) ficam com a largura
	// errada em até uma amostra e viram bits trocados.
	trans.clear();
	nivel    = ((WavFrame(&w, 0) - media) >= 0.0) ? 1 : 0;
	nivelIni = (BYTE)nivel;
	for (i = 0; i < w.frames; i++)
	{
		double v = WavFrame(&w, i) - media;
		int    virou = 0;
		if (nivel)
		{
			if (v < -histe) { nivel = 0; virou = 1; }
		}
		else
		{
			if (v >  histe) { nivel = 1; virou = 1; }
		}
		if (virou)
		{
			double t  = (double)i - 0.5;
			double vj = v;
			DWORD  j  = i;
			int    passos = 0;
			while (j > 0 && passos < 64)
			{
				double vp = WavFrame(&w, j-1) - media;
				if ((nivel && vp <= 0.0) || (!nivel && vp >= 0.0))
				{
					if (vj != vp)
						t = (double)(j-1) + (0.0 - vp) / (vj - vp);
					break;
				}
				j--;
				vj = vp;
				passos++;
			}
			if (!trans.empty() && t <= trans[trans.size()-1])
				t = trans[trans.size()-1] + 1e-6;
			trans.push_back(t);
		}
	}
	total = (double)w.frames;
	ConsertaFalhas(trans, taxa);
	return 1;
}

//===========================================================================
// Renderiza as transições como onda quadrada e grava o WAV. Retorna 1 se OK.
static int GravaWav(const char *arq, const std::vector<unsigned __int64> &tr, char *erro)
{
	FILE   *f;
	BYTE    cab[44];
	BYTE    buf[8192];
	size_t  nb = 0;
	size_t  p  = 0;
	DWORD   taxa  = TAPE_TAXA_SAVE;
	DWORD   folga = taxa / 10;					// 0,1 s de silêncio no começo e no fim
	double  ciclosamostra = CLK_6502 / (double)taxa;
	double  duracao = (double)(__int64)(tr[tr.size()-1] - tr[0]);
	DWORD   total = folga + (DWORD)(duracao / ciclosamostra) + folga + 1;
	DWORD   s;
	int     ok = 1;

	f = fopen(arq, "wb");
	if (!f)
	{
		strcpy(erro, "Não foi possível criar o arquivo WAV.");
		return 0;
	}

	// Cabeçalho WAV (PCM, mono, 8 bits)
	memcpy(cab,      "RIFF", 4);
	Grava32(cab+4,   36 + total);
	memcpy(cab+8,    "WAVE", 4);
	memcpy(cab+12,   "fmt ", 4);
	Grava32(cab+16,  16);
	Grava16(cab+20,  1);			// PCM
	Grava16(cab+22,  1);			// mono
	Grava32(cab+24,  taxa);
	Grava32(cab+28,  taxa);			// bytes por segundo
	Grava16(cab+32,  1);			// alinhamento
	Grava16(cab+34,  8);			// bits
	memcpy(cab+36,   "data", 4);
	Grava32(cab+40,  total);
	if (fwrite(cab, 1, sizeof(cab), f) != sizeof(cab))
		ok = 0;

	for (s = 0; ok && s < total; s++)
	{
		double t = ((double)s - (double)folga) * ciclosamostra;	// ciclos relativos à 1a transição
		while (p < tr.size() && (double)(__int64)(tr[p] - tr[0]) <= t)
			p++;
		buf[nb++] = (p & 1) ? TAPE_NIVEL_ALTO : TAPE_NIVEL_BAIXO;
		if (nb == sizeof(buf))
		{
			if (fwrite(buf, 1, nb, f) != nb)
				ok = 0;
			nb = 0;
		}
	}
	if (ok && nb && fwrite(buf, 1, nb, f) != nb)
		ok = 0;
	if (fclose(f) != 0)
		ok = 0;
	if (!ok)
		strcpy(erro, "Erro ao gravar o arquivo WAV (disco cheio?).");
	return ok;
}

//===========================================================================
// Encerra a gravação em andamento, gravando o WAV. Retorna 1 se gravou,
// 0 se não havia nada gravado, -1 se deu erro (mensagem em erro).
static int FinalizaGravacao(char *erro)
{
	int r = 0;

	if (TapeEstado != TAPE_GRAVANDO)
		return 0;
	if (TapeTransicoes.size() >= 2)
		r = GravaWav(TapeArqGravacao, TapeTransicoes, erro) ? 1 : -1;
	TapeTransicoes.clear();
	std::vector<unsigned __int64>().swap(TapeTransicoes);
	TapeEstado = TAPE_OCIOSO;
	return r;
}

// Funções Globais

//===========================================================================
void TapeFinaliza()
{
	char erro[256];

	FinalizaGravacao(erro);		// Não perde a gravação ao fechar o emulador
#ifdef DEBUGTAPE
	FreeConsole();
#endif
}

//===========================================================================
void TapeInicializa()
{
	TapeEstado   = TAPE_OCIOSO;
	TapeCicloIni = 0;
	TapeUltimo   = 0;
	TapeNivelIni = 0;
	TapeTotal    = 0.0;
	TapeIdxCache = 0;
	TapeTaxa     = 0;
	TapeBase     = 0.0;
	TapeUltAcesso = 0;
#ifdef _WINDOWS
	{
		DWORD v = 0;
		if (RegLoadValue(PREFERENCIAS, PAUSAAUTOK7, 1, &v))
			TapePausaAuto = v ? 1 : 0;
		if (RegLoadValue(PREFERENCIAS, INVERTEK7, 1, &v))
			TapeInverte = v ? 1 : 0;
	}
#endif
	std::vector<double>().swap(TapeTrans);
	std::vector<unsigned __int64>().swap(TapeTransicoes);
#ifdef DEBUGTAPE
	AllocConsole();
#endif
}

//===========================================================================
void TapeAtualiza(DWORD totalcycles)
{
	// A posição da fita e as transições são calculadas direto pelo contador
	// de ciclos (g_nCumulativeCycles) nos acessos a $C060/$C020. Aqui só se
	// detecta a pausa por inatividade e se atualiza o indicador K7.
	// Enquanto o motor do disco está ligado a fita fica parada: no emulador
	// o disco gravado/lido na velocidade máxima leva muito mais tempo de
	// ciclos do que num Apple real, e a fita (que tem trechos de tom
	// reservados para a gravação em disco) chegaria ao fim antes da hora.
	if (TapeEstado == TAPE_REPRODUZINDO && DiskIsSpinning())
		TapeCicloIni += totalcycles;
	VerificaPausa();
	AtualizaIndicador();
}

//===========================================================================
// Carrega o WAV a ser lido. Retorna 1 se OK, ou 0 com a mensagem em erro.
int TapeCarregar(char *arq, char *erro)
{
	std::vector<double> trans;
	BYTE   nivelIni = 0;
	double total = 0.0;
	DWORD  taxa = 0;
	char   erro2[256];

	if (!CarregaWav(arq, trans, nivelIni, total, taxa, erro))
		return 0;
	FinalizaGravacao(erro2);			// Se estava gravando, encerra
	TapeTrans.swap(trans);
	TapeNivelIni = nivelIni;
	TapeTotal    = total;
	TapeIdxCache = 0;
	TapeTaxa     = taxa;
	TapeUltimo   = NivelEm(TapeTotal);
	TapeEstado = TAPE_ARMADO;
	TapeBase   = 0.0;
	AtualizaIndicador();
	return 1;
}

//===========================================================================
// Começa a gravar para o arquivo WAV. Retorna 1 se OK, ou 0 com a mensagem
// em erro. O arquivo só é completado quando a fita é parada.
int TapeGravar(char *arq, char *erro)
{
	FILE *f;
	char  erro2[256];

	f = fopen(arq, "ab");		// Testa se dá para criar o arquivo (sem apagar o conteúdo)
	if (!f)
	{
		strcpy(erro, "Não foi possível criar o arquivo WAV.");
		return 0;
	}
	fclose(f);

	FinalizaGravacao(erro2);	// Se já estava gravando outro, fecha
	std::vector<double>().swap(TapeTrans);
	TapeTotal = 0.0;
	TapeTransicoes.clear();
	strncpy(TapeArqGravacao, arq, MAX_PATH-1);
	TapeArqGravacao[MAX_PATH-1] = 0;
	TapeCicloGrav = g_nCumulativeCycles;
	TapeEstado = TAPE_GRAVANDO;
	AtualizaIndicador();
	return 1;
}

//===========================================================================
// Para a fita. Se estava gravando, grava o WAV. Retorna o mesmo que
// FinalizaGravacao (1 gravou, 0 nada gravado/nada a fazer, -1 erro).
int TapeParar(char *erro)
{
	int r = 0;

	erro[0] = 0;
	if (TapeEstado == TAPE_GRAVANDO)
		r = FinalizaGravacao(erro);
	std::vector<double>().swap(TapeTrans);
	TapeTotal = 0.0;
	TapeEstado = TAPE_OCIOSO;
	AtualizaIndicador();
	return r;
}

//===========================================================================
// Reset (frio ou quente): a fita de leitura pára onde estava (pausa manual,
// podendo continuar ou rebobinar); se estava gravando, fecha o WAV.
void TapeReset()
{
	char erro[256];

	switch (TapeEstado)
	{
	case TAPE_GRAVANDO:
		FinalizaGravacao(erro);
		break;
	case TAPE_REPRODUZINDO:
		TapeBase       = PosicaoFita(g_nCumulativeCycles);	// Congela agora
		TapeAntesPausa = TAPE_REPRODUZINDO;
		TapeEstado     = TAPE_PAUSA_MANUAL;
		break;
	case TAPE_PAUSADO:
		TapeAntesPausa = TAPE_PAUSADO;
		TapeEstado     = TAPE_PAUSA_MANUAL;
		break;
	default:						// ARMADO, FIM, PAUSA_MANUAL, OCIOSO: nada a fazer
		break;
	}
	AtualizaIndicador();
}

//===========================================================================
int TapePausaAutomatica()
{
	return TapePausaAuto;
}

//===========================================================================
// Liga/desliga a pausa automática (menu Fita K7) e guarda a escolha
void TapeAlternaPausaAuto()
{
	TapePausaAuto = !TapePausaAuto;
	if (!TapePausaAuto && TapeEstado == TAPE_PAUSADO)
	{
		// Desligou com a fita parada por inatividade: volta a andar agora
		TapeEstado    = TAPE_REPRODUZINDO;
		TapeCicloIni  = g_nCumulativeCycles;
		TapeUltAcesso = g_nCumulativeCycles;
	}
#ifdef _WINDOWS
	RegSaveValue(PREFERENCIAS, PAUSAAUTOK7, 1, (DWORD)TapePausaAuto);
#endif
	AtualizaIndicador();
}

//===========================================================================
int TapePolaridadeInvertida()
{
	return TapeInverte;
}

//===========================================================================
// Inverte a polaridade do sinal lido (menu Fita K7). Necessário para fitas
// rápidas que medem só um dos semiciclos, como alguns WAVs "diskserver".
void TapeAlternaInverte()
{
	TapeInverte = !TapeInverte;
	TapeUltimo ^= 1;				// Nível parado da fita também inverte
	TapeLuzAtual = -1;				// Força redesenhar o indicador (mostra "(inv)")
#ifdef _WINDOWS
	RegSaveValue(PREFERENCIAS, INVERTEK7, 1, (DWORD)TapeInverte);
#endif
	AtualizaIndicador();
}

//===========================================================================
// Volta a fita de leitura ao começo. Retorna 1 se havia fita para rebobinar.
int TapeRebobinar()
{
	if (TapeTotal <= 0.0 || TapeEstado == TAPE_GRAVANDO || TapeEstado == TAPE_OCIOSO)
		return 0;
	TapeBase   = 0.0;
	TapeEstado = TAPE_ARMADO;		// Volta a esperar o próximo acesso a $C060 (e sai da pausa manual)
	AtualizaIndicador();
	return 1;
}

//===========================================================================
// Pausa ou continua a fita de leitura (menu Pausar). Retorna 1 se mudou o
// estado, 0 se não havia o que pausar (sem fita, gravando ou fim da fita).
int TapePausar()
{
	switch (TapeEstado)
	{
	case TAPE_PAUSA_MANUAL:			// Continua de onde parou
		TapeEstado = TapeAntesPausa;
		if (TapeEstado == TAPE_REPRODUZINDO)
		{
			TapeCicloIni  = g_nCumulativeCycles;
			TapeUltAcesso = g_nCumulativeCycles;
		}
		break;
	case TAPE_REPRODUZINDO:
		TapeBase       = PosicaoFita(g_nCumulativeCycles);	// Congela agora
		TapeAntesPausa = TAPE_REPRODUZINDO;
		TapeEstado     = TAPE_PAUSA_MANUAL;
		break;
	case TAPE_ARMADO:
	case TAPE_PAUSADO:
		TapeAntesPausa = TapeEstado;
		TapeEstado     = TAPE_PAUSA_MANUAL;
		break;
	default:
		return 0;
	}
	AtualizaIndicador();
	return 1;
}

//===========================================================================
int TapePausada()
{
	return TapeEstado == TAPE_PAUSA_MANUAL;
}

//===========================================================================
// Estado do indicador K7: 0 escuro (sem atividade), 1 amarelo (fita pronta,
// aguardando), 2 verde claro (lendo), 3 vermelho (gravando)
int TapeLuz()
{
	switch (TapeEstado)
	{
	case TAPE_ARMADO:
	case TAPE_PAUSADO:
	case TAPE_PAUSA_MANUAL:	return 1;
	case TAPE_REPRODUZINDO:	return 2;
	case TAPE_GRAVANDO:		return 3;
	default:				return 0;
	}
}

//===========================================================================
// Contador estilo gravador K7 (000 a 999): 1 unidade por segundo de fita.
// Na leitura é a posição no WAV; na gravação, o tempo gravado. Zera ao
// rebobinar, ao selecionar outro WAV e ao começar uma gravação.
int TapeContador()
{
	double seg = 0.0;

	switch (TapeEstado)
	{
	case TAPE_REPRODUZINDO:
		if (TapeTaxa) seg = PosicaoFita(g_nCumulativeCycles) / (double)TapeTaxa;
		break;
	case TAPE_ARMADO:
	case TAPE_PAUSADO:
	case TAPE_PAUSA_MANUAL:
		if (TapeTaxa) seg = TapeBase / (double)TapeTaxa;
		break;
	case TAPE_FIM:
		seg = TapeDuracao();
		break;
	case TAPE_GRAVANDO:
		seg = (double)(__int64)(g_nCumulativeCycles - TapeCicloGrav) / CLK_6502;
		break;
	}
	if (seg < 0.0)
		seg = 0.0;
	return (int)seg % 1000;
}

//===========================================================================
const char *TapeTextoEstado()
{
	static char texto[48];
	const char *nome;

	switch (TapeEstado)
	{
	case TAPE_ARMADO:
	case TAPE_PAUSADO:		nome = "aguardando";	break;
	case TAPE_REPRODUZINDO:	nome = "lendo";			break;
	case TAPE_PAUSA_MANUAL:	nome = "pausada";		break;
	case TAPE_GRAVANDO:		nome = "gravando";		break;
	case TAPE_FIM:			nome = "fim";			break;
	default:
		sprintf(texto, "sem fita%s", TapeInverte ? " (inv)" : "");
		return texto;
	}
	sprintf(texto, "%s %03d%s", nome, TapeContador(), TapeInverte ? " (inv)" : "");
	return texto;
}

//===========================================================================
int TapeSituacao()
{
	return TapeEstado;
}

//===========================================================================
double TapeDuracao()
{
	if (TapeTaxa == 0)
		return 0.0;
	return TapeTotal / (double)TapeTaxa;
}

// Softswitches

//===========================================================================
BYTE __stdcall TapeLeitura(WORD programcounter, BYTE address, BYTE write, BYTE value)
{
	BYTE ruido = MemReturnRandomData(1) & 0x7F;		// Bits 0-6 flutuando

	if (write)
		return ruido;
	VerificaPausa();
	switch (TapeEstado)
	{
	case TAPE_ARMADO:
		TapeBase = 0.0;
		// continua...
	case TAPE_PAUSADO:
		TapeCicloIni  = g_nCumulativeCycles;		// A fita começa (ou retoma) agora
		TapeEstado    = TAPE_REPRODUZINDO;
		// continua...
	case TAPE_REPRODUZINDO:
		{
			double idx = PosicaoFita(g_nCumulativeCycles);
			TapeUltAcesso = g_nCumulativeCycles;
			if (idx >= TapeTotal)
			{
				TapeEstado = TAPE_FIM;
				return ruido | (TapeUltimo ? 0x80 : 0);
			}
			return ruido | (NivelEm(idx) ? 0x80 : 0);
		}
	case TAPE_PAUSA_MANUAL:
		{
			// Fita parada pelo usuário: sinal constante, como um gravador em STOP
			double x = TapeBase;
			if (x >= TapeTotal)
				x = TapeTotal;
			return ruido | (NivelEm(x) ? 0x80 : 0);
		}
	case TAPE_FIM:
		return ruido | (TapeUltimo ? 0x80 : 0);		// Fita parada, sinal constante
	default:
		return MemReturnRandomData(1);				// Sem fita: entrada flutuando
	}
}

//===========================================================================
BYTE __stdcall TapeEscrita(WORD programcounter, BYTE address, BYTE write, BYTE value)
{
	if (TapeEstado == TAPE_GRAVANDO && TapeTransicoes.size() < TAPE_MAX_TRANSICOES)
		TapeTransicoes.push_back(g_nCumulativeCycles);	// Cada acesso inverte o nível
	return MemReturnRandomData(1);
}

// Interface (menu Fita K7)

#ifdef _WINDOWS
//===========================================================================
void TapePausaFita()
{
	if (!TapePausar())
		MessageBox((HWND)framewindow,
					"Não há fita de leitura em andamento para pausar.",
					"Fita K7", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

//===========================================================================
void TapeRebobinaFita()
{
	if (!TapeRebobinar())
		MessageBox((HWND)framewindow,
					"Não há fita de leitura para rebobinar.\nUse Fita K7 > LOAD para selecionar um WAV.",
					"Fita K7", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

//===========================================================================
void TapeSelecionaLeitura()
{
	char directory[MAX_PATH] = "";
	char filename[MAX_PATH]  = "";
	char erro[256]           = "";
	char msg[512];
	OPENFILENAME ofn;

	RegLoadString(PREFERENCIAS, DIRINICK7, 1, directory, MAX_PATH);
	ZeroMemory(&ofn, sizeof(OPENFILENAME));
	ofn.lStructSize     = sizeof(OPENFILENAME);
	ofn.hwndOwner       = (HWND)framewindow;
	ofn.hInstance       = (HINSTANCE)instance;
	ofn.lpstrFilter     = "Fitas K7 (WAV)\0*.wav\0"
						  "Todos os Arquivos\0*.*\0";
	ofn.lpstrFile       = filename;
	ofn.nMaxFile        = MAX_PATH;
	ofn.lpstrInitialDir = directory;
	ofn.Flags           = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
	ofn.lpstrTitle      = "LOAD - Selecione o arquivo WAV da fita a ser lida";
	if (GetOpenFileName(&ofn))
	{
		if (!TapeCarregar(filename, erro))
		{
			FrameMostraMensagemErro(erro);
			return;
		}
		filename[ofn.nFileOffset] = 0;
		if (_stricmp(directory, filename))
			RegSaveString(PREFERENCIAS, DIRINICK7, 1, filename);
		sprintf(msg, "Fita carregada (%.1f segundos).\n\n"
					 "Ela começa a rodar quando o computador ler a entrada de "
					 "cassete (comando LOAD do BASIC ou R do monitor).",
					 TapeDuracao());
		MessageBox((HWND)framewindow, msg, "Fita K7",
					MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
	}
}

//===========================================================================
void TapeSelecionaGravacao()
{
	char directory[MAX_PATH] = "";
	char filename[MAX_PATH]  = "";
	char erro[256]           = "";
	OPENFILENAME ofn;

	RegLoadString(PREFERENCIAS, DIRINICK7, 1, directory, MAX_PATH);
	ZeroMemory(&ofn, sizeof(OPENFILENAME));
	ofn.lStructSize     = sizeof(OPENFILENAME);
	ofn.hwndOwner       = (HWND)framewindow;
	ofn.hInstance       = (HINSTANCE)instance;
	ofn.lpstrFilter     = "Fitas K7 (WAV)\0*.wav\0"
						  "Todos os Arquivos\0*.*\0";
	ofn.lpstrFile       = filename;
	ofn.nMaxFile        = MAX_PATH;
	ofn.lpstrInitialDir = directory;
	ofn.lpstrDefExt     = "wav";
	ofn.Flags           = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
	ofn.lpstrTitle      = "SAVE - Selecione o arquivo WAV onde a fita será gravada";
	if (GetSaveFileName(&ofn))
	{
		if (!TapeGravar(filename, erro))
		{
			FrameMostraMensagemErro(erro);
			return;
		}
		filename[ofn.nFileOffset] = 0;
		if (_stricmp(directory, filename))
			RegSaveString(PREFERENCIAS, DIRINICK7, 1, filename);
		MessageBox((HWND)framewindow,
					"Gravação armada.\n\n"
					"Use o comando SAVE do BASIC ou W do monitor. Ao terminar, "
					"escolha \"Parar fita\" no menu para fechar o arquivo WAV.",
					"Fita K7", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
	}
}

//===========================================================================
void TapeParaFita()
{
	char erro[256];
	int  gravando = (TapeEstado == TAPE_GRAVANDO);
	int  r = TapeParar(erro);

	if (r < 0)
		FrameMostraMensagemErro(erro);
	else if (gravando)
		MessageBox((HWND)framewindow,
					r ? "Gravação encerrada, arquivo WAV salvo."
					  : "Nada foi gravado (o programa não usou a saída de cassete).",
					"Fita K7", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}
#else // _WINDOWS
//===========================================================================
void TapeSelecionaLeitura()
{

}

//===========================================================================
void TapeSelecionaGravacao()
{

}

//===========================================================================
void TapeParaFita()
{

}

//===========================================================================
void TapeRebobinaFita()
{

}

//===========================================================================
void TapePausaFita()
{

}
#endif // _WINDOWS

// EOF
