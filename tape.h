
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

#ifndef TAPE_H
#define TAPE_H

// Protótipos
void TapeFinaliza();
void TapeInicializa();
void TapeAtualiza(DWORD);
int    TapeCarregar(char *, char *);	// Carrega o WAV a ser lido (LOAD)
int    TapeGravar(char *, char *);	// Começa a gravar para o WAV (SAVE)
int    TapeParar(char *);		// Para a fita (grava o WAV se estava gravando)
int    TapeSituacao();
int    TapeRebobinar();
void   TapeReset();				// Reset do micro: a fita pára (pausa) onde estava
int    TapePolaridadeInvertida();	// 1 se a polaridade da fita estiver invertida
void   TapeAlternaInverte();		// Liga/desliga a inversão de polaridade
int    TapePausar();				// Pausa/continua a fita de leitura
int    TapePausada();
int    TapePausaAutomatica();		// 1 se a pausa automática estiver ligada
void   TapeAlternaPausaAuto();			// Volta a fita de leitura ao começo
int    TapeLuz();				// Indicador K7: 0 escuro, 1 amarelo, 2 verde claro, 3 vermelho
int    TapeContador();			// Contador estilo gravador, 000 a 999 (1 por segundo)
const char *TapeTextoEstado();
double TapeDuracao();

// Interface (menu Fita K7)
void TapeSelecionaLeitura();
void TapeSelecionaGravacao();
void TapeParaFita();
void TapeRebobinaFita();
void TapePausaFita();

// Protótipos Softswitches
BYTE __stdcall TapeLeitura(WORD,BYTE,BYTE,BYTE);
BYTE __stdcall TapeEscrita(WORD,BYTE,BYTE,BYTE);

#endif
// EOF